/*
 * Copyright 2021,2026 Collabora, Ltd.
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

#include <assert.h>
#include <string.h>

#include <libweston/libweston.h>
#include "color.h"
#include "color-operations.h"
#include "color-properties.h"
#include "gl-renderer.h"
#include "gl-renderer-internal.h"

#include "shared/xalloc.h"
#include "shared/weston-assert.h"
#include "shared/weston-egl-ext.h"

struct gl_renderer_color_curve {
	enum gl_shader_color_curve type;
	union gl_shader_config_color_curve u;
};

struct gl_renderer_color_mapping {
	enum gl_shader_color_mapping type;
	union gl_shader_config_color_mapping u;
};

struct gl_renderer_color_transform {
	struct weston_color_transform *owner;
	struct wl_listener destroy_listener;
	struct gl_renderer_color_curve pre_curve;
	struct gl_renderer_color_mapping mapping;
	struct gl_renderer_color_curve post_curve;
};

struct gl_renderer_color_effect {
	struct weston_output_color_effect *owner;
	struct wl_listener destroy_listener;
	enum gl_shader_color_effect type;
	union gl_shader_config_color_effect u;
};

/** for in-shader blending */
struct gl_shader_blender {
	struct gl_renderer_color_curve fb_fetch_curve;
	struct gl_renderer_color_curve fb_store_curve;
};

static void
gl_renderer_color_curve_fini(struct gl_renderer_color_curve *gl_curve)
{
	switch (gl_curve->type) {
	case SHADER_COLOR_CURVE_IDENTITY:
	case SHADER_COLOR_CURVE_PQ:
	case SHADER_COLOR_CURVE_PQ_INVERSE:
	case SHADER_COLOR_CURVE_LINPOW:
	case SHADER_COLOR_CURVE_POWLIN:
		break;
	case SHADER_COLOR_CURVE_LUT_3x1D:
		gl_texture_fini(&gl_curve->u.lut_3x1d.tex);
		break;
	};
}

static void
gl_renderer_color_mapping_fini(struct gl_renderer_color_mapping *gl_mapping)
{
	if (gl_mapping->type == SHADER_COLOR_MAPPING_3DLUT &&
	    gl_mapping->u.lut3d.tex3d)
		gl_texture_fini(&gl_mapping->u.lut3d.tex3d);
}

static void
gl_renderer_color_transform_destroy(struct gl_renderer_color_transform *gl_xform)
{
	gl_renderer_color_curve_fini(&gl_xform->pre_curve);
	gl_renderer_color_curve_fini(&gl_xform->post_curve);
	gl_renderer_color_mapping_fini(&gl_xform->mapping);
	wl_list_remove(&gl_xform->destroy_listener.link);
	free(gl_xform);
}

static void
color_transform_destroy_handler(struct wl_listener *l, void *data)
{
	struct gl_renderer_color_transform *gl_xform;

	gl_xform = wl_container_of(l, gl_xform, destroy_listener);
	assert(gl_xform->owner == data);

	gl_renderer_color_transform_destroy(gl_xform);
}

static struct gl_renderer_color_transform *
gl_renderer_color_transform_create(struct weston_color_transform *xform)
{
	struct gl_renderer_color_transform *gl_xform;

	gl_xform = zalloc(sizeof *gl_xform);
	if (!gl_xform)
		return NULL;

	gl_xform->owner = xform;
	gl_xform->destroy_listener.notify = color_transform_destroy_handler;
	wl_signal_add(&xform->destroy_signal, &gl_xform->destroy_listener);

	return gl_xform;
}

static struct gl_renderer_color_transform *
gl_renderer_color_transform_get(struct weston_color_transform *xform)
{
	struct wl_listener *l;

	l = wl_signal_get(&xform->destroy_signal,
			  color_transform_destroy_handler);
	if (!l)
		return NULL;

	return container_of(l, struct gl_renderer_color_transform,
			    destroy_listener);
}

