/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2009 Chris Wilson
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

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <libgen.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <assert.h>
#include <errno.h>
#include <linux/input.h>

#include <wayland-client.h>

#include "window.h"
#include "shared/cairo-util.h"
#include "shared/helpers.h"
#include "shared/image-loader.h"

bool verbose;

#define verbose_print(...) do { \
	if (verbose) \
		fprintf(stderr, __VA_ARGS__); \
} while (0)

struct image {
	struct window *window;

	/* Decorations, buttons, etc. */
	struct widget *frame_widget;

	/* Where we draw the image content. */
	struct widget *image_widget;

	struct display *display;
	char *filename;
	cairo_surface_t *image;
	int fullscreen;
	int *image_counter;
	int32_t width, height;

	struct {
		double x;
		double y;
	} pointer;
	bool button_pressed;

	bool initialized;
	cairo_matrix_t matrix;
};

struct cli_render_intent_option {
	int render_intent;
	const char *cli_option;
};

static const struct cli_render_intent_option
cli_ri_table[] = {
	{
		.render_intent = -1,
		.cli_option = "off",
	},
	{
		.render_intent = RENDER_INTENT_PERCEPTUAL,
		.cli_option = "per",
	},
	{
		.render_intent = RENDER_INTENT_RELATIVE,
		.cli_option = "rel",
	},
	{
		.render_intent = RENDER_INTENT_RELATIVE_BPC,
		.cli_option = "rel-bpc",
	},
	{
		.render_intent = RENDER_INTENT_SATURATION,
		.cli_option = "sat",
	},
	{
		.render_intent = RENDER_INTENT_ABSOLUTE,
		.cli_option = "abs",
	},
};

static double
get_scale(struct image *image)
{
	assert(image->matrix.xy == 0.0 &&
	       image->matrix.yx == 0.0 &&
	       image->matrix.xx == image->matrix.yy);
	return image->matrix.xx;
}

static void
clamp_view(struct image *image)
{
	struct rectangle allocation;
	double scale = get_scale(image);
	double sw, sh;

	sw = image->width * scale;
	sh = image->height * scale;
	widget_get_allocation(image->frame_widget, &allocation);

	if (sw < allocation.width) {
		image->matrix.x0 =
			(allocation.width - image->width * scale) / 2;
	} else {
		if (image->matrix.x0 > 0.0)
			image->matrix.x0 = 0.0;
		if (sw + image->matrix.x0 < allocation.width)
			image->matrix.x0 = allocation.width - sw;
	}

	if (sh < allocation.height) {
		image->matrix.y0 =
			(allocation.height - image->height * scale) / 2;
	} else {
		if (image->matrix.y0 > 0.0)
			image->matrix.y0 = 0.0;
		if (sh + image->matrix.y0 < allocation.height)
			image->matrix.y0 = allocation.height - sh;
	}
}

static void
frame_redraw_handler(struct widget *widget, void *data)
{
	struct rectangle allocation;
	cairo_t *cr;

	widget_get_allocation(widget, &allocation);

	cr = widget_cairo_create(widget);
	cairo_rectangle(cr, allocation.x, allocation.y,
			allocation.width, allocation.height);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_fill(cr);
	cairo_destroy(cr);
}

static void
frame_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct image *image = data;

	clamp_view(image);
}

static void
image_redraw_handler(struct widget *widget, void *data)
{
	struct image *image = data;
	struct rectangle allocation;
	cairo_t *cr;
	double width, height, doc_aspect, window_aspect, scale;

	widget_get_allocation(widget, &allocation);

	cr = widget_cairo_create(widget);
	cairo_rectangle(cr, allocation.x, allocation.y,
			allocation.width, allocation.height);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);

	if (!image->initialized) {
		image->initialized = true;
		width = cairo_image_surface_get_width(image->image);
		height = cairo_image_surface_get_height(image->image);

		doc_aspect = width / height;
		window_aspect = (double) allocation.width / allocation.height;
		if (doc_aspect < window_aspect)
			scale = allocation.height / height;
		else
			scale = allocation.width / width;

		image->width = width;
		image->height = height;
		cairo_matrix_init_scale(&image->matrix, scale, scale);

		clamp_view(image);
	}

	cairo_set_matrix(cr, &image->matrix);
	cairo_set_source_surface(cr, image->image, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_paint(cr);
	cairo_destroy(cr);
}

