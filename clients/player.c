/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE
 */
#include "config.h"

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <wayland-client.h>

#include "window.h"

#define INITIAL_WIDTH 640
#define INITIAL_HEIGHT 480

struct player {
	GstVideoOverlay *overlay;
};

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height,
	       void *data)
{
	struct player *player = data;
	struct rectangle allocation;

	widget_get_allocation(widget, &allocation);

	gst_video_overlay_set_render_rectangle(player->overlay,
					       allocation.x, allocation.y,
					       allocation.width,
					       allocation.height);
}

static GstElement*
find_wayland_sink(GstElement *pipeline) {
	GstElement *sink = NULL;
	GstIterator *it = gst_bin_iterate_elements(GST_BIN(pipeline));
	GValue item = G_VALUE_INIT;

	while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
		GstElement *element = g_value_get_object(&item);
		GstElementFactory *factory = gst_element_get_factory(element);
		if (factory &&
		    !strcmp(GST_OBJECT_NAME(factory), "waylandsink")) {
			sink = element;
			gst_object_ref(sink);
			break;
		}
		g_value_unset(&item);
	}
	gst_iterator_free(it);
	return sink;
}

int
main(int argc, char *argv[])
{
	struct player *player;
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct wl_surface *surface;

	GstElement *pipeline;
	GstContext *context;
	GstStructure *s;
	GstElement *sink;
	GError *error = NULL;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <GStreamer pipeline>\n", argv[0]);
		return EXIT_FAILURE;
	}

	display = display_create(&argc, argv);
	if (!display) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	player = zalloc(sizeof (struct player));
	if (!player)
		return -1;

	window = window_create(display);
	widget = window_frame_create(window, player);
	widget_set_transparent(widget, 0);
	window_set_title(window, "Wayland Player Demo");

	widget_set_resize_handler(widget, resize_handler);

	setenv("WAYLANDSINK_PLACE_ABOVE", "1", 1);

	gst_init(&argc, &argv);

	pipeline = gst_parse_launch(argv[1], &error);
	if (!pipeline) {
		fprintf(stderr, "Pipeline error: %s\n", error->message);
		g_error_free(error);
		return -1;
	}

	sink = find_wayland_sink(pipeline);
	if (!sink) {
		fprintf(stderr, "Wayland sink not found in pipeline\n");
		return -1;
	}

	player->overlay = GST_VIDEO_OVERLAY(sink);

	context = gst_context_new("GstWlDisplayHandleContextType", FALSE);
	s = gst_context_writable_structure(context);
	gst_structure_set(s, "display", G_TYPE_POINTER,
			  display_get_display(display), NULL);
	gst_element_set_context(pipeline, context);
	gst_context_unref(context);

	surface = window_get_wl_surface(window);
	gst_video_overlay_set_window_handle(player->overlay, (guintptr)surface);

	window_schedule_resize(window, INITIAL_WIDTH, INITIAL_HEIGHT);

	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	display_run(display);

	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline);

	widget_destroy(widget);
	window_destroy(window);
	free(player);

	display_destroy(display);

	return 0;
}
