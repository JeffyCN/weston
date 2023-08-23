/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <dlfcn.h>

#include "drm-internal.h"
#include "pixman-renderer.h"
#include "pixel-formats.h"
#include "renderer-gl/gl-renderer.h"
#include "renderer-vulkan/vulkan-renderer.h"
#include "shared/weston-assert.h"
#include "shared/weston-egl-ext.h"
#include "linux-dmabuf.h"
#include "linux-explicit-synchronization.h"
#include "shared/xalloc.h"

/* When initializing EGL, if the preferred buffer format isn't available
 * we may be able to substitute an ARGB format for an XRGB one.
 *
 * This returns NULL if substitution isn't possible. The caller is responsible
 * for checking for NULL before calling gl_renderer->create().
 *
 * This works around https://bugs.freedesktop.org/show_bug.cgi?id=89689
 * but it's entirely possible we'll see this again on other implementations.
 */
static const struct pixel_format_info *
fallback_format_for(const struct pixel_format_info *format)
{
	return pixel_format_get_info_by_opaque_substitute(format->format);
}

static int
drm_backend_create_gl_renderer(struct drm_backend *b)
{
	const struct pixel_format_info *format[3] = {
		b->format,
		fallback_format_for(b->format),
	};
	struct gl_renderer_display_options options = {
		.egl_platform = EGL_PLATFORM_GBM_KHR,
		.egl_native_display = b->gbm,
		.egl_surface_type = EGL_WINDOW_BIT,
		.formats = format,
		.formats_count = 1,
	};

	if (format[1])
		options.formats_count = 2;

	return weston_compositor_init_renderer(b->compositor,
					       WESTON_RENDERER_GL,
					       &options.base);
}

static int
drm_backend_create_vulkan_renderer(struct drm_backend *b)
{
	const struct pixel_format_info *format[3] = {
		b->format,
		fallback_format_for(b->format),
	};
	struct vulkan_renderer_display_options options = {
		.formats = format,
		.formats_count = 1,
	};

	if (format[1])
		options.formats_count = 2;

	return weston_compositor_init_renderer(b->compositor,
					       WESTON_RENDERER_VULKAN,
					       &options.base);
}

int
init_egl(struct drm_backend *b)
{
	struct drm_device *device = b->drm;

	b->gbm = gbm_create_device(device->kms_device->fd);
	if (!b->gbm)
		return -1;

	if (drm_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		b->gbm = NULL;
		return -1;
	}

	return 0;
}

int
init_vulkan(struct drm_backend *b)
{
	struct drm_device *device = b->drm;

	b->gbm = gbm_create_device(device->kms_device->fd);
	if (!b->gbm)
		return -1;

	if (drm_backend_create_vulkan_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		b->gbm = NULL;
		return -1;
	}

	return 0;
}

/**
 * create_gbm_surface - Create a single GBM surface for DRM output
 * @gbm: GBM device handle
 * @output: Target DRM output
 * @fmt: Pre-validated DRM format for the output
 *
 * Return: Valid GBM surface on success, NULL on failure
 */
static struct gbm_surface *
create_gbm_surface(struct gbm_device *gbm, struct drm_output *output,
		   struct weston_drm_format *fmt)
{
	struct weston_mode *mode = output->base.current_mode;
	struct drm_device *device = output->device;
	struct gbm_surface *gbm_surface = NULL;
	const uint64_t *modifiers;
	unsigned int num_modifiers;

	/* HACK: Prefer valid modifiers when fb_modifiers is enabled.
	 * Note: modifiers pointer is reassigned to local array, but
	 * gbm_surface_create_with_modifiers copies the data, so this is safe.
	 */
	if (device->fb_modifiers) {
#define MAX_MODIFIERS 128
		uint64_t _modifiers[MAX_MODIFIERS];
		int i, j;
		modifiers = weston_drm_format_get_modifiers(fmt, &num_modifiers);
		for (i = 0, j = 0; i < (int)num_modifiers; i++) {
			if (DRM_MOD_VALID(modifiers[i]) && j < MAX_MODIFIERS)
				_modifiers[j++] = modifiers[i];
		}
		modifiers = _modifiers;
		num_modifiers = j;

		gbm_surface =
			gbm_surface_create_with_modifiers(gbm,
							  mode->width, mode->height,
							  output->format->format,
							  modifiers, num_modifiers);
	}

