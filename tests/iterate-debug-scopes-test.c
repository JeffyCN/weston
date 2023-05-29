/*
 * Copyright 2023 Collabora, Ltd.
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

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libweston/weston-log.h>
#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"


static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
       struct compositor_setup setup;

       compositor_setup_defaults(&setup);
       setup.shell = SHELL_TEST_DESKTOP;

       return weston_test_harness_execute_as_plugin(harness, &setup);
}

DECLARE_FIXTURE_SETUP(fixture_setup);

static void
iterate_debug_scopes(struct weston_compositor *compositor)
{
	struct weston_log_scope *nscope = NULL;
	const char *test_harness_scope = "test-harness-plugin";
	bool found_test_harness_debug_scope = false;
	struct weston_log_context *log_ctx = compositor->weston_log_ctx;

	weston_log("Printing available debug scopes:\n");

	while ((nscope = weston_log_scopes_iterate(log_ctx, nscope))) {
		const char *scope_name;
		const char *desc_name;

		scope_name = weston_log_scope_get_name(nscope);
		assert(scope_name);

		desc_name = weston_log_scope_get_description(nscope);
		assert(desc_name);

		weston_log("\tscope name: %s, desc: %s\n", scope_name, desc_name);

		if (strcmp(test_harness_scope, scope_name) == 0)
			found_test_harness_debug_scope = true;
	}
	weston_log("\n");

	assert(found_test_harness_debug_scope);
}

PLUGIN_TEST(iterate_default_debug_scopes)
{
       iterate_debug_scopes(compositor);
}
