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

static cmsUInt32Number
cmlcms_get_render_intent(enum cmlcms_category cat,
			 struct weston_surface *surface,
			 struct weston_output *output)
{
	/*
	 * TODO: Take into account client provided content profile,
	 * output profile, and the category of the wanted color
	 * transformation.
	 */
	cmsUInt32Number intent = INTENT_RELATIVE_COLORIMETRIC;
	return intent;
}

static void
setup_search_param(enum cmlcms_category cat,
		   struct weston_surface *surface,
		   struct weston_output *output,
		   struct cmlcms_color_profile *stock_sRGB_profile,
		   struct cmlcms_color_transform_search_param *search_param)
{
	struct cmlcms_color_profile *input_profile = NULL;
	struct cmlcms_color_profile *output_profile = NULL;

	/*
	 * TODO: un-comment when declare color_profile in struct weston_surface
	 */
	/* if (surface && surface->color_profile)
		input_profile = get_cprof(surface->color_profile); */
	if (output && output->color_profile)
		output_profile = get_cprof(output->color_profile);

	search_param->category = cat;
	switch (cat) {
	case CMLCMS_CATEGORY_INPUT_TO_BLEND:
	case CMLCMS_CATEGORY_INPUT_TO_OUTPUT:
		search_param->input_profile =
			input_profile ? input_profile : stock_sRGB_profile;
		search_param->output_profile =
			output_profile ? output_profile : stock_sRGB_profile;
		break;
	case CMLCMS_CATEGORY_BLEND_TO_OUTPUT:
		search_param->output_profile =
			output_profile ? output_profile : stock_sRGB_profile;
		break;
	}
	search_param->intent_output = cmlcms_get_render_intent(cat, surface,
							       output);
}


static void
cmlcms_destroy_color_transform(struct weston_color_transform *xform_base)
{
	struct cmlcms_color_transform *xform = get_xform(xform_base);

	cmlcms_color_transform_destroy(xform);
}

static bool
cmlcms_get_surface_color_transform(struct weston_color_manager *cm_base,
				   struct weston_surface *surface,
				   struct weston_output *output,
				   struct weston_surface_color_transform *surf_xform)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);
	struct cmlcms_color_transform_search_param param = {};
	struct cmlcms_color_transform *xform;

	/* TODO: take weston_output::eotf_mode into account */

	setup_search_param(CMLCMS_CATEGORY_INPUT_TO_BLEND, surface, output,
			   cm->sRGB_profile, &param);

	xform = cmlcms_color_transform_get(cm, &param);
	if (!xform)
		return false;

	surf_xform->transform = &xform->base;
	/*
	 * When we introduce LCMS plug-in we can precisely answer this question
	 * by examining the color pipeline using precision parameters. For now
	 * we just compare if it is same pointer or not.
	 */
	if (xform->search_key.input_profile == xform->search_key.output_profile)
		surf_xform->identity_pipeline = true;
	else
		surf_xform->identity_pipeline = false;

	return true;
}

static bool
cmlcms_get_blend_to_output_color_transform(struct weston_color_manager_lcms *cm,
					   struct weston_output *output,
					   struct weston_color_transform **xform_out)
{
	struct cmlcms_color_transform_search_param param = {};
	struct cmlcms_color_transform *xform;

	/* TODO: take weston_output::eotf_mode into account */

	setup_search_param(CMLCMS_CATEGORY_BLEND_TO_OUTPUT, NULL, output,
			   cm->sRGB_profile, &param);

	xform = cmlcms_color_transform_get(cm, &param);
	if (!xform)
		return false;

	*xform_out = &xform->base;
	return true;
}

static bool
cmlcms_get_sRGB_to_output_color_transform(struct weston_color_manager_lcms *cm,
					  struct weston_output *output,
					  struct weston_color_transform **xform_out)
{
	struct cmlcms_color_transform_search_param param = {};
	struct cmlcms_color_transform *xform;

	/* TODO: take weston_output::eotf_mode into account */

