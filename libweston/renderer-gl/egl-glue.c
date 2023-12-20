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
#include "shared/string-helpers.h"

#include "gl-renderer.h"
#include "gl-renderer-internal.h"
#include "pixel-formats.h"
#include "shared/weston-egl-ext.h"

#include <assert.h>

struct egl_config_print_info {
	const EGLint *attrs;
	unsigned attrs_count;
	const char *prefix;
	const char *separator;
	int field_width;
};

/* Keep in sync with gl-renderer-internal.h. */
static const struct gl_extension_table client_table[] = {
	EXT("EGL_EXT_device_query", EXTENSION_EXT_DEVICE_QUERY),
	EXT("EGL_EXT_platform_base", EXTENSION_EXT_PLATFORM_BASE),
	EXT("EGL_EXT_platform_wayland", EXTENSION_EXT_PLATFORM_WAYLAND),
	EXT("EGL_EXT_platform_x11", EXTENSION_EXT_PLATFORM_X11),
	EXT("EGL_KHR_platform_gbm", EXTENSION_KHR_PLATFORM_GBM),
	EXT("EGL_KHR_platform_wayland", EXTENSION_KHR_PLATFORM_WAYLAND),
	EXT("EGL_KHR_platform_x11", EXTENSION_KHR_PLATFORM_X11),
	EXT("EGL_MESA_platform_gbm", EXTENSION_MESA_PLATFORM_GBM),
	EXT("EGL_MESA_platform_surfaceless", EXTENSION_MESA_PLATFORM_SURFACELESS),
	{ NULL, 0, 0 }
};

/* Keep in sync with gl-renderer-internal.h. */
static const struct gl_extension_table device_table[] = {
	EXT("EGL_EXT_device_drm", EXTENSION_EXT_DEVICE_DRM),
	EXT("EGL_EXT_device_drm_render_node", EXTENSION_EXT_DEVICE_DRM_RENDER_NODE),
	{ NULL, 0, 0 }
};

/* Keep in sync with gl-renderer-internal.h. */
static const struct gl_extension_table display_table[] = {
	EXT("EGL_ANDROID_native_fence_sync", EXTENSION_ANDROID_NATIVE_FENCE_SYNC),
	EXT("EGL_EXT_buffer_age", EXTENSION_EXT_BUFFER_AGE),
	EXT("EGL_EXT_image_dma_buf_import", EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT),
	EXT("EGL_EXT_image_dma_buf_import_modifiers", EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT_MODIFIERS),
	EXT("EGL_EXT_pixel_format_float", EXTENSION_EXT_PIXEL_FORMAT_FLOAT),
	EXT("EGL_EXT_swap_buffers_with_damage", EXTENSION_EXT_SWAP_BUFFERS_WITH_DAMAGE),
	EXT("EGL_IMG_context_priority", EXTENSION_IMG_CONTEXT_PRIORITY),
	EXT("EGL_KHR_fence_sync", EXTENSION_KHR_FENCE_SYNC),
	EXT("EGL_KHR_get_all_proc_addresses", EXTENSION_KHR_GET_ALL_PROC_ADDRESSES),
	EXT("EGL_KHR_image_base", EXTENSION_KHR_IMAGE_BASE),
	EXT("EGL_KHR_no_config_context", EXTENSION_KHR_NO_CONFIG_CONTEXT),
	EXT("EGL_KHR_partial_update", EXTENSION_KHR_PARTIAL_UPDATE),
	EXT("EGL_KHR_surfaceless_context", EXTENSION_KHR_SURFACELESS_CONTEXT),
	EXT("EGL_KHR_swap_buffers_with_damage", EXTENSION_KHR_SWAP_BUFFERS_WITH_DAMAGE),
	EXT("EGL_KHR_wait_sync", EXTENSION_KHR_WAIT_SYNC),
	EXT("EGL_MESA_configless_context", EXTENSION_MESA_CONFIGLESS_CONTEXT),
	EXT("EGL_WL_bind_wayland_display", EXTENSION_WL_BIND_WAYLAND_DISPLAY),
	{ NULL, 0, 0 }
};

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