	/* We may allocate with no modifiers in the following situations:
	 *
	 * 1. the KMS driver does not support modifiers;
	 * 2. if allocating with modifiers failed, what can happen when the KMS
	 *    display device supports modifiers but the GBM driver does not,
	 *    e.g. the old i915 Mesa driver.
	 */
	if (!gbm_surface)
		gbm_surface = gbm_surface_create(gbm, mode->width, mode->height,
						 output->format->format,
						 output->gbm_bo_flags);

	return gbm_surface;
}

/**
 * create_gbm_surfaces - Create multiple GBM surfaces for multi-buffering
 * @gbm: GBM device handle
 * @output: Target DRM output
 *
 * Each GBM surface is paired with two render buffers (double buffering per surface).
 * Return: true on success, false on failure
 */
static bool
create_gbm_surfaces(struct gbm_device *gbm, struct drm_output *output)
{
	struct drm_plane *plane = output->scanout_handle->plane;
	struct weston_drm_format *fmt;
	unsigned int i;

	fmt = weston_drm_format_array_find_format(&plane->formats,
						  output->format->format);
	if (!fmt) {
		weston_log("format %s not supported by output %s\n",
			   output->format->drm_format_name,
			   output->base.name);
		return false;
	}

	/*
	 * If we cannot use modifiers to allocate the GBM surface and the GBM
	 * device differs from the KMS display device (because we are rendering
	 * on a different GPU), we have to use linear buffers to make sure that
	 * the allocated GBM surface is correctly displayed on the KMS device.
	 */
	if (gbm_device_get_fd(gbm) != output->device->kms_device->fd)
		output->gbm_bo_flags |= GBM_BO_USE_LINEAR;

	/* Create multiple GBM surfaces for multi-buffering */
	for (i = 0; i < output->num_surfaces; i++) {
		output->gbm_surfaces[i] = create_gbm_surface(gbm, output, fmt);
		if (!output->gbm_surfaces[i]) {
			weston_log("failed to create gbm surface\n");
			goto err;
		}
	}

	return true;
err:
	for (i = 0; i < output->num_surfaces; i++) {
		if (output->gbm_surfaces[i])
			gbm_surface_destroy(output->gbm_surfaces[i]);
		output->gbm_surfaces[i] = NULL;
	}
	return false;
}

enum format_alpha_required {
	FORMAT_ALPHA_REQUIRED = true,
	FORMAT_ALPHA_NOT_REQUIRED = false,
};

enum format_component_type {
	FORMAT_COMPONENT_TYPE_ANY,
	FORMAT_COMPONENT_TYPE_FLOAT_ONLY,
};

static const struct pixel_format_info *
find_compatible_format(struct weston_compositor *compositor,
		       struct wl_array *formats, int min_bpc,
		       enum format_component_type component_type,
		       enum format_alpha_required alpha_required)
{
	const struct pixel_format_info **tmp, *p;
	const struct pixel_format_info *candidate = NULL;

	/**
	 * Given a format array, this looks for a format respecting a few
	 * criteria. First of all, this ignores formats that do not contain an
	 * alpha channel when alpha_required == FORMAT_ALPHA_REQUIRED. Similar
	 * for formats that are not floating point when component_type ==
	 * FORMAT_COMPONENT_TYPE_FLOAT_ONLY. Also, it ignores formats that do
	 * not have bits per color channel (bpc) bigger or equal to min_bpc.
	 *
	 * When we have multiple formats matching these criteria, we use the
	 * following to choose:
	 *
	 * 1. a format with lower bytes per pixel (bpp) is favored.
	 *
	 * 2. if FORMAT_ALPHA_REQUIRED:
	 *	  we prefer the format with more bits on the alpha channel
	 *    else
	 *        we prefer the format with more bits on the color channels
	 */
	wl_array_for_each(tmp, formats) {
		p = *tmp;

		/* Skip candidates that do not match minimum criteria. */
		if (component_type == FORMAT_COMPONENT_TYPE_FLOAT_ONLY &&
		    p->component_type != PIXEL_COMPONENT_TYPE_FLOAT)
			continue;
		if (alpha_required == FORMAT_ALPHA_REQUIRED && p->bits.a == 0)
			continue;
		if (p->bits.r < min_bpc || p->bits.g < min_bpc || p->bits.b < min_bpc)
			continue;

		/* No other good candidate so far, so pick this one. */
		if (!candidate) {
			candidate = p;
			continue;
		}

		/**
		 * New candidate, let's compare with old and untie.
		 */

		if (p->bpp > candidate->bpp)
			continue;

		if (alpha_required == FORMAT_ALPHA_REQUIRED) {
			if (p->bits.a <= candidate->bits.a)
				continue;
		} else {
			if (p->bits.r + p->bits.g + p->bits.b <=
			    candidate->bits.r + candidate->bits.g + candidate->bits.b)
				continue;
		}

		candidate = p;
	}

