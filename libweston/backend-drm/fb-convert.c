/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd
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

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "pixel-formats.h"
#include "drm-internal.h"

#if defined(HAVE_EGL) && defined(HAVE_GLES2) && defined(HAVE_GBM)
#define EGL_CONVERT
#endif

#ifdef EGL_CONVERT
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

#ifdef HAVE_RGA
#include <rga/rga.h>
#include <rga/RgaApi.h>
#endif

struct fb_convert_ctx {
#ifdef EGL_CONVERT
	int drm_fd;
	struct gbm_device *gbm;
	EGLDisplay display;
	EGLContext context;
	GLuint vs;
	GLuint fs;
	GLuint program;

	bool egl_inited;
#endif
#ifdef HAVE_RGA
	bool rga_unavailable;
	bool rga_inited;
#endif
} fb_ctx;

#ifdef EGL_CONVERT
#define EGL_LOAD_PROC(val, type, func) \
	do { val = (type) eglGetProcAddress(func); } while (0)

static const char* vs_src =
"attribute vec4 position;\n"
"uniform int rotation;\n"
"varying vec2 texcoord;\n"
"void main() {\n"
"   vec2 pos = position.xy;\n"
"   if (rotation == 1) {\n"
"       pos = vec2(pos.y, -pos.x);\n"
"   } else if (rotation == 2) {\n"
"       pos = vec2(-pos.x, -pos.y);\n"
"   } else if (rotation == 3) {\n"
"       pos = vec2(-pos.y, pos.x);\n"
"   }\n"
"   gl_Position = vec4(pos, 0.0, 1.0);\n"
"   vec2 orig_tex = pos.xy * 0.5 + 0.5;\n"
"   if (rotation == 1) {\n"
"       texcoord = vec2(orig_tex.y, 1.0 - orig_tex.x);\n"
"   } else if (rotation == 2) {\n"
"       texcoord = vec2(1.0 - orig_tex.x, 1.0 - orig_tex.y);\n"
"   } else if (rotation == 3) {\n"
"       texcoord = vec2(1.0 - orig_tex.y, orig_tex.x);\n"
"   } else {\n"
"       texcoord = orig_tex;\n"
"   }\n"
"}\n";

static const char* fs_src =
"#extension GL_OES_EGL_image_external : require\n"
"precision mediump float;\n"
"varying vec2 texcoord;\n"
"uniform samplerExternalOES tex;\n"
"void main() {\n"
"   gl_FragColor = texture2D(tex, texcoord);\n"
"}\n";

static void
egl_deinit(void)
{
	if (fb_ctx.display != EGL_NO_DISPLAY) {
		if (fb_ctx.program)
			glDeleteProgram(fb_ctx.program);
		if (fb_ctx.vs)
			glDeleteShader(fb_ctx.vs);
		if (fb_ctx.fs)
			glDeleteShader(fb_ctx.fs);
		if (fb_ctx.context != EGL_NO_CONTEXT)
			eglDestroyContext(fb_ctx.display, fb_ctx.context);

		eglTerminate(fb_ctx.display);
	}

	if (fb_ctx.gbm)
		gbm_device_destroy(fb_ctx.gbm);

	if (fb_ctx.drm_fd >= 0)
		close(fb_ctx.drm_fd);

	memset(&fb_ctx, 0, sizeof(fb_ctx));
}

static GLuint
egl_compile_shader(GLenum type, const char* src) {
	GLuint shader;
	GLint status;

	shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
		return 0;

	return shader;
}