static bool
gl_color_curve_parametric(struct gl_renderer *gr,
			  struct gl_renderer_color_curve *gl_curve,
			  const struct weston_color_curve *curve)
{
	const struct weston_color_curve_parametric *parametric = &curve->u.parametric;

	gl_curve->u.parametric.params = parametric->params;
	gl_curve->u.parametric.clamped_input = parametric->clamped_input;

	switch(parametric->type) {
	case WESTON_COLOR_CURVE_PARAMETRIC_TYPE_LINPOW:
		gl_curve->type = SHADER_COLOR_CURVE_LINPOW;
		return true;
	case WESTON_COLOR_CURVE_PARAMETRIC_TYPE_POWLIN:
		gl_curve->type = SHADER_COLOR_CURVE_POWLIN;
		return true;
	}

	weston_assert_not_reached(gr->compositor, "unknown parametric color curve");
}

static bool
gl_color_curve_enum(struct gl_renderer *gr,
		    struct gl_renderer_color_curve *gl_curve,
		    const struct weston_color_curve *curve)
{
	struct weston_color_curve_parametric parametric;
	bool ret;

	/**
	 * Handle enum curve (if TF is implemented) or fallback to a parametric
	 * curve.
	 */
	switch (curve->u.enumerated.tf.info->tf) {
	case WESTON_TF_ST2084_PQ:
		gl_curve->type = (curve->u.enumerated.tf_direction == WESTON_FORWARD_TF) ?
				 SHADER_COLOR_CURVE_PQ : SHADER_COLOR_CURVE_PQ_INVERSE;
		return true;
	default:
		ret = weston_color_curve_enum_get_parametric(gr->compositor,
							     &curve->u.enumerated,
							     &parametric);
		if (!ret)
			return false;
		break;
	}

	/* Handle parametric curve that we got from TF. */

	gl_curve->u.parametric.params = parametric.params;
	gl_curve->u.parametric.clamped_input = parametric.clamped_input;

	switch(parametric.type) {
	case WESTON_COLOR_CURVE_PARAMETRIC_TYPE_LINPOW:
		gl_curve->type = SHADER_COLOR_CURVE_LINPOW;
		return true;
	case WESTON_COLOR_CURVE_PARAMETRIC_TYPE_POWLIN:
		gl_curve->type = SHADER_COLOR_CURVE_POWLIN;
		return true;
	}

	weston_assert_not_reached(gr->compositor, "unknown parametric color curve");
}

static bool
gl_color_curve_lut_3x1d_init(struct gl_renderer *gr,
			     struct gl_renderer_color_curve *gl_curve,
			     uint32_t lut_len, float *lut)
{
	GLint filters[] = { GL_LINEAR, GL_LINEAR };
	const unsigned nr_rows = 4;
	struct gl_texture_parameters params;
	GLuint tex;

	/**
	 * Four rows, see fragment.glsl sample_lut_1d(). The fourth row is
	 * unused.
	 */
	gl_texture_2d_init(gr, 1, GL_R32F, lut_len, nr_rows, &tex);

	/**
	 * lut is a linearized 3x1D LUT, it must occupy the first 3 rows of the
	 * 4-row lut.
	 */
	gl_texture_2d_store(gr, 0, 0, 0, lut_len, 3, GL_RED, GL_FLOAT,
			    lut);

	gl_texture_parameters_init(gr, &params, GL_TEXTURE_2D, filters, NULL,
				   NULL, true);

	glBindTexture(GL_TEXTURE_2D, 0);
	gl_curve->type = SHADER_COLOR_CURVE_LUT_3x1D;
	gl_curve->u.lut_3x1d.tex = tex;
	gl_curve->u.lut_3x1d.scale = (float)(lut_len - 1) / lut_len;
	gl_curve->u.lut_3x1d.offset = 0.5f / lut_len;

	return true;
}

static bool
gl_color_curve_lut_3x1d(struct gl_renderer *gr,
			struct gl_renderer_color_curve *gl_curve,
			const struct weston_color_curve *curve,
			struct weston_color_transform *xform)
{
	const size_t lut_len = curve->u.lut_3x1d.optimal_len;
	struct weston_vec3f *tmp;
	float *lut;
	bool ret;

	tmp = calloc(lut_len, sizeof *tmp);
	lut = calloc(lut_len, 3 * sizeof *lut);
	if (!tmp || !lut) {
		ret = false;
		goto out;
	}

	curve->u.lut_3x1d.fill_in(xform, tmp, lut_len);
	weston_v3f_array_to_planar(lut, tmp, lut_len);

	ret = gl_color_curve_lut_3x1d_init(gr, gl_curve, lut_len, lut);

out:
	free(lut);
	free(tmp);

