/*
 * Copyright © 2012 Intel Corporation
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

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#include <libweston/libweston.h>
#include <libweston/weston-log.h>
#include "backend.h"
#include "libweston-internal.h"
#include "compositor/weston.h"
#include "weston-test-server-protocol.h"
#include "weston.h"
#include "weston-testsuite-data.h"

#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "shared/xalloc.h"

#define MAX_TOUCH_DEVICES 32

struct weston_test {
	struct weston_compositor *compositor;
	struct wl_listener destroy_listener;

	struct weston_log_scope *log;

	struct weston_layer layer;
	struct weston_seat seat;
	struct weston_touch_device *touch_device[MAX_TOUCH_DEVICES];
	int nr_touch_devices;
	bool is_seat_initialized;

	pthread_t client_thread;
	struct wl_event_source *client_source;

	struct wl_list output_list;
	struct wl_listener output_created_listener;
	struct wl_listener output_destroyed_listener;
};

struct weston_test_surface {
	struct weston_surface *surface;
	struct wl_listener surface_destroy_listener;
	struct weston_view *view;
	int32_t x, y;
	struct weston_test *test;
};

struct weston_test_output {
	struct weston_test *test;
	struct weston_output *output;
	struct wl_listener repaint_listener;
	struct wl_list link;
};

static void
maybe_breakpoint(struct weston_test *test,
		 enum weston_test_breakpoint breakpoint,
		 void *resource)
{
	struct wet_test_pending_breakpoint *bp, *tmp;
	struct wet_testsuite_data *tsd = weston_compositor_get_test_data(test->compositor);

	wl_list_for_each_safe(bp, tmp, &tsd->breakpoints.list, link) {
		struct wet_test_active_breakpoint *active_bp;

		if (breakpoint != bp->breakpoint)
			continue;
		if (bp->resource && resource != bp->resource)
			continue;

		/* Remove this breakpoint from the list; ownership passes to
		 * the active breakpoint */
		wl_list_remove(&bp->link);

		/* The active breakpoint and the pending one which triggered it
		 * are now owned by the client */
		active_bp = xzalloc(sizeof(*active_bp));
		active_bp->compositor = test->compositor;
		active_bp->resource = resource;
		active_bp->template_ = bp;

		/* Wake the client with the active breakpoint, and wait for it
		 * to return control */
		tsd->breakpoints.active_bp = active_bp;
		wet_test_post_sem(&tsd->breakpoints.client_break);
		wet_test_wait_sem(&tsd->breakpoints.server_release);

		/* Only ever trigger a single breakpoint at a time */
		return;
	}
}

static void
output_repaint_listener(struct wl_listener *listener, void *data)
{
	struct weston_test_output *to =
		container_of(listener, struct weston_test_output,
			     repaint_listener);
	struct weston_head *head;

	wl_list_for_each(head, &to->output->head_list, output_link) {
		maybe_breakpoint(to->test, WESTON_TEST_BREAKPOINT_POST_REPAINT,
				 head);
	}
}

static void
output_created_listener(struct wl_listener *listener, void *data)
{
	struct weston_output *output = data;
	struct weston_test_output *to = xzalloc(sizeof(*to));
	struct weston_test *test =
		container_of(listener, struct weston_test,
			     output_created_listener);

	to->test = test;
	to->output = output;
	to->repaint_listener.notify = output_repaint_listener;
	wl_signal_add(&output->frame_signal, &to->repaint_listener);
	wl_list_insert(&test->output_list, &to->link);
}

static void
output_destroyed_listener(struct wl_listener *listener, void *data)
{
	struct weston_output *output = data;
	struct weston_test_output *to, *tmp;
	struct weston_test *test =
		container_of(listener, struct weston_test,
			     output_destroyed_listener);

	wl_list_for_each_safe(to, tmp, &test->output_list, link) {
		if (to->output != output)
			continue;

		wl_list_remove(&to->repaint_listener.link);
		wl_list_remove(&to->link);
		free(to);
	}
}

