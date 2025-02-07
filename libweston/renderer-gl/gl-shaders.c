/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019,2021-2025 Collabora, Ltd.
 * Copyright 2016 NVIDIA Corporation
 * Copyright 2019 Harish Krupo
 * Copyright 2019 Intel Corporation
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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libweston/color-representation.h>
#include <libweston/libweston.h>
#include <libweston/weston-log.h>

#include <string.h>

#include "gl-renderer.h"
#include "gl-renderer-internal.h"
#include "pixel-formats.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "shared/weston-assert.h"

/* static const char vertex_shader[]; vertex.glsl */
#include "vertex-shader.h"

/* static const char fragment_shader[]; fragment.glsl */
#include "fragment-shader.h"

union gl_shader_color_curve_uniforms {
	struct {
		GLint tex_2d_uniform;
		GLint scale_offset_uniform;
	} lut_3x1d;
	struct {
		GLint params_uniform;
		GLint clamped_input_uniform;
	} parametric;
};

union gl_shader_color_mapping_uniforms {
	struct {
		GLint tex_uniform;
		GLint scale_offset_uniform;
	} lut3d;
	struct {
		GLint matrix_uniform;
		GLint offset_uniform;
	} mat;
};

struct gl_shader {
	struct wl_list link; /* gl_renderer::shader_list */
	struct timespec last_used;
	struct gl_shader_requirements key;
	GLuint program;
	GLuint vertex_shader, fragment_shader;
	GLint proj_uniform;
	GLint surface_to_buffer_uniform;
	GLint tex_uniforms[3];
	GLint swizzle_idx[3];
	GLint swizzle_mask[3];
	GLint swizzle_sub[3];
	GLint tex_uniform_wireframe;
	GLint view_alpha_uniform;
	GLint color_uniform;
	GLint tint_uniform;
	GLint cvd_correction_uniform;
	union gl_shader_color_curve_uniforms color_pre_curve;
	union gl_shader_color_mapping_uniforms color_mapping;
	union gl_shader_color_curve_uniforms color_post_curve;
	union gl_shader_color_curve_uniforms fb_fetch_curve;
	union gl_shader_color_curve_uniforms fb_store_curve;
	GLint yuv_offsets_uniform;
	GLint yuv_coefficients_uniform;
};

struct gl_shader_enum_map {
	const char *symbol;
	const char *desc;
};

static const struct gl_shader_enum_map *
gl_shader_enum_map_get_(const struct gl_shader_enum_map *map,
		        size_t map_len,
		        unsigned value)
{
	if (value >= map_len)
		abort();

	if (!map[value].symbol)
		abort();

	return &map[value];
}

#define gl_shader_enum_map_get(map, value) gl_shader_enum_map_get_((map), ARRAY_LENGTH(map), (value))

#define ENUMVAL(sym, text) [sym] = { #sym, text }

static const struct gl_shader_enum_map gl_shader_texcoord_input_mapping[] = {
	ENUMVAL(SHADER_TEXCOORD_INPUT_SURFACE, "surf"),
	ENUMVAL(SHADER_TEXCOORD_INPUT_ATTRIB, "attr"),
};

static const struct gl_shader_enum_map *
gl_shader_texcoord_input_to_string(enum gl_shader_texcoord_input kind)
{
	return gl_shader_enum_map_get(gl_shader_texcoord_input_mapping, kind);
}

static const struct gl_shader_enum_map gl_shader_texture_variant_mapping[] = {
	ENUMVAL(SHADER_VARIANT_NONE, "none"),
	ENUMVAL(SHADER_VARIANT_RGBA, "RGBA"),
	ENUMVAL(SHADER_VARIANT_Y_U_V, "Y_U_V"),
	ENUMVAL(SHADER_VARIANT_Y_UV, "Y_UV"),
	ENUMVAL(SHADER_VARIANT_XYUV, "XYUV"),
	ENUMVAL(SHADER_VARIANT_SOLID, "solid"),
	ENUMVAL(SHADER_VARIANT_EXTERNAL, "external"),
};

static const struct gl_shader_enum_map *
gl_shader_texture_variant_to_string(enum gl_shader_texture_variant v)
{
	return gl_shader_enum_map_get(gl_shader_texture_variant_mapping, v);
}