	return ret;
}

static bool
gl_color_curve_init(struct gl_renderer *gr,
		    struct gl_renderer_color_curve *gl_curve,
		    const struct weston_color_curve *curve,
		    struct weston_color_transform *xform)
{
	switch (curve->type) {
	case WESTON_COLOR_CURVE_TYPE_IDENTITY:
		*gl_curve = (struct gl_renderer_color_curve){
			.type = SHADER_COLOR_CURVE_IDENTITY,
		};
		return true;
	case WESTON_COLOR_CURVE_TYPE_LUT_3x1D:
		weston_assert_ptr_not_null(gr->compositor, xform);
		return gl_color_curve_lut_3x1d(gr, gl_curve, curve, xform);
	case WESTON_COLOR_CURVE_TYPE_PARAMETRIC:
		return gl_color_curve_parametric(gr, gl_curve, curve);
	case WESTON_COLOR_CURVE_TYPE_ENUM:
		return gl_color_curve_enum(gr, gl_curve, curve);
	}

	weston_assert_not_reached(gr->compositor, "invalid weston_color_curve_type");
}

static void
gl_color_mapping_init(struct gl_renderer *gr,
		      struct gl_renderer_color_mapping *gl_mapping,
		      const struct weston_color_mapping *mapping)
{
	switch (mapping->type) {
	case WESTON_COLOR_MAPPING_TYPE_IDENTITY:
		*gl_mapping = (struct gl_renderer_color_mapping){
			.type = SHADER_COLOR_MAPPING_IDENTITY,
		};
		return;
	case WESTON_COLOR_MAPPING_TYPE_MATRIX:
		gl_mapping->type = SHADER_COLOR_MAPPING_MATRIX;
		gl_mapping->u.mat = mapping->u.mat;
		return;
	}

	weston_assert_not_reached(gr->compositor, "invalid weston_color_mapping_type");
}

static void
gl_color_mapping_lut_3d_init(struct gl_renderer *gr,
			     struct gl_renderer_color_mapping *gl_mapping,
			     uint32_t dim_size, float *lut)
{
	GLint filters[] = { GL_LINEAR, GL_LINEAR };
	struct gl_texture_parameters params;
	GLuint tex3d;

	gl_texture_3d_init(gr, 1, GL_RGB32F, dim_size, dim_size, dim_size,
			   &tex3d);
	gl_texture_3d_store(gr, 0, 0, 0, 0, dim_size, dim_size, dim_size,
			    GL_RGB, GL_FLOAT, lut);
	gl_texture_parameters_init(gr, &params, GL_TEXTURE_3D, filters, NULL,
				   NULL, true);

	glBindTexture(GL_TEXTURE_3D, 0);
	gl_mapping->type = SHADER_COLOR_MAPPING_3DLUT;
	gl_mapping->u.lut3d.tex3d = tex3d;
	gl_mapping->u.lut3d.scale = (float)(dim_size - 1) / dim_size;
	gl_mapping->u.lut3d.offset = 0.5f / dim_size;
}

static const struct gl_renderer_color_transform *
gl_renderer_color_transform_create_steps(struct gl_renderer *gr,
					 struct weston_color_transform *xform)
{
	struct gl_renderer_color_transform *gl_xform;

	gl_xform = gl_renderer_color_transform_create(xform);
	if (!gl_xform)
		return NULL;

	if (!gl_color_curve_init(gr, &gl_xform->pre_curve,
				 &xform->pre_curve, xform)) {
		gl_renderer_color_transform_destroy(gl_xform);
		return NULL;
	}

	gl_color_mapping_init(gr, &gl_xform->mapping, &xform->mapping);

	if (!gl_color_curve_init(gr, &gl_xform->post_curve,
				 &xform->post_curve, xform)) {
		gl_renderer_color_transform_destroy(gl_xform);
		return NULL;
	}

	return gl_xform;
}

static const struct gl_renderer_color_transform *
gl_renderer_color_transform_create_3dlut(struct gl_renderer *gr,
					 struct weston_color_transform *xform)
{
	struct gl_renderer_color_transform *gl_xform = NULL;
	float *shaper = NULL;
	float *lut3d = NULL;
	float len_shaper;
	float len_lut3d;
	bool ok;

#ifndef HAVE_GLES3
	goto err;
#endif