static void
touch_device_add(struct weston_test *test)
{
	char buf[128];
	int i = test->nr_touch_devices;

	assert(i < MAX_TOUCH_DEVICES);
	assert(!test->touch_device[i]);

	snprintf(buf, sizeof buf, "test-touch-device-%d", i);

	test->touch_device[i] = weston_touch_create_touch_device(
				test->seat.touch_state, buf, NULL, NULL);
	test->nr_touch_devices++;
}

static void
touch_device_remove(struct weston_test *test)
{
	int i = test->nr_touch_devices - 1;

	assert(i >= 0);
	assert(test->touch_device[i]);
	weston_touch_device_destroy(test->touch_device[i]);
	test->touch_device[i] = NULL;
	--test->nr_touch_devices;
}

static int
test_seat_init(struct weston_test *test)
{
	assert(!test->is_seat_initialized &&
	       "Trying to add already added test seat");

	/* create our own seat */
	weston_seat_init(&test->seat, test->compositor, "test-seat");
	test->is_seat_initialized = true;

	/* add devices */
	weston_seat_init_pointer(&test->seat);
	if (weston_seat_init_keyboard(&test->seat, NULL) < 0)
		return -1;
	weston_seat_init_touch(&test->seat);
	touch_device_add(test);

	return 0;
}

static void
test_seat_release(struct weston_test *test)
{
	while (test->nr_touch_devices > 0)
		touch_device_remove(test);

	assert(test->is_seat_initialized &&
	       "Trying to release already released test seat");
	test->is_seat_initialized = false;
	weston_seat_release(&test->seat);
	memset(&test->seat, 0, sizeof test->seat);
}

static struct weston_seat *
get_seat(struct weston_test *test)
{
	return &test->seat;
}

static void
notify_pointer_position(struct weston_test *test, struct wl_resource *resource)
{
	struct weston_seat *seat = get_seat(test);
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);

	weston_test_send_pointer_position(resource,
					  wl_fixed_from_double(pointer->pos.c.x),
					  wl_fixed_from_double(pointer->pos.c.y));
}

static void
test_surface_committed(struct weston_surface *surface,
		       struct weston_coord_surface new_origin)
{
	struct weston_test_surface *test_surface = surface->committed_private;
	struct weston_test *test = test_surface->test;
	struct weston_coord_global pos;

	weston_surface_map(test_surface->surface);

	pos.c = weston_coord(test_surface->x, test_surface->y);
	weston_view_set_position(test_surface->view, pos);

	if (wl_list_empty(&test_surface->view->layer_link.link)) {
		weston_view_move_to_layer(test_surface->view,
					  &test->layer.view_list);
	}

	weston_view_update_transform(test_surface->view);
}

static int
test_surface_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "test suite surface");
}

static void
test_surface_destroy(struct weston_test_surface *test_surface)
{
	weston_view_destroy(test_surface->view);

	test_surface->surface->committed = NULL;
	test_surface->surface->committed_private = NULL;
	weston_surface_set_label_func(test_surface->surface, NULL);

	wl_list_remove(&test_surface->surface_destroy_listener.link);
	free(test_surface);
}

static void
test_surface_handle_surface_destroy(struct wl_listener *l, void *data)
{
	struct weston_test_surface *test_surface =
		wl_container_of(l, test_surface, surface_destroy_listener);

	assert(test_surface->surface == data);

	test_surface_destroy(test_surface);
}

static struct weston_test_surface *
weston_test_surface_create(struct wl_resource *test_resource,
			   struct weston_surface *surface)
{
	struct wl_client *client = wl_resource_get_client(test_resource);
	struct wl_resource *display_resource;
	struct weston_test_surface *test_surface;

	test_surface = zalloc(sizeof *test_surface);
	if (!test_surface)
		goto err_post_no_mem;

