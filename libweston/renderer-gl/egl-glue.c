/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2015, 2019 Collabora, Ltd.
 * Copyright © 2016 NVIDIA Corporation
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

#include <assert.h>

#include "shared/helpers.h"
#include "shared/platform.h"

#include "gl-renderer.h"
#include "gl-renderer-internal.h"
#include "pixel-formats.h"
#include "weston-egl-ext.h"

#include <assert.h>

static const char *
egl_error_string(EGLint code)
{
#define MYERRCODE(x) case x: return #x;
	switch (code) {
	MYERRCODE(EGL_SUCCESS)
	MYERRCODE(EGL_NOT_INITIALIZED)
	MYERRCODE(EGL_BAD_ACCESS)
	MYERRCODE(EGL_BAD_ALLOC)
	MYERRCODE(EGL_BAD_ATTRIBUTE)
	MYERRCODE(EGL_BAD_CONTEXT)
	MYERRCODE(EGL_BAD_CONFIG)
	MYERRCODE(EGL_BAD_CURRENT_SURFACE)
	MYERRCODE(EGL_BAD_DISPLAY)
	MYERRCODE(EGL_BAD_SURFACE)
	MYERRCODE(EGL_BAD_MATCH)
	MYERRCODE(EGL_BAD_PARAMETER)
	MYERRCODE(EGL_BAD_NATIVE_PIXMAP)
	MYERRCODE(EGL_BAD_NATIVE_WINDOW)
	MYERRCODE(EGL_CONTEXT_LOST)
	default:
		return "unknown";
	}
#undef MYERRCODE
}

void
gl_renderer_print_egl_error_state(void)
{
	EGLint code;

	code = eglGetError();
	weston_log("EGL error state: %s (0x%04lx)\n",
		egl_error_string(code), (long)code);
}

static void
print_egl_surface_type_bits(FILE *fp, EGLint egl_surface_type)
{
	const char *sep = "";
	unsigned i;

	static const struct {
		EGLint bit;
		const char *str;
	} egl_surf_bits[] = {
		{ EGL_WINDOW_BIT, "win" },
		{ EGL_PIXMAP_BIT, "pix" },
		{ EGL_PBUFFER_BIT, "pbf" },
		{ EGL_MULTISAMPLE_RESOLVE_BOX_BIT, "ms_resolve_box" },
		{ EGL_SWAP_BEHAVIOR_PRESERVED_BIT, "swap_preserved" },
	};

	for (i = 0; i < ARRAY_LENGTH(egl_surf_bits); i++) {
		if (egl_surface_type & egl_surf_bits[i].bit) {
			fprintf(fp, "%s%s", sep, egl_surf_bits[i].str);
			sep = "|";
		}
	}
}

void
log_egl_config_info(EGLDisplay egldpy, EGLConfig eglconfig)
{
	EGLint r, g, b, a;

	weston_log("Chosen EGL config details:\n");

	weston_log_continue(STAMP_SPACE "RGBA bits");
	if (eglGetConfigAttrib(egldpy, eglconfig, EGL_RED_SIZE, &r) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_GREEN_SIZE, &g) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_BLUE_SIZE, &b) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_ALPHA_SIZE, &a))
		weston_log_continue(": %d %d %d %d\n", r, g, b, a);
	else
		weston_log_continue(" unknown\n");

	weston_log_continue(STAMP_SPACE "swap interval range");
	if (eglGetConfigAttrib(egldpy, eglconfig, EGL_MIN_SWAP_INTERVAL, &a) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_MAX_SWAP_INTERVAL, &b))
		weston_log_continue(": %d - %d\n", a, b);
	else
		weston_log_continue(" unknown\n");
}

static bool
egl_config_pixel_format_matches(struct gl_renderer *gr,
				EGLConfig config,
				const struct pixel_format_info *pinfo)
{
	static const EGLint attribs[4] = {
		EGL_ALPHA_SIZE, EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE
	};
	const int *argb[4] = {
		&pinfo->bits.a, &pinfo->bits.r, &pinfo->bits.g, &pinfo->bits.b
	};
	unsigned i;
	EGLint value;

	if (gr->platform == EGL_PLATFORM_GBM_KHR) {
		if (!eglGetConfigAttrib(gr->egl_display, config,
					EGL_NATIVE_VISUAL_ID, &value))
			return false;

		return ((uint32_t)value) == pinfo->format;
	}

	for (i = 0; i < 4; i++) {
		if (!eglGetConfigAttrib(gr->egl_display, config,
					attribs[i], &value))
			return false;
		if (value != *argb[i])
			return false;
	}

	return true;
}