	/**
	 * These are values that allow us to have good precision without
	 * excessive memory consumption.
	 */
	len_shaper = 1024;
	len_lut3d = 33;

	shaper = zalloc(len_shaper * 3 * sizeof(*shaper));
	if (!shaper)
		goto err;

	lut3d = zalloc(3 * len_lut3d * len_lut3d * len_lut3d * sizeof(*lut3d));
	if (!lut3d)
		goto err;

	gl_xform = gl_renderer_color_transform_create(xform);
	if (!gl_xform)
		goto err;

	ok = xform->to_shaper_plus_3dlut(xform, len_shaper, shaper,
					 len_lut3d, lut3d);
	if (!ok)
		goto err;

	ok = gl_color_curve_lut_3x1d_init(gr, &gl_xform->pre_curve,
					  len_shaper, shaper);
	if (!ok)
		goto err;

	gl_color_mapping_lut_3d_init(gr, &gl_xform->mapping,
				     len_lut3d, lut3d);

	free(shaper);
	free(lut3d);

	return gl_xform;

err:
	if (shaper)
		free(shaper);
	if (lut3d)
		free(lut3d);
	if (gl_xform)
		gl_renderer_color_transform_destroy(gl_xform);
	return NULL;
}

static const struct gl_renderer_color_transform *
gl_renderer_color_transform_from(struct gl_renderer *gr,
				 struct weston_color_transform *xform)
{
	static const struct gl_renderer_color_transform no_op_gl_xform = {
		.pre_curve.type = SHADER_COLOR_CURVE_IDENTITY,
		.mapping.type = SHADER_COLOR_MAPPING_IDENTITY,
		.post_curve.type = SHADER_COLOR_CURVE_IDENTITY,
	};
	struct gl_renderer_color_transform *gl_xform;

	/* Identity transformation */
	if (!xform)
		return &no_op_gl_xform;

	/* Cached transformation */
	gl_xform = gl_renderer_color_transform_get(xform);
	if (gl_xform)
		return gl_xform;

	/* New transformation */

	if (xform->steps_valid)
		return gl_renderer_color_transform_create_steps(gr, xform);

	return gl_renderer_color_transform_create_3dlut(gr, xform);
}

bool
gl_shader_config_set_color_transform(struct gl_renderer *gr,
				     struct gl_shader_config *sconf,
				     struct weston_color_transform *xform)
{
	const struct gl_renderer_color_transform *gl_xform;

	gl_xform = gl_renderer_color_transform_from(gr, xform);
	if (!gl_xform)
		return false;

	sconf->req.color_pre_curve = gl_xform->pre_curve.type;
	sconf->color_pre_curve = gl_xform->pre_curve.u;

	sconf->req.color_post_curve = gl_xform->post_curve.type;
	sconf->color_post_curve = gl_xform->post_curve.u;

	sconf->req.color_mapping = gl_xform->mapping.type;
	sconf->color_mapping = gl_xform->mapping.u;

	return true;
}

static void
gl_renderer_color_effect_destroy(struct gl_renderer_color_effect *gl_effect)
{
	wl_list_remove(&gl_effect->destroy_listener.link);
	free(gl_effect);
}

static void
color_effect_destroy_handler(struct wl_listener *l, void *data)
{
	struct gl_renderer_color_effect *gl_effect;

	gl_effect = wl_container_of(l, gl_effect, destroy_listener);
	weston_assert_ptr_eq(gl_effect->owner->compositor, gl_effect->owner, data);

	gl_renderer_color_effect_destroy(gl_effect);
}

static struct gl_renderer_color_effect *
gl_renderer_color_effect_create(struct weston_output_color_effect *effect)
{
	struct gl_renderer_color_effect *gl_effect;

	gl_effect = zalloc(sizeof *gl_effect);
	if (!gl_effect)
		return NULL;

	gl_effect->owner = effect;
	gl_effect->destroy_listener.notify = color_effect_destroy_handler;
	wl_signal_add(&effect->destroy_signal, &gl_effect->destroy_listener);

	return gl_effect;
}