static void
image_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct image *image = data;
	struct rectangle allocation;

	widget_get_allocation(image->frame_widget, &allocation);

	widget_set_allocation(widget,
			      allocation.x, allocation.y,
			      allocation.width, allocation.height);
}

static int
image_enter_handler(struct widget *widget,
		    struct input *input,
		    float x, float y, void *data)
{
	struct image *image = data;
	struct rectangle allocation;

	widget_get_allocation(widget, &allocation);
	x -= allocation.x;
	y -= allocation.y;

	image->pointer.x = x;
	image->pointer.y = y;

	return 1;
}

static void
move_viewport(struct image *image, double dx, double dy)
{
	double scale = get_scale(image);

	if (!image->initialized)
		return;

	cairo_matrix_translate(&image->matrix, -dx/scale, -dy/scale);
	clamp_view(image);

	window_schedule_redraw(image->window);
}

static int
image_motion_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     float x, float y, void *data)
{
	struct image *image = data;
	struct rectangle allocation;

	widget_get_allocation(widget, &allocation);
	x -= allocation.x;
	y -= allocation.y;

	if (image->button_pressed)
		move_viewport(image, image->pointer.x - x,
			      image->pointer.y - y);

	image->pointer.x = x;
	image->pointer.y = y;

	return image->button_pressed ? CURSOR_DRAGGING : CURSOR_LEFT_PTR;
}

static void
image_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button,
		     enum wl_pointer_button_state state,
		     void *data)
{
	struct image *image = data;

	if (button == BTN_LEFT) {
		image->button_pressed =
			state == WL_POINTER_BUTTON_STATE_PRESSED;

		if (state == WL_POINTER_BUTTON_STATE_PRESSED)
			input_set_pointer_image(input, CURSOR_DRAGGING);
		else
			input_set_pointer_image(input, CURSOR_LEFT_PTR);
	}
}

static void
zoom(struct image *image, double scale)
{
	double x = image->pointer.x;
	double y = image->pointer.y;
	cairo_matrix_t scale_matrix;

	if (!image->initialized)
		return;

	if (get_scale(image) * scale > 20.0 ||
	    get_scale(image) * scale < 0.02)
		return;

	cairo_matrix_init_identity(&scale_matrix);
	cairo_matrix_translate(&scale_matrix, x, y);
	cairo_matrix_scale(&scale_matrix, scale, scale);
	cairo_matrix_translate(&scale_matrix, -x, -y);

	cairo_matrix_multiply(&image->matrix, &image->matrix, &scale_matrix);
	clamp_view(image);
}

static void
image_axis_handler(struct widget *widget, struct input *input, uint32_t time,
		   uint32_t axis, wl_fixed_t value, void *data)
{
	struct image *image = data;

	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL &&
	    input_get_modifiers(input) == MOD_CONTROL_MASK) {
		/* set zoom level to 2% per 10 axis units */
		zoom(image, (1.0 - wl_fixed_to_double(value) / 500.0));

		window_schedule_redraw(image->window);
	} else if (input_get_modifiers(input) == 0) {
		if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
			move_viewport(image, 0, wl_fixed_to_double(value));
		else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
			move_viewport(image, wl_fixed_to_double(value), 0);
	}
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	struct image *image = data;

	window_schedule_redraw(image->window);
}

static void
key_handler(struct window *window, struct input *input, uint32_t time,
	    uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
	    void *data)
{
	struct image *image = data;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	switch (sym) {
	case XKB_KEY_minus:
		zoom(image, 0.8);
		window_schedule_redraw(image->window);
		break;
	case XKB_KEY_equal:
	case XKB_KEY_plus:
		zoom(image, 1.2);
		window_schedule_redraw(image->window);
		break;
	case XKB_KEY_1:
		image->matrix.xx = 1.0;
		image->matrix.xy = 0.0;
		image->matrix.yx = 0.0;
		image->matrix.yy = 1.0;
		clamp_view(image);
		window_schedule_redraw(image->window);
		break;
	}
}

