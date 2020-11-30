/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019 Collabora, Ltd.
 * Copyright 2016 NVIDIA Corporation
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

#include <GLES2/gl2.h>

#include "gl-renderer.h"
#include "gl-renderer-internal.h"

/* static const char vertex_shader[]; vertex.glsl */
#include "vertex-shader.h"

/* static const char fragment_shader[]; fragment.glsl */
#include "fragment-shader.h"

static int
compile_shader(GLenum type, int count, const char **sources)
{
	GLuint s;
	char msg[512];
	GLint status;

	s = glCreateShader(type);
	glShaderSource(s, count, sources, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof msg, NULL, msg);
		weston_log("shader info: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

int
shader_init(struct gl_shader *shader, struct gl_renderer *renderer,
		   const char *vertex_source, const char *fragment_variant)
{
	static const char fragment_shader_attrs_fmt[] =
		"#define DEF_GREEN_TINT %s\n"
		"#define DEF_VARIANT %s\n"
		;
	char msg[512];
	GLint status;
	int ret;
	const char *sources[3];
	char *attrs;
	const char *def_green_tint;

	shader->vertex_shader =
		compile_shader(GL_VERTEX_SHADER, 1, &vertex_source);
	if (shader->vertex_shader == GL_NONE)
		return -1;

	if (renderer->fragment_shader_debug)
		def_green_tint = "true";
	else
		def_green_tint = "false";

	ret = asprintf(&attrs, fragment_shader_attrs_fmt,
		       def_green_tint, fragment_variant);
	if (ret < 0)
		return -1;

	sources[0] = "#version 100\n";
	sources[1] = attrs;
	sources[2] = fragment_shader;
	shader->fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
						 3, sources);
	free(attrs);
	if (shader->fragment_shader == GL_NONE)
		return -1;

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vertex_shader);
	glAttachShader(shader->program, shader->fragment_shader);
	glBindAttribLocation(shader->program, 0, "position");
	glBindAttribLocation(shader->program, 1, "texcoord");

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(shader->program, sizeof msg, NULL, msg);
		weston_log("link info: %s\n", msg);
		return -1;
	}

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->tex_uniforms[0] = glGetUniformLocation(shader->program, "tex");
	shader->tex_uniforms[1] = glGetUniformLocation(shader->program, "tex1");
	shader->tex_uniforms[2] = glGetUniformLocation(shader->program, "tex2");
	shader->alpha_uniform = glGetUniformLocation(shader->program, "alpha");
	shader->color_uniform = glGetUniformLocation(shader->program, "color");

	return 0;
}

void
shader_release(struct gl_shader *shader)
{
	glDeleteShader(shader->vertex_shader);
	glDeleteShader(shader->fragment_shader);
	glDeleteProgram(shader->program);

	shader->vertex_shader = 0;
	shader->fragment_shader = 0;
	shader->program = 0;
}

int
compile_shaders(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);

	gr->texture_shader_rgba.vertex_source = vertex_shader;
	gr->texture_shader_rgba.fragment_source = "SHADER_VARIANT_RGBA";

	gr->texture_shader_rgbx.vertex_source = vertex_shader;
	gr->texture_shader_rgbx.fragment_source = "SHADER_VARIANT_RGBX";

	gr->texture_shader_egl_external.vertex_source = vertex_shader;
	gr->texture_shader_egl_external.fragment_source = "SHADER_VARIANT_EXTERNAL";

	gr->texture_shader_y_uv.vertex_source = vertex_shader;
	gr->texture_shader_y_uv.fragment_source = "SHADER_VARIANT_Y_UV";

	gr->texture_shader_y_u_v.vertex_source = vertex_shader;
	gr->texture_shader_y_u_v.fragment_source = "SHADER_VARIANT_Y_U_V";

	gr->texture_shader_y_xuxv.vertex_source = vertex_shader;
	gr->texture_shader_y_xuxv.fragment_source = "SHADER_VARIANT_Y_XUXV";

	gr->texture_shader_xyuv.vertex_source = vertex_shader;
	gr->texture_shader_xyuv.fragment_source = "SHADER_VARIANT_XYUV";

	gr->solid_shader.vertex_source = vertex_shader;
	gr->solid_shader.fragment_source = "SHADER_VARIANT_SOLID";

	return 0;
}