static const struct gl_shader_enum_map gl_shader_color_effect_mapping[] = {
	ENUMVAL(SHADER_COLOR_EFFECT_NONE, "no"),
	ENUMVAL(SHADER_COLOR_EFFECT_INVERSION, "inv"),
	ENUMVAL(SHADER_COLOR_EFFECT_GRAYSCALE, "gray"),
	ENUMVAL(SHADER_COLOR_EFFECT_CVD_CORRECTION, "CVD-corr"),
};

static const struct gl_shader_enum_map *
gl_shader_color_effect_to_string(enum gl_shader_color_effect kind)
{
	return gl_shader_enum_map_get(gl_shader_color_effect_mapping, kind);
}

static const struct gl_shader_enum_map gl_shader_color_curve_mapping[] = {
	ENUMVAL(SHADER_COLOR_CURVE_IDENTITY, "I"),
	ENUMVAL(SHADER_COLOR_CURVE_LUT_3x1D, "3x1D"),
	ENUMVAL(SHADER_COLOR_CURVE_LINPOW, "linpow"),
	ENUMVAL(SHADER_COLOR_CURVE_POWLIN, "powlin"),
	ENUMVAL(SHADER_COLOR_CURVE_PQ, "PQ"),
	ENUMVAL(SHADER_COLOR_CURVE_PQ_INVERSE, "PQ⁻¹"),
};

static const struct gl_shader_enum_map *
gl_shader_color_curve_to_string(enum gl_shader_color_curve kind)
{
	return gl_shader_enum_map_get(gl_shader_color_curve_mapping, kind);
}

static const struct gl_shader_enum_map gl_shader_color_mapping_mapping[] = {
	ENUMVAL(SHADER_COLOR_MAPPING_IDENTITY, "I"),
	ENUMVAL(SHADER_COLOR_MAPPING_3DLUT, "3DLUT"),
	ENUMVAL(SHADER_COLOR_MAPPING_MATRIX, "M"),
};

