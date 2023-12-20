/*
 * Copyright © 2019-2025 Collabora, Ltd.
 * Copyright © 2019 Harish Krupo
 * Copyright © 2019 Intel Corporation
 * Copyright 2021 Advanced Micro Devices, Inc.
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

/*
 * GL renderer best practices:
 *
 * 1. Extensions and features
 *
 *    1. An extension flag ensures the availability of an EGL or OpenGL ES
 *       extension at run-time, independently of the version.
 *    2. A feature flag ensures the availability of a minimal OpenGL ES version
 *       and/or extensions at run-time in order to enable the use of a specific
 *       feature.
 *    3. Any function pointers declared in the gl_renderer structure must be
 *       loaded at setup so that an extension availability check can ensure
 *       valid pointers.
 *    4. OpenGL ES 3 functions must be loaded at run-time after having checked
 *       for EGL_KHR_get_all_proc_addresses extension availability in order to
 *       correctly link against OpenGL ES 2 only implementations.
 *
 * 2. Pixel storage modes
 *
 *    1. Any functions changing modes must restore them to their default values
 *       before return so that other functions can assume default values.
 *
 * 3. Texture units
 *
 *    1. Fixed allocation using the gl_tex_unit enumeration.
 *    2. Any functions changing the active unit must restore it to 0 before
 *       return so that other functions can assume a default value.
 */

#ifndef GL_RENDERER_INTERNAL_H
#define GL_RENDERER_INTERNAL_H

#include <stdbool.h>
#include <time.h>

#include <wayland-util.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl32.h>

#include "color.h"
#include "libweston-internal.h"
#include "shared/weston-egl-ext.h"  /* for PFN* stuff */
#include "shared/helpers.h"

/* Max number of images per buffer. */
#define SHADER_INPUT_TEX_MAX 3

#define GET_PROC_ADDRESS(dest, proc) do { \
	dest = (void *) eglGetProcAddress(proc); \
	assert(dest); \
} while (0)

#define EXT(string, flag) { string, ARRAY_LENGTH(string) - 1, (uint64_t) flag }

/* Keep in sync with egl-glue.c. */
enum egl_client_extension_flag {
	EXTENSION_EXT_DEVICE_QUERY          = 1ull << 0,
	EXTENSION_EXT_PLATFORM_BASE         = 1ull << 1,
	EXTENSION_EXT_PLATFORM_WAYLAND      = 1ull << 2,
	EXTENSION_EXT_PLATFORM_X11          = 1ull << 3,
	EXTENSION_KHR_PLATFORM_GBM          = 1ull << 4,
	EXTENSION_KHR_PLATFORM_WAYLAND      = 1ull << 5,
	EXTENSION_KHR_PLATFORM_X11          = 1ull << 6,
	EXTENSION_MESA_PLATFORM_GBM         = 1ull << 7,
	EXTENSION_MESA_PLATFORM_SURFACELESS = 1ull << 8,
};

/* Keep in sync with egl-glue.c. */
enum egl_device_extension_flag {
	EXTENSION_EXT_DEVICE_DRM             = 1ull << 0,
	EXTENSION_EXT_DEVICE_DRM_RENDER_NODE = 1ull << 1,
};

/* Keep in sync with egl-glue.c. */
enum egl_display_extension_flag {
	EXTENSION_ANDROID_NATIVE_FENCE_SYNC          = 1ull << 0,
	EXTENSION_EXT_BUFFER_AGE                     = 1ull << 1,
	EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT           = 1ull << 2,
	EXTENSION_EXT_IMAGE_DMA_BUF_IMPORT_MODIFIERS = 1ull << 3,
	EXTENSION_EXT_PIXEL_FORMAT_FLOAT             = 1ull << 4,
	EXTENSION_EXT_SWAP_BUFFERS_WITH_DAMAGE       = 1ull << 5,
	EXTENSION_IMG_CONTEXT_PRIORITY               = 1ull << 6,
	EXTENSION_KHR_FENCE_SYNC                     = 1ull << 7,
	EXTENSION_KHR_GET_ALL_PROC_ADDRESSES         = 1ull << 8,
	EXTENSION_KHR_IMAGE_BASE                     = 1ull << 9,
	EXTENSION_KHR_NO_CONFIG_CONTEXT              = 1ull << 10,
	EXTENSION_KHR_PARTIAL_UPDATE                 = 1ull << 11,
	EXTENSION_KHR_SURFACELESS_CONTEXT            = 1ull << 12,
	EXTENSION_KHR_SWAP_BUFFERS_WITH_DAMAGE       = 1ull << 13,
	EXTENSION_KHR_WAIT_SYNC                      = 1ull << 14,
	EXTENSION_MESA_CONFIGLESS_CONTEXT            = 1ull << 15,
	EXTENSION_WL_BIND_WAYLAND_DISPLAY            = 1ull << 16,
};