	setup_search_param(CMLCMS_CATEGORY_INPUT_TO_OUTPUT, NULL, output,
			   cm->sRGB_profile, &param);
	/*
	 * Create a color transformation when output profile is not stock
	 * sRGB profile.
	 */
	if (param.output_profile != cm->sRGB_profile) {
		xform = cmlcms_color_transform_get(cm, &param);
		if (!xform)
			return false;
		*xform_out = &xform->base;
	} else {
		*xform_out = NULL; /* Identity transform */
	}

	return true;
}

static bool
cmlcms_get_sRGB_to_blend_color_transform(struct weston_color_manager_lcms *cm,
					 struct weston_output *output,
					 struct weston_color_transform **xform_out)
{
	struct cmlcms_color_transform_search_param param = {};
	struct cmlcms_color_transform *xform;

	/* TODO: take weston_output::eotf_mode into account */

	setup_search_param(CMLCMS_CATEGORY_INPUT_TO_BLEND, NULL, output,
			   cm->sRGB_profile, &param);

	xform = cmlcms_color_transform_get(cm, &param);
	if (!xform)
		return false;

	*xform_out = &xform->base;
	return true;
}

static struct weston_output_color_outcome *
cmlcms_create_output_color_outcome(struct weston_color_manager *cm_base,
				   struct weston_output *output)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);
	struct weston_output_color_outcome *co;

	co = zalloc(sizeof *co);
	if (!co)
		return NULL;

	if (!cmlcms_get_blend_to_output_color_transform(cm, output,
							&co->from_blend_to_output))
		goto out_fail;

	if (!cmlcms_get_sRGB_to_blend_color_transform(cm, output,
						      &co->from_sRGB_to_blend))
		goto out_fail;

	if (!cmlcms_get_sRGB_to_output_color_transform(cm, output,
						       &co->from_sRGB_to_output))
		goto out_fail;

	return co;

out_fail:
	weston_output_color_outcome_destroy(&co);
	return NULL;
}

static void
lcms_error_logger(cmsContext context_id,
		  cmsUInt32Number error_code,
		  const char *text)
{
	weston_log("LittleCMS error: %s\n", text);
}

static bool
cmlcms_init(struct weston_color_manager *cm_base)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);

	if (!(cm->base.compositor->capabilities & WESTON_CAP_COLOR_OPS)) {
		weston_log("color-lcms: error: color operations capability missing. Is GL-renderer not in use?\n");
		return false;
	}

	cm->lcms_ctx = cmsCreateContext(NULL, cm);
	if (!cm->lcms_ctx) {
		weston_log("color-lcms error: creating LittCMS context failed.\n");
		return false;
	}

	cmsSetLogErrorHandlerTHR(cm->lcms_ctx, lcms_error_logger);

	if (!cmlcms_create_stock_profile(cm)) {
		weston_log("color-lcms: error: cmlcms_create_stock_profile failed\n");
		return false;
	}
	weston_log("LittleCMS %d initialized.\n", cmsGetEncodedCMMversion());

	return true;
}

static void
cmlcms_destroy(struct weston_color_manager *cm_base)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);

	if (cm->sRGB_profile)
		cmlcms_color_profile_destroy(cm->sRGB_profile);
	assert(wl_list_empty(&cm->color_transform_list));
	assert(wl_list_empty(&cm->color_profile_list));

	cmsDeleteContext(cm->lcms_ctx);
	free(cm);
}

WL_EXPORT struct weston_color_manager *
weston_color_manager_create(struct weston_compositor *compositor)
{
	struct weston_color_manager_lcms *cm;

	cm = zalloc(sizeof *cm);
	if (!cm)
		return NULL;

	cm->base.name = "work-in-progress";
	cm->base.compositor = compositor;
	cm->base.supports_client_protocol = true;
	cm->base.init = cmlcms_init;
	cm->base.destroy = cmlcms_destroy;
	cm->base.destroy_color_profile = cmlcms_destroy_color_profile;
	cm->base.get_color_profile_from_icc = cmlcms_get_color_profile_from_icc;
	cm->base.destroy_color_transform = cmlcms_destroy_color_transform;
	cm->base.get_surface_color_transform = cmlcms_get_surface_color_transform;
	cm->base.create_output_color_outcome = cmlcms_create_output_color_outcome;

	wl_list_init(&cm->color_transform_list);
	wl_list_init(&cm->color_profile_list);

	return &cm->base;
}
