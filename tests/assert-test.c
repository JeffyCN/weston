/*
 * Copyright 2022 Collabora, Ltd.
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

#include <stdlib.h>
#include <stdarg.h>

#define custom_assert_fail_ test_assert_report

#include "shared/weston-assert.h"
#include "weston-test-runner.h"

__attribute__((format(printf, 2, 3)))
static void
test_assert_report(const struct weston_compositor *compositor, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void
abort_if_not(bool cond)
{
	if (!cond)
		abort();
}

struct my_type {
	int x;
	float y;
};

/* Demonstration of custom type comparison */
static int
my_type_cmp(const struct my_type *a, const struct my_type *b)
{
	if (a->x < b->x)
		return -1;
	if (a->x > b->x)
		return 1;
	if (a->y < b->y)
		return -1;
	if (a->y > b->y)
		return 1;
	return 0;
}

#define weston_assert_my_type_lt(compositor, a, b) \
	weston_assert_fn_(compositor, my_type_cmp, a, b, const struct my_type *, "my_type %p", <)

TEST(asserts)
{
	/* Unused by the macros for now, so let's just use NULL. */
	struct weston_compositor *compositor = NULL;
	bool ret;

	ret = weston_assert_true(compositor, false);
	abort_if_not(ret == false);

	ret = weston_assert_true(compositor, true);
	abort_if_not(ret);

	ret = weston_assert_true(compositor, true && false);
	abort_if_not(ret == false);

	ret = weston_assert_ptr(compositor, &ret);
	abort_if_not(ret);

	ret = weston_assert_ptr(compositor, NULL);
	abort_if_not(ret == false);

	ret = weston_assert_ptr_is_null(compositor, NULL);
	abort_if_not(ret);

	ret = weston_assert_ptr_is_null(compositor, &ret);
	abort_if_not(ret == false);

	ret = weston_assert_ptr_eq(compositor, &ret, &ret);
	abort_if_not(ret);

	ret = weston_assert_ptr_eq(compositor, &ret, &ret + 1);
	abort_if_not(ret == false);

	double fifteen = 15.0;
	ret = weston_assert_double_eq(compositor, fifteen, 15.000001);
	abort_if_not(ret == false);

	ret = weston_assert_double_eq(compositor, fifteen, 15);
	abort_if_not(ret);

	const char *nom = "bar";
	ret = weston_assert_str_eq(compositor, nom, "bar");
	abort_if_not(ret);
	ret = weston_assert_str_eq(compositor, nom, "baz");
	abort_if_not(ret == false);

	struct my_type a = { 1, 2.0 };
	struct my_type b = { 0, 2.0 };
	ret = weston_assert_my_type_lt(compositor, &b, &a);
	abort_if_not(ret);
	ret = weston_assert_my_type_lt(compositor, &a, &b);
	abort_if_not(ret == false);
}