/* Keep in sync with gl-renderer.c. */
enum gl_extension_flag {
	EXTENSION_ANGLE_PACK_REVERSE_ROW_ORDER    = 1ull << 1,
	EXTENSION_APPLE_TEXTURE_PACKED_FLOAT      = 1ull << 3,
	EXTENSION_ARM_RGBA8                       = 1ull << 4,
	EXTENSION_EXT_COLOR_BUFFER_FLOAT          = 1ull << 5,
	EXTENSION_EXT_COLOR_BUFFER_HALF_FLOAT     = 1ull << 6,
	EXTENSION_EXT_DISJOINT_TIMER_QUERY        = 1ull << 7,
	EXTENSION_EXT_EGL_IMAGE_STORAGE           = 1ull << 8,
	EXTENSION_EXT_MAP_BUFFER_RANGE            = 1ull << 9,
	EXTENSION_EXT_READ_FORMAT_BGRA            = 1ull << 10,
	EXTENSION_EXT_SHADER_FB_FETCH_NC          = 1ull << 11,
	EXTENSION_EXT_TEXTURE_FORMAT_BGRA8888     = 1ull << 12,
	EXTENSION_EXT_TEXTURE_NORM16              = 1ull << 13,
	EXTENSION_EXT_TEXTURE_RG                  = 1ull << 14,
	EXTENSION_EXT_TEXTURE_SRGB_R8             = 1ull << 15,
	EXTENSION_EXT_TEXTURE_SRGB_RG8            = 1ull << 16,
	EXTENSION_EXT_TEXTURE_STORAGE             = 1ull << 17,
	EXTENSION_EXT_TEXTURE_TYPE_2_10_10_10_REV = 1ull << 18,
	EXTENSION_EXT_UNPACK_SUBIMAGE             = 1ull << 19,
	EXTENSION_NV_PACKED_FLOAT                 = 1ull << 20,
	EXTENSION_NV_PIXEL_BUFFER_OBJECT          = 1ull << 21,
	EXTENSION_OES_EGL_IMAGE                   = 1ull << 22,
	EXTENSION_OES_EGL_IMAGE_EXTERNAL          = 1ull << 23,
	EXTENSION_OES_MAPBUFFER                   = 1ull << 24,
	EXTENSION_OES_REQUIRED_INTERNALFORMAT     = 1ull << 25,
	EXTENSION_OES_RGB8_RGBA8                  = 1ull << 26,
	EXTENSION_OES_TEXTURE_3D                  = 1ull << 27,
	EXTENSION_OES_TEXTURE_FLOAT               = 1ull << 28,
	EXTENSION_OES_TEXTURE_FLOAT_LINEAR        = 1ull << 29,
	EXTENSION_OES_TEXTURE_HALF_FLOAT          = 1ull << 30,
	EXTENSION_QCOM_RENDER_SRGB_R8_RG8         = 1ull << 31,
};

enum gl_feature_flag {
	/* GL renderer can create contexts without specifying an EGLConfig. */
	FEATURE_NO_CONFIG_CONTEXT = 1ull << 0,

	/* GL renderer can pass a list of damage rectangles at buffer swap in
	 * order to reduce recomposition costs. */
	FEATURE_SWAP_BUFFERS_WITH_DAMAGE = 1ull << 1,