	test_surface->surface = surface;
	test_surface->test = wl_resource_get_user_data(test_resource);

	test_surface->view = weston_view_create(surface);
	if (!test_surface->view)
		goto err_free_surface;

	/* Protocol does not define this error so abuse wl_display */
	display_resource = wl_client_get_object(client, 1);
	if (weston_surface_set_role(surface, "weston_test_surface",
				    display_resource,
				    WL_DISPLAY_ERROR_INVALID_OBJECT) < 0)
		goto err_free_view;

	surface->committed_private = test_surface;
	surface->committed = test_surface_committed;
	weston_surface_set_label_func(surface, test_surface_get_label);

	test_surface->surface_destroy_listener.notify =
		test_surface_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &test_surface->surface_destroy_listener);

	return test_surface;

err_free_view:
	weston_view_destroy(test_surface->view);

err_free_surface:
	free(test_surface);

err_post_no_mem:
	wl_resource_post_no_memory(test_resource);
	return NULL;
}

static void
move_surface(struct wl_client *client, struct wl_resource *resource,
	     struct wl_resource *surface_resource,
	     int32_t x, int32_t y)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct weston_test_surface *test_surface;
	struct wl_resource *display_resource;

	if (surface->committed &&
	    surface->committed != test_surface_committed) {
		display_resource = wl_client_get_object(client, 1);
		wl_resource_post_error(display_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "weston_test.move_surface: wl_surface@%u has a role.",
				       wl_resource_get_id(surface_resource));
		return;
	}

	test_surface = surface->committed_private;
	if (!test_surface)
		test_surface = weston_test_surface_create(resource, surface);
	if (!test_surface)
		return;

	test_surface->x = x;
	test_surface->y = y;
}

static void
move_pointer(struct wl_client *client, struct wl_resource *resource,
	     uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
	     int32_t x, int32_t y)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat = get_seat(test);
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct weston_pointer_motion_event event = { 0 };
	struct weston_coord_global pos;
	struct timespec time;

	pos.c = weston_coord(x, y);
	event = (struct weston_pointer_motion_event) {
		.mask = WESTON_POINTER_MOTION_REL,
		.rel = weston_coord_sub(pos.c, pointer->pos.c),
	};

	timespec_from_proto(&time, tv_sec_hi, tv_sec_lo, tv_nsec);

	notify_motion(seat, &time, &event);

	notify_pointer_position(test, resource);
}

static void
send_button(struct wl_client *client, struct wl_resource *resource,
	    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
	    int32_t button, uint32_t state)
{
	struct timespec time;

	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat = get_seat(test);

	timespec_from_proto(&time, tv_sec_hi, tv_sec_lo, tv_nsec);

	notify_button(seat, &time, button, state);
}

static void
send_axis(struct wl_client *client, struct wl_resource *resource,
	  uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
	  uint32_t axis, wl_fixed_t value)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat = get_seat(test);
	struct timespec time;
	struct weston_pointer_axis_event axis_event;

	timespec_from_proto(&time, tv_sec_hi, tv_sec_lo, tv_nsec);
	axis_event.axis = axis;
	axis_event.value = wl_fixed_to_double(value);
	axis_event.has_discrete = false;
	axis_event.discrete = 0;

	notify_axis(seat, &time, &axis_event);
}

static void
activate_surface(struct wl_client *client, struct wl_resource *resource,
		 struct wl_resource *surface_resource)
{
	struct weston_surface *surface = surface_resource ?
		wl_resource_get_user_data(surface_resource) : NULL;
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat;
	struct weston_keyboard *keyboard;

	seat = get_seat(test);
	keyboard = weston_seat_get_keyboard(seat);
	if (surface) {
		weston_seat_set_keyboard_focus(seat, surface);
		notify_keyboard_focus_in(seat, &keyboard->keys,
					 STATE_UPDATE_AUTOMATIC);
	}
	else {
		notify_keyboard_focus_out(seat);
		weston_seat_set_keyboard_focus(seat, surface);
	}
}

