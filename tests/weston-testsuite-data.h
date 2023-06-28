/*
 * Copyright 2019 Collabora, Ltd.
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

#ifndef WESTON_TESTSUITE_DATA_H
#define WESTON_TESTSUITE_DATA_H

#include <assert.h>
#include <errno.h>
#include <semaphore.h>

/** Standard return codes
 *
 * Both Autotools and Meson use these codes as test program exit codes
 * to denote the test result for the whole process.
 *
 * \ingroup testharness
 */
enum test_result_code {
	RESULT_OK = 0,
	RESULT_SKIP = 77,
	RESULT_FAIL = 1,
	RESULT_HARD_ERROR = 99,
};

struct weston_test;
struct weston_compositor;

/** Weston test types
 *
 * \sa weston_test_harness_execute_standalone
 * weston_test_harness_execute_as_plugin
 * weston_test_harness_execute_as_client
 *
 * \ingroup testharness_private
 */
enum test_type {
	TEST_TYPE_STANDALONE,
	TEST_TYPE_PLUGIN,
	TEST_TYPE_CLIENT,
};

/** Safely handle posting a semaphore to wake a waiter
 *
 * \ingroup testharness_private
 */
static inline void wet_test_post_sem(sem_t *sem)
{
	int ret = sem_post(sem);
	assert(ret == 0); /* only fails on programming errors */
}

/** Safely handle waiting on a semaphore
 *
 * \ingroup testharness_private
 */
static inline void wet_test_wait_sem(sem_t *sem)
{
	int ret;

	do {
		ret = sem_wait(sem);
	} while (ret != 0 && errno == EINTR);
	assert(ret == 0); /* all other failures are programming errors */
}

/** An individual breakpoint set for the server
 *
 * This breakpoint data is created and placed in a list by either the server
 * (when handling protocol messages) or the client (when directly manipulating
 * the list during a breakpoint).
 *
 * It must be freed by the client.
 *
 * \ingroup testharness_private
 */
struct wet_test_pending_breakpoint {
	/** breakpoint type - enum weston_test_breakpoint from protocol */
	uint32_t breakpoint;
	/** type-specific resource to filter on (optional) */
	void *resource;
	struct wl_list link; /**< wet_testsuite_breakpoints.list */
};

/** Information about the server's active breakpoint
 *
 * This breakpoint data is created by the server and passed to the client when
 * the server enters a breakpoint.
 *
 * It must be freed by the client.
 *
 * \ingroup testharness_private
 */
struct wet_test_active_breakpoint {
	/** libweston compositor instance in use */
	struct weston_compositor *compositor;
	/** type-specific pointer to resource which triggered this breakpoint */
	void *resource;
	/** on release, reinsert the template to trigger next time */
	bool rearm_on_release;
	/** client's original breakpoint request */
	struct wet_test_pending_breakpoint *template_;
};

/** Client/compositor synchronisation for breakpoint state
 *
 * Manages the set of active breakpoints placed for the server, as well as
 * signalling the pausing/continuing of server actions.
 *
 * \ingroup testharness_private
 */
struct wet_testsuite_breakpoints {
	/** signalled by the server when it reaches a breakpoint */
	sem_t client_break;
	/** signalled by the client to resume server execution */
	sem_t server_release;

	/** Pushed by the server when a breakpoint is triggered, immediately
	  * before it signals the client_break semaphore. Client consumes this
	  * and takes ownership after the wait succeeds. */
	struct wet_test_active_breakpoint *active_bp;

	/** client-internal state; set by consuming active_bp, cleared by
	  * signalling server_release */
	bool in_client_break;

	/** list of pending breakpoints: owned by the server during normal
	  * execution (ordinarily added to by a protocol request, and
	  * traversed to find a possible breakpoint to trigger), and owned by
	  * the client wtihin a breakpoint (pending breakpoints may be added
	  * or removed). Members are wet_test_pending_breakpoint.link */
	struct wl_list list;
};

/** Test harness specific data for running tests
 *
 * \ingroup testharness_private
 */
struct wet_testsuite_data {
	void (*run)(struct wet_testsuite_data *);

	void *wl_client;

	/* test definitions */
	const struct weston_test_entry *tests;
	unsigned tests_count;
	int case_index;
	enum test_type type;
	struct weston_compositor *compositor;

	/* client thread control */
	int thread_event_pipe;
	struct wet_testsuite_breakpoints breakpoints;

	/* informational run state */
	int fixture_iteration;
	const char *fixture_name;

	/* test counts */
	unsigned counter;
	unsigned passed;
	unsigned skipped;
	unsigned failed;
	unsigned total;
};

#endif /* WESTON_TESTSUITE_DATA_H */
