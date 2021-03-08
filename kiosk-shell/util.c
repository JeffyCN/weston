/*
 * Copyright 2010-2012 Intel Corporation
 * Copyright 2013 Raspberry Pi Foundation
 * Copyright 2011-2012,2020 Collabora, Ltd.
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
 */

/* Helper functions for kiosk-shell */

#include "util.h"
#include "shared/helpers.h"
#include <libweston/libweston.h>

static void
colored_surface_committed(struct weston_surface *es, int32_t sx, int32_t sy)
{
}

struct weston_view *
create_colored_surface(struct weston_compositor *compositor,
		       float r, float g, float b,
		       float x, float y, int w, int h)
{
	struct weston_surface *surface = NULL;
	struct weston_view *view;

	surface = weston_surface_create(compositor);
	if (surface == NULL) {
		weston_log("no memory\n");
		return NULL;
	}
	view = weston_view_create(surface);
	if (surface == NULL) {
		weston_log("no memory\n");
		weston_surface_destroy(surface);
		return NULL;
	}

	surface->committed = colored_surface_committed;
	surface->committed_private = NULL;

	weston_surface_set_color(surface, r, g, b, 1.0);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_init_rect(&surface->opaque, 0, 0, w, h);
	pixman_region32_fini(&surface->input);
	pixman_region32_init_rect(&surface->input, 0, 0, w, h);

	weston_surface_set_size(surface, w, h);
	weston_view_set_position(view, x, y);

	return view;
}
