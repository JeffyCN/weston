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
	struct cmlcms_color_transform_search_param param = {
		/*
		 * Assumes both content and output color spaces are sRGB SDR.
		 * This defines the blending space as optical sRGB SDR.
		 */
		.type = CMLCMS_TYPE_EOTF_sRGB,
	};
	struct cmlcms_color_transform *xform;

	/* TODO: use output color profile */
	if (output->color_profile)
		return false;

	xform = cmlcms_color_transform_get(cm, &param);
	if (!xform)
		return false;

	surf_xform->transform = &xform->base;
	surf_xform->identity_pipeline = true;

	return true;
}

static bool
cmlcms_get_output_color_transform(struct weston_color_manager *cm_base,
				  struct weston_output *output,
				  struct weston_color_transform **xform_out)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);
	struct cmlcms_color_transform_search_param param = {
		/*
		 * Assumes blending space is optical sRGB SDR and
		 * output color space is sRGB SDR.
		 */
		.type = CMLCMS_TYPE_EOTF_sRGB_INV,
	};
	struct cmlcms_color_transform *xform;

	/* TODO: use output color profile */
	if (output->color_profile)
		return false;

	xform = cmlcms_color_transform_get(cm, &param);
	if (!xform)
		return false;

	*xform_out = &xform->base;
	return true;
}

static bool
cmlcms_get_sRGB_to_output_color_transform(struct weston_color_manager *cm_base,
					  struct weston_output *output,
					  struct weston_color_transform **xform_out)
{
	/* Assumes output color space is sRGB SDR */

	/* TODO: use output color profile */
	if (output->color_profile)
		return false;

	/* Identity transform */
	*xform_out = NULL;

	return true;
}

static bool
cmlcms_get_sRGB_to_blend_color_transform(struct weston_color_manager *cm_base,
					 struct weston_output *output,
					 struct weston_color_transform **xform_out)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);
	struct cmlcms_color_transform_search_param param = {
		/* Assumes blending space is optical sRGB SDR */
		.type = CMLCMS_TYPE_EOTF_sRGB,
	};
	struct cmlcms_color_transform *xform;

	/* TODO: use output color profile */
	if (output->color_profile)
		return false;

	xform = cmlcms_color_transform_get(cm, &param);
	if (!xform)
		return false;

	*xform_out = &xform->base;
	return true;
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
	cm->base.get_surface_color_transform =
	      cmlcms_get_surface_color_transform;
	cm->base.get_output_color_transform = cmlcms_get_output_color_transform;
	cm->base.get_sRGB_to_output_color_transform =
	      cmlcms_get_sRGB_to_output_color_transform;
	cm->base.get_sRGB_to_blend_color_transform =
	      cmlcms_get_sRGB_to_blend_color_transform;

	wl_list_init(&cm->color_transform_list);
	wl_list_init(&cm->color_profile_list);

	return &cm->base;
}