	/* GL renderer can create native sync objects and wait on them. This
	 * enables support for the Linux explicit sync Wayland protocol. */
	FEATURE_EXPLICIT_SYNC = 1ull << 2,

	/* GL renderer can asynchronously map the framebuffer into CPU memory
	 * for reading. This is exposed by binding a Pixel Buffer Object (PBO)
	 * to the GL_PIXEL_PACK_BUFFER target before read-back with
	 * glReadPixels(). map_buffer_range() is then called to sync and map and
	 * unmap_buffer() to unmap once read. A fence sync can be used to signal
	 * pixel transfer completion, this is flagged as another feature. */
	FEATURE_ASYNC_READBACK = 1ull << 3,

	/* GL renderer can create 16-bit floating-point framebuffers and
	 * transform colours using linearly interpolated 3D look-up tables.
	 *
	 * Strong dependency on the texture 3D feature. */
	FEATURE_COLOR_TRANSFORMS = 1ull << 4,

	/* GL renderer can instrument output repaint time and report it through
	 * the timeline logging scope. */
	FEATURE_GPU_TIMELINE = 1ull << 5,

	/* GL renderer can specify the entire structure of a texture in a single
	 * call. Once specified, format and dimensions can't be changed.
	 *
	 * Weak dependency on the texture 3D feature, enabling immutability of
	 * of 3D textures. */
	FEATURE_TEXTURE_IMMUTABILITY = 1ull << 6,

	/* GL renderer can create two-component red-green textures. */
	FEATURE_TEXTURE_RG = 1ull << 7,

	/* GL renderer supports the GL_EXT_texture_format_BGRA8888 extension
	 * with the GL_BGRA8_EXT sized internal format revision (23/06/2024) for
	 * renderbuffer objects. */
	FEATURE_SIZED_BGRA8_RENDERBUFFER = 1ull << 8,

	/* GL renderer can create 3D textures. */
	FEATURE_TEXTURE_3D = 1ull << 9,

	/* GL renderer can do blending explicitly in the fragment shader,
	 * using framebuffer fetch and store curves. */
	FEATURE_SHADER_BLENDING = 1ull << 10,
};

/* Keep the following in sync with vertex.glsl. */
enum gl_shader_texcoord_input {
	SHADER_TEXCOORD_INPUT_ATTRIB = 0,
	SHADER_TEXCOORD_INPUT_SURFACE,
};

enum gl_shader_texture_variant {
	SHADER_VARIANT_NONE = 0,
/* Keep the following in sync with fragment.glsl. */
	SHADER_VARIANT_RGBA,
	SHADER_VARIANT_Y_U_V,
	SHADER_VARIANT_Y_UV,
	SHADER_VARIANT_XYUV,
	SHADER_VARIANT_SOLID,
	SHADER_VARIANT_EXTERNAL,
};

/* Keep the following in sync with fragment.glsl. */
enum gl_shader_color_effect {
	SHADER_COLOR_EFFECT_NONE = 0,
	SHADER_COLOR_EFFECT_INVERSION,
	SHADER_COLOR_EFFECT_GRAYSCALE,
	SHADER_COLOR_EFFECT_CVD_CORRECTION,
};

/* Keep the following in sync with fragment.glsl. */
enum gl_shader_color_curve {
	SHADER_COLOR_CURVE_IDENTITY = 0,
	SHADER_COLOR_CURVE_LUT_3x1D,
	SHADER_COLOR_CURVE_LINPOW,
	SHADER_COLOR_CURVE_POWLIN,
	SHADER_COLOR_CURVE_PQ,
	SHADER_COLOR_CURVE_PQ_INVERSE,
};

/* Keep the following in sync with fragment.glsl. */
enum gl_shader_color_mapping {
	SHADER_COLOR_MAPPING_IDENTITY = 0,
	SHADER_COLOR_MAPPING_3DLUT,
	SHADER_COLOR_MAPPING_MATRIX,
};