	return candidate;
}

static bool
drm_output_pick_format_egl(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_compositor *compositor = b->compositor;
	const struct weston_renderer *renderer = compositor->renderer;
	const struct pixel_format_info **renderer_formats;
	const struct pixel_format_info **f;
	unsigned int renderer_formats_count;
	struct wl_array supported_formats;
	enum format_component_type component_type;
	uint32_t min_bpc;
	unsigned int i;
	bool ret = true;
	bool found;

	wl_array_init(&supported_formats);

	/**
	 * This computes the intersection between renderer formats supported by
	 * EGL and the output scanout plane supported formats. We need that as
	 * we want to select a format supported by both.
	 */
	renderer_formats =
		renderer->gl->get_supported_rendering_formats(b->compositor,
							      &renderer_formats_count);
	for (i = 0; i < renderer_formats_count; i++) {
		struct drm_plane *scanout_plane = output->scanout_handle->plane;

		if (!weston_drm_format_array_find_format(&scanout_plane->formats,
							 renderer_formats[i]->format))
			continue;

		f = wl_array_add(&supported_formats, sizeof(*f));
		*f = renderer_formats[i];
	}

	if (output->base.from_blend_to_output_by_backend) {
		component_type = FORMAT_COMPONENT_TYPE_FLOAT_ONLY;
		min_bpc = 16;
	} else if (output->base.eotf_mode != WESTON_EOTF_MODE_SDR) {
		component_type = FORMAT_COMPONENT_TYPE_ANY;
		min_bpc = 10;
	} else {
		/**
		 * If no requirements, we simply use b->format instead of
		 * looking for a format with bpc >= min_bpc.
		 */
		min_bpc = 0;
	}

	if (min_bpc != 0) {
		if (output->has_underlay) {
			output->format =
				find_compatible_format(compositor, &supported_formats,
						       min_bpc, component_type,
						       FORMAT_ALPHA_REQUIRED);
			if (output->format)
				goto done;

			weston_log("Disabling underlay planes: EGL GBM or the primary plane for output '%s'\n" \
				   "does not support format with min bpc %u and alpha channel.\n",
				   output->base.name, min_bpc);
			output->has_underlay = false;
		}

		output->format =
			find_compatible_format(compositor, &supported_formats,
					       min_bpc, component_type,
					       FORMAT_ALPHA_NOT_REQUIRED);
		if (output->format)
			goto done;

		weston_log("Error: EGL GBM or the primary plane for output '%s' does not support format\n" \
			   "with min bpc %u.\n", output->base.name, min_bpc);
		ret = false;
		goto done;
	}

	found = false;
	wl_array_for_each(f, &supported_formats) {
		if ((*f)->format == b->format->format) {
			found = true;
			break;
		}
	}
	if (!found) {
		weston_log("Error: format %s unsupported by EGL GBM or the primary plane for output '%s'.\n",
			   b->format->drm_format_name, output->base.name);
		ret = false;
		goto done;
	}

	if (output->has_underlay && (b->format->bits.a == 0)) {
		weston_log("Disabling underlay planes: b->format %s does not have alpha channel,\n"
			   "which is required to support underlay planes.\n",
			   b->format->drm_format_name);
		output->has_underlay = false;
	}

	output->format = b->format;

done:
	wl_array_release(&supported_formats);
	return ret;
}

