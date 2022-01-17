/*
 * Copyright 2010-2012 Intel Corporation
 * Copyright 2013 Raspberry Pi Foundation
 * Copyright 2011-2012,2021 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "config.h"
#include "shared/shell-utils.h"
#include <libweston-desktop/libweston-desktop.h>

struct weston_output *
get_default_output(struct weston_compositor *compositor)
{
	if (wl_list_empty(&compositor->output_list))
		return NULL;

	return container_of(compositor->output_list.next,
			    struct weston_output, link);
}

struct weston_output *
get_focused_output(struct weston_compositor *compositor)
{
	struct weston_seat *seat;
	struct weston_output *output = NULL;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_touch *touch = weston_seat_get_touch(seat);
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		/* Priority has touch focus, then pointer and
		 * then keyboard focus. We should probably have
		 * three for loops and check first for touch,
		 * then for pointer, etc. but unless somebody has some
		 * objections, I think this is sufficient. */
		if (touch && touch->focus)
			output = touch->focus->output;
		else if (pointer && pointer->focus)
			output = pointer->focus->output;
		else if (keyboard && keyboard->focus)
			output = keyboard->focus->output;

		if (output)
			break;
	}

	return output;
}

/* TODO: Fix this function to take into account nested subsurfaces. */
void
surface_subsurfaces_boundingbox(struct weston_surface *surface, int32_t *x,
				int32_t *y, int32_t *w, int32_t *h)
{
	pixman_region32_t region;
	pixman_box32_t *box;
	struct weston_subsurface *subsurface;

	pixman_region32_init_rect(&region, 0, 0,
	                          surface->width,
	                          surface->height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link) {
		pixman_region32_union_rect(&region, &region,
		                           subsurface->position.x,
		                           subsurface->position.y,
		                           subsurface->surface->width,
		                           subsurface->surface->height);
	}

	box = pixman_region32_extents(&region);
	if (x)
		*x = box->x1;
	if (y)
		*y = box->y1;
	if (w)
		*w = box->x2 - box->x1;
	if (h)
		*h = box->y2 - box->y1;

	pixman_region32_fini(&region);
}

void
center_on_output(struct weston_view *view, struct weston_output *output)
{
	int32_t surf_x, surf_y, width, height;
	float x, y;

	if (!output) {
		weston_view_set_position(view, 0, 0);
		return;
	}

	surface_subsurfaces_boundingbox(view->surface, &surf_x, &surf_y, &width, &height);

	x = output->x + (output->width - width) / 2 - surf_x / 2;
	y = output->y + (output->height - height) / 2 - surf_y / 2;

	weston_view_set_position(view, x, y);
}

int
surface_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	const char *t, *c;
	struct weston_desktop_surface *desktop_surface =
		weston_surface_get_desktop_surface(surface);

	t = weston_desktop_surface_get_title(desktop_surface);
	c = weston_desktop_surface_get_app_id(desktop_surface);

	return snprintf(buf, len, "%s window%s%s%s%s%s",
		"top-level",
		t ? " '" : "", t ?: "", t ? "'" : "",
		c ? " of " : "", c ?: "");
}

struct weston_view *
weston_curtain_create(struct weston_compositor *compositor,
		      struct weston_curtain_params *params)
{
	struct weston_surface *surface = NULL;
	struct weston_view *view;

	surface = weston_surface_create(compositor);
	if (surface == NULL) {
		weston_log("no memory\n");
		return NULL;
	}
	view = weston_view_create(surface);
	if (view == NULL) {
		weston_log("no memory\n");
		weston_surface_destroy(surface);
		return NULL;
	}

	surface->committed = params->surface_committed;
	surface->committed_private = params->surface_private;

	weston_surface_set_color(surface, params->r, params->g, params->b, 1.0);
	weston_surface_set_label_func(surface, params->get_label);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_init_rect(&surface->opaque, 0, 0,
				  params->width, params->height);
	pixman_region32_fini(&surface->input);
	pixman_region32_init_rect(&surface->input, 0, 0,
				  params->width, params->height);

	weston_surface_set_size(surface, params->width, params->height);
	weston_view_set_position(view, params->x, params->y);

	return view;
}