static const struct gl_shader_enum_map *
gl_shader_color_mapping_to_string(enum gl_shader_color_mapping kind)
{
	return gl_shader_enum_map_get(gl_shader_color_mapping_mapping, kind);
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

static GLuint
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
create_shader_description_string(const struct gl_shader_requirements *req)
{
	int size;
	char *str;

	size = asprintf(&str, "%s tc, %s tex, %s effect, CP{ %s, %s, %s }, %cpremult_in %ctint %cshader_blending (%s, %s)",
			gl_shader_texcoord_input_to_string(req->texcoord_input)->desc,
			gl_shader_texture_variant_to_string(req->variant)->desc,
			gl_shader_color_effect_to_string(req->color_effect)->desc,
			gl_shader_color_curve_to_string(req->color_pre_curve)->desc,
			gl_shader_color_mapping_to_string(req->color_mapping)->desc,
			gl_shader_color_curve_to_string(req->color_post_curve)->desc,
			req->input_is_premult ? '+' : '-',
			req->tint ? '+' : '-',
			req->shader_blending ? '+' : '-',
			gl_shader_color_curve_to_string(req->fb_fetch_curve)->desc,
			gl_shader_color_curve_to_string(req->fb_store_curve)->desc);
	if (size < 0)
		return NULL;
	return str;
}

static char *
create_vertex_shader_config_string(const struct gl_shader_requirements *req)
{
	int size;
	char *str;

	size = asprintf(&str,
			"#define DEF_TEXCOORD_INPUT %s\n"
			"#define DEF_WIREFRAME %s\n",
			gl_shader_texcoord_input_to_string(req->texcoord_input)->symbol,
			req->wireframe ? "true" : "false");

	if (size < 0)
		return NULL;
	return str;
}

static char *
create_fragment_shader_config_string(const struct gl_shader_requirements *req)
{
	int size;
	char *str;

	size = asprintf(&str,
			"#define MAX_CURVE_PARAMS %zu\n"
			"#define DEF_TINT %s\n"
			"#define DEF_INPUT_IS_PREMULT %s\n"
			"#define DEF_WIREFRAME %s\n"
			"#define DEF_COLOR_PRE_CURVE %s\n"
			"#define DEF_COLOR_MAPPING %s\n"
			"#define DEF_COLOR_POST_CURVE %s\n"
			"#define DEF_SHADER_BLENDING %s\n"
			"#define DEF_FB_FETCH_CURVE %s\n"
			"#define DEF_FB_STORE_CURVE %s\n"
			"#define DEF_COLOR_EFFECT %s\n"
			"#define DEF_VARIANT %s\n",
			ARRAY_LENGTH(((union weston_color_curve_parametric_chan_data){}).data),
			req->tint ? "true" : "false",
			req->input_is_premult ? "true" : "false",
			req->wireframe ? "true" : "false",
			gl_shader_color_curve_to_string(req->color_pre_curve)->symbol,
			gl_shader_color_mapping_to_string(req->color_mapping)->symbol,
			gl_shader_color_curve_to_string(req->color_post_curve)->symbol,
			req->shader_blending ? "1" : "0",
			gl_shader_color_curve_to_string(req->fb_fetch_curve)->symbol,
			gl_shader_color_curve_to_string(req->fb_store_curve)->symbol,
			gl_shader_color_effect_to_string(req->color_effect)->symbol,
			gl_shader_texture_variant_to_string(req->variant)->symbol);
	if (size < 0)
		return NULL;
	return str;
}

static GLint
get_uniform_location(struct gl_renderer *gr,
		     GLuint program,
		     const char *prefix,
		     const char *field)
{
	char str[128];
	int ret;

	ret = snprintf(str, sizeof str, "%s_%s", prefix, field);
	weston_assert_u32_lt(gr->compositor, ret, sizeof str);

	return glGetUniformLocation(program, str);
}

static void
get_curve_uniform_locations(struct gl_renderer *gr,
			    union gl_shader_color_curve_uniforms *out,
			    enum gl_shader_color_curve type,
			    GLuint program,
			    const char *namespace)
{
	switch (type) {
	case SHADER_COLOR_CURVE_IDENTITY:
	case SHADER_COLOR_CURVE_PQ:
	case SHADER_COLOR_CURVE_PQ_INVERSE:
		return;
	case SHADER_COLOR_CURVE_LINPOW:
	case SHADER_COLOR_CURVE_POWLIN:
		out->parametric.params_uniform =
			get_uniform_location(gr, program, namespace, "par.params");
		out->parametric.clamped_input_uniform =
			get_uniform_location(gr, program, namespace, "par.clamped_input");
		return;
	case SHADER_COLOR_CURVE_LUT_3x1D:
		out->lut_3x1d.tex_2d_uniform =
			get_uniform_location(gr, program, namespace, "lut.lut_2d");
		out->lut_3x1d.scale_offset_uniform =
			get_uniform_location(gr, program, namespace, "lut.scale_offset");
		return;
	}
}

static struct gl_shader *
gl_shader_create(struct gl_renderer *gr,
		 const struct gl_shader_requirements *requirements)
{
	bool verbose = weston_log_scope_is_enabled(gr->shader_scope);
	struct gl_shader *shader = NULL;
	char buffer[512];
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

	if (verbose) {
		char *desc;

		desc = create_shader_description_string(requirements);
		weston_log_scope_printf(gr->shader_scope,
					"Compiling shader program for: %s\n",
					desc);
		free(desc);
	}

	conf = create_vertex_shader_config_string(&shader->key);
	if (!conf)
		goto error_vertex;

	sources[0] = conf;
	sources[1] = vertex_shader;
	shader->vertex_shader = compile_shader(GL_VERTEX_SHADER, 2, sources);
	if (shader->vertex_shader == GL_NONE)
		goto error_vertex;

	free(conf);
	conf = create_fragment_shader_config_string(&shader->key);
	if (!conf)
		goto error_fragment;

	sprintf(buffer,
		"#version 100\n"
		"#define GLES_API_MAJOR_VERSION %d\n",
		gr->gl_version >= gl_version(3, 0) ? 3 : 2);

	sources[0] = buffer;
	sources[1] = conf;
	sources[2] = fragment_shader;
	shader->fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
						 3, sources);
	if (shader->fragment_shader == GL_NONE)
		goto error_fragment;

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vertex_shader);
	glAttachShader(shader->program, shader->fragment_shader);

	glBindAttribLocation(shader->program, SHADER_ATTRIB_LOC_POSITION,
			     "position");
	if (requirements->texcoord_input == SHADER_TEXCOORD_INPUT_ATTRIB)
		glBindAttribLocation(shader->program,
				     SHADER_ATTRIB_LOC_TEXCOORD, "texcoord");
	if (requirements->wireframe)
		glBindAttribLocation(shader->program,
				     SHADER_ATTRIB_LOC_BARYCENTRIC,
				     "barycentric");

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(shader->program, sizeof buffer, NULL,
				    buffer);
		weston_log("link info: %s\n", buffer);
		goto error_link;
	}

	glDeleteShader(shader->vertex_shader);
	glDeleteShader(shader->fragment_shader);

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->surface_to_buffer_uniform =
		glGetUniformLocation(shader->program, "surface_to_buffer");
	shader->tex_uniforms[0] = glGetUniformLocation(shader->program, "tex");
	shader->tex_uniforms[1] = glGetUniformLocation(shader->program, "tex1");
	shader->tex_uniforms[2] = glGetUniformLocation(shader->program, "tex2");
	if (requirements->wireframe)
		shader->tex_uniform_wireframe =
			glGetUniformLocation(shader->program, "tex_wireframe");
	if (gr->gl_version < gl_version(3, 0)) {
		shader->swizzle_idx[0] = glGetUniformLocation(shader->program, "swizzle_idx[0]");
		shader->swizzle_idx[1] = glGetUniformLocation(shader->program, "swizzle_idx[1]");
		shader->swizzle_idx[2] = glGetUniformLocation(shader->program, "swizzle_idx[2]");
		shader->swizzle_mask[0] = glGetUniformLocation(shader->program, "swizzle_mask[0]");
		shader->swizzle_mask[1] = glGetUniformLocation(shader->program, "swizzle_mask[1]");
		shader->swizzle_mask[2] = glGetUniformLocation(shader->program, "swizzle_mask[2]");
		shader->swizzle_sub[0] = glGetUniformLocation(shader->program, "swizzle_sub[0]");
		shader->swizzle_sub[1] = glGetUniformLocation(shader->program, "swizzle_sub[1]");
		shader->swizzle_sub[2] = glGetUniformLocation(shader->program, "swizzle_sub[2]");
	}
	shader->view_alpha_uniform = glGetUniformLocation(shader->program, "view_alpha");
	if (requirements->variant == SHADER_VARIANT_SOLID) {
		shader->color_uniform = glGetUniformLocation(shader->program,
							     "unicolor");
		assert(shader->color_uniform != -1);
	} else {
		shader->color_uniform = -1;
	}
	if (requirements->tint) {
		shader->tint_uniform = glGetUniformLocation(shader->program,
							    "tint");
		assert(shader->tint_uniform != -1);
	} else {
		shader->tint_uniform = -1;
	}

	if (requirements->color_effect == SHADER_COLOR_EFFECT_CVD_CORRECTION) {
		shader->cvd_correction_uniform =
			glGetUniformLocation(shader->program, "cvd_correction_matrix");
	} else {
		shader->cvd_correction_uniform = -1;
	}

	get_curve_uniform_locations(gr, &shader->color_pre_curve,
				    requirements->color_pre_curve,
				    shader->program, "color_pre_curve");
	get_curve_uniform_locations(gr, &shader->color_post_curve,
				    requirements->color_post_curve,
				    shader->program, "color_post_curve");

	switch(requirements->color_mapping) {
	case SHADER_COLOR_MAPPING_3DLUT:
		shader->color_mapping.lut3d.tex_uniform =
			glGetUniformLocation(shader->program,
					     "color_mapping_lut_3d");
		shader->color_mapping.lut3d.scale_offset_uniform =
			glGetUniformLocation(shader->program,
					     "color_mapping_lut_scale_offset");
		break;
	case SHADER_COLOR_MAPPING_MATRIX:
		shader->color_mapping.mat.matrix_uniform =
			glGetUniformLocation(shader->program,
					     "color_mapping_matrix");
		shader->color_mapping.mat.offset_uniform =
			glGetUniformLocation(shader->program,
					     "color_mapping_offset");
		break;
	case SHADER_COLOR_MAPPING_IDENTITY:
		break;
	}

	shader->yuv_coefficients_uniform =
		glGetUniformLocation(shader->program, "yuv_coefficients");
	shader->yuv_offsets_uniform = glGetUniformLocation(shader->program,
							   "yuv_offsets");

	get_curve_uniform_locations(gr, &shader->fb_fetch_curve,
				    requirements->fb_fetch_curve,
				    shader->program, "fb_fetch_curve");
	get_curve_uniform_locations(gr, &shader->fb_store_curve,
				    requirements->fb_store_curve,
				    shader->program, "fb_store_curve");

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

