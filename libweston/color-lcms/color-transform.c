/*
 * Copyright 2021 Collabora, Ltd.
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
#include <libweston/libweston.h>

#include "color.h"
#include "color-lcms.h"
#include "shared/helpers.h"

/* Arguments to cmsBuildParametricToneCurve() */
struct tone_curve_def {
	cmsInt32Number cmstype;
	cmsFloat64Number params[5];
};

/*
 * LCMS uses the required number of 'params' based on 'cmstype', the parametric
 * tone curve number. LCMS honors negative 'cmstype' as inverse function.
 * These are LCMS built-in parametric tone curves.
 */
static const struct tone_curve_def predefined_eotf_curves[] = {
	[CMLCMS_TYPE_EOTF_sRGB] = {
		.cmstype = 4,
		.params = { 2.4, 1. / 1.055, 0.055 / 1.055, 1. / 12.92, 0.04045 },
	},
	[CMLCMS_TYPE_EOTF_sRGB_INV] = {
		.cmstype = -4,
		.params = { 2.4, 1. / 1.055, 0.055 / 1.055, 1. / 12.92, 0.04045 },
	},
};

static void
cmlcms_fill_in_tone_curve(struct weston_color_transform *xform_base,
			  float *values, unsigned len)
{
	struct cmlcms_color_transform *xform = get_xform(xform_base);
	float *R_lut = values;
	float *G_lut = R_lut + len;
	float *B_lut = G_lut + len;
	unsigned i;
	cmsFloat32Number x, y;

	assert(xform->curve != NULL);
	assert(len > 1);

	for (i = 0; i < len; i++) {
		x = (double)i / (len - 1);
		y = cmsEvalToneCurveFloat(xform->curve, x);
		R_lut[i] = y;
		G_lut[i] = y;
		B_lut[i] = y;
	}
}

void
cmlcms_color_transform_destroy(struct cmlcms_color_transform *xform)
{
	wl_list_remove(&xform->link);
	if (xform->curve)
		cmsFreeToneCurve(xform->curve);
	free(xform);
}

static struct cmlcms_color_transform *
cmlcms_color_transform_create(struct weston_color_manager_lcms *cm,
			const struct cmlcms_color_transform_search_param *param)
{
	struct cmlcms_color_transform *xform;
	const struct tone_curve_def *tonedef;

	if (param->type < 0 || param->type >= CMLCMS_TYPE__END) {
		weston_log("color-lcms error: bad color transform type in %s.\n",
			   __func__);
		return NULL;
	}
	tonedef = &predefined_eotf_curves[param->type];

	xform = zalloc(sizeof *xform);
	if (!xform)
		return NULL;

	xform->curve = cmsBuildParametricToneCurve(cm->lcms_ctx,
						   tonedef->cmstype,
						   tonedef->params);
	if (xform->curve == NULL) {
		weston_log("color-lcms error: failed to build parametric tone curve.\n");
		free(xform);
		return NULL;
	}

	weston_color_transform_init(&xform->base, &cm->base);
	xform->search_key = *param;

	xform->base.pre_curve.type = WESTON_COLOR_CURVE_TYPE_LUT_3x1D;
	xform->base.pre_curve.u.lut_3x1d.fill_in = cmlcms_fill_in_tone_curve;
	xform->base.pre_curve.u.lut_3x1d.optimal_len = 256;

	wl_list_insert(&cm->color_transform_list, &xform->link);

	return xform;
}

static bool
transform_matches_params(const struct cmlcms_color_transform *xform,
			 const struct cmlcms_color_transform_search_param *param)
{
	if (xform->search_key.type != param->type)
		return false;

	if (xform->search_key.category != param->category)
		return false;

	if (xform->search_key.intent_output  != param->intent_output ||
	    xform->search_key.output_profile != param->output_profile ||
	    xform->search_key.input_profile != param->input_profile)
		return false;

	return true;
}

struct cmlcms_color_transform *
cmlcms_color_transform_get(struct weston_color_manager_lcms *cm,
			   const struct cmlcms_color_transform_search_param *param)
{
	struct cmlcms_color_transform *xform;

	wl_list_for_each(xform, &cm->color_transform_list, link) {
		if (transform_matches_params(xform, param)) {
			weston_color_transform_ref(&xform->base);
			return xform;
		}
	}

	xform = cmlcms_color_transform_create(cm, param);
	if (!xform)
		weston_log("color-lcms error: failed to create a color transformation.\n");

	return xform;
}