static bool
egl_init(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	GLint status;

	const EGLint context_attrs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};

	if (fb_ctx.egl_inited)
		return true;

	EGL_LOAD_PROC(get_platform_display, PFNEGLGETPLATFORMDISPLAYEXTPROC,
		      "eglGetPlatformDisplayEXT");
	if (!get_platform_display)
		return false;

	fb_ctx.drm_fd = drmOpen("rockchip", NULL);
	if (fb_ctx.drm_fd < 0)
		goto err;

	fb_ctx.gbm = gbm_create_device(fb_ctx.drm_fd);
	if (!fb_ctx.gbm)
		goto err;

	fb_ctx.display = get_platform_display(EGL_PLATFORM_GBM_KHR,
					    (void*)fb_ctx.gbm, NULL);
	if (fb_ctx.display == EGL_NO_DISPLAY)
		goto err;

	if (!eglInitialize(fb_ctx.display, NULL, NULL))
		goto err;

	if (!eglBindAPI(EGL_OPENGL_ES_API))
		goto err;

	fb_ctx.context = eglCreateContext(fb_ctx.display, NULL, EGL_NO_CONTEXT,
					context_attrs);
	if (fb_ctx.context == EGL_NO_CONTEXT)
		goto err;

	eglMakeCurrent(fb_ctx.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       fb_ctx.context);

	fb_ctx.vs = egl_compile_shader(GL_VERTEX_SHADER, vs_src);
	fb_ctx.fs = egl_compile_shader(GL_FRAGMENT_SHADER, fs_src);

	fb_ctx.program = glCreateProgram();
	glAttachShader(fb_ctx.program, fb_ctx.vs);
	glAttachShader(fb_ctx.program, fb_ctx.fs);
	glLinkProgram(fb_ctx.program);

	glGetProgramiv(fb_ctx.program, GL_LINK_STATUS, &status);
	if (!status)
		goto err;

	eglMakeCurrent(fb_ctx.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	fb_ctx.egl_inited = true;
	return true;
err:
	egl_deinit();
	return false;
}

static bool
egl_load_dmabuf(EGLDisplay display, GLuint tex,
		int fd, uint32_t format, int width, int height, int stride) {
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d = NULL;
	PFNEGLCREATEIMAGEKHRPROC create_image = NULL;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image = NULL;
	EGLint attribs[] = {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_LINUX_DRM_FOURCC_EXT, format,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_NONE,
	};
	EGLImageKHR image;

	EGL_LOAD_PROC(create_image, PFNEGLCREATEIMAGEKHRPROC,
		      "eglCreateImageKHR");
	EGL_LOAD_PROC(destroy_image, PFNEGLDESTROYIMAGEKHRPROC,
		      "eglDestroyImageKHR");
	EGL_LOAD_PROC(image_target_texture_2d, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,
		      "glEGLImageTargetTexture2DOES");
	if (!create_image || !destroy_image || !image_target_texture_2d)
		return false;

	image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
			     NULL, attribs);
	if (image == EGL_NO_IMAGE)
		return false;

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
	image_target_texture_2d(GL_TEXTURE_EXTERNAL_OES, image);

	destroy_image(display, image);
	return true;
}

static bool
drm_fb_convert_egl(struct drm_fb *src, struct drm_fb *dst, int rotation,
		   int src_width, int src_height)
{
	GLuint fbo = 0, src_tex = 0, dst_tex = 0;
	GLint rot_loc, pos_loc;
	GLint status;
	int src_fd = -1, dst_fd = -1;
	bool ret = false;

	EGLDisplay old_display = eglGetCurrentDisplay();
	EGLContext old_context = eglGetCurrentContext();
	EGLSurface old_draw_surface = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface old_read_surface = eglGetCurrentSurface(EGL_READ);

	GLfloat verts[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f,
	};

	if (!egl_init())
		goto out;

	if (drmPrimeHandleToFD(src->fd, src->handles[0],
			       DRM_CLOEXEC, &src_fd) < 0)
		goto out;

	if (drmPrimeHandleToFD(dst->fd, dst->handles[0],
			       DRM_CLOEXEC, &dst_fd) < 0)
		goto out;

	eglMakeCurrent(fb_ctx.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       fb_ctx.context);

	glGenTextures(1, &src_tex);
	if (!egl_load_dmabuf(fb_ctx.display, src_tex, src_fd, src->format->format,
			     src_width, src_height, src->strides[0]))
		goto out;

	glGenTextures(1, &dst_tex);
	if (!egl_load_dmabuf(fb_ctx.display, dst_tex, dst_fd, dst->format->format,
			     dst->width, dst->height, dst->strides[0]))
		goto out;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_EXTERNAL_OES, dst_tex, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		goto out;

	glViewport(0, 0, dst->width, dst->height);

	glUseProgram(fb_ctx.program);

	rot_loc = glGetUniformLocation(fb_ctx.program, "rotation");
	glUniform1i(rot_loc, rotation / 90);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_tex);

	glUniform1i(glGetUniformLocation(fb_ctx.program, "tex"), 0);

	pos_loc = glGetAttribLocation(fb_ctx.program, "position");
	glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glEnableVertexAttribArray(pos_loc);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();

	ret = true;
