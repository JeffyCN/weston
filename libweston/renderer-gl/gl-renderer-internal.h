/*
 * Copyright © 2019 Collabora, Ltd.
 * Copyright © 2019 Harish Krupo
 * Copyright © 2019 Intel Corporation
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

#ifndef GL_RENDERER_INTERNAL_H
#define GL_RENDERER_INTERNAL_H

#include <stdbool.h>

#include <wayland-util.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "shared/weston-egl-ext.h"  /* for PFN* stuff */
#include "shared/helpers.h"

enum gl_shader_texture_variant {
	SHADER_VARIANT_NONE = 0,
/* Keep the following in sync with fragment.glsl. */
	SHADER_VARIANT_RGBX,
	SHADER_VARIANT_RGBA,
	SHADER_VARIANT_Y_U_V,
	SHADER_VARIANT_Y_UV,
	SHADER_VARIANT_Y_XUXV,
	SHADER_VARIANT_XYUV,
	SHADER_VARIANT_SOLID,
	SHADER_VARIANT_EXTERNAL,
};

/** GL shader requirements key
 *
 * This structure is used as a binary blob key for building and searching
 * shaders. Therefore it must not contain any bytes or bits the C compiler
 * would be free to leave undefined e.g. after struct initialization,
 * struct assignment, or member operations.
 *
 * Use 'pahole' from package 'dwarves' to inspect this structure.
 */
struct gl_shader_requirements
{
	unsigned variant:4; /* enum gl_shader_texture_variant */
	bool green_tint:1;

	/*
	 * The total size of all bitfields plus pad_bits_ must fill up exactly
	 * how many bytes the compiler allocates for them together.
	 */
	unsigned pad_bits_:27;
};
static_assert(sizeof(struct gl_shader_requirements) ==
	      4 /* total bitfield size in bytes */,
	      "struct gl_shader_requirements must not contain implicit padding");

struct gl_shader {
	struct gl_shader_requirements key;
	GLuint program;
	GLuint vertex_shader, fragment_shader;
	GLint proj_uniform;
	GLint tex_uniforms[3];
	GLint alpha_uniform;
	GLint color_uniform;
	struct wl_list link; /* gl_renderer::shader_list */
};

struct gl_renderer {
	struct weston_renderer base;
	bool fragment_shader_debug;
	bool fan_debug;
	struct weston_binding *fragment_binding;
	struct weston_binding *fan_binding;

	EGLenum platform;
	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLConfig egl_config;

	EGLSurface dummy_surface;

	uint32_t gl_version;

	struct wl_array vertices;
	struct wl_array vtxcnt;

	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window;
	bool has_platform_base;

	PFNEGLBINDWAYLANDDISPLAYWL bind_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_display;
	PFNEGLQUERYWAYLANDBUFFERWL query_buffer;
	bool has_bind_display;

	bool has_context_priority;

	bool has_egl_image_external;

	bool has_egl_buffer_age;
	bool has_egl_partial_update;
	PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region;

	bool has_configless_context;

	bool has_surfaceless_context;

	bool has_dmabuf_import;
	struct wl_list dmabuf_images;
	struct wl_list dmabuf_formats;

	bool has_gl_texture_rg;

	struct gl_shader *current_shader;

	struct wl_signal destroy_signal;

	struct wl_listener output_destroy_listener;

	bool has_dmabuf_import_modifiers;
	PFNEGLQUERYDMABUFFORMATSEXTPROC query_dmabuf_formats;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dmabuf_modifiers;

	bool has_native_fence_sync;
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;

	bool has_wait_sync;
	PFNEGLWAITSYNCKHRPROC wait_sync;

	/** struct gl_shader::link
	 *
	 * List constains cached shaders built from struct gl_shader_requirements
	 */
	struct wl_list shader_list;
};

static inline struct gl_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct gl_renderer *)ec->renderer;
}

void
gl_renderer_print_egl_error_state(void);

void
gl_renderer_log_extensions(const char *name, const char *extensions);

void
log_egl_config_info(EGLDisplay egldpy, EGLConfig eglconfig);

EGLConfig
gl_renderer_get_egl_config(struct gl_renderer *gr,
			   EGLint egl_surface_type,
			   const uint32_t *drm_formats,
			   unsigned drm_formats_count);

int
gl_renderer_setup_egl_display(struct gl_renderer *gr, void *native_display);

int
gl_renderer_setup_egl_client_extensions(struct gl_renderer *gr);

int
gl_renderer_setup_egl_extensions(struct weston_compositor *ec);

void
gl_shader_destroy(struct gl_shader *shader);

struct gl_shader *
gl_shader_create(struct gl_renderer *gr,
		 const struct gl_shader_requirements *requirements);

int
gl_shader_requirements_cmp(const struct gl_shader_requirements *a,
			   const struct gl_shader_requirements *b);

#endif /* GL_RENDERER_INTERNAL_H */