void
gl_shader_destroy(struct gl_renderer *gr, struct gl_shader *shader)
{
	char *desc;

	if (weston_log_scope_is_enabled(gr->shader_scope)) {
		desc = create_shader_description_string(&shader->key);
		weston_log_scope_printf(gr->shader_scope,
					"Deleting shader program for: %s\n",
					desc);
		free(desc);
	}

	glDeleteProgram(shader->program);
	wl_list_remove(&shader->link);
	free(shader);
}

void
gl_renderer_shader_list_destroy(struct gl_renderer *gr)
{
	struct gl_shader *shader, *next_shader;

	wl_list_for_each_safe(shader, next_shader, &gr->shader_list, link)
		gl_shader_destroy(gr, shader);
}

static int
gl_shader_requirements_cmp(const struct gl_shader_requirements *a,
			   const struct gl_shader_requirements *b)
{
	return memcmp(a, b, sizeof(*a));
}

static void
gl_shader_scope_new_subscription(struct weston_log_subscription *subs,
				 void *data)
{
	static const char bar[] = "-----------------------------------------------------------------------------";
	struct gl_renderer *gr = data;
	struct gl_shader *shader;
	struct timespec now;
	int msecs;
	int count = 0;
	char *desc;

	if (!wl_list_empty(&gr->shader_list))
		weston_compositor_read_presentation_clock(gr->compositor, &now);

	weston_log_subscription_printf(subs,
				       "Vertex shader body:\n"
				       "%s\n%s\n"
				       "Fragment shader body:\n"
				       "%s\n%s\n%s\n",
				       bar, vertex_shader,
				       bar, fragment_shader, bar);

	weston_log_subscription_printf(subs,
		"Cached GLSL programs:\n    id: (used secs ago) description +/-flags\n");
	wl_list_for_each(shader, &gr->shader_list, link) {
		count++;
		msecs = timespec_sub_to_msec(&now, &shader->last_used);
		desc = create_shader_description_string(&shader->key);
		weston_log_subscription_printf(subs,
					       "%6u: (%.1f) %s\n",
					       shader->program,
					       msecs / 1000.0, desc);
	}
	weston_log_subscription_printf(subs, "Total: %d programs.\n", count);
}

