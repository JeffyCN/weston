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

/**
 * The method is used in linearization of an arbitrary color profile
 * when EOTF is retrieved we want to know a generic way to decide the number
 * of points
 */
unsigned int
cmlcms_reasonable_1D_points(void)
{
	return 1024;
}

static unsigned int
cmlcms_reasonable_3D_points(void)
{
	return 33;
}

static void
fill_in_curves(cmsToneCurve *curves[3], float *values, unsigned len)
{
	float *R_lut = values;
	float *G_lut = R_lut + len;
	float *B_lut = G_lut + len;
	unsigned i;
	cmsFloat32Number x;

	for (i = 0; i < len; i++) {
		x = (double)i / (len - 1);
		R_lut[i] = cmsEvalToneCurveFloat(curves[0], x);
		G_lut[i] = cmsEvalToneCurveFloat(curves[1], x);
		B_lut[i] = cmsEvalToneCurveFloat(curves[2], x);
	}
}

static void
cmlcms_fill_in_output_inv_eotf_vcgt(struct weston_color_transform *xform_base,
				    float *values, unsigned len)
{
	struct cmlcms_color_transform *xform = get_xform(xform_base);

	assert(xform->search_key.category == CMLCMS_CATEGORY_BLEND_TO_OUTPUT);

	assert(len > 1);

	fill_in_curves(xform->search_key.output_profile->output_inv_eotf_vcgt,
		       values, len);
}

/**
 * Clamp value to [0.0, 1.0], except pass NaN through.
 *
 * This function is not intended for hiding NaN.
 */
static float
ensure_unorm(float v)
{
	if (v <= 0.0f)
		return 0.0f;
	if (v > 1.0f)
		return 1.0f;
	return v;
}

static void
cmlcms_fill_in_3dlut(struct weston_color_transform *xform_base,
		     float *lut, unsigned int len)
{
	struct cmlcms_color_transform *xform = get_xform(xform_base);
	float rgb_in[3];
	float rgb_out[3];
	unsigned int index;
	unsigned int value_b, value_r, value_g;
	float divider = len - 1;

	assert(xform->search_key.category == CMLCMS_CATEGORY_INPUT_TO_BLEND ||
	       xform->search_key.category == CMLCMS_CATEGORY_INPUT_TO_OUTPUT);

	for (value_b = 0; value_b < len; value_b++) {
		for (value_g = 0; value_g < len; value_g++) {
			for (value_r = 0; value_r < len; value_r++) {
				rgb_in[0] = (float)value_r / divider;
				rgb_in[1] = (float)value_g / divider;
				rgb_in[2] = (float)value_b / divider;

				cmsDoTransform(xform->cmap_3dlut, rgb_in, rgb_out, 1);

				index = 3 * (value_r + len * (value_g + len * value_b));
				lut[index    ] = ensure_unorm(rgb_out[0]);
				lut[index + 1] = ensure_unorm(rgb_out[1]);
				lut[index + 2] = ensure_unorm(rgb_out[2]);
			}
		}
	}
}

void
cmlcms_color_transform_destroy(struct cmlcms_color_transform *xform)
{
	wl_list_remove(&xform->link);

	if (xform->cmap_3dlut)
		cmsDeleteTransform(xform->cmap_3dlut);

	unref_cprof(xform->search_key.input_profile);
	unref_cprof(xform->search_key.output_profile);
	free(xform);
}

static bool
xform_set_cmap_3dlut(struct cmlcms_color_transform *xform,
		     cmsHPROFILE input_profile,
		     cmsHPROFILE output_profile,
		     cmsToneCurve *curves[3],
		     cmsUInt32Number intent)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(xform->base.cm);
	cmsHPROFILE arr_prof[3] = { input_profile, output_profile, NULL };
	int num_profiles = 2;

	if (curves[0]) {
		arr_prof[2] = cmsCreateLinearizationDeviceLinkTHR(cm->lcms_ctx,
								  cmsSigRgbData,
								  curves);
		if (!arr_prof[2])
			return false;

		num_profiles = 3;
	}

	xform->cmap_3dlut = cmsCreateMultiprofileTransformTHR(cm->lcms_ctx,
							      arr_prof,
							      num_profiles,
							      TYPE_RGB_FLT,
							      TYPE_RGB_FLT,
							      intent,
							      0);
	if (!xform->cmap_3dlut) {
		cmsCloseProfile(arr_prof[2]);
		weston_log("color-lcms error: fail cmsCreateMultiprofileTransformTHR.\n");
		return false;
	}
	xform->base.mapping.type = WESTON_COLOR_MAPPING_TYPE_3D_LUT;
	xform->base.mapping.u.lut3d.fill_in = cmlcms_fill_in_3dlut;
	xform->base.mapping.u.lut3d.optimal_len =
				cmlcms_reasonable_3D_points();
	cmsCloseProfile(arr_prof[2]);

	return true;
}

static struct cmlcms_color_transform *
cmlcms_color_transform_create(struct weston_color_manager_lcms *cm,
			      const struct cmlcms_color_transform_search_param *search_param)
{
	struct cmlcms_color_profile *input_profile = search_param->input_profile;
	struct cmlcms_color_profile *output_profile = search_param->output_profile;
	struct cmlcms_color_transform *xform;
	bool ok = false;

	xform = zalloc(sizeof *xform);
	if (!xform)
		return NULL;

	weston_color_transform_init(&xform->base, &cm->base);
	wl_list_init(&xform->link);
	xform->search_key = *search_param;
	xform->search_key.input_profile = ref_cprof(input_profile);
	xform->search_key.output_profile = ref_cprof(output_profile);

	/* Ensure the linearization etc. have been extracted. */
	if (!output_profile->eotf[0]) {
		if (!retrieve_eotf_and_output_inv_eotf(cm->lcms_ctx,
						       output_profile->profile,
						       output_profile->eotf,
						       output_profile->output_inv_eotf_vcgt,
						       output_profile->vcgt,
						       cmlcms_reasonable_1D_points()))
			goto error;
	}

	switch (search_param->category) {
	case CMLCMS_CATEGORY_INPUT_TO_BLEND:
		/* Use EOTF to linearize the result. */
		ok = xform_set_cmap_3dlut(xform, input_profile->profile,
					  output_profile->profile,
					  output_profile->eotf,
					  search_param->intent_output);
		break;

	case CMLCMS_CATEGORY_INPUT_TO_OUTPUT:
		/* Apply also VCGT if it exists. */
		ok = xform_set_cmap_3dlut(xform, input_profile->profile,
					  output_profile->profile,
					  output_profile->vcgt,
					  search_param->intent_output);
		break;

	case CMLCMS_CATEGORY_BLEND_TO_OUTPUT:
		xform->base.pre_curve.type = WESTON_COLOR_CURVE_TYPE_LUT_3x1D;
		xform->base.pre_curve.u.lut_3x1d.fill_in = cmlcms_fill_in_output_inv_eotf_vcgt;
		xform->base.pre_curve.u.lut_3x1d.optimal_len =
				cmlcms_reasonable_1D_points();
		ok = true;
		break;
	}

	if (!ok)
		goto error;

	wl_list_insert(&cm->color_transform_list, &xform->link);
	return xform;

error:
	cmlcms_color_transform_destroy(xform);
	weston_log("CM cmlcms_color_transform_create failed\n");
	return NULL;
}

static bool
transform_matches_params(const struct cmlcms_color_transform *xform,
			 const struct cmlcms_color_transform_search_param *param)
{
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