static int
egl_choose_config(struct gl_renderer *gr,
		  const EGLint *attribs,
		  const struct pixel_format_info *const *pinfo,
		  unsigned pinfo_count,
		  EGLConfig *config_out)
{
	EGLint count = 0;
	EGLint matched = 0;
	EGLConfig *configs;
	unsigned i;
	EGLint j;
	int config_index = -1;

	if (!eglGetConfigs(gr->egl_display, NULL, 0, &count) || count < 1) {
		weston_log("No EGL configs to choose from.\n");
		return -1;
	}
	configs = calloc(count, sizeof *configs);
	if (!configs)
		return -1;

	if (!eglChooseConfig(gr->egl_display, attribs, configs,
			      count, &matched) || !matched) {
		weston_log("No EGL configs with appropriate attributes.\n");
		goto out;
	}

	if (pinfo_count == 0)
		config_index = 0;

	for (i = 0; config_index == -1 && i < pinfo_count; i++)
		for (j = 0; config_index == -1 && j < matched; j++)
			if (egl_config_pixel_format_matches(gr, configs[j],
							    pinfo[i]))
				config_index = j;

	if (config_index != -1)
		*config_out = configs[config_index];

out:
	free(configs);
	if (config_index == -1)
		return -1;

	if (i > 1)
		weston_log("Unable to use first choice EGL config with"
			   " %s, succeeded with alternate %s.\n",
			   pinfo[0]->drm_format_name,
			   pinfo[i - 1]->drm_format_name);
	return 0;
}

static bool
egl_config_is_compatible(struct gl_renderer *gr,
			 EGLConfig config,
			 EGLint egl_surface_type,
			 const struct pixel_format_info *const *pinfo,
			 unsigned pinfo_count)
{
	EGLint value;
	unsigned i;

	if (config == EGL_NO_CONFIG_KHR)
		return false;

	if (!eglGetConfigAttrib(gr->egl_display, config,
				EGL_SURFACE_TYPE, &value))
		return false;
	if ((value & egl_surface_type) != egl_surface_type)
		return false;

	for (i = 0; i < pinfo_count; i++) {
		if (egl_config_pixel_format_matches(gr, config, pinfo[i]))
			return true;
	}
	return false;
}

/* The caller must free() the string */
static char *
explain_egl_config_criteria(EGLint egl_surface_type,
			    const struct pixel_format_info *const *pinfo,
			    unsigned pinfo_count)
{
	FILE *fp;
	char *str = NULL;
	size_t size = 0;
	const char *sep;
	unsigned i;

	fp = open_memstream(&str, &size);
	if (!fp)
		return NULL;

	fputs("{ ", fp);

	print_egl_surface_type_bits(fp, egl_surface_type);
	fputs("; ", fp);

	sep = "";
	for (i = 0; i < pinfo_count; i++) {
		fprintf(fp, "%s%s", sep, pinfo[i]->drm_format_name);
		sep = ", ";
	}

	fputs(" }", fp);

	fclose(fp);

	return str;
}

EGLConfig
gl_renderer_get_egl_config(struct gl_renderer *gr,
			   EGLint egl_surface_type,
			   const uint32_t *drm_formats,
			   unsigned drm_formats_count)
{
	EGLConfig egl_config;
	const struct pixel_format_info *pinfo[16];
	unsigned pinfo_count;
	unsigned i;
	char *what;
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE,    egl_surface_type,
		EGL_RED_SIZE,        1,
		EGL_GREEN_SIZE,      1,
		EGL_BLUE_SIZE,       1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	assert(drm_formats_count < ARRAY_LENGTH(pinfo));
	drm_formats_count = MIN(drm_formats_count, ARRAY_LENGTH(pinfo));

	for (pinfo_count = 0, i = 0; i < drm_formats_count; i++) {
		pinfo[pinfo_count] = pixel_format_get_info(drm_formats[i]);
		if (!pinfo[pinfo_count]) {
			weston_log("Bad/unknown DRM format code 0x%08x.\n",
				   drm_formats[i]);
			continue;
		}
		pinfo_count++;
	}

	if (egl_config_is_compatible(gr, gr->egl_config, egl_surface_type,
				     pinfo, pinfo_count))
		return gr->egl_config;

	if (egl_choose_config(gr, config_attribs, pinfo, pinfo_count,
			      &egl_config) < 0) {
		what = explain_egl_config_criteria(egl_surface_type,
						   pinfo, pinfo_count);
		weston_log("No EGLConfig matches %s.\n", what);
		free(what);
		return EGL_NO_CONFIG_KHR;
	}

	/*
	 * If we do not have configless context support, all EGLConfigs must
	 * be the one and the same, because we use just one GL context for
	 * everything.
	 */
	if (gr->egl_config != EGL_NO_CONFIG_KHR &&
	    egl_config != gr->egl_config) {
		what = explain_egl_config_criteria(egl_surface_type,
						   pinfo, pinfo_count);
		weston_log("Found an EGLConfig matching %s but it is not usable"
			   " because neither EGL_KHR_no_config_context nor "
			   "EGL_MESA_configless_context are supported by EGL.\n",
			   what);
		free(what);
		return EGL_NO_CONFIG_KHR;
	}

	return egl_config;
}