static void
send_key(struct wl_client *client, struct wl_resource *resource,
	 uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
	 uint32_t key, enum wl_keyboard_key_state state)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat = get_seat(test);
	struct timespec time;

	timespec_from_proto(&time, tv_sec_hi, tv_sec_lo, tv_nsec);

	notify_key(seat, &time, key, state, STATE_UPDATE_AUTOMATIC);
}

static void
device_release(struct wl_client *client,
	       struct wl_resource *resource, const char *device)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat = get_seat(test);

	if (strcmp(device, "pointer") == 0) {
		weston_seat_release_pointer(seat);
	} else if (strcmp(device, "keyboard") == 0) {
		weston_seat_release_keyboard(seat);
	} else if (strcmp(device, "touch") == 0) {
		touch_device_remove(test);
		weston_seat_release_touch(seat);
	} else if (strcmp(device, "seat") == 0) {
		test_seat_release(test);
	} else {
		assert(0 && "Unsupported device");
	}
}

static void
device_add(struct wl_client *client,
	   struct wl_resource *resource, const char *device)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_seat *seat = get_seat(test);

	if (strcmp(device, "pointer") == 0) {
		weston_seat_init_pointer(seat);
	} else if (strcmp(device, "keyboard") == 0) {
		weston_seat_init_keyboard(seat, NULL);
	} else if (strcmp(device, "touch") == 0) {
		weston_seat_init_touch(seat);
		touch_device_add(test);
	} else if (strcmp(device, "seat") == 0) {
		test_seat_init(test);
	} else {
		assert(0 && "Unsupported device");
	}
}

static void
send_touch(struct wl_client *client, struct wl_resource *resource,
	   uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
	   int32_t touch_id, wl_fixed_t x, wl_fixed_t y, uint32_t touch_type)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct weston_touch_device *device = test->touch_device[0];
	struct timespec time;
	struct weston_coord_global pos;

	assert(device);

	timespec_from_proto(&time, tv_sec_hi, tv_sec_lo, tv_nsec);

	if (touch_type == WL_TOUCH_UP) {
		if (x != 0 || y != 0) {
			wl_resource_post_error(resource,
					       WESTON_TEST_ERROR_TOUCH_UP_WITH_COORDINATE,
					       "Test protocol sent valid "
					       "coordinates with WL_TOUCH_UP");

			return;
		}

		notify_touch(device, &time, touch_id, NULL, touch_type);
	} else {
		pos.c = weston_coord_from_fixed(x, y);
		notify_touch(device, &time, touch_id, &pos, touch_type);
	}
}

static void
client_break(struct wl_client *client, struct wl_resource *resource,
	     uint32_t _breakpoint, uint32_t resource_id)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct wet_testsuite_data *tsd = weston_compositor_get_test_data(test->compositor);
	struct wet_test_pending_breakpoint *bp;
	enum weston_test_breakpoint breakpoint = _breakpoint;

	bp = calloc(1, sizeof(*bp));
	bp->breakpoint = breakpoint;

	if (resource_id != 0) {
		struct wl_resource *resource =
			wl_client_get_object(client, resource_id);
		assert(resource);
		bp->resource = wl_resource_get_user_data(resource);
	}

	wl_list_insert(&tsd->breakpoints.list, &bp->link);
}

static const struct weston_test_interface test_implementation = {
	move_surface,
	move_pointer,
	send_button,
	send_axis,
	activate_surface,
	send_key,
	device_release,
	device_add,
	send_touch,
	client_break,
};

static void
destroy_test(struct wl_resource *resource)
{
	struct weston_test *test = wl_resource_get_user_data(resource);
	struct wet_testsuite_data *tsd = weston_compositor_get_test_data(test->compositor);

	assert(tsd->wl_client);
	tsd->wl_client = NULL;
}