/* Init output state that depends on gl or gbm */
int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b)
{
	const struct weston_renderer *renderer = b->compositor->renderer;
	const struct weston_mode *mode = output->base.current_mode;
	const struct pixel_format_info *format[2] = { 0 };
	struct gl_renderer_output_options options;

	if (!output->format && !drm_output_pick_format_egl(output))
		return -1;

	format[0] = output->format;
	if (!output->has_underlay)
		format[1] = fallback_format_for(output->format);

	options.formats = format;
	options.formats_count = format[1] ? 2 : 1;
	options.area.x = 0;
	options.area.y = 0;
	options.area.width = mode->width;
	options.area.height = mode->height;
	options.fb_size.width = mode->width;
	options.fb_size.height = mode->height;
	options.window_for_legacy = NULL;
	options.window_for_platform = NULL;
	unsigned int i;

	output->current_image = 0;

	if (!create_gbm_surfaces(b->gbm, output))
		return -1;

	if (renderer->gl->output_window_create(&output->base, &options) < 0) {
		weston_log("failed to create gl renderer output state\n");
		drm_output_fini_egl(output);
		return -1;
	}

	/* Each GBM surface is associated with two render buffers */
	for (i = 0; i < output->num_images; i += 2) {
		struct gbm_surface *gbm_surface = output->gbm_surfaces[i / 2];
		options.window_for_legacy =
			(EGLNativeWindowType) gbm_surface;
		options.window_for_platform = gbm_surface;

		output->renderbuffer[i] =
			renderer->gl->create_buffer(&output->base,
						    &options);
		if (!output->renderbuffer[i]) {
			weston_log("failed to create window surface\n");
			drm_output_fini_egl(output);
			return -1;
		}

		/* There are two buffers per surface, duplicate buffer to
		 * share EGL surface (no ownership)
		 */
		output->renderbuffer[i + 1] =
			renderer->gl->dup_buffer(&output->base,
						 output->renderbuffer[i]);
		if (!output->renderbuffer[i + 1]) {
			weston_log("failed to dup window surface\n");
			drm_output_fini_egl(output);
			return -1;
		}
	}

	return 0;
}

static struct gbm_bo *
drm_gbm_create_bo(struct gbm_device *gbm, struct drm_output *output)
{
	struct weston_mode *mode = output->base.current_mode;
	struct drm_plane *plane = output->scanout_handle->plane;
	struct weston_drm_format *fmt;
	const uint64_t *modifiers;
	unsigned int num_modifiers;
	struct gbm_bo *bo = NULL;

	/*
	 * TODO: Currently, this method allocates a buffer based on the list
	 * of acceptable modifiers received from the DRM backend but does not
	 * check it against formats renderable by the renderer.
	 * To support cases where the renderer may not support the same
	 * modifiers (e.g. Vulkan software renderer) it should match against
	 * renderer modifiers.
	 */

	fmt = weston_drm_format_array_find_format(&plane->formats,
						  output->format->format);
	if (!fmt) {
		weston_log("format %s not supported by output %s\n",
			   output->format->drm_format_name,
			   output->base.name);
		return NULL;
	}

	if (!weston_drm_format_has_modifier(fmt, DRM_FORMAT_MOD_INVALID)) {
		modifiers = weston_drm_format_get_modifiers(fmt, &num_modifiers);
		bo = gbm_bo_create_with_modifiers(gbm, mode->width, mode->height,
						  output->format->format,
						  modifiers, num_modifiers);
	}

	/*
	 * If we cannot use modifiers to allocate the GBM surface and
	 * the GBM device differs from the KMS display device, try to
	 * use linear buffers and hope that the allocated GBM surface
	 * is correctly displayed on the KMS device.
	 */
	if (gbm_device_get_fd(gbm) != output->device->kms_device->fd)
		output->gbm_bo_flags |= GBM_BO_USE_LINEAR;

	if (!bo) {
		bo = gbm_bo_create(gbm, mode->width, mode->height,
				   output->format->format, output->gbm_bo_flags);
	}

	return bo;
}

struct drm_gbm_dmabuf {
	struct linux_dmabuf_memory base;
	struct gbm_bo *bo;
};

static void
drm_gbm_dmabuf_destroy(struct linux_dmabuf_memory *dmabuf)
{
	struct dmabuf_attributes *attributes;
	struct drm_gbm_dmabuf *drm_gbm_dmabuf;
	struct gbm_bo *bo;

	drm_gbm_dmabuf = container_of(dmabuf, struct drm_gbm_dmabuf, base);
	bo = drm_gbm_dmabuf->bo;
	assert(bo);

	gbm_bo_destroy(bo);

	attributes = dmabuf->attributes;
	for (int i = 0; i < attributes->n_planes; ++i)
		close(attributes->fd[i]);
	free(dmabuf->attributes);

	free(drm_gbm_dmabuf);
}