static const struct egl_config_print_info config_info_ints[] = {
#define ARRAY(...) ((const EGLint[]) { __VA_ARGS__ })

	{ ARRAY(EGL_CONFIG_ID), 1, "id: ", "", 3 },
	{ ARRAY(EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE), 4,
	  "rgba: ", " ", 1 },
	{ ARRAY(EGL_BUFFER_SIZE), 1, "buf: ", "", 2 },
	{ ARRAY(EGL_DEPTH_SIZE), 1, "dep: ", "", 2 },
	{ ARRAY(EGL_STENCIL_SIZE), 1, "stcl: ", "", 1 },
	{ ARRAY(EGL_MIN_SWAP_INTERVAL, EGL_MAX_SWAP_INTERVAL), 2,
	  "int: ", "-", 1 },

#undef ARRAY
};

static void
print_egl_config_ints(FILE *fp, EGLDisplay egldpy, EGLConfig eglconfig)
{
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(config_info_ints); i++) {
		const struct egl_config_print_info *info = &config_info_ints[i];
		unsigned j;
		const char *sep = "";

		fputs(info->prefix, fp);
		for (j = 0; j < info->attrs_count; j++) {
			EGLint value;

			if (eglGetConfigAttrib(egldpy, eglconfig,
					       info->attrs[j], &value)) {
				fprintf(fp, "%s%*d",
					sep, info->field_width, value);
			} else {
				fprintf(fp, "%s!", sep);
			}
			sep = info->separator;
		}

		fputs(" ", fp);
	}
}

static void
print_egl_config_info(FILE *fp, struct gl_renderer *gr, EGLConfig eglconfig)
{
	EGLint value;

	print_egl_config_ints(fp, gr->egl_display, eglconfig);

	fputs("type: ", fp);
	if (eglGetConfigAttrib(gr->egl_display, eglconfig, EGL_SURFACE_TYPE, &value))
		print_egl_surface_type_bits(fp, value);
	else
		fputs("-", fp);

	fputs(" val: ", fp);
	if (egl_display_has(gr, EXTENSION_EXT_PIXEL_FORMAT_FLOAT) &&
	    eglGetConfigAttrib(gr->egl_display, eglconfig, EGL_COLOR_COMPONENT_TYPE_EXT, &value) &&
	    value == EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT) {
		fputs("flt", fp);
	} else {
		fputs("fix", fp);
	}

	fputs(" vis_id: ", fp);
	if (eglGetConfigAttrib(gr->egl_display, eglconfig, EGL_NATIVE_VISUAL_ID, &value)) {
		if (value != 0) {
			const struct pixel_format_info *p = NULL;

			if (gr->platform == EGL_PLATFORM_GBM_KHR)
				p = pixel_format_get_info(value);

			if (p) {
				fprintf(fp, "%s (0x%x)",
					p->drm_format_name, (unsigned)value);
			} else {
				fprintf(fp, "0x%x", (unsigned)value);
			}
		} else {
			fputs("0", fp);
		}
	} else {
		fputs("-", fp);
	}
}

static void
log_all_egl_configs(struct gl_renderer *gr)
{
	EGLint count = 0;
	EGLConfig *configs;
	int i;
	char *strbuf = NULL;
	size_t strsize = 0;
	FILE *fp;

	weston_log("All available EGLConfigs:\n");

	if (!eglGetConfigs(gr->egl_display, NULL, 0, &count) || count < 1)
		return;

	configs = calloc(count, sizeof *configs);
	if (!configs)
		return;

	if (!eglGetConfigs(gr->egl_display, configs, count, &count))
		return;

	fp = open_memstream(&strbuf, &strsize);
	if (!fp)
		goto out;

	for (i = 0; i < count; i++) {
		print_egl_config_info(fp, gr, configs[i]);
		fputc(0, fp);
		fflush(fp);
		weston_log_continue(STAMP_SPACE "%s\n", strbuf);
		rewind(fp);
	}

	fclose(fp);
	free(strbuf);

out:
	free(configs);
}