static void
bind_test(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct weston_test *test = data;
	struct wet_testsuite_data *tsd = weston_compositor_get_test_data(test->compositor);
	struct wl_resource *resource;

	resource = wl_resource_create(client, &weston_test_interface, 1, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource,
				       &test_implementation, test,
				       destroy_test);

	/* There can only be one wl_client bound */
	assert(!tsd->wl_client);
	tsd->wl_client = client;
	notify_pointer_position(test, resource);
}

static void
client_thread_cleanup(void *data_)
{
	struct wet_testsuite_data *data = data_;

	close(data->thread_event_pipe);
	data->thread_event_pipe = -1;
}

static void *
client_thread_routine(void *data_)
{
	struct wet_testsuite_data *data = data_;

	pthread_setname_np(pthread_self(), "client");
	pthread_cleanup_push(client_thread_cleanup, data);
	data->run(data);
	pthread_cleanup_pop(true);

	return NULL;
}

static void
client_thread_join(struct weston_test *test)
{
	assert(test->client_source);

	pthread_join(test->client_thread, NULL);
	wl_event_source_remove(test->client_source);
	test->client_source = NULL;

	weston_log_scope_printf(test->log, "Test thread reaped.\n");
}

static int
handle_client_thread_event(int fd, uint32_t mask, void *data_)
{
	struct weston_test *test = data_;

	weston_log_scope_printf(test->log,
				"Received thread event mask 0x%x\n", mask);

	if (mask != WL_EVENT_HANGUP)
		weston_log("%s: unexpected event %u\n", __func__, mask);

	client_thread_join(test);
	weston_compositor_exit(test->compositor);

	return 0;
}

static int
create_client_thread(struct weston_test *test, struct wet_testsuite_data *data)
{
	struct wl_event_loop *loop;
	int pipefd[2] = { -1, -1 };
	sigset_t saved;
	sigset_t blocked;
	int ret;

	weston_log_scope_printf(test->log, "Creating a thread for running tests...\n");

	if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) {
		weston_log("Creating pipe for a client thread failed: %s\n",
			   strerror(errno));
		return -1;
	}

	loop = wl_display_get_event_loop(test->compositor->wl_display);
	test->client_source = wl_event_loop_add_fd(loop, pipefd[0],
						   WL_EVENT_READABLE,
						   handle_client_thread_event,
						   test);
	close(pipefd[0]);

	if (!test->client_source) {
		weston_log("Adding client thread fd to event loop failed.\n");
		goto out_pipe;
	}

	data->thread_event_pipe = pipefd[1];

	ret = sem_init(&data->breakpoints.client_break, 0, 0);
	if (ret != 0) {
		weston_log("Creating breakpoint semaphore failed: %s (%d)\n",
			   strerror(errno), errno);
		goto out_source;
	}

	ret = sem_init(&data->breakpoints.server_release, 0, 0);
	if (ret != 0) {
		weston_log("Creating release semaphore failed: %s (%d)\n",
			   strerror(errno), errno);
		goto out_source;
	}


	/* Ensure we don't accidentally get signals to the thread. */
	sigfillset(&blocked);
	sigdelset(&blocked, SIGSEGV);
	sigdelset(&blocked, SIGFPE);
	sigdelset(&blocked, SIGILL);
	sigdelset(&blocked, SIGCONT);
	sigdelset(&blocked, SIGSYS);
	if (pthread_sigmask(SIG_BLOCK, &blocked, &saved) != 0)
		goto out_source;

	ret = pthread_create(&test->client_thread, NULL,
			     client_thread_routine, data);

	pthread_sigmask(SIG_SETMASK, &saved, NULL);

	if (ret != 0) {
		weston_log("Creating client thread failed: %s (%d)\n",
			   strerror(ret), ret);
		goto out_source;
	}

	return 0;

out_source:
	data->thread_event_pipe = -1;
	wl_event_source_remove(test->client_source);
	test->client_source = NULL;

out_pipe:
	close(pipefd[1]);

	return -1;
}