/* Keep the following in sync with fragment.glsl. */
enum gl_shader_fb_alpha_encoding {
	SHADER_FB_ALPHA_PREMULT = 0,
	SHADER_FB_ALPHA_STRAIGHT,
};

enum gl_shader_attrib_loc {
	SHADER_ATTRIB_LOC_POSITION = 0,
	SHADER_ATTRIB_LOC_TEXCOORD,
	SHADER_ATTRIB_LOC_BARYCENTRIC,
};

enum gl_tex_unit {
	TEX_UNIT_IMAGES = 0,
	TEX_UNIT_COLOR_PRE_CURVE = SHADER_INPUT_TEX_MAX,
	TEX_UNIT_COLOR_MAPPING,
	TEX_UNIT_COLOR_POST_CURVE,
	TEX_UNIT_WIREFRAME,

	TEX_UNIT_COUNT_REQUIRED,
	/* For all units below, check gr->max_texture_image_units etc. */

	TEX_UNIT_FB_FETCH_CURVE = TEX_UNIT_COUNT_REQUIRED,
	TEX_UNIT_FB_STORE_CURVE,
	TEX_UNIT_COUNT_OPTIONAL,
};
static_assert(TEX_UNIT_COUNT_REQUIRED <= 8, "OpenGL ES 2.0 requires at least 8 texture "
	      "units. Cannot assume having more without a runtime check.");

enum gl_bgra8_texture_support {
	BGRA8_TEXTURE_SUPPORT_STORAGE = 0,
	BGRA8_TEXTURE_SUPPORT_IMAGE_REVISED,
	BGRA8_TEXTURE_SUPPORT_IMAGE_ORIGINAL,
	BGRA8_TEXTURE_SUPPORT_NONE,
};

struct gl_extension_table {
	const char *str;
	size_t len;
	uint64_t flag;
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
	unsigned texcoord_input:1; /* enum gl_shader_texcoord_input */

	unsigned variant:4; /* enum gl_shader_texture_variant */
	bool input_is_premult:1;
	bool tint:1;
	bool wireframe:1;
	bool swizzle_idx:1;

	unsigned color_effect:2; /* enum gl_shader_color_effect */

	unsigned color_pre_curve:3; /* enum gl_shader_color_curve */
	unsigned color_mapping:2; /* enum gl_shader_color_mapping */
	unsigned color_post_curve:3; /* enum gl_shader_color_curve */

	bool shader_blending:1;
	unsigned fb_alpha_encoding:1;  /* enum gl_shader_fb_alpha_encoding */
	unsigned fb_fetch_curve:3; /* enum gl_shader_color_curve */
	unsigned fb_store_curve:3; /* enum gl_shader_color_curve */

	/*
	 * The total size of all bitfields plus pad_bits_ must fill up exactly
	 * how many bytes the compiler allocates for them together.
	 */
	unsigned pad_bits_:5;
};
static_assert(sizeof(struct gl_shader_requirements) ==
	      4 /* total bitfield size in bytes */,
	      "struct gl_shader_requirements must not contain implicit padding");

enum gl_texture_flag {
	TEXTURE_FILTERS_DIRTY    = 1 << 0,
	TEXTURE_WRAP_MODES_DIRTY = 1 << 1,
	TEXTURE_SWIZZLES_DIRTY   = 1 << 2,
	TEXTURE_ALL_DIRTY        = (TEXTURE_FILTERS_DIRTY |
				    TEXTURE_WRAP_MODES_DIRTY |
				    TEXTURE_SWIZZLES_DIRTY),
};

struct gl_texture_parameters {
	GLenum target;
	union {
		struct { GLint min, mag; };
		GLint array[2];
	} filters;
	union {
		struct { GLint s, t, r; };
		GLint array[3];
	} wrap_modes;
	union {
		struct { GLint r, g, b, a; };
		GLint array[4];
	} swizzles;
	uint32_t flags;
};

struct gl_shader;
struct gl_shader_blender;
struct weston_color_transform;
struct dmabuf_allocator;

union gl_shader_config_color_effect {
	struct weston_cvd_correction cvd;
};