const struct pixel_format_info **
egl_set_supported_rendering_formats(EGLDisplay egldpy,
				    unsigned int *formats_count)
{
	struct wl_array formats;
	const struct pixel_format_info **f, *p;
	EGLConfig *configs;
	EGLint count = 0;
	EGLint value;
	EGLint i;
	bool found;

	wl_array_init(&formats);

	if (!eglGetConfigs(egldpy, NULL, 0, &count) || count < 1)
		return NULL;

	configs = zalloc(count * sizeof(*configs));
	if (!configs)
		return NULL;

	if (!eglGetConfigs(egldpy, configs, count, &count)) {
		free(configs);
		return NULL;
	}

	for (i = 0; i < count; i++) {
		if (!eglGetConfigAttrib(egldpy, configs[i], EGL_SURFACE_TYPE, &value))
			continue;
		if (!(value & EGL_WINDOW_BIT))
			continue;

		if (!eglGetConfigAttrib(egldpy, configs[i], EGL_NATIVE_VISUAL_ID, &value))
			continue;
		if (value == 0)
			continue;

		p = pixel_format_get_info(value);
		if (!p)
			continue;

		found = false;
		wl_array_for_each(f, &formats) {
			if (p == (*f)) {
				found = true;
				break;
			}
		}
		if (found)
			continue;

		f = wl_array_add(&formats, sizeof(*f));
		*f = p;
	}

	free(configs);

	*formats_count = formats.size / sizeof(p);
	return formats.data;
}

void
log_egl_config_info(struct gl_renderer *gr, EGLConfig eglconfig)
{
	char *strbuf = NULL;
	size_t strsize = 0;
	FILE *fp;

	fp = open_memstream(&strbuf, &strsize);
	if (fp) {
		print_egl_config_info(fp, gr, eglconfig);
		fclose(fp);
	}

	weston_log("Chosen EGL config details: %s\n", strbuf ? strbuf : "?");
	free(strbuf);
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
	bool fixed_point = (pinfo->component_type == PIXEL_COMPONENT_TYPE_FIXED);
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

	if (!eglGetConfigAttrib(gr->egl_display, config,
				EGL_COLOR_COMPONENT_TYPE_EXT, &value))
		value = EGL_COLOR_COMPONENT_TYPE_FIXED_EXT;

	if (fixed_point && value != EGL_COLOR_COMPONENT_TYPE_FIXED_EXT)
		return false;
	if (!fixed_point && value != EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT)
		return false;

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
			   const struct pixel_format_info *const *formats,
			   unsigned formats_count)
{
	EGLConfig egl_config;
	unsigned i;
	char *what;
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE,    egl_surface_type,
		EGL_RED_SIZE,        1,
		EGL_GREEN_SIZE,      1,
		EGL_BLUE_SIZE,       1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE, EGL_NONE, /* we may change this latter */
		EGL_NONE
	};

	for (i = 0; i < formats_count; i++)
		assert(formats[i]);

	if (egl_config_is_compatible(gr, gr->egl_config, egl_surface_type,
				     formats, formats_count))
		return gr->egl_config;

	if (egl_display_has(gr, EXTENSION_EXT_PIXEL_FORMAT_FLOAT)) {
		uint32_t index = ARRAY_LENGTH(config_attribs) - 3;
		config_attribs[index] = EGL_COLOR_COMPONENT_TYPE_EXT;
		config_attribs[++index] = EGL_DONT_CARE;
	}

	if (egl_choose_config(gr, config_attribs, formats, formats_count,
			      &egl_config) < 0) {
		what = explain_egl_config_criteria(egl_surface_type,
						   formats, formats_count);
		weston_log("No EGLConfig matches %s.\n", what);
		free(what);
		log_all_egl_configs(gr);
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
						   formats, formats_count);
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
gl_renderer_set_egl_device(struct gl_renderer *gr)
{
	EGLAttrib attrib;
	const char *extensions;

	assert(egl_client_has(gr, EXTENSION_EXT_DEVICE_QUERY));

	if (!gr->query_display_attrib(gr->egl_display, EGL_DEVICE_EXT, &attrib)) {
		weston_log("failed to get EGL device\n");
		gl_renderer_print_egl_error_state();
		return;
	}

	gr->egl_device = (EGLDeviceEXT) attrib;

	extensions = gr->query_device_string(gr->egl_device, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("failed to get EGL extensions\n");
		return;
	}

	gl_renderer_log_extensions(gr, "EGL device extensions", extensions);
	gl_extensions_add(device_table, extensions, &gr->egl_device_extensions);

	/* Try to query the render node using EGL_DRM_RENDER_NODE_FILE_EXT */
	if (egl_device_has(gr, EXTENSION_EXT_DEVICE_DRM_RENDER_NODE))
		gr->drm_device = gr->query_device_string(gr->egl_device,
							 EGL_DRM_RENDER_NODE_FILE_EXT);

	/* The extension is not supported by the Mesa version of the system or
	 * the query failed. Fallback to EGL_DRM_DEVICE_FILE_EXT */
	if (!gr->drm_device &&
	    egl_device_has(gr, EXTENSION_EXT_DEVICE_DRM))
		gr->drm_device = gr->query_device_string(gr->egl_device,
							 EGL_DRM_DEVICE_FILE_EXT);

	if (gr->drm_device)
		weston_log("Using rendering device: %s\n", gr->drm_device);
	else
		weston_log("warning: failed to query rendering device from EGL\n");
}

