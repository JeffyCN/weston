/*
 * Copyright © 2016-2023 Collabora, Ltd.
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

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "libweston-internal.h"
#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"

struct setup_args {
	struct fixture_metadata meta;
	enum weston_renderer_type renderer;
};

static const struct setup_args my_setup_args[] = {
	{
		.renderer = WESTON_RENDERER_PIXMAN,
		.meta.name = "pixman"
	},
	{
		.renderer = WESTON_RENDERER_GL,
		.meta.name = "GL"
	},
};

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.renderer = arg->renderer;
	setup.width = 320;
	setup.height = 240;
	setup.shell = SHELL_TEST_DESKTOP;
	setup.logging_scopes = "log,test-harness-plugin";

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args, meta);

static struct buffer *
surface_commit_color(struct client *client, struct wl_surface *surface,
		     pixman_color_t *color, int width, int height)
{
	struct buffer *buf;

	buf = create_shm_buffer_a8r8g8b8(client, width, height);
	fill_image_with_color(buf->image, color);
	wl_surface_attach(surface, buf->proxy, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	return buf;
}

#define DECLARE_LIST_ITERATOR(name, parent, list, child, link)			\
static child *									\
next_##name(parent *from, child *pos)						\
{										\
	struct wl_list *entry = pos ? &pos->link : &from->list;			\
	if (entry->next == &from->list)						\
		return NULL;							\
	return container_of(entry->next, child, link);				\
}

DECLARE_LIST_ITERATOR(output, struct weston_compositor, output_list,
		      struct weston_output, link);
DECLARE_LIST_ITERATOR(pnode_from_z, struct weston_output, paint_node_z_order_list,
		      struct weston_paint_node, z_order_link);

TEST(top_surface_present_in_output_repaint)
{
	struct wet_testsuite_data *suite_data = TEST_GET_SUITE_DATA();
	struct client *client;
	struct buffer *buf;
	pixman_color_t red;

	color_rgb888(&red, 255, 0, 0);

	client = create_client_and_test_surface(100, 50, 100, 100);
	assert(client);

	/* move the pointer clearly away from our screenshooting area */
	weston_test_move_pointer(client->test->weston_test, 0, 1, 0, 2, 30);

	client_push_breakpoint(client, suite_data,
			       WESTON_TEST_BREAKPOINT_POST_REPAINT,
			       (struct wl_proxy *) client->output->wl_output);

	buf = surface_commit_color(client, client->surface->wl_surface, &red, 100, 100);

	RUN_INSIDE_BREAKPOINT(client, suite_data) {
		struct weston_compositor *compositor;
		struct weston_output *output;
		struct weston_head *head;
		struct weston_paint_node *pnode;
		struct weston_view *view;
		struct weston_surface *surface;
		struct weston_buffer *buffer;

		assert(breakpoint->template_->breakpoint ==
		       WESTON_TEST_BREAKPOINT_POST_REPAINT);
		compositor = breakpoint->compositor;
		head = breakpoint->resource;
		output = next_output(compositor, NULL);
		assert(output == head->output);
		assert(strcmp(output->name, "headless") == 0);
		assert(!next_output(compositor, output));

		/* check that our surface is top of the paint node list */
		pnode = next_pnode_from_z(output, NULL);
		assert(pnode);
		view = pnode->view;
		surface = view->surface;
		buffer = surface->buffer_ref.buffer;
		assert(surface->resource);
		assert(wl_resource_get_client(surface->resource) ==
		       suite_data->wl_client);
		assert(weston_view_is_mapped(view));
		assert(weston_surface_is_mapped(surface));
		assert(surface->width == 100);
		assert(surface->height == 100);
		assert(buffer->width == surface->width);
		assert(buffer->height == surface->height);
		assert(buffer->type == WESTON_BUFFER_SHM);
	}

	buffer_destroy(buf);
	client_destroy(client);
}
