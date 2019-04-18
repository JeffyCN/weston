/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019 Collabora, Ltd.
 * Copyright 2016 NVIDIA Corporation
 * Copyright 2019 Harish Krupo
 * Copyright 2019 Intel Corporation
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libweston/libweston.h>
#include <GLES2/gl2.h>

#include <string.h>

#include "gl-renderer.h"
#include "gl-renderer-internal.h"
#include "shared/helpers.h"

/* static const char vertex_shader[]; vertex.glsl */
#include "vertex-shader.h"

/* static const char fragment_shader[]; fragment.glsl */
#include "fragment-shader.h"

static const char *
gl_shader_texture_variant_to_string(enum gl_shader_texture_variant v)
{
	switch (v) {
#define CASERET(x) case x: return #x;
	CASERET(SHADER_VARIANT_NONE)
	CASERET(SHADER_VARIANT_RGBX)
	CASERET(SHADER_VARIANT_RGBA)
	CASERET(SHADER_VARIANT_Y_U_V)
	CASERET(SHADER_VARIANT_Y_UV)
	CASERET(SHADER_VARIANT_Y_XUXV)
	CASERET(SHADER_VARIANT_XYUV)
	CASERET(SHADER_VARIANT_SOLID)
	CASERET(SHADER_VARIANT_EXTERNAL)
#undef CASERET
	}

	return "!?!?"; /* never reached */
}

static void
dump_program_with_line_numbers(int count, const char **sources)
{
	FILE *fp;
	char *dumpstr;
	size_t dumpstrsz;
	const char *cur;
	const char *delim;
	int line = 1;
	int i;
	bool new_line = true;

	fp = open_memstream(&dumpstr, &dumpstrsz);
	if (!fp)
		return;

	for (i = 0; i < count; i++) {
		cur = sources[i];
		while ((delim = strchr(cur, '\n'))) {
			if (new_line)
				fprintf(fp, "%6d: ", line++);
			fprintf(fp, "%.*s\n", (int)(delim - cur), cur);
			new_line = true;
			cur = delim + 1;
		}
		if (new_line)
			fprintf(fp, "%6d: ", line++);
		new_line = false;
		fprintf(fp, "%s", cur);
	}

	if (fclose(fp) == 0)
		weston_log_continue("%s\n", dumpstr);

	free(dumpstr);
}

void
gl_shader_destroy(struct gl_shader *shader)
{
	glDeleteProgram(shader->program);
	wl_list_remove(&shader->link);
	free(shader);
}

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
		weston_log("shader source:\n");
		dump_program_with_line_numbers(count, sources);
		return GL_NONE;
	}

	return s;
}

static char *
create_shader_config_string(const struct gl_shader_requirements *req)
{
	int size;
	char *str;

	size = asprintf(&str,
			"#define DEF_GREEN_TINT %s\n"
			"#define DEF_VARIANT %s\n",
			req->green_tint ? "true" : "false",
			gl_shader_texture_variant_to_string(req->variant));
	if (size < 0)
		return NULL;
	return str;
}

struct gl_shader *
gl_shader_create(struct gl_renderer *gr,
		 const struct gl_shader_requirements *requirements)
{
	struct gl_shader *shader = NULL;
	char msg[512];
	GLint status;
	const char *sources[3];
	char *conf = NULL;

	shader = zalloc(sizeof *shader);
	if (!shader) {
		weston_log("could not create shader\n");
		goto error_vertex;
	}

	wl_list_init(&shader->link);
	shader->key = *requirements;

	sources[0] = vertex_shader;
	shader->vertex_shader = compile_shader(GL_VERTEX_SHADER, 1, sources);
	if (shader->vertex_shader == GL_NONE)
		goto error_vertex;

	conf = create_shader_config_string(&shader->key);
	if (!conf)
		goto error_fragment;

	sources[0] = "#version 100\n";
	sources[1] = conf;
	sources[2] = fragment_shader;
	shader->fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
						 3, sources);
	if (shader->fragment_shader == GL_NONE)
		goto error_fragment;

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
		goto error_link;
	}

	glDeleteShader(shader->vertex_shader);
	glDeleteShader(shader->fragment_shader);

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->tex_uniforms[0] = glGetUniformLocation(shader->program, "tex");
	shader->tex_uniforms[1] = glGetUniformLocation(shader->program, "tex1");
	shader->tex_uniforms[2] = glGetUniformLocation(shader->program, "tex2");
	shader->alpha_uniform = glGetUniformLocation(shader->program, "alpha");
	shader->color_uniform = glGetUniformLocation(shader->program, "color");

	free(conf);

	wl_list_insert(&gr->shader_list, &shader->link);

	return shader;

error_link:
	glDeleteProgram(shader->program);
	glDeleteShader(shader->fragment_shader);

error_fragment:
	glDeleteShader(shader->vertex_shader);

error_vertex:
	free(conf);
	free(shader);
	return NULL;
}

int
gl_shader_requirements_cmp(const struct gl_shader_requirements *a,
			   const struct gl_shader_requirements *b)
{
	return memcmp(a, b, sizeof(*a));
}