int
gl_renderer_setup_egl_display(struct gl_renderer *gr,
			      void *native_display)
{
	EGLint major, minor;
	gr->egl_display = NULL;

	if (egl_client_has(gr, EXTENSION_EXT_PLATFORM_BASE))
		gr->egl_display = gr->get_platform_display(gr->platform,
							   native_display,
							   NULL);

	if (!gr->egl_display) {
		weston_log("warning: either no EGL_EXT_platform_base "
			   "support or specific platform support; "
			   "falling back to eglGetDisplay.\n");
		gr->egl_display = eglGetDisplay(native_display);
	}

	if (!gr->egl_display) {
		weston_log("failed to create display\n");
		return -1;
	}

	if (!eglInitialize(gr->egl_display, &major, &minor)) {
		weston_log("failed to initialize display\n");
		goto fail;
	}

	if (gl_version(major, minor) < gl_version(1, 2)) {
		weston_log("EGL version >= 1.2 is required.\n");
		return -1;
	}

	if (egl_client_has(gr, EXTENSION_EXT_DEVICE_QUERY))
		gl_renderer_set_egl_device(gr);

	return 0;

fail:
	gl_renderer_print_egl_error_state();
	return -1;
}

/** Checks for EGL client extensions (i.e. independent of EGL display),
 * loads the function pointers, and checks if the platform is supported.
 *
 * \param gr The OpenGL renderer
 * \return 0 for success, -1 if platform is unsupported
 */
