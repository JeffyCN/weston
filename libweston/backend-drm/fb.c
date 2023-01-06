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

#include <stdint.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libweston/libweston.h>
#include <libweston/backend-drm.h>
#include <libweston/pixel-formats.h>
#include <libweston/linux-dmabuf.h>
#include "shared/helpers.h"
#include "shared/weston-drm-fourcc.h"
#include "drm-internal.h"
#include "linux-dmabuf.h"

static void
drm_fb_destroy(struct drm_fb *fb)
{
	if (fb->fb_id != 0)
		drmModeRmFB(fb->fd, fb->fb_id);
	free(fb);
}

static void
drm_fb_destroy_dumb(struct drm_fb *fb)
{
	struct drm_mode_destroy_dumb destroy_arg;

	assert(fb->type == BUFFER_PIXMAN_DUMB);

	if (fb->map && fb->size > 0)
		munmap(fb->map, fb->size);

	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = fb->handles[0];
	drmIoctl(fb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);

	drm_fb_destroy(fb);
}

static int
drm_fb_addfb(struct drm_device *device, struct drm_fb *fb)
{
	int ret = -EINVAL;
	uint64_t mods[4] = { };
	size_t i;

	/* If we have a modifier set, we must only use the WithModifiers
	 * entrypoint; we cannot import it through legacy ioctls. */
	if (device->fb_modifiers && fb->modifier != DRM_FORMAT_MOD_INVALID) {
		/* KMS demands that if a modifier is set, it must be the same
		 * for all planes. */
		for (i = 0; i < ARRAY_LENGTH(mods) && fb->handles[i]; i++)
			mods[i] = fb->modifier;
		ret = drmModeAddFB2WithModifiers(fb->fd, fb->width, fb->height,
						 fb->format->format,
						 fb->handles, fb->strides,
						 fb->offsets, mods, &fb->fb_id,
						 DRM_MODE_FB_MODIFIERS);
		return ret;
	}

	ret = drmModeAddFB2(fb->fd, fb->width, fb->height, fb->format->format,
			    fb->handles, fb->strides, fb->offsets, &fb->fb_id,
			    0);
	if (ret == 0)
		return 0;

	/* Legacy AddFB can't always infer the format from depth/bpp alone, so
	 * check if our format is one of the lucky ones. */
	if (!fb->format->addfb_legacy_depth || !fb->format->bpp)
		return ret;

	/* Cannot fall back to AddFB for multi-planar formats either. */
	if (fb->handles[1] || fb->handles[2] || fb->handles[3])
		return ret;

	ret = drmModeAddFB(fb->fd, fb->width, fb->height,
			   fb->format->addfb_legacy_depth, fb->format->bpp,
			   fb->strides[0], fb->handles[0], &fb->fb_id);
	return ret;
}