static void
fullscreen_handler(struct window *window, void *data)
{
	struct image *image = data;

	image->fullscreen ^= 1;
	window_set_fullscreen(window, image->fullscreen);
}

static void
close_handler(void *data)
{
	struct image *image = data;

	*image->image_counter -= 1;

	if (*image->image_counter == 0)
		display_exit(image->display);

	cairo_surface_destroy(image->image);

	free(image->filename);

	widget_destroy(image->image_widget);
	widget_destroy(image->frame_widget);
	window_destroy(image->window);

	free(image);
}

static void
set_empty_input_region(struct widget *widget, struct display *display)
{
	struct wl_compositor *compositor;
	struct wl_surface *surface;
	struct wl_region *region;

	compositor = display_get_compositor(display);
	surface = widget_get_wl_surface(widget);
	region = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(surface, region);
	wl_region_destroy(region);
}

static struct image *
image_create(struct display *display, const char *filename,
	     int *image_counter, int render_intent)
{
	struct image *image;
	struct weston_image *wimage;
	char *b, *copy, title[512];
	char *err_msg;
	bool ret;

	image = zalloc(sizeof *image);
	if (image == NULL)
		return image;

	copy = strdup(filename);
	b = basename(copy);
	snprintf(title, sizeof title, "Wayland Image - %s", b);
	free(copy);

	image->filename = strdup(filename);
	image->image = load_cairo_surface(filename, true);

	if (!image->image) {
		free(image->filename);
		free(image);
		return NULL;
	}

	image->window = window_create(display);
	window_set_title(image->window, title);
	window_set_appid(image->window, "org.freedesktop.weston.wayland-image");
	image->display = display;
	image->image_counter = image_counter;
	*image_counter += 1;
	image->initialized = false;

	window_set_user_data(image->window, image);
	window_set_keyboard_focus_handler(image->window,
					  keyboard_focus_handler);
	window_set_fullscreen_handler(image->window, fullscreen_handler);
	window_set_close_handler(image->window, close_handler);
	window_set_key_handler(image->window, key_handler);

	image->frame_widget = window_frame_create(image->window, image);
	widget_set_redraw_handler(image->frame_widget, frame_redraw_handler);
	widget_set_resize_handler(image->frame_widget, frame_resize_handler);

	image->image_widget = window_add_subsurface(image->window, image,
						    SUBSURFACE_SYNCHRONIZED);
	/* We set the input region of the subsurface where the image is draw as
	 * NULL, as the input region of the parent surface is automatically set
	 * by the toytoolkit. But as the window that finds the widget in a
	 * certain (x, y) position looks for surfaces that are on top first, it
	 * will call the image_widget handlers for input related stuff. */
	set_empty_input_region(image->image_widget, display);
	widget_set_redraw_handler(image->image_widget, image_redraw_handler);
	widget_set_resize_handler(image->image_widget, image_resize_handler);
	widget_set_enter_handler(image->image_widget, image_enter_handler);
	widget_set_motion_handler(image->image_widget, image_motion_handler);
	widget_set_button_handler(image->image_widget, image_button_handler);
	widget_set_axis_handler(image->image_widget, image_axis_handler);

	wimage = load_cairo_surface_get_user_data(image->image);
	assert(wimage);
	if (wimage->icc_profile_data && render_intent != -1) {
		verbose_print("Image contains ICC file embedded, let's try to use the Wayland\n" \
			      "color-management protocol to set the surface image description\n" \
			      "using this ICC file.\n");
		ret = widget_set_image_description_icc(image->image_widget,
						       wimage->icc_profile_data->fd,
						       wimage->icc_profile_data->length,
						       wimage->icc_profile_data->offset,
						       render_intent, &err_msg);
		if (ret) {
			verbose_print("Successfully set surface image description " \
				      "using ICC file.\n");
		} else {
			fprintf(stderr, "Failed to set surface image description:\n%s\n",
					err_msg);
			free(err_msg);
		}
	}
	/* TODO: investigate if/how to get colorimetry info from the
	 * PNG/JPEG/etc image. Then use that to create a parametric image
	 * description and set it as the widget image description. Also, if
	 * clients do not enforce us to avoid setting an image description (i.e.
	 * render_intent != -1) but no colorimetry data is present, we can
	 * create a sRGB image description (through parameters) and set it as
	 * the image description to use. For now Weston do not support creating
	 * image description from parameters, that's why we've added only the
	 * code above that depends on ICC profiles. */

	widget_schedule_resize(image->frame_widget, 500, 400);

	return image;
}