int
gl_renderer_setup_egl_client_extensions(struct gl_renderer *gr)
{
	const char *extensions;
	const char *platform = NULL;

	extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL client extension string failed.\n");
		return 0;
	}

	gl_renderer_log_extensions(gr, "EGL client extensions", extensions);
	gl_extensions_add(client_table, extensions, &gr->egl_client_extensions);

	if (egl_client_has(gr, EXTENSION_EXT_DEVICE_QUERY)) {
		GET_PROC_ADDRESS(gr->query_display_attrib,
				 "eglQueryDisplayAttribEXT");
		GET_PROC_ADDRESS(gr->query_device_string,
				 "eglQueryDeviceStringEXT");
	}

	if (egl_client_has(gr, EXTENSION_EXT_PLATFORM_BASE)) {
		GET_PROC_ADDRESS(gr->get_platform_display,
				 "eglGetPlatformDisplayEXT");
		GET_PROC_ADDRESS(gr->create_platform_window,
				 "eglCreatePlatformWindowSurfaceEXT");
	} else if (gr->platform != EGL_PLATFORM_SURFACELESS_MESA) {
		weston_log("warning: EGL_EXT_platform_base not supported.\n");
		return 0;
	} else {
		weston_log("Error: EGL surfaceless platform cannot be used.\n");
		return -1;
	}

	switch (gr->platform) {
	case EGL_PLATFORM_GBM_KHR:
		if (egl_client_has(gr, EXTENSION_KHR_PLATFORM_GBM) ||
		    egl_client_has(gr, EXTENSION_MESA_PLATFORM_GBM))
			return 0;
		platform = "GBM";
		break;
	case EGL_PLATFORM_WAYLAND_KHR:
		if (egl_client_has(gr, EXTENSION_KHR_PLATFORM_WAYLAND) ||
		    egl_client_has(gr, EXTENSION_EXT_PLATFORM_WAYLAND))
			return 0;
		platform = "Wayland";
		break;
	case EGL_PLATFORM_X11_KHR:
		if (egl_client_has(gr, EXTENSION_KHR_PLATFORM_X11) ||
		    egl_client_has(gr, EXTENSION_EXT_PLATFORM_X11))
			return 0;
		platform = "X11";
		break;
	case EGL_PLATFORM_SURFACELESS_MESA:
		if (egl_client_has(gr, EXTENSION_MESA_PLATFORM_SURFACELESS))
			return 0;
		platform = "surfaceless";
		break;
	default:
		unreachable("bad EGL platform enum");
		return -1;
	}

	/* HACK: Fallback to GBM platform if surfaceless is not supported */
	if (gr->platform == EGL_PLATFORM_SURFACELESS_MESA) {
		gr->platform = EGL_PLATFORM_GBM_KHR;
		weston_log("Warn: EGL does not support %s platform, try GBM\n",
			   platform);
		return gl_renderer_setup_egl_client_extensions(gr);
	}

	/* at this point we definitely have some platform extensions but
	 * haven't found the supplied platform, so chances are it's
	 * not supported. */
	weston_log("Error: EGL does not support %s platform.\n", platform);

	return -1;
}