struct weston_log_scope *
gl_shader_scope_create(struct gl_renderer *gr)
{

	return weston_compositor_add_log_scope(gr->compositor,
		"gl-shader-generator",
		"GL renderer shader compilation and cache.\n",
		gl_shader_scope_new_subscription,
		NULL,
		gr);
}

struct gl_shader *
gl_renderer_create_fallback_shader(struct gl_renderer *gr)
{
	static const struct gl_shader_requirements fallback_requirements = {
		.variant = SHADER_VARIANT_SOLID,
		.input_is_premult = true,
		.color_pre_curve = SHADER_COLOR_CURVE_IDENTITY,
		.color_mapping = SHADER_COLOR_MAPPING_IDENTITY,
		.color_post_curve = SHADER_COLOR_CURVE_IDENTITY,
	};
	struct gl_shader *shader;

	shader = gl_shader_create(gr, &fallback_requirements);
	if (!shader)
		return NULL;

	/*
	 * This shader must be exempt from any automatic garbage collection.
	 * It is destroyed explicitly.
	 */
	wl_list_remove(&shader->link);
	wl_list_init(&shader->link);

	return shader;
}

static struct gl_shader *
gl_renderer_get_program(struct gl_renderer *gr,
			const struct gl_shader_requirements *requirements)
{
	struct gl_shader_requirements reqs = *requirements;
	struct gl_shader *shader;

	assert(reqs.pad_bits_ == 0);

	if (gr->current_shader &&
	    gl_shader_requirements_cmp(&reqs, &gr->current_shader->key) == 0)
		return gr->current_shader;

	wl_list_for_each(shader, &gr->shader_list, link) {
		if (gl_shader_requirements_cmp(&reqs, &shader->key) == 0)
			return shader;
	}

	shader = gl_shader_create(gr, &reqs);
	if (shader)
		return shader;

	return NULL;
}

void
gl_renderer_garbage_collect_programs(struct gl_renderer *gr)
{
	struct gl_shader *shader, *tmp;
	unsigned count = 0;

	wl_list_for_each_safe(shader, tmp, &gr->shader_list, link) {
		/* Keep the 10 most recently used always. */
		if (count++ < 10)
			continue;

		/* Keep everything used in the past 1 minute. */
		if (timespec_sub_to_msec(&gr->compositor->last_repaint_start,
					 &shader->last_used) < 60000)
			continue;

		/* The rest throw away. */
		gl_shader_destroy(gr, shader);
	}
}

