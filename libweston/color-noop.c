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

#include <libweston/libweston.h>

#include "color.h"
#include "shared/helpers.h"

struct weston_color_manager_noop {
	struct weston_color_manager base;
};

static struct weston_color_manager_noop *
get_cmnoop(struct weston_color_manager *cm_base)
{
	return container_of(cm_base, struct weston_color_manager_noop, base);
}

static void
cmnoop_destroy_color_transform(struct weston_color_transform *xform)
{
	/* Never called, as never creates an actual color transform. */
}

static bool
cmnoop_get_surface_color_transform(struct weston_color_manager *cm_base,
				   struct weston_surface *surface,
				   struct weston_output *output,
				   struct weston_surface_color_transform *surf_xform)
{
	/* TODO: Assert surface has no colorspace set */
	/* TODO: Assert output has no colorspace set */

	/* Identity transform */
	surf_xform->transform = NULL;
	surf_xform->identity_pipeline = true;

	return true;
}

static bool
cmnoop_get_output_color_transform(struct weston_color_manager *cm_base,
				  struct weston_output *output,
				  struct weston_color_transform **xform_out)
{
	/* TODO: Assert output has no colorspace set */

	/* Identity transform */
	*xform_out = NULL;

	return true;
}

static bool
cmnoop_init(struct weston_color_manager *cm_base)
{
	/* No renderer requirements to check. */
	/* Nothing to initialize. */
	return true;
}

static void
cmnoop_destroy(struct weston_color_manager *cm_base)
{
	struct weston_color_manager_noop *cmnoop = get_cmnoop(cm_base);

	free(cmnoop);
}

struct weston_color_manager *
weston_color_manager_noop_create(struct weston_compositor *compositor)
{
	struct weston_color_manager_noop *cm;

	cm = zalloc(sizeof *cm);
	if (!cm)
		return NULL;

	cm->base.name = "no-op";
	cm->base.compositor = compositor;
	cm->base.supports_client_protocol = false;
	cm->base.init = cmnoop_init;
	cm->base.destroy = cmnoop_destroy;
	cm->base.destroy_color_transform = cmnoop_destroy_color_transform;
	cm->base.get_surface_color_transform =
	      cmnoop_get_surface_color_transform;
	cm->base.get_output_color_transform = cmnoop_get_output_color_transform;

	return &cm->base;
}
