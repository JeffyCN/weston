/*
 * Copyright 2020 Collabora, Ltd.
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

#include <math.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <libweston/matrix.h>
#include "color_util.h"
#include "shared/helpers.h"

static_assert(sizeof(struct color_float) == 4 * sizeof(float),
	      "unexpected padding in struct color_float");
static_assert(offsetof(struct color_float, r) == offsetof(struct color_float, rgb[COLOR_CHAN_R]),
	      "unexpected offset for struct color_float::r");
static_assert(offsetof(struct color_float, g) == offsetof(struct color_float, rgb[COLOR_CHAN_G]),
	      "unexpected offset for struct color_float::g");
static_assert(offsetof(struct color_float, b) == offsetof(struct color_float, rgb[COLOR_CHAN_B]),
	      "unexpected offset for struct color_float::b");

struct color_tone_curve {
	enum transfer_fn fn;
	enum transfer_fn inv_fn;

	/* LCMS2 API */
	int internal_type;
	double param[5];
};

/* Mapping from enum transfer_fn to LittleCMS curve parameters. */
const struct color_tone_curve arr_curves[] = {
	{
		.fn = TRANSFER_FN_SRGB_EOTF,
		.inv_fn = TRANSFER_FN_SRGB_EOTF_INVERSE,
		.internal_type = 4,
		.param = { 2.4, 1. / 1.055, 0.055 / 1.055, 1. / 12.92, 0.04045 },
	},
	{
		.fn = TRANSFER_FN_ADOBE_RGB_EOTF,
		.inv_fn = TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE,
		.internal_type = 1,
		.param = { 563./256., 0.0, 0.0, 0.0 , 0.0 },
	},
	{
		.fn = TRANSFER_FN_POWER2_4_EOTF,
		.inv_fn = TRANSFER_FN_POWER2_4_EOTF_INVERSE,
		.internal_type = 1,
		.param = { 2.4, 0.0, 0.0, 0.0 , 0.0 },
	}
};

bool
find_tone_curve_type(enum transfer_fn fn, int *type, double params[5])
{
	const int size_arr = ARRAY_LENGTH(arr_curves);
	const struct color_tone_curve *curve;

	for (curve = &arr_curves[0]; curve < &arr_curves[size_arr]; curve++ ) {
		if (curve->fn == fn )
			*type = curve->internal_type;
		else if (curve->inv_fn == fn)
			*type = -curve->internal_type;
		else
			continue;

		memcpy(params, curve->param, sizeof(curve->param));
		return true;
	}

	return false;
}

enum transfer_fn
transfer_fn_invert(enum transfer_fn fn)
{
	switch (fn) {
	case TRANSFER_FN_ADOBE_RGB_EOTF:
		return TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE;
	case TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE:
		return TRANSFER_FN_ADOBE_RGB_EOTF;
	case TRANSFER_FN_IDENTITY:
		return TRANSFER_FN_IDENTITY;
	case TRANSFER_FN_POWER2_4_EOTF:
		return TRANSFER_FN_POWER2_4_EOTF_INVERSE;
	case TRANSFER_FN_POWER2_4_EOTF_INVERSE:
		return TRANSFER_FN_POWER2_4_EOTF;
	case TRANSFER_FN_SRGB_EOTF:
		return TRANSFER_FN_SRGB_EOTF_INVERSE;
	case TRANSFER_FN_SRGB_EOTF_INVERSE:
		return TRANSFER_FN_SRGB_EOTF;
	}
	assert(0 && "bad transfer_fn");
	return 0;
}

/**
 * NaN comes out as is
 *This function is not intended for hiding NaN.
 */
static float
ensure_unit_range(float v)
{
	const float tol = 1e-5f;
	const float lim_lo = -tol;
	const float lim_hi = 1.0f + tol;

	assert(v >= lim_lo);
	if (v < 0.0f)
		return 0.0f;
	assert(v <= lim_hi);
	if (v > 1.0f)
		return 1.0f;
	return v;
}

static float
sRGB_EOTF(float e)
{
	e = ensure_unit_range(e);
	if (e <= 0.04045)
		return e / 12.92;
	else
		return pow((e + 0.055) / 1.055, 2.4);
}

static float
sRGB_EOTF_inv(float o)
{
	o = ensure_unit_range(o);
	if (o <= 0.04045 / 12.92)
		return o * 12.92;
	else
		return pow(o, 1.0 / 2.4) * 1.055 - 0.055;
}

static float
AdobeRGB_EOTF(float e)
{
	e = ensure_unit_range(e);
	return pow(e, 563./256.);
}