static void
gl_shader_load_config_curve(struct weston_compositor *compositor,
			    enum gl_shader_color_curve type,
			    const union gl_shader_config_color_curve *sconf,
			    const union gl_shader_color_curve_uniforms *unif,
			    enum gl_tex_unit tex_unit)
{
	GLsizei n_params;

	switch (type) {
	case SHADER_COLOR_CURVE_IDENTITY:
	case SHADER_COLOR_CURVE_PQ:
	case SHADER_COLOR_CURVE_PQ_INVERSE:
		return;
	case SHADER_COLOR_CURVE_LUT_3x1D:
		assert(unif->lut_3x1d.tex_2d_uniform != -1);
		assert(unif->lut_3x1d.scale_offset_uniform != -1);
		assert(sconf->lut_3x1d.tex != 0);
		assert(sconf->lut_3x1d.scale > 0.0);
		assert(sconf->lut_3x1d.offset > 0.0);

		glActiveTexture(GL_TEXTURE0 + tex_unit);
		glBindTexture(GL_TEXTURE_2D, sconf->lut_3x1d.tex);
		glUniform1i(unif->lut_3x1d.tex_2d_uniform, tex_unit);
		glUniform2f(unif->lut_3x1d.scale_offset_uniform,
			    sconf->lut_3x1d.scale, sconf->lut_3x1d.offset);
		return;
	case SHADER_COLOR_CURVE_LINPOW:
	case SHADER_COLOR_CURVE_POWLIN:
		n_params = ARRAY_LENGTH(sconf->parametric.params.array);
		glUniform1fv(unif->parametric.params_uniform, n_params,
			     sconf->parametric.params.array);
		glUniform1i(unif->parametric.clamped_input_uniform,
			    sconf->parametric.clamped_input);
		return;
	}

	weston_assert_not_reached(compositor, "unknown enum gl_shader_color_curve value");
}

static void
gl_shader_load_config_mapping(struct weston_compositor *compositor,
			      enum gl_shader_color_mapping mapping_type,
			      const union gl_shader_config_color_mapping *sconf,
			      const union gl_shader_color_mapping_uniforms *unif)
{
	switch (mapping_type) {
	case SHADER_COLOR_MAPPING_IDENTITY:
		return;
	case SHADER_COLOR_MAPPING_3DLUT:
		assert(unif->lut3d.tex_uniform != -1);
		assert(unif->lut3d.scale_offset_uniform != -1);
		assert(sconf->lut3d.tex3d != 0);
		assert(sconf->lut3d.scale > 0.0);
		assert(sconf->lut3d.offset > 0.0);

		glActiveTexture(GL_TEXTURE0 + TEX_UNIT_COLOR_MAPPING);
		glBindTexture(GL_TEXTURE_3D, sconf->lut3d.tex3d);
		glUniform1i(unif->lut3d.tex_uniform, TEX_UNIT_COLOR_MAPPING);
		glUniform2f(unif->lut3d.scale_offset_uniform,
			    sconf->lut3d.scale, sconf->lut3d.offset);
		return;
	case SHADER_COLOR_MAPPING_MATRIX:
		assert(unif->mat.matrix_uniform != -1);
		assert(unif->mat.offset_uniform != -1);

		glUniformMatrix3fv(unif->mat.matrix_uniform,
				   1, GL_FALSE, sconf->mat.matrix.colmaj);
		glUniform3fv(unif->mat.offset_uniform, 1, sconf->mat.offset.el);
		return;
	}

	weston_assert_not_reached(compositor, "unknown enum gl_shader_color_mapping value");
}

static void
gl_shader_load_config_representation(struct weston_compositor *compositor,
				     struct gl_shader *shader,
				     const struct gl_shader_config *sconf)
{
	struct weston_color_representation_matrix cr_matrix;

	if (sconf->yuv_coefficients == WESTON_COLOR_MATRIX_COEF_UNSET ||
	    sconf->yuv_coefficients == WESTON_COLOR_MATRIX_COEF_IDENTITY) {
		/*
		 * In this case the yuv_coefficients and yuv_offsets uniforms
		 * should never be used and thus get optimized away by the
		 * shader compiler. Unfortunately on some drivers this is not
		 * the case, so we can't assert on it.
		 */
		return;
	}

	if (shader->yuv_coefficients_uniform == -1 ||
	    shader->yuv_offsets_uniform == -1) {
		assert(shader->key.variant == SHADER_VARIANT_EXTERNAL);
		return;
	}

	weston_get_color_representation_matrix(compositor,
		sconf->yuv_coefficients, sconf->yuv_range, &cr_matrix);

	glUniformMatrix3fv(shader->yuv_coefficients_uniform, 1, GL_FALSE,
			   cr_matrix.matrix.colmaj);
	glUniform3fv(shader->yuv_offsets_uniform, 1, cr_matrix.offset.el);
}