union gl_shader_config_color_curve {
	struct {
		GLuint tex;
		float scale;
		float offset;
	} lut_3x1d;
	struct {
		union weston_color_curve_parametric_data params;
		GLboolean clamped_input;
	} parametric;
};

union gl_shader_config_color_mapping {
	struct {
		GLuint tex3d;
		float scale;
		float offset;
	} lut3d;
	struct weston_color_mapping_matrix mat;
};

struct gl_shader_config {
	struct gl_shader_requirements req;

	struct weston_matrix projection;
	struct weston_matrix surface_to_buffer;
	float paint_node_alpha;
	GLfloat unicolor[4];
	GLfloat tint[4];

	struct gl_texture_parameters *input_param;
	GLuint *input_tex;
	int input_num;

	GLuint wireframe_tex;

	union gl_shader_config_color_effect color_effect;

	union gl_shader_config_color_curve color_pre_curve;
	union gl_shader_config_color_mapping color_mapping;
	union gl_shader_config_color_curve color_post_curve;

	union gl_shader_config_color_curve fb_fetch_curve;
	union gl_shader_config_color_curve fb_store_curve;

	enum weston_color_matrix_coef yuv_coefficients;
	enum weston_color_quant_range yuv_range;
};

struct gl_renderer {
	struct weston_renderer base;
	struct weston_compositor *compositor;
	struct weston_log_scope *extensions_scope;
	struct weston_log_scope *paint_node_scope;

	/* Debug modes. */
	struct weston_binding *debug_mode_binding;
	int debug_mode;
	bool debug_clear;
	bool wireframe_dirty;
	GLuint wireframe_tex;
	int wireframe_size;
	int nbatches;

	EGLenum platform;
	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLConfig egl_config;

	uint32_t gl_version;

	/* Vertex streams. */
	struct wl_array position_stream;
	struct wl_array barycentric_stream;
	struct wl_array indices;

	EGLDeviceEXT egl_device;
	const char *drm_device;

	struct weston_drm_format_array supported_dmabuf_formats;

	unsigned int supported_rendering_formats_count;
	const struct pixel_format_info **supported_rendering_formats;

	uint64_t egl_client_extensions;
	uint64_t egl_device_extensions;
	uint64_t egl_display_extensions;

	/* EGL_EXT_device_query */
	PFNEGLQUERYDISPLAYATTRIBEXTPROC query_display_attrib;
	PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string;

	/* EGL_EXT_platform_base */
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window;

	/* EGL_KHR_image_base */
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;

	/* EGL_WL_bind_wayland_display */
	PFNEGLBINDWAYLANDDISPLAYWL bind_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_display;
	PFNEGLQUERYWAYLANDBUFFERWL query_buffer;
	bool display_bound;

	/* EGL_KHR_partial_update */
	PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region;

	/* EGL_KHR_swap_buffers_with_damage
	 * EGL_EXT_swap_buffers_with_damage */
	PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;

	/* EGL_EXT_image_dma_buf_import_modifiers */
	PFNEGLQUERYDMABUFFORMATSEXTPROC query_dmabuf_formats;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dmabuf_modifiers;

	/* EGL_KHR_fence_sync */
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;

	/* EGL_ANDROID_native_fence_sync */
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;

	/* EGL_KHR_wait_sync */
	PFNEGLWAITSYNCKHRPROC wait_sync;

	uint64_t gl_extensions;

	/* GL_OES_EGL_image */
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_renderbuffer_storage;

	/* GL_EXT_EGL_image_storage */
	PFNGLEGLIMAGETARGETTEXTURESTORAGEEXTPROC image_target_tex_storage;

	/* GL_OES_mapbuffer */
	PFNGLUNMAPBUFFEROESPROC unmap_buffer;

	/* GL_EXT_map_buffer_range */
	PFNGLMAPBUFFERRANGEEXTPROC map_buffer_range;

	/* GL_OES_texture_3d */
	PFNGLTEXIMAGE3DOESPROC tex_image_3d;
	PFNGLTEXSUBIMAGE3DOESPROC tex_sub_image_3d;

