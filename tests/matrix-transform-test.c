/*
 * Copyright © 2022 Collabora, Ltd.
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
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <wayland-client.h>
#include "libweston-internal.h"
#include "libweston/matrix.h"

#include "weston-test-client-helper.h"

static void
transform_expect(struct weston_matrix *a, bool valid, enum wl_output_transform ewt)
{
	enum wl_output_transform wt;

	assert(weston_matrix_to_transform(a, &wt) == valid);
	if (valid)
		assert(wt == ewt);
}

TEST(transformation_matrix)
{
	struct weston_matrix a, b;
	int i;

	weston_matrix_init(&a);
	weston_matrix_init(&b);

	weston_matrix_multiply(&a, &b);
	assert(a.type == 0);

	/* Make b a matrix that rotates a surface on the x,y plane by 90
	 * degrees counter-clockwise */
	weston_matrix_rotate_xy(&b, 0, -1);
	assert(b.type == WESTON_MATRIX_TRANSFORM_ROTATE);
	for (i = 0; i < 10; i++) {
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_90);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_180);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_270);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
	}

	weston_matrix_init(&b);
	/* Make b a matrix that rotates a surface on the x,y plane by 45
	 * degrees counter-clockwise. This should alternate between a
	 * standard transform and a rotation that fails to match any
	 * known rotations. */
	weston_matrix_rotate_xy(&b, cos(-M_PI / 4.0), sin(-M_PI / 4.0));
	assert(b.type == WESTON_MATRIX_TRANSFORM_ROTATE);
	for (i = 0; i < 10; i++) {
		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_90);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_180);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_270);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		assert(a.type == WESTON_MATRIX_TRANSFORM_ROTATE);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
	}

	weston_matrix_init(&b);
	/* Make b a matrix that rotates a surface on the x,y plane by 45
	 * degrees counter-clockwise. This should alternate between a
	 * standard transform and a rotation that fails to match any known
	 * rotations. */
	weston_matrix_rotate_xy(&b, cos(-M_PI / 4.0), sin(-M_PI / 4.0));
	/* Flip a */
	weston_matrix_scale(&a, -1.0, 1.0, 1.0);
	for (i = 0; i < 10; i++) {
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		/* Since we're not translated or scaled, any matrix that
		 * matches a standard wl_output_transform should not need
		 * filtering when used to transform images - but any
		 * matrix that fails to match will. */
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_90);
		assert(!weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_180);
		assert(!weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_270);
		assert(!weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED);
		assert(!weston_matrix_needs_filtering(&a));
	}

	weston_matrix_init(&a);
	/* Flip a around Y*/
	weston_matrix_scale(&a, 1.0, -1.0, 1.0);
	for (i = 0; i < 100; i++) {
		/* Throw some arbitrary translation in here to make sure it
		 * doesn't have any impact. */
		weston_matrix_translate(&a, 31.0, -25.0, 0.0);
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_270);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_90);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_180);
	}

	/* Scale shouldn't matter, as long as it's positive */
	weston_matrix_scale(&a, 4.0, 3.0, 1.0);
	/* Invert b so it rotates the opposite direction, go back the other way. */
	weston_matrix_invert(&b, &b);
	for (i = 0; i < 100; i++) {
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_90);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_270);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, false, 0);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_FLIPPED_180);
		assert(weston_matrix_needs_filtering(&a));
	}

	/* Flipping Y should return us from here to normal */
	weston_matrix_scale(&a, 1.0, -1.0, 1.0);
	transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);

	weston_matrix_init(&a);
	weston_matrix_init(&b);
	weston_matrix_translate(&b, 0.5, -0.75, 0);
	/* Crawl along with translations, 0.5 and .75 will both hit an integer multiple
	 * at the same time every 4th step, so assert that only the 4th steps don't need
	 * filtering */
	for (i = 0; i < 100; i++) {
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
		assert(weston_matrix_needs_filtering(&a));

		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
		assert(!weston_matrix_needs_filtering(&a));
	}

	weston_matrix_init(&b);
	weston_matrix_scale(&b, 1.5, 2.0, 1.0);
	for (i = 0; i < 10; i++) {
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
		assert(weston_matrix_needs_filtering(&a));
	}
	weston_matrix_invert(&b, &b);
	for (i = 0; i < 9; i++) {
		weston_matrix_multiply(&a, &b);
		transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
		assert(weston_matrix_needs_filtering(&a));
	}
	/* Last step should bring us back to a matrix that doesn't need
	 * a filter */
	weston_matrix_multiply(&a, &b);
	transform_expect(&a, true, WL_OUTPUT_TRANSFORM_NORMAL);
	assert(!weston_matrix_needs_filtering(&a));
}
