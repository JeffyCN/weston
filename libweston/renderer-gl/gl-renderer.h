/*
 * Copyright © 2012 John Kåre Alsaker
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

#pragma once

#include "config.h"

#include <stdint.h>

#include <libweston/libweston.h>
#include "backend.h"
#include "libweston-internal.h"

#ifdef ENABLE_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>

#else

typedef int EGLint;
typedef int EGLenum;
typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef intptr_t EGLNativeDisplayType;
typedef intptr_t EGLNativeWindowType;
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_PBUFFER_BIT                   0x0001
#define EGL_WINDOW_BIT                    0x0004

#endif /* ENABLE_EGL */

/**
 * Options passed to the \c display_create method of the GL renderer interface.
 *
 * \see struct gl_renderer_interface
 */
struct gl_renderer_display_options {
	struct weston_renderer_options base;
	/** The EGL platform identifier */
	EGLenum egl_platform;
	/** The native display corresponding to the given EGL platform */
	void *egl_native_display;
	/** EGL_SURFACE_TYPE bits for the base EGLConfig */
	EGLint egl_surface_type;
	/** Array of pixel formats acceptable for the base EGLConfig */
	const struct pixel_format_info **formats;
	/** The \c formats array length */
	unsigned formats_count;
};

struct gl_renderer_output_options {
	/** Native window handle for \c eglCreateWindowSurface */
	EGLNativeWindowType window_for_legacy;
	/** Native window handle for \c eglCreatePlatformWindowSurface */
	void *window_for_platform;
	/** Size of the framebuffer in pixels, including borders */
	struct weston_size fb_size;
	/** Area inside the framebuffer in pixels for composited content */
	struct weston_geometry area;
	/** Array of pixel formats acceptable for the window */
	const struct pixel_format_info **formats;
	/** The \c formats array length */
	unsigned formats_count;
};

struct gl_renderer_fbo_options {
	/** Size of the framebuffer in pixels, including borders */
	struct weston_size fb_size;
	/** Area inside the framebuffer in pixels for composited content */
	struct weston_geometry area;
};

struct gl_renderer_interface {
	/**
	 * Initialize GL-renderer with the given EGL platform and native display
	 *
	 * \param ec The weston_compositor where to initialize.
	 * \param options The options struct describing display configuration
	 * \return 0 on success, -1 on failure.
	 *
	 * This function creates an EGLDisplay and initializes it. It also
	 * creates the GL ES context and sets it up. It attempts GL ES 3.0
	 * and falls back to GL ES 2.0 if 3.0 is not supported.
	 *
	 * If \c platform is zero or EGL_EXT_platform_base is not supported,
	 * choosing the platform is left for the EGL implementation. Otherwise
	 * the given platform is used explicitly if the EGL implementation
	 * advertises it. Without the advertisement this function fails.
	 *
	 * If neither EGL_KHR_no_config_context or EGL_MESA_configless_context
	 * are supported, the arguments egl_surface_type, formats, and
	 * formats_count are used to find a so called base EGLConfig. The
	 * GL context is created with the base EGLConfig, and outputs will be
	 * required to use the same config as well. If one or both of the
	 * extensions are supported, these arguments are unused, and each
	 * output can use a different EGLConfig (pixel format).
	 *
	 * The first format in formats that matches any EGLConfig
	 * determines which EGLConfig is chosen. On EGL GBM platform, the
	 * pixel format must match exactly. On other platforms, it is enough
	 * that each R, G, B, A channel has the same number of bits as in the
	 * DRM format.
	 */
	int (*display_create)(struct weston_compositor *ec,
			      const struct gl_renderer_display_options *options);

	/**
	 * create_buffer - Create window-type renderbuffer with EGL surface
	 * @output: Target weston output
	 * @options: Output options (EGL window handle, size, format, etc.)
	 *
	 * Creates a renderbuffer that owns the EGL surface (destroys it on cleanup).
	 * Return: Valid renderbuffer on success; NULL on failure.
	 */
	struct weston_renderbuffer *(*create_buffer)(struct weston_output *output,
						     const struct gl_renderer_output_options *options);

	/**
	 * dup_buffer - Duplicate renderbuffer (share EGL surface)
	 * @output: Target weston output
	 * @renderbuffer: Original renderbuffer to duplicate
	 *
	 * Duplicates a renderbuffer that shares the EGL surface (no ownership, avoids double-free).
	 * Return: Valid renderbuffer on success; NULL on failure.
	 */
	struct weston_renderbuffer *(*dup_buffer)(struct weston_output *output,
						  struct weston_renderbuffer *renderbuffer);

	/**
	 * Attach GL-renderer to the output with a native window
	 *
	 * \param output The output to create a rendering surface for.
	 * \param options The options struct describing output configuration
	 * \return 0 on success, -1 on failure.
	 *
	 * This function creates the renderer data structures needed to repaint
	 * the output. The repaint results will be directed to the given native
	 * window.
	 *
	 * If EGL_EXT_platform_base is supported then \c window_for_platform is
	 * used, otherwise \c window_for_legacy is used. This is because the
	 * handle on X11 platform is different between the two.
	 *
	 * The first format in formats that matches any EGLConfig
	 * determines which EGLConfig is chosen. See \c display_create about
	 * how the matching works and the possible limitations.
	 *
	 * This function should be used only if \c display_create was called
	 * with \c EGL_WINDOW_BIT in \c egl_surface_type.
	 */
	int (*output_window_create)(struct weston_output *output,
				    const struct gl_renderer_output_options *options);

	/**
	 * Query rendering formats supported by EGL on GBM platform
	 *
	 * \param ec The weston_compositor.
	 * \param formats_count The number of formats in the returned array.
	 * \return The array of formats supported, or NULL on failure.
	 *
	 * This function returns the formats that are present in the EGLConfig's
	 * that we query from the EGLDisplay. Such formats can be used for
	 * rendering.
	 */
	const struct pixel_format_info **
	(*get_supported_rendering_formats)(struct weston_compositor *ec,
					   unsigned int *formats_count);

	/**
	 * Attach GL-renderer to the output with a frame buffer object
	 *
	 * \param output The output to prepare for FBO rendering.
	 * \param options The options struct describing the render geometry
	 * \return 0 on success, -1 on failure.
	 *
	 * This function creates the renderer data structures needed to repaint
	 * the output. The repaint results will be stored in FBO renderbuffers
	 * passed to \c repaint_output.
	 */
	int (*output_fbo_create)(struct weston_output *output,
				 const struct gl_renderer_fbo_options *options);

	void (*output_destroy)(struct weston_output *output);

	/* Create fence sync FD to wait for GPU rendering.
	 *
	 * Return FD on success, -1 on failure or unsupported
	 * EGL_ANDROID_native_fence_sync extension.
	 */
	int (*create_fence_fd)(struct weston_output *output);

	/**
	 * Get GL renderer display options
	 * @compositor: Weston compositor instance
	 * Return: Pointer to gl_renderer_display_options (const)
	 *
	 * Expose the GL renderer's EGL/GBM display options for VNC backend usage
	 */
	const struct gl_renderer_display_options *(*get_display_options)(struct weston_compositor *ec);
};