bool
gl_shader_texture_variant_can_be_premult(enum gl_shader_texture_variant v)
{
	switch (v) {
	case SHADER_VARIANT_SOLID:
	case SHADER_VARIANT_RGBA:
	case SHADER_VARIANT_EXTERNAL:
		return true;
	case SHADER_VARIANT_NONE:
	case SHADER_VARIANT_Y_U_V:
	case SHADER_VARIANT_Y_UV:
	case SHADER_VARIANT_XYUV:
		return false;
	}
	return true;
}

GLenum
gl_shader_texture_variant_get_target(enum gl_shader_texture_variant v)
{
	if (v == SHADER_VARIANT_EXTERNAL)
		return GL_TEXTURE_EXTERNAL_OES;
	else
		return GL_TEXTURE_2D;
}

static void
gl_shader_load_config(struct gl_renderer *gr,
		      struct gl_shader *shader,
		      const struct gl_shader_config *sconf)
{
	GLint *swizzles;
	int swizzle_idx[4];
	float swizzle_mask[4];
	float swizzle_sub[4];
	int i, j;

	glUniformMatrix4fv(shader->proj_uniform,
			   1, GL_FALSE, sconf->projection.M.colmaj);

	if (shader->surface_to_buffer_uniform != -1)
		glUniformMatrix4fv(shader->surface_to_buffer_uniform,
			           1, GL_FALSE, sconf->surface_to_buffer.M.colmaj);

	if (shader->color_uniform != -1) {
		weston_log_scope_printf(gr->paint_node_scope,
			"\t\tcolor: r: %.2f, g: %.2f, b: %.2f, a: %.2f\n",
			sconf->unicolor[0], sconf->unicolor[1],
			sconf->unicolor[2], sconf->unicolor[3]);
		glUniform4fv(shader->color_uniform, 1, sconf->unicolor);
	}
	if (shader->tint_uniform != -1) {
		weston_log_scope_printf(gr->paint_node_scope,
				"\t\ttint: r: %.2f, g: %.2f, b: %.2f, a: %.2f\n",
				sconf->tint[0], sconf->tint[1],
				sconf->tint[2], sconf->tint[3]);
		glUniform4fv(shader->tint_uniform, 1, sconf->tint);
	}

	weston_log_scope_printf(gr->paint_node_scope, "\t\talpha: %.2f\n", sconf->view_alpha);
	glUniform1f(shader->view_alpha_uniform, sconf->view_alpha);

	assert(sconf->input_num <= SHADER_INPUT_TEX_MAX);
	for (i = 0; i < sconf->input_num; i++) {
		assert(shader->tex_uniforms[i] != -1);

		/* If the OpenGL ES implementation lacks swizzles as texture
		 * parameters (OpenGL ES 2), the fragment shader loads swizzling
		 * info from uniforms. */
		if (gr->gl_version < gl_version(3, 0)) {
			swizzles = sconf->input_param[i].swizzles.array;
			for (j = 0; j < 4; j++) {
				swizzle_idx[j] = swizzles[j] - GL_RED;
				if (swizzle_idx[j] >= 0) {
					/* Swizzle is GL_RED, GL_GREEN, GL_BLUE
					 * or GL_ALPHA. */
					swizzle_mask[j] = 1.0f;
					swizzle_sub[j] = 0.0f;
				} else {
					/* Swizzle is GL_ZERO (0) or GL_ONE
					 * (1). */
					swizzle_idx[j] = 0;
					swizzle_mask[j] = 0.0f;
					swizzle_sub[j] = (float) swizzles[j];
				}
			}
			glUniform4iv(shader->swizzle_idx[i], 1, swizzle_idx);
			glUniform4fv(shader->swizzle_mask[i], 1, swizzle_mask);
			glUniform4fv(shader->swizzle_sub[i], 1, swizzle_sub);
		}

		glUniform1i(shader->tex_uniforms[i], TEX_UNIT_IMAGES + i);
		glActiveTexture(GL_TEXTURE0 + TEX_UNIT_IMAGES + i);
		glBindTexture(sconf->input_param[i].target,
			      sconf->input_tex[i]);
		if (sconf->input_tex[i])
			gl_texture_parameters_flush(gr, &sconf->input_param[i]);
	}

	switch (sconf->req.color_effect) {
	case SHADER_COLOR_EFFECT_NONE:
		break;
	case SHADER_COLOR_EFFECT_INVERSION:
		weston_log_scope_printf(gr->paint_node_scope, "\t\tcolor effect: inversion\n");
		break;
	case SHADER_COLOR_EFFECT_GRAYSCALE:
		weston_log_scope_printf(gr->paint_node_scope, "\t\tcolor effect: grayscale\n");
		break;
	case SHADER_COLOR_EFFECT_CVD_CORRECTION:
		weston_assert_int_ne(gr->compositor, shader->cvd_correction_uniform, -1);
		weston_log_scope_printf(gr->paint_node_scope, "\t\tcolor effect: cvd - %s\n",
					weston_output_cvd_type_to_str(sconf->color_effect.cvd));
		glUniformMatrix3fv(shader->cvd_correction_uniform,
				   1, GL_FALSE,
				   sconf->color_effect.cvd.correction.colmaj);
		break;
	}

	/* Fixed texture unit for color_pre_curve LUT if it is available */
	gl_shader_load_config_curve(gr->compositor, sconf->req.color_pre_curve,
				    &sconf->color_pre_curve, &shader->color_pre_curve,
				    TEX_UNIT_COLOR_PRE_CURVE);
	gl_shader_load_config_mapping(gr->compositor, sconf->req.color_mapping,
				      &sconf->color_mapping, &shader->color_mapping);
	gl_shader_load_config_curve(gr->compositor, sconf->req.color_post_curve,
				    &sconf->color_post_curve, &shader->color_post_curve,
				    TEX_UNIT_COLOR_POST_CURVE);

	gl_shader_load_config_representation(gr->compositor, shader, sconf);

	if (sconf->req.wireframe)
		glUniform1i(shader->tex_uniform_wireframe, TEX_UNIT_WIREFRAME);

	if (sconf->req.shader_blending) {
		gl_shader_load_config_curve(gr->compositor, sconf->req.fb_fetch_curve,
					    &sconf->fb_fetch_curve, &shader->fb_fetch_curve,
					    TEX_UNIT_FB_FETCH_CURVE);
		gl_shader_load_config_curve(gr->compositor, sconf->req.fb_store_curve,
					    &sconf->fb_store_curve, &shader->fb_store_curve,
					    TEX_UNIT_FB_STORE_CURVE);
	}

	glActiveTexture(GL_TEXTURE0);
}