out:
	if (src_fd >= 0)
		close(src_fd);
	if (dst_fd >= 0)
		close(dst_fd);

	if (fbo)
		glDeleteFramebuffers(1, &fbo);
	if (dst_tex)
		glDeleteTextures(1, &dst_tex);
	if (src_tex)
		glDeleteTextures(1, &src_tex);

	eglMakeCurrent(old_display, old_draw_surface, old_read_surface,
		       old_context);

	return ret;
}
#endif

#ifdef HAVE_RGA
static inline RgaSURF_FORMAT
rga_get_format(const struct pixel_format_info *format)
{
	switch (format->format) {
	case DRM_FORMAT_XRGB8888:
		return RK_FORMAT_BGRX_8888;
	case DRM_FORMAT_ARGB8888:
		return RK_FORMAT_BGRA_8888;
	case DRM_FORMAT_RGB565:
		return RK_FORMAT_RGB_565;
	default:
		return RK_FORMAT_UNKNOWN;
	}
}

static bool
drm_fb_convert_rga(struct drm_fb *src, struct drm_fb *dst, int rotation,
		   int src_width, int src_height)
{
	RgaSURF_FORMAT src_format, dst_format;
	rga_info_t src_info = {0};
	rga_info_t dst_info = {0};
	int src_fd, dst_fd;
	int ret;

	if (fb_ctx.rga_unavailable)
		return false;

	if (!fb_ctx.rga_inited) {
		ret = c_RkRgaInit();
		if (ret < 0) {
			weston_log("RGA not available\n");
			fb_ctx.rga_unavailable = true;
			return false;
		}
		fb_ctx.rga_inited = true;
	}

	src_format = rga_get_format(src->format);
	dst_format = rga_get_format(dst->format);

	if (src_format == RK_FORMAT_UNKNOWN ||
	    dst_format == RK_FORMAT_UNKNOWN)
		return false;

	ret = drmPrimeHandleToFD(src->fd, src->handles[0],
				 DRM_CLOEXEC, &src_fd);
	if (ret < 0)
		return false;

	ret = drmPrimeHandleToFD(dst->fd, dst->handles[0],
				 DRM_CLOEXEC, &dst_fd);
	if (ret < 0)
		goto close_src;

	src_info.fd = src_fd;
	src_info.mmuFlag = 1;

	rga_set_rect(&src_info.rect, 0, 0, src_width, src_height,
		     src->strides[0] * 8 / src->format->bpp, src->height,
		     src_format);

	if (rotation == 90)
		src_info.rotation = HAL_TRANSFORM_ROT_90;
	else if (rotation == 180)
		src_info.rotation = HAL_TRANSFORM_ROT_180;
	else if (rotation == 270)
		src_info.rotation = HAL_TRANSFORM_ROT_270;

	dst_info.fd = dst_fd;
	dst_info.mmuFlag = 1;

	rga_set_rect(&dst_info.rect, 0, 0, dst->width, dst->height,
		     dst->strides[0] * 8 / dst->format->bpp, dst->height,
		     dst_format);

	ret = c_RkRgaBlit(&src_info, &dst_info, NULL);
	close(dst_fd);
close_src:
	close(src_fd);
	return ret < 0 ? false : true;
}
#endif

bool
drm_fb_convert(struct drm_fb *src, struct drm_fb *dst, int rotation,
	       int src_width, int src_height)
{
#ifdef EGL_CONVERT
	if (drm_fb_convert_egl(src, dst, rotation, src_width, src_height)) {
		return true;
}
#endif
#ifdef HAVE_RGA
	if (drm_fb_convert_rga(src, dst, rotation, src_width, src_height))
		return true;
#endif
	weston_log("Unable to convert fb!\n");
	return false;
}
