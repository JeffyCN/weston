/*
 * Copyright 2021 Collabora, Ltd.
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

#include <lcms2.h>

#include <libweston/libweston.h>

#include "color.h"
#include "color-lcms.h"
#include "shared/helpers.h"

static void
cmlcms_destroy_color_transform(struct weston_color_transform *xform_base)
{
}

static bool
cmlcms_get_surface_color_transform(struct weston_color_manager *cm_base,
				   struct weston_surface *surface,
				   struct weston_output *output,
				   struct weston_surface_color_transform *surf_xform)
{
	/* Identity transform */
	surf_xform->transform = NULL;
	surf_xform->identity_pipeline = true;

	return true;
}

static bool
cmlcms_get_output_color_transform(struct weston_color_manager *cm_base,
				  struct weston_output *output,
				  struct weston_color_transform **xform_out)
{
	/* Identity transform */
	*xform_out = NULL;

	return true;
}

static bool
cmlcms_get_sRGB_to_output_color_transform(struct weston_color_manager *cm_base,
					  struct weston_output *output,
					  struct weston_color_transform **xform_out)
{
	/* Identity transform */
	*xform_out = NULL;

	return true;
}

static bool
cmlcms_get_sRGB_to_blend_color_transform(struct weston_color_manager *cm_base,
					 struct weston_output *output,
					 struct weston_color_transform **xform_out)
{
	/* Identity transform */
	*xform_out = NULL;

	return true;
}

static bool
cmlcms_init(struct weston_color_manager *cm_base)
{
	if (!(cm_base->compositor->capabilities & WESTON_CAP_COLOR_OPS)) {
		weston_log("color-lcms: error: color operations capability missing. Is GL-renderer not in use?\n");
		return false;
	}

	return true;
}

static void
cmlcms_destroy(struct weston_color_manager *cm_base)
{
	struct weston_color_manager_lcms *cmlcms = get_cmlcms(cm_base);

	free(cmlcms);
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
	cm->base.destroy_color_transform = cmlcms_destroy_color_transform;
	cm->base.get_surface_color_transform =
	      cmlcms_get_surface_color_transform;
	cm->base.get_output_color_transform = cmlcms_get_output_color_transform;
	cm->base.get_sRGB_to_output_color_transform =
	      cmlcms_get_sRGB_to_output_color_transform;
	cm->base.get_sRGB_to_blend_color_transform =
	      cmlcms_get_sRGB_to_blend_color_transform;

	return &cm->base;
}