static struct drm_gbm_dmabuf *
drm_gbm_bo_get_dmabuf(struct gbm_device *gbm, struct drm_output *output, struct gbm_bo *bo)
{
	struct drm_gbm_dmabuf *drm_gbm_dmabuf;
	struct dmabuf_attributes *attributes;

	attributes = xzalloc(sizeof(*attributes));
	attributes->width = gbm_bo_get_width(bo);
	attributes->height = gbm_bo_get_height(bo);
	attributes->format = gbm_bo_get_format(bo);
	attributes->n_planes = gbm_bo_get_plane_count(bo);
	for (int i = 0; i < attributes->n_planes; ++i) {
		attributes->fd[i] = gbm_bo_get_fd(bo);
		attributes->stride[i] = gbm_bo_get_stride_for_plane(bo, i);
		attributes->offset[i] = gbm_bo_get_offset(bo, i);
	}
	attributes->modifier = gbm_bo_get_modifier(bo);

	drm_gbm_dmabuf = xzalloc(sizeof(*drm_gbm_dmabuf));
	drm_gbm_dmabuf->base.attributes = attributes;
	drm_gbm_dmabuf->base.destroy = drm_gbm_dmabuf_destroy;
	drm_gbm_dmabuf->bo = bo;

	return drm_gbm_dmabuf;
}

static void
create_renderbuffers(struct gbm_device *gbm, struct drm_output *output, unsigned int n)
{
	struct weston_renderer *renderer = output->base.compositor->renderer;

	for (unsigned int i = 0; i < n; i++) {
		struct drm_gbm_dmabuf *drm_gbm_dmabuf;
		struct gbm_bo *bo;

		bo = drm_gbm_create_bo(gbm, output);
		if (!bo) {
			weston_log("failed to allocate bo\n");
			return;
		}

		drm_gbm_dmabuf = drm_gbm_bo_get_dmabuf(gbm, output, bo);
		if (!drm_gbm_dmabuf) {
			weston_log("failed to allocate dmabuf\n");
			return;
		}

		output->renderbuffer[i] =
			renderer->create_renderbuffer_dmabuf(&output->base,
							     &drm_gbm_dmabuf->base,
							     NULL, NULL);
		if (!output->renderbuffer[i]) {
			weston_log("failed to allocate renderbuffer\n");
			return;
		}

		output->linux_dmabuf_memory[i] = &drm_gbm_dmabuf->base;
	}
}

static bool
drm_output_pick_format_vulkan(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;

	/* Any other value of eotf_mode requires color-management, which is not
	 * yet supported by vulkan-renderer. */
	assert(output->base.eotf_mode == WESTON_EOTF_MODE_SDR);

	if (!b->format->vulkan_format) {
		weston_log("Error: failed to pick format for output '%s', format %s unsupported by vulkan-renderer.\n",
			   output->base.name, b->format->drm_format_name);
		return false;
	}

	assert(b->format);
	output->format = b->format;

	if (output->has_underlay && (output->format->bits.a == 0)) {
		weston_log("Disabling underlay planes: output '%s' with format %s does not have alpha channel,\n"
			   "which is required to support underlay planes.\n",
			   output->base.name, output->format->drm_format_name);
		output->has_underlay = false;
	}

	return true;
}

/* Init output state that depends on vulkan */
int
drm_output_init_vulkan(struct drm_output *output, struct drm_backend *b)
{
	const struct weston_mode *mode = output->base.current_mode;
	struct weston_renderer *renderer = b->compositor->renderer;

	if (!output->format && !drm_output_pick_format_vulkan(output))
		return -1;

	const struct vulkan_renderer_surfaceless_options options = {
		.area.x = 0,
		.area.y = 0,
		.area.width = mode->width,
		.area.height = mode->height,
		.fb_size.width = mode->width,
		.fb_size.height = mode->height,
	};

	if (renderer->vulkan->output_surfaceless_create(&output->base, &options) < 0) {
		weston_log("failed to create vulkan renderer output state\n");
		return -1;
	}

	create_renderbuffers(b->gbm, output, ARRAY_LENGTH(output->renderbuffer));
	if (!output->linux_dmabuf_memory[0]) {
		weston_log("failed to create dmabufs\n");
		return -1;
	}

	return 0;
}