static struct gl_renderer_color_effect *
gl_renderer_color_effect_get(struct weston_output_color_effect *effect)
{
	struct wl_listener *l;

	l = wl_signal_get(&effect->destroy_signal,
			  color_effect_destroy_handler);
	if (!l)
		return NULL;

	return container_of(l, struct gl_renderer_color_effect,
			    destroy_listener);
}

static const struct gl_renderer_color_effect *
gl_renderer_color_effect_from(struct weston_output_color_effect *effect)
{
	struct gl_renderer_color_effect *gl_effect;

	/* Cached effect */
	gl_effect = gl_renderer_color_effect_get(effect);
	if (gl_effect)
		return gl_effect;

	/* New effect */
	return gl_renderer_color_effect_create(effect);
}

bool
gl_shader_config_set_color_effect(struct gl_renderer *gr,
				  struct gl_shader_config *sconf,
				  struct weston_output_color_effect *effect)
{
	const struct gl_renderer_color_effect *gl_effect;

	if (!effect) {
		sconf->req.color_effect = SHADER_COLOR_EFFECT_NONE;
		return true;
	}

	gl_effect = gl_renderer_color_effect_from(effect);
	if (!gl_effect)
		return false;

	switch (effect->type) {
	case WESTON_OUTPUT_COLOR_EFFECT_TYPE_INVERSION:
		sconf->req.color_effect = SHADER_COLOR_EFFECT_INVERSION;
		break;
	case WESTON_OUTPUT_COLOR_EFFECT_TYPE_GRAYSCALE:
		sconf->req.color_effect = SHADER_COLOR_EFFECT_GRAYSCALE;
		break;
	case WESTON_OUTPUT_COLOR_EFFECT_TYPE_CVD_CORRECTION:
		sconf->req.color_effect = SHADER_COLOR_EFFECT_CVD_CORRECTION;
		sconf->color_effect.cvd.correction = effect->u.cvd.correction;
		sconf->color_effect.cvd.type = effect->u.cvd.type;
		break;
	}
	weston_assert_u32_ne(gr->compositor, sconf->req.color_effect,
			     SHADER_COLOR_EFFECT_NONE);

	return true;
}

void
gl_shader_blender_destroy(struct gl_shader_blender *shader_blender)
{
	if (!shader_blender)
		return;

	gl_renderer_color_curve_fini(&shader_blender->fb_fetch_curve);
	gl_renderer_color_curve_fini(&shader_blender->fb_store_curve);
	free(shader_blender);
}

struct gl_shader_blender *
gl_shader_blender_create(struct gl_renderer *gr, struct weston_output *output)
{
	struct gl_shader_blender *shader_blender;
	struct weston_color_curve *fb_fetch;
	const struct weston_color_curve *fb_store;
	const struct weston_color_transform *xform;
	bool ok;

	if (!gl_features_has(gr, FEATURE_SHADER_BLENDING))
		return NULL;

	xform = output->color_outcome->from_blend_to_output;
	if (!xform)
		return NULL;

	fb_store = weston_color_transform_as_single_curve(xform);
	if (!fb_store)
		return NULL;

	fb_fetch = weston_color_curve_create_inverse(fb_store);
	if (!fb_fetch)
		return NULL;

	shader_blender = xzalloc(sizeof *shader_blender);
	ok = gl_color_curve_init(gr, &shader_blender->fb_store_curve, fb_store, NULL) &&
	     gl_color_curve_init(gr, &shader_blender->fb_fetch_curve, fb_fetch, NULL);
	free(fb_fetch);

	if (!ok) {
		free(shader_blender);
		return NULL;
	}

	return shader_blender;
}

void
gl_shader_config_set_blender(struct gl_renderer *gr,
			     struct gl_shader_config *sconf,
			     const struct gl_shader_blender *shader_blender)
{
	if (shader_blender) {
		sconf->req.shader_blending = true;

		sconf->req.fb_fetch_curve = shader_blender->fb_fetch_curve.type;
		sconf->fb_fetch_curve = shader_blender->fb_fetch_curve.u;

		sconf->req.fb_store_curve = shader_blender->fb_store_curve.type;
		sconf->fb_store_curve = shader_blender->fb_store_curve.u;
	} else {
		sconf->req.shader_blending = false;
		sconf->req.fb_fetch_curve = SHADER_COLOR_CURVE_IDENTITY;
		sconf->req.fb_store_curve = SHADER_COLOR_CURVE_IDENTITY;
	}
}