static void
print_usage(const char *program_name)
{
	const struct render_intent_info *intent_info;
	const char *desc;
	unsigned int i;

	fprintf(stderr, "Usage:\n  %s [OPTIONS] [FILENAME0] [FILENAME1] ...\n\n" \
			"Options:\n", program_name);

	fprintf(stderr, "-v or --verbose to print verbose log information.\n\n");

	fprintf(stderr, "-h or --help to open this HELP dialogue.\n\n");

	fprintf(stderr, "-r or --rendering-intent to choose the color-management rendering intent.\n\n    " \
			"The rendering intent is used when an image file has colorimetry data embedded,\n    " \
			"and the compositor should present this image taking this into account. We use\n    " \
			"the Wayland color-management protocol extension to set the image description\n    " \
			"and a rendering intent, which is up to the client to decide. This is optional,\n    " \
			"and if nothing set we'll use 'perceptual'. Supported values:\n\n");

	for (i = 0; i < ARRAY_LENGTH(cli_ri_table); i++) {
		/* "off" option does not have a corresponding render_intent_info
		 * object from which we would be able to get the description. */
		intent_info = render_intent_info_from(cli_ri_table[i].render_intent);
		if (intent_info)
			desc = intent_info->desc;
		else
			desc = "No render intent (do not set image description)";

		fprintf(stderr, "        %s: %s.\n", cli_ri_table[i].cli_option, desc);
	}
}

static int
get_render_intent(int *render_intent, const char *opt_rendering_intent)
{
	unsigned int i;

	/* The default, if client does not set anything. */
	if (!opt_rendering_intent) {
		*render_intent = RENDER_INTENT_PERCEPTUAL;
		return 0;
	}

	for (i = 0; i < ARRAY_LENGTH(cli_ri_table); i++) {
		if (strcmp(opt_rendering_intent, cli_ri_table[i].cli_option) == 0) {
			*render_intent = cli_ri_table[i].render_intent;
			return 0;
		}
	}

	fprintf(stderr, "Error: unknown rendering intent: %s.\n\n",
			opt_rendering_intent);
	return -1;
}

int
main(int argc, char *argv[])
{
	struct display *d;
	int i;
	int image_counter = 0;
	int render_intent;
	bool opt_help = false;
	char *opt_rendering_intent = NULL;
	struct weston_option cli_options[] = {
		{ WESTON_OPTION_BOOLEAN, "help", 'h', &opt_help },
		{ WESTON_OPTION_BOOLEAN, "verbose", 'v', &verbose },
		{ WESTON_OPTION_STRING, "rendering-intent", 'r', &opt_rendering_intent },
	};

	parse_options(cli_options, ARRAY_LENGTH(cli_options), &argc, argv);

	if (argc <= 1 || opt_help ||
	    get_render_intent(&render_intent, opt_rendering_intent) < 0) {
		free(opt_rendering_intent);
		print_usage(argv[0]);
		return 1;
	}

	free(opt_rendering_intent);

	d = display_create(&argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	for (i = 1; i < argc; i++)
		image_create(d, argv[i], &image_counter, render_intent);

	if (image_counter > 0)
		display_run(d);

	display_destroy(d);

	return 0;
}