int
gl_renderer_setup_egl_extensions(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);
	const char *extensions;

	extensions =
		(const char *) eglQueryString(gr->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		return -1;
	}

	gl_extensions_add(display_table, extensions,
			  &gr->egl_display_extensions);

	/* eglCreateImage() and eglDestroyImage() from EGL 1.5 could be used
	 * instead when available but the type of the attribute list passed to
	 * eglCreateImage() is different and Mesa does a conversion anyway. */
	if (egl_display_has(gr, EXTENSION_KHR_IMAGE_BASE)) {
		GET_PROC_ADDRESS(gr->create_image, "eglCreateImageKHR");
		GET_PROC_ADDRESS(gr->destroy_image, "eglDestroyImageKHR");
	}

	if (egl_display_has(gr, EXTENSION_WL_BIND_WAYLAND_DISPLAY)) {
		GET_PROC_ADDRESS(gr->bind_display, "eglBindWaylandDisplayWL");
		GET_PROC_ADDRESS(gr->unbind_display,
				 "eglUnbindWaylandDisplayWL");
		GET_PROC_ADDRESS(gr->query_buffer, "eglQueryWaylandBufferWL");
	}

	if (egl_display_has(gr, EXTENSION_KHR_PARTIAL_UPDATE))
		GET_PROC_ADDRESS(gr->set_damage_region,
				 "eglSetDamageRegionKHR");

	if (egl_display_has(gr, EXTENSION_EXT_SWAP_BUFFERS_WITH_DAMAGE))
		GET_PROC_ADDRESS(gr->swap_buffers_with_damage,
				 "eglSwapBuffersWithDamageEXT");
	else if (egl_display_has(gr, EXTENSION_KHR_SWAP_BUFFERS_WITH_DAMAGE))
		GET_PROC_ADDRESS(gr->swap_buffers_with_damage,
				 "eglSwapBuffersWithDamageKHR");

	if (egl_display_has(gr, EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT_MODIFIERS)) {
		GET_PROC_ADDRESS(gr->query_dmabuf_formats,
				 "eglQueryDmaBufFormatsEXT");
		GET_PROC_ADDRESS(gr->query_dmabuf_modifiers,
				 "eglQueryDmaBufModifiersEXT");
	}

	if (egl_display_has(gr, EXTENSION_KHR_FENCE_SYNC)) {
		GET_PROC_ADDRESS(gr->create_sync, "eglCreateSyncKHR");
		GET_PROC_ADDRESS(gr->destroy_sync, "eglDestroySyncKHR");
	}

	if (egl_display_has(gr, EXTENSION_ANDROID_NATIVE_FENCE_SYNC))
		GET_PROC_ADDRESS(gr->dup_native_fence_fd,
				 "eglDupNativeFenceFDANDROID");

	if (egl_display_has(gr, EXTENSION_KHR_WAIT_SYNC))
		GET_PROC_ADDRESS(gr->wait_sync, "eglWaitSyncKHR");

	/* No config context feature. */
	if (egl_display_has(gr, EXTENSION_KHR_NO_CONFIG_CONTEXT) ||
	    egl_display_has(gr, EXTENSION_MESA_CONFIGLESS_CONTEXT))
		gr->features |= FEATURE_NO_CONFIG_CONTEXT;

	/* Swap buffers with damage feature. */
	if (egl_display_has(gr, EXTENSION_KHR_SWAP_BUFFERS_WITH_DAMAGE) ||
	    egl_display_has(gr, EXTENSION_EXT_SWAP_BUFFERS_WITH_DAMAGE))
		gr->features |= FEATURE_SWAP_BUFFERS_WITH_DAMAGE;

	/* Explicit sync feature. */
	if (egl_display_has(gr, EXTENSION_ANDROID_NATIVE_FENCE_SYNC) &&
	    egl_display_has(gr, EXTENSION_KHR_WAIT_SYNC))
		gr->features |= FEATURE_EXPLICIT_SYNC;

	weston_log("EGL features:\n");
	weston_log_continue(STAMP_SPACE "EGL Wayland extension: %s\n",
			    yesno(egl_display_has(gr, EXTENSION_WL_BIND_WAYLAND_DISPLAY)));
	weston_log_continue(STAMP_SPACE "context priority: %s\n",
			    yesno(egl_display_has(gr, EXTENSION_IMG_CONTEXT_PRIORITY)));
	weston_log_continue(STAMP_SPACE "buffer age: %s\n",
			    yesno(egl_display_has(gr, EXTENSION_EXT_BUFFER_AGE)));
	weston_log_continue(STAMP_SPACE "partial update: %s\n",
			    yesno(egl_display_has(gr, EXTENSION_KHR_PARTIAL_UPDATE)));
	weston_log_continue(STAMP_SPACE "swap buffers with damage: %s\n",
			    yesno(gl_features_has(gr, FEATURE_SWAP_BUFFERS_WITH_DAMAGE)));
	weston_log_continue(STAMP_SPACE "configless context: %s\n",
			    yesno(gl_features_has(gr, FEATURE_NO_CONFIG_CONTEXT)));
	weston_log_continue(STAMP_SPACE "surfaceless context: %s\n",
			    yesno(egl_display_has(gr, EXTENSION_KHR_SURFACELESS_CONTEXT)));
	weston_log_continue(STAMP_SPACE "dmabuf support: %s\n",
			    !egl_display_has(gr, EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT) ? "no" :
			    !egl_display_has(gr, EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT_MODIFIERS) ? "legacy" :
			    "modifiers");
	weston_log_continue(STAMP_SPACE "fence sync: %s\n",
			    !egl_display_has(gr, EXTENSION_KHR_FENCE_SYNC) ? "no" :
			    !egl_display_has(gr, EXTENSION_ANDROID_NATIVE_FENCE_SYNC) &&
			    !egl_display_has(gr, EXTENSION_KHR_WAIT_SYNC) ? "yes" :
			    !egl_display_has(gr, EXTENSION_ANDROID_NATIVE_FENCE_SYNC) ? "yes (wait)" :
			    !egl_display_has(gr, EXTENSION_KHR_WAIT_SYNC) ? "yes (native)" :
			    "yes (native, wait)");

	return 0;
}