bool
gl_renderer_use_program(struct gl_renderer *gr,
			const struct gl_shader_config *sconf)
{
	static const GLfloat fallback_shader_color[4] = { 0.2, 0.1, 0.0, 1.0 };
	struct gl_shader *shader;

	shader = gl_renderer_get_program(gr, &sconf->req);
	if (!shader) {
		weston_log("Error: failed to generate shader program.\n");
		gr->current_shader = NULL;

		if (!gr->fallback_shader) {
			gr->fallback_shader =
				gl_renderer_create_fallback_shader(gr);
			if (!gr->fallback_shader) {
				weston_log("Error: compiling fallback shader failed.\n");
				return false;
			}
		}

		/*
		 * We only have one fallback shader, so it cannot do correct
		 * color on color managed outputs. Hence, what is painted
		 * with this one will have undefined look. Therefore the
		 * fallback is important to not be too bright as that might
		 * be shocking on a monitor in HDR mode.
		 */

		shader = gr->fallback_shader;
		glUseProgram(shader->program);
		glUniform4fv(shader->color_uniform, 1, fallback_shader_color);
		glUniform1f(shader->view_alpha_uniform, 1.0f);
		weston_log_scope_printf(gr->paint_node_scope, "\t\tFailed to generate shader program. "
					"Using the fallback shader\n");
		return false;
	}

	if (shader != gr->fallback_shader) {
		/* Update list order for most recently used. */
		wl_list_remove(&shader->link);
		wl_list_insert(&gr->shader_list, &shader->link);
	}
	shader->last_used = gr->compositor->last_repaint_start;

	if (gr->current_shader != shader) {
		glUseProgram(shader->program);
		gr->current_shader = shader;
	}

	weston_log_scope_printf(gr->paint_node_scope,
				"\t\t\tshader id: %d\n", gr->current_shader->program);
	gl_shader_load_config(gr, shader, sconf);

	return true;
}