	/* GL_EXT_disjoint_timer_query */
	PFNGLGENQUERIESEXTPROC gen_queries;
	PFNGLDELETEQUERIESEXTPROC delete_queries;
	PFNGLBEGINQUERYEXTPROC begin_query;
	PFNGLENDQUERYEXTPROC end_query;
#if !defined(NDEBUG)
	PFNGLGETQUERYOBJECTIVEXTPROC get_query_object_iv;
#endif
	PFNGLGETQUERYOBJECTUI64VEXTPROC get_query_object_ui64v;

	/* GL_EXT_texture_storage */
	PFNGLTEXSTORAGE2DEXTPROC tex_storage_2d;
	PFNGLTEXSTORAGE3DEXTPROC tex_storage_3d;

	/* GL_EXT_shader_framebuffer_fetch_non_coherent */
	PFNGLFRAMEBUFFERFETCHBARRIEREXTPROC framebuffer_fetch_barrier;

	uint64_t features;

	GLenum pbo_usage;
	enum gl_bgra8_texture_support bgra8_texture_support;
	int max_texture_image_units;
	int max_combined_texture_image_units;

	bool blend_state;

	struct wl_list dmabuf_images;
	struct wl_list dmabuf_formats;
	struct wl_list pending_capture_list;

	struct gl_shader *current_shader;
	struct gl_shader *fallback_shader;

	struct wl_signal destroy_signal;

	/** Shader program cache in most recently used order
	 *
	 * Uses struct gl_shader::link.
	 */
	struct wl_list shader_list;
	struct weston_log_scope *shader_scope;

	struct dmabuf_allocator *allocator;

	int drm_fd;
	struct gbm_device *gbm;
};

static inline uint32_t
gl_version(uint16_t major, uint16_t minor)
{
	return ((uint32_t)major << 16) | minor;
}

static inline int
gl_version_major(uint32_t ver)
{
	return ver >> 16;
}

static inline int
gl_version_minor(uint32_t ver)
{
	return ver & 0xffff;
}

void
gl_extensions_add(const struct gl_extension_table *table,
		  const char *extensions,
		  uint64_t *flags_out);

static inline bool
egl_client_has(struct gl_renderer *gr,
	       enum egl_client_extension_flag flag)
{
	return (bool) (gr->egl_client_extensions & ((uint64_t) flag));
}

static inline bool
egl_device_has(struct gl_renderer *gr,
	       enum egl_device_extension_flag flag)
{
	return (bool) (gr->egl_device_extensions & ((uint64_t) flag));
}

static inline bool
egl_display_has(struct gl_renderer *gr,
		enum egl_display_extension_flag flag)
{
	return (bool) (gr->egl_display_extensions & ((uint64_t) flag));
}

static inline bool
gl_extensions_has(struct gl_renderer *gr,
		  enum gl_extension_flag flag)
{
	return (bool) (gr->gl_extensions & ((uint64_t) flag));
}

static inline bool
gl_features_has(struct gl_renderer *gr,
		enum gl_feature_flag flag)
{
	return (bool) (gr->features & ((uint64_t) flag));
}

enum gl_bgra8_texture_support
gl_get_bgra8_texture_support(struct gl_renderer *gr);

bool
gl_has_sized_bgra8_renderbuffer(struct gl_renderer *gr);

bool
gl_texture_is_format_supported(struct gl_renderer *gr,
			       GLenum format);

bool
gl_texture_2d_init(struct gl_renderer *gr,
		   int levels,
		   GLenum format,
		   int width,
		   int height,
		   GLuint *tex_out);

void
gl_texture_2d_store(struct gl_renderer *gr,
		    int level,
		    int x,
		    int y,
		    int width,
		    int height,
		    GLenum format,
		    GLenum type,
		    const void *data);

bool
gl_texture_3d_init(struct gl_renderer *gr,
		   int levels,
		   GLenum format,
		   int width,
		   int height,
		   int depth,
		   GLuint *tex_out);

