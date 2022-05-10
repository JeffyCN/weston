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
#include "color_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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

const struct color_tone_curve arr_curves[] = {
		{
			.fn = TRANSFER_FN_SRGB_EOTF,
			.inv_fn = TRANSFER_FN_SRGB_EOTF_INVERSE,
			.internal_type = 4,
			.param = { 2.4, 1. / 1.055, 0.055 / 1.055, 1. / 12.92, 0.04045 } ,
		},
		{
			.fn = TRANSFER_FN_ADOBE_RGB_EOTF,
			.inv_fn = TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE,
			.internal_type = 1,
			.param = { 563./256., 0.0, 0.0, 0.0 , 0.0 } ,
		},
		{
			.fn = TRANSFER_FN_POWER2_4_EOTF,
			.inv_fn = TRANSFER_FN_POWER2_4_EOTF_INVERSE,
			.internal_type = 1,
			.param = { 2.4, 0.0, 0.0, 0.0 , 0.0 } ,
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

void
sRGB_linearize(struct color_float *cf)
{
	int i;

	for (i = 0; i < COLOR_CHAN_NUM; i++)
		cf->rgb[i] = sRGB_EOTF(cf->rgb[i]);
}

static float
apply_tone_curve(enum transfer_fn fn, float r)
{
	float ret = 0;

	switch(fn) {
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

void
sRGB_delinearize(struct color_float *cf)
{
	int i;

	for (i = 0; i < COLOR_CHAN_NUM; i++)
		cf->rgb[i] = sRGB_EOTF_inv(cf->rgb[i]);
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
process_pixel_using_pipeline(enum transfer_fn pre_curve,
			     const struct lcmsMAT3 *mat,
			     enum transfer_fn post_curve,
			     const struct color_float *in,
			     struct color_float *out)
{
	int i, j;
	struct color_float cf;
	float tmp;

	cf = color_float_apply_curve(pre_curve, *in);

	for (i = 0; i < 3; i++) {
		tmp = 0.0f;
		for (j = 0; j < 3; j++)
			tmp += cf.rgb[j] * mat->v[j].n[i];
		out->rgb[i] = tmp;
	}

	*out = color_float_apply_curve(post_curve, *out);
}