static void
idle_launch_testsuite(void *test_)
{
	struct weston_test *test = test_;
	struct wet_testsuite_data *data = weston_compositor_get_test_data(test->compositor);

	if (!data)
		return;

	wl_list_init(&data->breakpoints.list);

	switch (data->type) {
	case TEST_TYPE_CLIENT:
		if (create_client_thread(test, data) < 0) {
			weston_log("Error: creating client thread for test suite failed.\n");
			weston_compositor_exit_with_code(test->compositor,
							 RESULT_HARD_ERROR);
		}
		break;

	case TEST_TYPE_PLUGIN:
		data->compositor = test->compositor;
		weston_log_scope_printf(test->log,
					"Running tests from idle handler...\n");
		data->run(data);
		weston_compositor_exit(test->compositor);
		break;

	case TEST_TYPE_STANDALONE:
		weston_log("Error: unknown test internal type %d.\n",
			   data->type);
		weston_compositor_exit_with_code(test->compositor,
						 RESULT_HARD_ERROR);
	}
}

static void
handle_compositor_destroy(struct wl_listener *listener,
			  void *weston_compositor)
{
	struct weston_compositor *compositor = weston_compositor;
	struct wet_testsuite_data *data;
	struct weston_test *test;
	struct weston_output *output;

	test = wl_container_of(listener, test, destroy_listener);
	data = weston_compositor_get_test_data(test->compositor);

	wl_list_remove(&test->destroy_listener.link);
	wl_list_remove(&test->output_created_listener.link);
	wl_list_remove(&test->output_destroyed_listener.link);
	wl_list_for_each(output, &compositor->output_list, link) {
		output_destroyed_listener(&test->output_destroyed_listener,
					  output);
	}

	if (test->client_source) {
		weston_log_scope_printf(test->log, "Cancelling client thread...\n");
		pthread_cancel(test->client_thread);
		client_thread_join(test);
	}

	if (test->is_seat_initialized)
		test_seat_release(test);

	data->wl_client = NULL;

	wl_list_remove(&test->layer.view_list.link);
	wl_list_remove(&test->layer.link);

	weston_log_scope_destroy(test->log);
	free(test);
}

WL_EXPORT int
wet_module_init(struct weston_compositor *ec,
		int *argc, char *argv[])
{
	struct weston_test *test;
	struct weston_output *output;
	struct wl_event_loop *loop;

	test = zalloc(sizeof *test);
	if (test == NULL)
		return -1;

	if (!weston_compositor_add_destroy_listener_once(ec,
							 &test->destroy_listener,
							 handle_compositor_destroy)) {
		free(test);
		return 0;
	}

	test->compositor = ec;
	weston_layer_init(&test->layer, ec);
	weston_layer_set_position(&test->layer, WESTON_LAYER_POSITION_CURSOR - 1);

	wl_list_init(&test->output_list);
	wl_list_for_each(output, &ec->output_list, link)
		output_created_listener(&test->output_created_listener, output);
	test->output_created_listener.notify = output_created_listener;
	wl_signal_add(&ec->output_created_signal, &test->output_created_listener);
	test->output_destroyed_listener.notify = output_destroyed_listener;
	wl_signal_add(&ec->output_destroyed_signal,
		      &test->output_destroyed_listener);

	test->log = weston_compositor_add_log_scope(ec, "test-harness-plugin",
					"weston-test plugin's own actions",
					NULL, NULL, NULL);

	if (wl_global_create(ec->wl_display, &weston_test_interface, 1,
			     test, bind_test) == NULL)
		goto out_free;

	if (test_seat_init(test) == -1)
		goto out_free;

	loop = wl_display_get_event_loop(ec->wl_display);
	wl_event_loop_add_idle(loop, idle_launch_testsuite, test);

	return 0;

out_free:
	wl_list_remove(&test->destroy_listener.link);
	free(test);
	return -1;
}
