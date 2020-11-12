/*
 * Copyright 2020 Collabora, Ltd.
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

#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"

struct setup_args {
	enum renderer_type renderer;
};

static const int ALPHA_STEPS = 256;
static const int BLOCK_WIDTH = 3;

static const struct setup_args my_setup_args[] = {
	{ RENDERER_PIXMAN },
	{ RENDERER_GL },
};

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.renderer = arg->renderer;
	setup.width = BLOCK_WIDTH * ALPHA_STEPS;
	setup.height = 16;
	setup.shell = SHELL_TEST_DESKTOP;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args);

static void
set_opaque_rect(struct client *client,
		struct surface *surface,
		const struct rectangle *rect)
{
	struct wl_region *region;

	region = wl_compositor_create_region(client->wl_compositor);
	wl_region_add(region, rect->x, rect->y, rect->width, rect->height);
	wl_surface_set_opaque_region(surface->wl_surface, region);
	wl_region_destroy(region);
}

static uint32_t
premult_color(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
	uint32_t c = 0;

	c |= a << 24;
	c |= (a * r / 255) << 16;
	c |= (a * g / 255) << 8;
	c |= a * b / 255;

	return c;
}

static void
fill_alpha_pattern(struct buffer *buf)
{
	void *pixels;
	int stride_bytes;
	int w, h;
	int y;

	assert(pixman_image_get_format(buf->image) == PIXMAN_a8r8g8b8);

	pixels = pixman_image_get_data(buf->image);
	stride_bytes = pixman_image_get_stride(buf->image);
	w = pixman_image_get_width(buf->image);
	h = pixman_image_get_height(buf->image);

	assert(w == BLOCK_WIDTH * ALPHA_STEPS);

	for (y = 0; y < h; y++) {
		uint32_t *row = pixels + y * stride_bytes;
		uint32_t step;

		for (step = 0; step < (uint32_t)ALPHA_STEPS; step++) {
			uint32_t alpha = step * 255 / (ALPHA_STEPS - 1);
			uint32_t color;
			int i;

			color = premult_color(alpha, 0, 255 - alpha, 255);
			for (i = 0; i < BLOCK_WIDTH; i++)
				*row++ = color;
		}
	}
}

static uint8_t
red(uint32_t v)
{
	return (v >> 16) & 0xff;
}

static uint8_t
blue(uint32_t v)
{
	return v & 0xff;
}

static bool
pixels_monotonic(const uint32_t *row, int x)
{
	bool ret = true;

	if (red(row[x + 1]) > red(row[x])) {
		testlog("pixel %d -> next: red value increases\n", x);
		ret = false;
	}

	if (blue(row[x + 1]) < blue(row[x])) {
		testlog("pixel %d -> next: blue value decreases\n", x);
		ret = false;
	}

	return ret;
}

static bool
check_blend_pattern(struct buffer *shot)
{
	bool ret = true;
	const int y = (BLOCK_WIDTH - 1) / 2; /* middle row */
	int x;
	void *pixels;
	int stride_bytes;
	uint32_t *row;

	assert(pixman_image_get_width(shot->image) >= BLOCK_WIDTH * ALPHA_STEPS);
	assert(pixman_image_get_height(shot->image) >= BLOCK_WIDTH);

	pixels = pixman_image_get_data(shot->image);
	stride_bytes = pixman_image_get_stride(shot->image);
	row = pixels + y * stride_bytes;

	for (x = 0; x < BLOCK_WIDTH * ALPHA_STEPS - 1; x++)
		if (!pixels_monotonic(row, x))
			ret = false;

	return ret;
}

/*
 * Test that alpha blending is roughly correct, and that an alpha ramp
 * results in a stricly monotonic color ramp. This should ensure that any
 * animation that varies alpha never goes "backwards" as that is easily
 * noticeable.
 *
 * The background is a constant color. On top of that, there is an
 * alpha-blended gradient with ramps in both alpha and color. Sub-surface
 * ensures the correct positioning and stacking.
 *
 * The gradient consists of ALPHA_STEPS number of blocks. Block size is
 * BLOCK_WIDTH x BLOCK_WIDTH and a block has a uniform color.
 *
 * In the blending result over x axis:
 * - red goes from 1.0 to 0.0, monotonic
 * - green is not monotonic
 * - blue goes from 0.0 to 1.0, monotonic
 */
TEST(alpha_blend_monotonic)
{
	const int width = BLOCK_WIDTH * ALPHA_STEPS;
	const int height = BLOCK_WIDTH;
	const pixman_color_t background_color = {
		.red   = 0xffff,
		.green = 0x8080,
		.blue  = 0x0000,
		.alpha = 0xffff
	};
	struct client *client;
	struct buffer *buf;
	struct wl_subcompositor *subco;
	struct wl_surface *surf;
	struct wl_subsurface *sub;
	struct buffer *shot;
	bool match;

	client = create_client();
	subco = bind_to_singleton_global(client, &wl_subcompositor_interface, 1);

	/* background window content */
	buf = create_shm_buffer_a8r8g8b8(client, width, height);
	fill_image_with_color(buf->image, &background_color);

	/* background window, main surface */
	client->surface = create_test_surface(client);
	client->surface->width = width;
	client->surface->height = height;
	client->surface->buffer = buf; /* pass ownership */
	set_opaque_rect(client, client->surface,
			&(struct rectangle){ 0, 0, width, height });

	/* foreground blended content */
	buf = create_shm_buffer_a8r8g8b8(client, width, height);
	fill_alpha_pattern(buf);

	/* foreground window, sub-surface */
	surf = wl_compositor_create_surface(client->wl_compositor);
	sub = wl_subcompositor_get_subsurface(subco, surf, client->surface->wl_surface);
	/* sub-surface defaults to position 0, 0, top-most, synchronized */
	wl_surface_attach(surf, buf->proxy, 0, 0);
	wl_surface_damage(surf, 0, 0, width, height);
	wl_surface_commit(surf);

	/* attach, damage, commit background window */
	move_client(client, 0, 0);

	shot = capture_screenshot_of_output(client);
	assert(shot);
	match = verify_image(shot, "alpha_blend_monotonic", 0, NULL, 0);
	assert(match);

	assert(check_blend_pattern(shot));
	buffer_destroy(shot);

	wl_subsurface_destroy(sub);
	wl_surface_destroy(surf);
	buffer_destroy(buf);
	client_destroy(client);
}