bool
gl_texture_3d_store(struct gl_renderer *gr,
		    int level,
		    int x,
		    int y,
		    int z,
		    int width,
		    int height,
		    int depth,
		    GLenum format,
		    GLenum type,
		    const void *data);

void
gl_texture_fini(GLuint *tex);

void
gl_texture_parameters_init(struct gl_renderer *gr,
			   struct gl_texture_parameters *parameters,
			   GLenum target,
			   const GLint *filters,
			   const GLint *wrap_modes,
			   const GLint *swizzles,
			   bool flush);

void
gl_texture_parameters_flush(struct gl_renderer *gr,
			    struct gl_texture_parameters *parameters);

bool
gl_fbo_is_format_supported(struct gl_renderer *gr,
			   GLenum format);

bool
gl_fbo_init(struct gl_renderer *gr,
	    GLenum format,
	    int width,
	    int height,
	    GLuint *fb_out,
	    GLuint *rb_out);

void
gl_fbo_fini(GLuint *fb,
	    GLuint *rb);

bool
gl_fbo_image_init(struct gl_renderer *gr,
		  EGLImageKHR image,
		  GLuint *fb_out,
		  GLuint *rb_out);

bool
gl_fbo_texture_init(struct gl_renderer *gr,
		    GLenum format,
		    int width,
		    int height,
		    GLuint *fb_out,
		    GLuint *tex_out);

void
gl_fbo_texture_fini(GLuint *fb,
		    GLuint *tex);

static inline struct gl_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct gl_renderer *)ec->renderer;
}

void
gl_renderer_print_egl_error_state(void);

void
gl_renderer_log_extensions(struct gl_renderer *gr,
			   const char *name, const char *extensions);

void
log_egl_config_info(struct gl_renderer *gr, EGLConfig eglconfig);

const struct pixel_format_info **
egl_set_supported_rendering_formats(EGLDisplay egldpy,
				    unsigned int *formats_count);

EGLConfig
gl_renderer_get_egl_config(struct gl_renderer *gr,
			   EGLint egl_surface_type,
			   const struct pixel_format_info *const *formats,
			   unsigned formats_count);

int
gl_renderer_setup_egl_display(struct gl_renderer *gr, void *native_display);

int
gl_renderer_setup_egl_client_extensions(struct gl_renderer *gr);

int
gl_renderer_setup_egl_extensions(struct weston_compositor *ec);

GLenum
gl_shader_texture_variant_get_target(enum gl_shader_texture_variant v);

bool
gl_shader_texture_variant_can_be_premult(enum gl_shader_texture_variant v);

void
gl_shader_destroy(struct gl_renderer *gr, struct gl_shader *shader);

void
gl_renderer_shader_list_destroy(struct gl_renderer *gr);

struct gl_shader *
gl_renderer_create_fallback_shader(struct gl_renderer *gr);

void
gl_renderer_garbage_collect_programs(struct gl_renderer *gr);

bool
gl_renderer_use_program(struct gl_renderer *gr, struct weston_paint_node *pnode,
			const struct gl_shader_config *sconf);

struct weston_log_scope *
gl_shader_scope_create(struct gl_renderer *gr);

bool
gl_shader_config_set_color_transform(struct gl_renderer *gr,
				     struct gl_shader_config *sconf,
				     struct weston_color_transform *xform);

bool
gl_shader_config_set_color_effect(struct gl_renderer *gr,
				  struct gl_shader_config *sconf,
				  struct weston_output_color_effect *effect);
const char *
weston_output_cvd_type_to_str(struct weston_cvd_correction cvd);

void
gl_shader_blender_destroy(struct gl_shader_blender *shader_blender);

struct gl_shader_blender *
gl_shader_blender_create(struct gl_renderer *gr, struct weston_output *output,
			 enum gl_shader_fb_alpha_encoding fb_alpha_encoding);

void
gl_shader_config_set_blender(struct gl_renderer *gr,
			     struct gl_shader_config *sconf,
			     const struct gl_shader_blender *shader_blender);

#endif /* GL_RENDERER_INTERNAL_H */