static float
AdobeRGB_EOTF_inv(float o)
{
	o = ensure_unit_range(o);
	return pow(o, 256./563.);
}

static float
Power2_4_EOTF(float e)
{
	e = ensure_unit_range(e);
	return pow(e, 2.4);
}

static float
Power2_4_EOTF_inv(float o)
{
	o = ensure_unit_range(o);
	return pow(o, 1./2.4);
}

static float
apply_tone_curve(enum transfer_fn fn, float r)
{
	float ret = 0;

	switch(fn) {
	case TRANSFER_FN_IDENTITY:
		ret = r;
		break;
	case TRANSFER_FN_SRGB_EOTF:
		ret = sRGB_EOTF(r);
		break;
	case TRANSFER_FN_SRGB_EOTF_INVERSE:
		ret = sRGB_EOTF_inv(r);
		break;
	case TRANSFER_FN_ADOBE_RGB_EOTF:
		ret = AdobeRGB_EOTF(r);
		break;
	case TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE:
		ret = AdobeRGB_EOTF_inv(r);
		break;
	case TRANSFER_FN_POWER2_4_EOTF:
		ret = Power2_4_EOTF(r);
		break;
	case TRANSFER_FN_POWER2_4_EOTF_INVERSE:
		ret = Power2_4_EOTF_inv(r);
		break;
	}

	return ret;
}

struct color_float
a8r8g8b8_to_float(uint32_t v)
{
	struct color_float cf;

	cf.a = ((v >> 24) & 0xff) / 255.f;
	cf.r = ((v >> 16) & 0xff) / 255.f;
	cf.g = ((v >>  8) & 0xff) / 255.f;
	cf.b = ((v >>  0) & 0xff) / 255.f;

	return cf;
}

static struct color_float
color_float_apply_curve(enum transfer_fn fn, struct color_float c)
{
	unsigned i;

	for (i = 0; i < COLOR_CHAN_NUM; i++)
		c.rgb[i] = apply_tone_curve(fn, c.rgb[i]);

	return c;
}

void
sRGB_linearize(struct color_float *cf)
{
	*cf = color_float_apply_curve(TRANSFER_FN_SRGB_EOTF, *cf);
}

void
sRGB_delinearize(struct color_float *cf)
{
	*cf = color_float_apply_curve(TRANSFER_FN_SRGB_EOTF_INVERSE, *cf);
}

/*
 * Returns the result of the matrix-vector multiplication mat * c.
 */
struct color_float
color_float_apply_matrix(const struct lcmsMAT3 *mat, struct color_float c)
{
	struct color_float result;
	unsigned i, j;

	/*
	 * The matrix has an array of columns, hence i indexes to rows and
	 * j indexes to columns.
	 */
	for (i = 0; i < 3; i++) {
		result.rgb[i] = 0.0f;
		for (j = 0; j < 3; j++)
			result.rgb[i] += mat->v[j].n[i] * c.rgb[j];
	}

	result.a = c.a;
	return result;
}

void
process_pixel_using_pipeline(enum transfer_fn pre_curve,
			     const struct lcmsMAT3 *mat,
			     enum transfer_fn post_curve,
			     const struct color_float *in,
			     struct color_float *out)
{
	struct color_float cf;

	cf = color_float_apply_curve(pre_curve, *in);
	cf = color_float_apply_matrix(mat, cf);
	*out = color_float_apply_curve(post_curve, cf);
}

static void
weston_matrix_from_lcmsMAT3(struct weston_matrix *w, const struct lcmsMAT3 *m)
{
	unsigned r, c;

	/* column-major */
	weston_matrix_init(w);

	for (c = 0; c < 3; c++) {
		for (r = 0; r < 3; r++)
			w->d[c * 4 + r] = m->v[c].n[r];
	}
}

static void
lcmsMAT3_from_weston_matrix(struct lcmsMAT3 *m, const struct weston_matrix *w)
{
	unsigned r, c;

	for (c = 0; c < 3; c++) {
		for (r = 0; r < 3; r++)
			m->v[c].n[r] = w->d[c * 4 + r];
	}
}

void
lcmsMAT3_invert(struct lcmsMAT3 *result, const struct lcmsMAT3 *mat)
{
	struct weston_matrix inv;
	struct weston_matrix w;
	int ret;

	weston_matrix_from_lcmsMAT3(&w, mat);
	ret = weston_matrix_invert(&inv, &w);
	assert(ret == 0);
	lcmsMAT3_from_weston_matrix(result, &inv);
}