static void
renderer_setup_egl_client_extensions(struct gl_renderer *gr)
{
	const char *extensions;

	extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL client extension string failed.\n");
		return;
	}

	if (weston_check_egl_extension(extensions, "EGL_EXT_platform_base"))
		gr->create_platform_window =
			(void *) eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	else
		weston_log("warning: EGL_EXT_platform_base not supported.\n");
}

int
gl_renderer_setup_egl_extensions(struct weston_compositor *ec)
{
	static const struct {
		char *extension, *entrypoint;
	} swap_damage_ext_to_entrypoint[] = {
		{
			.extension = "EGL_EXT_swap_buffers_with_damage",
			.entrypoint = "eglSwapBuffersWithDamageEXT",
		},
		{
			.extension = "EGL_KHR_swap_buffers_with_damage",
			.entrypoint = "eglSwapBuffersWithDamageKHR",
		},
	};
	struct gl_renderer *gr = get_renderer(ec);
	const char *extensions;
	EGLBoolean ret;
	unsigned i;

	gr->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	gr->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");

	gr->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	gr->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");
	gr->query_buffer =
		(void *) eglGetProcAddress("eglQueryWaylandBufferWL");
	gr->set_damage_region =
		(void *) eglGetProcAddress("eglSetDamageRegionKHR");

	extensions =
		(const char *) eglQueryString(gr->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		return -1;
	}

	if (weston_check_egl_extension(extensions, "EGL_IMG_context_priority"))
		gr->has_context_priority = true;

	if (weston_check_egl_extension(extensions, "EGL_WL_bind_wayland_display"))
		gr->has_bind_display = true;
	if (gr->has_bind_display) {
		assert(gr->bind_display);
		assert(gr->unbind_display);
		assert(gr->query_buffer);
		ret = gr->bind_display(gr->egl_display, ec->wl_display);
		if (!ret)
			gr->has_bind_display = false;
	}

	if (weston_check_egl_extension(extensions, "EGL_EXT_buffer_age"))
		gr->has_egl_buffer_age = true;

	if (weston_check_egl_extension(extensions, "EGL_KHR_partial_update")) {
		assert(gr->set_damage_region);
		gr->has_egl_partial_update = true;
	}

	for (i = 0; i < ARRAY_LENGTH(swap_damage_ext_to_entrypoint); i++) {
		if (weston_check_egl_extension(extensions,
				swap_damage_ext_to_entrypoint[i].extension)) {
			gr->swap_buffers_with_damage =
				(void *) eglGetProcAddress(
						swap_damage_ext_to_entrypoint[i].entrypoint);
			assert(gr->swap_buffers_with_damage);
			break;
		}
	}

	if (weston_check_egl_extension(extensions, "EGL_KHR_no_config_context") ||
	    weston_check_egl_extension(extensions, "EGL_MESA_configless_context"))
		gr->has_configless_context = true;

	if (weston_check_egl_extension(extensions, "EGL_KHR_surfaceless_context"))
		gr->has_surfaceless_context = true;

	if (weston_check_egl_extension(extensions, "EGL_EXT_image_dma_buf_import"))
		gr->has_dmabuf_import = true;

	if (weston_check_egl_extension(extensions,
				"EGL_EXT_image_dma_buf_import_modifiers")) {
		gr->query_dmabuf_formats =
			(void *) eglGetProcAddress("eglQueryDmaBufFormatsEXT");
		gr->query_dmabuf_modifiers =
			(void *) eglGetProcAddress("eglQueryDmaBufModifiersEXT");
		assert(gr->query_dmabuf_formats);
		assert(gr->query_dmabuf_modifiers);
		gr->has_dmabuf_import_modifiers = true;
	}

	if (weston_check_egl_extension(extensions, "EGL_KHR_fence_sync") &&
	    weston_check_egl_extension(extensions, "EGL_ANDROID_native_fence_sync")) {
		gr->create_sync =
			(void *) eglGetProcAddress("eglCreateSyncKHR");
		gr->destroy_sync =
			(void *) eglGetProcAddress("eglDestroySyncKHR");
		gr->dup_native_fence_fd =
			(void *) eglGetProcAddress("eglDupNativeFenceFDANDROID");
		assert(gr->create_sync);
		assert(gr->destroy_sync);
		assert(gr->dup_native_fence_fd);
		gr->has_native_fence_sync = true;
	} else {
		weston_log("warning: Disabling render GPU timeline and explicit "
			   "synchronization due to missing "
			   "EGL_ANDROID_native_fence_sync extension\n");
	}

	if (weston_check_egl_extension(extensions, "EGL_KHR_wait_sync")) {
		gr->wait_sync = (void *) eglGetProcAddress("eglWaitSyncKHR");
		assert(gr->wait_sync);
		gr->has_wait_sync = true;
	} else {
		weston_log("warning: Disabling explicit synchronization due"
			   "to missing EGL_KHR_wait_sync extension\n");
	}

	renderer_setup_egl_client_extensions(gr);

	return 0;
}