void
drm_output_fini_egl(struct drm_output *output)
{
	struct drm_backend *b = output->backend;
	const struct weston_renderer *renderer = b->compositor->renderer;
	unsigned int i;

	/* Destroying the GBM surface will destroy all our GBM buffers,
	 * regardless of refcount. Ensure we destroy them here. */
	if (!b->compositor->shutting_down && output->scanout_handle &&
	    output->scanout_handle->plane->state_cur->fb &&
	    output->scanout_handle->plane->state_cur->fb->type == BUFFER_GBM_SURFACE) {
		drm_plane_reset_state(output->scanout_handle->plane);
	}

	renderer->gl->output_destroy(&output->base);

	/* Should be destroyed in the gl->output_destroy() */
	for (i = 0; i < ARRAY_LENGTH(output->renderbuffer); i++)
		output->renderbuffer[i] = NULL;

	for (i = 0; i < output->num_surfaces; i++) {
		if (output->gbm_surfaces[i])
			gbm_surface_destroy(output->gbm_surfaces[i]);
		output->gbm_surfaces[i] = NULL;
	}
}

void
drm_output_fini_vulkan(struct drm_output *output)
{
	struct drm_backend *b = output->backend;
	const struct weston_renderer *renderer = b->compositor->renderer;

	if (!b->compositor->shutting_down && output->scanout_handle &&
	    output->scanout_handle->plane->state_cur->fb &&
	    output->scanout_handle->plane->state_cur->fb->type == BUFFER_DMABUF_BACKEND) {
		drm_plane_reset_state(output->scanout_handle->plane);
	}

	for (unsigned int i = 0; i < ARRAY_LENGTH(output->renderbuffer); i++)
		renderer->destroy_renderbuffer(output->renderbuffer[i]);

	renderer->vulkan->output_destroy(&output->base);
}

struct drm_fb *
drm_output_render_gl(struct drm_output_state *state, pixman_region32_t *damage)
{
	struct drm_output *output = state->output;
	struct drm_device *device = output->device;
	struct weston_renderbuffer *renderbuffer = NULL;
	struct gbm_surface *gbm_surface;
	struct gbm_bo *bo;
	struct drm_fb *ret;

	/* Cycle through GBM surfaces for multi-buffering */
	gbm_surface = output->gbm_surfaces[output->current_surface];

	/* HACK: Renderbuffer is only used to pass EGL surface to renderer */
	renderbuffer = output->renderbuffer[output->current_image];

	/* Cycle through buffers to reduce tearing */
	output->current_image = (output->current_image + 1) % output->num_images;
	output->current_surface = output->current_image / 2;

	output->base.compositor->renderer->repaint_output(&output->base,
							  damage, renderbuffer);

	bo = gbm_surface_lock_front_buffer(gbm_surface);
	if (!bo) {
		weston_log("failed to lock front buffer: %s\n",
			   strerror(errno));
		return NULL;
	}

	/* Output transparent/opaque image according to the format required by
	 * the client. */
	ret = drm_fb_get_from_bo(bo, device, !output->format->opaque_substitute,
	                         BUFFER_GBM_SURFACE);
	if (!ret) {
		weston_log("failed to get drm_fb for bo\n");
		gbm_surface_release_buffer(gbm_surface, bo);
		return NULL;
	}
	ret->gbm_surface = gbm_surface;

	return ret;
}

struct drm_fb *
drm_output_render_vulkan(struct drm_output_state *state, pixman_region32_t *damage)
{
	struct drm_output *output = state->output;
	struct drm_device *device = output->device;
	struct linux_dmabuf_memory *dmabuf;
	struct drm_fb *ret;

	output->base.compositor->renderer->repaint_output(&output->base,
							  damage,
							  output->renderbuffer[output->current_image]);

	dmabuf = output->linux_dmabuf_memory[output->current_image];
	if (!dmabuf) {
		weston_log("failed to get dmabuf\n");
		return NULL;
	}

	/* Output transparent/opaque image according to the format required by
	 * the client. */
	ret = drm_fb_get_from_dmabuf_attributes(dmabuf->attributes, device,
						!output->format->opaque_substitute,
						false, true, NULL);
	if (!ret) {
		weston_log("failed to get drm_fb for dmabuf\n");
		return NULL;
	}

	/* Cycle through buffers to reduce tearing. */
	output->current_image = (output->current_image + 1) % ARRAY_LENGTH(output->renderbuffer);

	return ret;
}