struct drm_fb *
drm_fb_create_dumb(struct drm_device *device, int width, int height,
		   uint32_t format)
{
	struct drm_fb *fb;
	int ret;

	struct drm_mode_create_dumb create_arg;
	struct drm_mode_destroy_dumb destroy_arg;
	struct drm_mode_map_dumb map_arg;

	fb = zalloc(sizeof *fb);
	if (!fb)
		return NULL;
	fb->refcnt = 1;

	fb->format = pixel_format_get_info(format);
	if (!fb->format) {
		weston_log("failed to look up format 0x%lx\n",
			   (unsigned long) format);
		goto err_fb;
	}

	if (!fb->format->addfb_legacy_depth || !fb->format->bpp) {
		weston_log("format 0x%lx is not compatible with dumb buffers\n",
			   (unsigned long) format);
		goto err_fb;
	}

	memset(&create_arg, 0, sizeof create_arg);
	create_arg.bpp = fb->format->bpp;
	create_arg.width = width;
	create_arg.height = height;

	ret = drmIoctl(device->drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
	if (ret)
		goto err_fb;

	fb->type = BUFFER_PIXMAN_DUMB;
	fb->modifier = DRM_FORMAT_MOD_INVALID;
	fb->handles[0] = create_arg.handle;
	fb->strides[0] = create_arg.pitch;
	fb->num_planes = 1;
	fb->size = create_arg.size;
	fb->width = width;
	fb->height = height;
	fb->fd = device->drm.fd;

	if (drm_fb_addfb(device, fb) != 0) {
		weston_log("failed to create kms fb: %s\n", strerror(errno));
		goto err_bo;
	}

	memset(&map_arg, 0, sizeof map_arg);
	map_arg.handle = fb->handles[0];
	ret = drmIoctl(fb->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
	if (ret)
		goto err_add_fb;

	fb->map = mmap(NULL, fb->size, PROT_WRITE,
		       MAP_SHARED, device->drm.fd, map_arg.offset);
	if (fb->map == MAP_FAILED)
		goto err_add_fb;

	return fb;

err_add_fb:
	drmModeRmFB(device->drm.fd, fb->fb_id);
err_bo:
	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = create_arg.handle;
	drmIoctl(device->drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
err_fb:
	free(fb);
	return NULL;
}

struct drm_fb *
drm_fb_ref(struct drm_fb *fb)
{
	fb->refcnt++;
	return fb;
}

#ifdef BUILD_DRM_GBM
static void
drm_fb_destroy_gbm(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;

	assert(fb->type == BUFFER_GBM_SURFACE || fb->type == BUFFER_CLIENT ||
	       fb->type == BUFFER_CURSOR);
	drm_fb_destroy(fb);
}

static void
drm_fb_destroy_dmabuf(struct drm_fb *fb)
{
	/* We deliberately do not close the GEM handles here; GBM manages
	 * their lifetime through the BO. */
	if (fb->bo)
		gbm_bo_destroy(fb->bo);
	drm_fb_destroy(fb);
}

static struct drm_fb *
drm_fb_get_from_dmabuf(struct linux_dmabuf_buffer *dmabuf,
		       struct drm_device *device, bool is_opaque,
		       uint32_t *try_view_on_plane_failure_reasons)
{
#ifndef HAVE_GBM_FD_IMPORT
	/* Importing a buffer to KMS requires explicit modifiers, so
	 * we can't continue with the legacy GBM_BO_IMPORT_FD instead
	 * of GBM_BO_IMPORT_FD_MODIFIER. */
	return NULL;
#else
	struct drm_backend *backend = device->backend;
	struct drm_fb *fb;
	int i;
	struct gbm_import_fd_modifier_data import_mod = {
		.width = dmabuf->attributes.width,
		.height = dmabuf->attributes.height,
		.format = dmabuf->attributes.format,
		.num_fds = dmabuf->attributes.n_planes,
		.modifier = dmabuf->attributes.modifier[0],
	};

	/* We should not import to KMS a buffer that has been allocated using no
	 * modifiers. Usually drivers use linear layouts to allocate with no
	 * modifiers, but this is not a rule. The driver could use, for
	 * instance, a tiling layout under the hood - and both Weston and the
	 * KMS driver can't know. So giving the buffer to KMS is not safe, as
	 * not knowing its layout can result in garbage being displayed. In
	 * short, importing a buffer to KMS requires explicit modifiers. */
	if (dmabuf->attributes.modifier[0] == DRM_FORMAT_MOD_INVALID) {
		if (try_view_on_plane_failure_reasons)
			*try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_DMABUF_MODIFIER_INVALID;
		return NULL;
	}

	/* XXX: TODO:
	 *
	 * Currently the buffer is rejected if any dmabuf attribute
	 * flag is set.  This keeps us from passing an inverted /
	 * interlaced / bottom-first buffer (or any other type that may
	 * be added in the future) through to an overlay.  Ultimately,
	 * these types of buffers should be handled through buffer
	 * transforms and not as spot-checks requiring specific
	 * knowledge. */
	if (dmabuf->attributes.flags)
		return NULL;

	fb = zalloc(sizeof *fb);
	if (fb == NULL)
		return NULL;

	fb->refcnt = 1;
	fb->type = BUFFER_DMABUF;

	ARRAY_COPY(import_mod.fds, dmabuf->attributes.fd);
	ARRAY_COPY(import_mod.strides, dmabuf->attributes.stride);
	ARRAY_COPY(import_mod.offsets, dmabuf->attributes.offset);

	fb->bo = gbm_bo_import(backend->gbm, GBM_BO_IMPORT_FD_MODIFIER,
			       &import_mod, GBM_BO_USE_SCANOUT);
	if (!fb->bo) {
		if (try_view_on_plane_failure_reasons)
			*try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_GBM_BO_IMPORT_FAILED;
		goto err_free;
	}

	fb->width = dmabuf->attributes.width;
	fb->height = dmabuf->attributes.height;
	fb->modifier = dmabuf->attributes.modifier[0];
	fb->size = 0;
	fb->fd = device->drm.fd;

	ARRAY_COPY(fb->strides, dmabuf->attributes.stride);
	ARRAY_COPY(fb->offsets, dmabuf->attributes.offset);

	fb->format = pixel_format_get_info(dmabuf->attributes.format);
	if (!fb->format) {
		weston_log("couldn't look up format info for 0x%lx\n",
			   (unsigned long) dmabuf->attributes.format);
		goto err_free;
	}

	if (is_opaque)
		fb->format = pixel_format_get_opaque_substitute(fb->format);

	if (device->min_width > fb->width ||
	    fb->width > device->max_width ||
	    device->min_height > fb->height ||
	    fb->height > device->max_height) {
		weston_log("bo geometry out of bounds\n");
		goto err_free;
	}

	fb->num_planes = dmabuf->attributes.n_planes;
	for (i = 0; i < dmabuf->attributes.n_planes; i++) {
		union gbm_bo_handle handle;

	        handle = gbm_bo_get_handle_for_plane(fb->bo, i);
		if (handle.s32 == -1) {
			*try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_GBM_BO_GET_HANDLE_FAILED;
			goto err_free;
		}
		fb->handles[i] = handle.u32;
	}

	if (drm_fb_addfb(device, fb) != 0) {
		if (try_view_on_plane_failure_reasons)
			*try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_ADD_FB_FAILED;
		goto err_free;
	}

	return fb;

err_free:
	drm_fb_destroy_dmabuf(fb);
	return NULL;
#endif
}

struct drm_fb *
drm_fb_get_from_bo(struct gbm_bo *bo, struct drm_device *device,
		   bool is_opaque, enum drm_fb_type type)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
#ifdef HAVE_GBM_MODIFIERS
	int i;
#endif

	if (fb) {
		assert(fb->type == type);
		return drm_fb_ref(fb);
	}

	fb = zalloc(sizeof *fb);
	if (fb == NULL)
		return NULL;

	fb->type = type;
	fb->refcnt = 1;
	fb->bo = bo;
	fb->fd = device->drm.fd;

	fb->width = gbm_bo_get_width(bo);
	fb->height = gbm_bo_get_height(bo);
	fb->format = pixel_format_get_info(gbm_bo_get_format(bo));
	fb->size = 0;

#ifdef HAVE_GBM_MODIFIERS
	fb->modifier = gbm_bo_get_modifier(bo);
	fb->num_planes = gbm_bo_get_plane_count(bo);
	for (i = 0; i < fb->num_planes; i++) {
		fb->strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		fb->handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
		fb->offsets[i] = gbm_bo_get_offset(bo, i);
	}
#else
	fb->num_planes = 1;
	fb->strides[0] = gbm_bo_get_stride(bo);
	fb->handles[0] = gbm_bo_get_handle(bo).u32;
	fb->modifier = DRM_FORMAT_MOD_INVALID;
#endif

	if (!fb->format) {
		weston_log("couldn't look up format 0x%lx\n",
			   (unsigned long) gbm_bo_get_format(bo));
		goto err_free;
	}

	/* We can scanout an ARGB buffer if the surface's opaque region covers
	 * the whole output, but we have to use XRGB as the KMS format code. */
	if (is_opaque)
		fb->format = pixel_format_get_opaque_substitute(fb->format);

	if (device->min_width > fb->width ||
	    fb->width > device->max_width ||
	    device->min_height > fb->height ||
	    fb->height > device->max_height) {
		weston_log("bo geometry out of bounds\n");
		goto err_free;
	}

	if (drm_fb_addfb(device, fb) != 0) {
		if (type == BUFFER_GBM_SURFACE)
			weston_log("failed to create kms fb: %s\n",
				   strerror(errno));
		goto err_free;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_gbm);

	return fb;

err_free:
	free(fb);
	return NULL;
}
#endif

void
drm_fb_unref(struct drm_fb *fb)
{
	if (!fb)
		return;

	assert(fb->refcnt > 0);
	if (--fb->refcnt > 0)
		return;

	switch (fb->type) {
	case BUFFER_PIXMAN_DUMB:
		drm_fb_destroy_dumb(fb);
		break;
#ifdef BUILD_DRM_GBM
	case BUFFER_CURSOR:
	case BUFFER_CLIENT:
		gbm_bo_destroy(fb->bo);
		break;
	case BUFFER_GBM_SURFACE:
		gbm_surface_release_buffer(fb->gbm_surface, fb->bo);
		break;
	case BUFFER_DMABUF:
		drm_fb_destroy_dmabuf(fb);
		break;
#endif
	default:
		assert(NULL);
		break;
	}
}

#ifdef BUILD_DRM_GBM
bool
drm_can_scanout_dmabuf(struct weston_compositor *ec,
		       struct linux_dmabuf_buffer *dmabuf)
{
	struct drm_fb *fb;
	struct drm_backend *b = to_drm_backend(ec);
	struct drm_device *device = b->drm;
	bool ret = false;
	uint32_t try_reason = 0x0;

	fb = drm_fb_get_from_dmabuf(dmabuf, device, true, &try_reason);
	if (fb)
		ret = true;

	drm_fb_unref(fb);
	drm_debug(b, "[dmabuf] dmabuf %p, import test %s, with reason 0x%x\n", dmabuf,
		      ret ? "succeeded" : "failed", try_reason);
	return ret;
}

static bool
drm_fb_compatible_with_plane(struct drm_fb *fb, struct drm_plane *plane)
{
	struct drm_device *device = plane->device;
	struct drm_backend *b = device->backend;
	struct weston_drm_format *fmt;

	/* Check whether the format is supported */
	fmt = weston_drm_format_array_find_format(&plane->formats,
						  fb->format->format);
	if (fmt) {
		/* We never try to promote a dmabuf with DRM_FORMAT_MOD_INVALID
		 * to a KMS plane (see drm_fb_get_from_dmabuf() for more details).
		 * So if fb->modifier == DRM_FORMAT_MOD_INVALID, we are sure
		 * that this is for the legacy GBM import path, in which a
		 * wl_drm is being used for scanout. Mesa is the only user we
		 * care in this case (even though recent versions are also using
		 * dmabufs), and it should know better what works or not. */
		if (fb->modifier == DRM_FORMAT_MOD_INVALID)
			return true;

		if (weston_drm_format_has_modifier(fmt, fb->modifier))
			return true;
	}

	drm_debug(b, "\t\t\t\t[%s] not placing view on %s: "
		  "no free %s planes matching format %s (0x%lx) "
		  "modifier 0x%llx\n",
		  drm_output_get_plane_type_name(plane),
		  drm_output_get_plane_type_name(plane),
		  drm_output_get_plane_type_name(plane),
		  fb->format->drm_format_name,
		  (unsigned long) fb->format->format,
		  (unsigned long long) fb->modifier);

	return false;
}

static void
drm_fb_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct drm_buffer_fb *buf_fb =
		container_of(listener, struct drm_buffer_fb, buffer_destroy_listener);

	wl_list_remove(&buf_fb->buffer_destroy_listener.link);

	if (buf_fb->fb) {
		assert(buf_fb->fb->type == BUFFER_CLIENT ||
		       buf_fb->fb->type == BUFFER_DMABUF);
		drm_fb_unref(buf_fb->fb);
	}

	free(buf_fb);
}

struct drm_fb *
drm_fb_get_from_paint_node(struct drm_output_state *state,
			   struct weston_paint_node *pnode)
{
	struct drm_output *output = state->output;
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct drm_device *device = output->device;
	struct weston_view *ev = pnode->view;
	struct weston_buffer *buffer = ev->surface->buffer_ref.buffer;
	struct drm_buffer_fb *buf_fb;
	bool is_opaque = weston_view_is_opaque(ev, &ev->transform.boundingbox);
	struct drm_fb *fb;
	struct drm_plane *plane;

	if (ev->alpha != 1.0f) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_GLOBAL_ALPHA;
		return NULL;
	}

	if (!drm_view_transform_supported(ev, &output->base)) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_INCOMPATIBLE_TRANSFORM;
		return NULL;
	}

	if (ev->surface->protection_mode == WESTON_SURFACE_PROTECTION_MODE_ENFORCED &&
	    ev->surface->desired_protection > output->base.current_protection) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_INADEQUATE_CONTENT_PROTECTION;
		return NULL;
	}

	if (!buffer) {
		pnode->try_view_on_plane_failure_reasons |= FAILURE_REASONS_NO_BUFFER;
		return NULL;
	}

	if (buffer->backend_private) {
		buf_fb = buffer->backend_private;
		pnode->try_view_on_plane_failure_reasons |= buf_fb->failure_reasons;
		return buf_fb->fb ? drm_fb_ref(buf_fb->fb) : NULL;
	}

	buf_fb = zalloc(sizeof(*buf_fb));
	buffer->backend_private = buf_fb;
	buf_fb->buffer_destroy_listener.notify = drm_fb_handle_buffer_destroy;
	wl_signal_add(&buffer->destroy_signal, &buf_fb->buffer_destroy_listener);

	/* GBM is used for dmabuf import as well as from client wl_buffer. */
	if (!b->gbm) {
		pnode->try_view_on_plane_failure_reasons |= FAILURE_REASONS_NO_GBM;
		goto unsuitable;
	}

	if (buffer->type == WESTON_BUFFER_DMABUF) {
		fb = drm_fb_get_from_dmabuf(buffer->dmabuf, device, is_opaque,
					    &buf_fb->failure_reasons);
		if (!fb)
			goto unsuitable;
	} else if (buffer->type == WESTON_BUFFER_RENDERER_OPAQUE) {
		struct gbm_bo *bo;

		bo = gbm_bo_import(b->gbm, GBM_BO_IMPORT_WL_BUFFER,
				   buffer->resource, GBM_BO_USE_SCANOUT);
		if (!bo)
			goto unsuitable;

		fb = drm_fb_get_from_bo(bo, device, is_opaque, BUFFER_CLIENT);
		if (!fb) {
			pnode->try_view_on_plane_failure_reasons |=
				(1 << FAILURE_REASONS_ADD_FB_FAILED);
			gbm_bo_destroy(bo);
			goto unsuitable;
		}
	} else {
		pnode->try_view_on_plane_failure_reasons |= FAILURE_REASONS_BUFFER_TYPE;
		goto unsuitable;
	}

	/* Check if this buffer can ever go on any planes. If it can't, we have
	 * no reason to ever have a drm_fb, so we fail it here. */
	wl_list_for_each(plane, &device->plane_list, link) {
		/* only SHM buffers can go into cursor planes */
		if (plane->type == WDRM_PLANE_TYPE_CURSOR)
			continue;
		if (drm_fb_compatible_with_plane(fb, plane))
			fb->plane_mask |= (1 << plane->plane_idx);
	}
	if (fb->plane_mask == 0) {
		drm_fb_unref(fb);
		buf_fb->failure_reasons |= FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE;
		goto unsuitable;
	}

	/* The caller holds its own ref to the drm_fb, so when creating a new
	 * drm_fb we take an additional ref for the weston_buffer's cache. */
	buf_fb->fb = drm_fb_ref(fb);

	drm_debug(b, "\t\t\t[view] view %p format: %s\n",
		  ev, fb->format->drm_format_name);
	return fb;

unsuitable:
	pnode->try_view_on_plane_failure_reasons |= buf_fb->failure_reasons;
	return NULL;
}
#endif
