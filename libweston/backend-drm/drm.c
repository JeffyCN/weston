/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <assert.h>
#include <sys/mman.h>
#include <time.h>
#include <poll.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libudev.h>

#include <libweston/libweston.h>
#include <libweston/backend-drm.h>
#include <libweston/weston-log.h>
#include "colorops.h"
#include "drm-internal.h"
#include "shared/hash.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "shared/string-helpers.h"
#include "shared/weston-assert.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/xalloc.h"
#include "output-capture.h"
#include "weston-trace.h"
#include "pixman-renderer.h"
#include "pixel-formats.h"
#include "libbacklight.h"
#include "libinput-seat.h"
#include "launcher-util.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "linux-explicit-synchronization.h"

static const char default_seat[] = "seat0";

void
drm_device_recovery_schedule(struct drm_device *device)
{
	struct drm_backend *backend = device->backend;
	struct weston_compositor *compositor = backend->compositor;

	assert(device->recovery_status == DRM_RECOVERY_WAIT_FOR_IDLE);
	assert(!device->atomic_completes_pending);

	device->recovery_status = DRM_RECOVERY_SCHEDULED;

	/* Perhaps a little aggressive if there are multiple backends,
	 * but we're probably not too interested in optimal performance
	 * if we need to regenerate this backend's state from scratch.
	 */
	weston_compositor_damage_all(compositor);

	/* Without atomics, the first head to repaint is a recovery */
	if (!device->atomic_modeset)
		return;

	/* We've scheduled updates on everything we know about, so defer anything
	 * else until that completes.
	 */
	weston_backend_set_deferred(&backend->base);
}

void
drm_device_recovery_required(struct drm_device *device)
{
	if (device->recovery_status != DRM_RECOVERY_UNNECESSARY)
		return;

	device->recovery_status = DRM_RECOVERY_WAIT_FOR_IDLE;

	if (device->atomic_completes_pending)
		return;

	drm_device_recovery_schedule(device);
}

void
drm_device_recovery_complete(struct drm_device *device)
{
	struct drm_backend *backend = device->backend;
	struct weston_compositor *compositor = backend->compositor;

	assert(device->recovery_status == DRM_RECOVERY_APPLIED);

	device->recovery_status = DRM_RECOVERY_UNNECESSARY;

	/* Without atomics, we didn't defer anything, so bail now. */
	if (!device->atomic_modeset)
		return;

	weston_backend_clear_deferred(&backend->base, compositor);
}

static void
drm_backend_create_faked_zpos(struct drm_device *device)
{
	struct drm_backend *b = device->backend;
	struct drm_plane *plane, *tmp;
	struct wl_list tmp_list;
	uint64_t zpos = 0ULL;
	uint64_t zpos_min_primary;
	uint64_t zpos_min_overlay;
	uint64_t zpos_min_cursor;

	/* if the property is there, bail out sooner */
	wl_list_for_each(plane, &device->plane_list, link) {
		if (plane->props[WDRM_PLANE_ZPOS].prop_id != 0)
			return;
	}

	drm_debug(b, "[drm-backend] zpos property not found. "
		     "Using invented immutable zpos values:\n");

	wl_list_init(&tmp_list);
	wl_list_insert_list(&tmp_list, &device->plane_list);
	wl_list_init(&device->plane_list);

	zpos_min_primary = zpos;
	wl_list_for_each_safe(plane, tmp, &tmp_list, link) {
		if (plane->type != WDRM_PLANE_TYPE_PRIMARY)
			continue;

		plane->zpos_min = zpos_min_primary;
		plane->zpos_max = zpos_min_primary;
		wl_list_remove(&plane->link);
		wl_list_insert(&device->plane_list, &plane->link);
		zpos++;

		drm_debug(b, "\t[plane] %s plane %d, zpos_min %"PRIu64", "
			      "zpos_max %"PRIu64"\n",
			      drm_output_get_plane_type_name(plane),
			      plane->plane_id, plane->zpos_min, plane->zpos_max);
	}

	zpos_min_overlay = zpos;
	wl_list_for_each_safe(plane, tmp, &tmp_list, link) {
		if (plane->type != WDRM_PLANE_TYPE_OVERLAY)
			continue;

		plane->zpos_min = zpos_min_overlay;
		plane->zpos_max = zpos_min_overlay;
		wl_list_remove(&plane->link);
		wl_list_insert(&device->plane_list, &plane->link);
		zpos++;

		drm_debug(b, "\t[plane] %s plane %d, zpos_min %"PRIu64", "
			      "zpos_max %"PRIu64"\n",
			      drm_output_get_plane_type_name(plane),
			      plane->plane_id, plane->zpos_min, plane->zpos_max);
	}

	zpos_min_cursor = zpos;
	wl_list_for_each_safe(plane, tmp, &tmp_list, link) {
		if (plane->type != WDRM_PLANE_TYPE_CURSOR)
			continue;

		plane->zpos_min = zpos_min_cursor;
		plane->zpos_max = zpos_min_cursor;
		wl_list_remove(&plane->link);
		wl_list_insert(&device->plane_list, &plane->link);
		zpos++;

		drm_debug(b, "\t[plane] %s plane %d, zpos_min %"PRIu64", "
			      "zpos_max %"PRIu64"\n",
			      drm_output_get_plane_type_name(plane),
			      plane->plane_id, plane->zpos_min, plane->zpos_max);
	}

	assert(wl_list_empty(&tmp_list));
}

static int
pageflip_timeout(void *data) {
	/*
	 * Our timer just went off, that means we're not receiving drm
	 * page flip events anymore for that output. Let's gracefully exit
	 * weston with a return value so devs can debug what's going on.
	 */
	struct drm_output *output = data;
	struct weston_compositor *compositor = output->base.compositor;

	weston_log("Pageflip timeout reached on output %s, your "
	           "driver is probably buggy!  Exiting.\n",
		   output->base.name);
	weston_compositor_exit_with_code(compositor, EXIT_FAILURE);

	return 0;
}

/* Creates the pageflip timer. Note that it isn't armed by default */
static int
drm_output_pageflip_timer_create(struct drm_output *output)
{
	struct wl_event_loop *loop = NULL;
	struct weston_compositor *ec = output->base.compositor;

	loop = wl_display_get_event_loop(ec->wl_display);
	assert(loop);
	output->pageflip_timer = wl_event_loop_add_timer(loop,
	                                                 pageflip_timeout,
	                                                 output);

	if (output->pageflip_timer == NULL) {
		weston_log("creating drm pageflip timer failed: %s\n",
			   strerror(errno));
		return -1;
	}

	return 0;
}

static int
pageflip_timer_counter_handler(void *data)
{
	struct drm_backend *b = data;
	struct weston_compositor *ec = b->compositor;
	struct weston_output *output_base;

	wl_list_for_each(output_base, &ec->output_list, link) {
		struct drm_output *output = to_drm_output(output_base);
		char desc[1024];

		/* Skip outputs on other backends */
		if (!output)
			continue;

		output->page_flips_per_timer_interval =
			(float) (output->page_flips_counted /
					b->perf_page_flips_stats.frame_counter_interval);

		snprintf(desc, sizeof(desc),
			 "output %s KMS page flips", output_base->name);

		WESTON_TRACE_SET_COUNTER(desc,
					output->page_flips_per_timer_interval);

		output->page_flips_counted = 0;
	}


	wl_event_source_timer_update(b->perf_page_flips_stats.pageflip_timer_counter,
				     1000 * b->perf_page_flips_stats.frame_counter_interval);

	return 0;
}

static int
drm_backend_pageflip_counter_timer_create(struct drm_backend *b, uint32_t interval)
{
	struct wl_event_loop *loop = NULL;
	struct weston_compositor *ec = b->compositor;

	loop = wl_display_get_event_loop(ec->wl_display);
	assert(loop);

	b->perf_page_flips_stats.pageflip_timer_counter =
		wl_event_loop_add_timer(loop, pageflip_timer_counter_handler, b);

	if (b->perf_page_flips_stats.pageflip_timer_counter == NULL) {
		weston_log("creating drm pageflip counter timer failed: %s\n",
			   strerror(errno));
		return -1;
	}

	b->perf_page_flips_stats.frame_counter_interval = interval;

	return 0;
}

static void
drm_backend_pageflip_counter_timer_arm(struct drm_backend *b)
{
	if (!b->perf_page_flips_stats.pageflip_timer_counter)
		return;

	wl_event_source_timer_update(b->perf_page_flips_stats.pageflip_timer_counter,
				     1000 * b->perf_page_flips_stats.frame_counter_interval);

	b->perf_page_flips_stats.timer_armed = true;
}

static void
drm_backend_pageflip_counter_timer_disarm(struct drm_backend *b)
{
	if (!b->perf_page_flips_stats.pageflip_timer_counter)
		return;

	/* do not disarm the timer if there are subscriptions to this log scope */
	if (weston_log_scope_is_enabled(b->debug))
		return;

	wl_event_source_timer_update(b->perf_page_flips_stats.pageflip_timer_counter, 0);

	b->perf_page_flips_stats.timer_armed = false;
}

static void
drm_backend_pageflip_counter_timer_disable_cb(struct weston_log_subscription *sub, void *data)
{
	struct drm_backend *b = data;

	if (b->perf_page_flips_stats.timer_armed)
		drm_backend_pageflip_counter_timer_disarm(b);
}

static void
drm_backend_pageflip_counter_timer_arm_cb(struct weston_log_subscription *sub, void *data)
{
	struct drm_backend *b = data;

	if (!b->perf_page_flips_stats.timer_armed)
		drm_backend_pageflip_counter_timer_arm(b);
}

/**
 * Returns true if the plane can be used on the given output for its current
 * repaint cycle.
 */
bool
drm_plane_is_available(struct drm_plane *plane, struct drm_output *output)
{
	assert(plane->state_cur);

	if (output->is_virtual)
		return false;

	/* The plane still has a request not yet completed by the kernel. */
	if (!plane->state_cur->complete)
		return false;

	/* The plane is still active on another output. */
	if (plane->state_cur->handle && plane->state_cur->handle->output != output)
		return false;

	/* This plane is not the primary plane for this CRTC. */
	if (plane->type == WDRM_PLANE_TYPE_PRIMARY &&
	    plane->plane_id != output->crtc->primary_plane_id)
		return false;

	/* Check whether the plane can be used with this CRTC; possible_crtcs
	 * is a bitmask of CRTC indices (pipe), rather than CRTC object ID. */
	return !!(plane->possible_crtcs & (1 << output->crtc->pipe));
}

struct drm_crtc *
drm_crtc_find(struct drm_device *device, uint32_t crtc_id)
{
	struct drm_crtc *crtc;

	wl_list_for_each(crtc, &device->crtc_list, link) {
		if (crtc->crtc_id == crtc_id)
			return crtc;
	}

	return NULL;
}

struct drm_head *
drm_head_find_by_connector(struct drm_backend *backend, struct drm_device *device, uint32_t connector_id)
{
	struct weston_head *base;
	struct drm_head *head;

	wl_list_for_each(base,
			 &backend->compositor->head_list, compositor_link) {
		head = to_drm_head(base);
		if (!head)
			continue;

		if (head->connector.device != device)
			continue;

		if (head->connector.connector_id != connector_id)
			continue;

		return head;
	}

	return NULL;
}

static struct drm_writeback *
drm_writeback_find_by_connector(struct drm_device *device, uint32_t connector_id)
{
	struct drm_writeback *writeback;

	wl_list_for_each(writeback, &device->writeback_connector_list, link) {
		if (writeback->connector.connector_id == connector_id)
			return writeback;
	}

	return NULL;
}

/**
 * Get output state to disable output
 *
 * Returns a pointer to an output_state object which can be used to disable
 * an output (e.g. DPMS off).
 *
 * @param pending_state The pending state object owning this update
 * @param output The output to disable
 * @returns A drm_output_state to disable the output
 */
static struct drm_output_state *
drm_output_get_disable_state(struct drm_pending_state *pending_state,
			     struct drm_output *output)
{
	struct drm_output_state *output_state;

	output_state = drm_output_state_duplicate(output->state_cur,
						  pending_state,
						  DRM_OUTPUT_STATE_CLEAR_PLANES);
	output_state->dpms = WESTON_DPMS_OFF;

	output_state->protection = WESTON_HDCP_DISABLE;

	return output_state;
}

static int
drm_output_apply_mode(struct drm_output *output);

/**
 * Mark a drm_output_state (the output's last state) as complete. This handles
 * any post-completion actions such as updating the repaint timer, disabling the
 * output, and finally freeing the state.
 */
void
drm_output_update_complete(struct drm_output *output, uint32_t flags,
			   unsigned int sec, unsigned int usec)
{
	struct drm_device *device = output->device;
	struct drm_plane_state *ps;
	struct timespec ts;

	/* Stop the pageflip timer instead of rearming it here */
	if (output->pageflip_timer)
		wl_event_source_timer_update(output->pageflip_timer, 0);

	drm_debug(device->backend, "output %s update complete at %u.%06u s, flags %#x\n",
		  output->base.name, sec, usec, flags);

	wl_list_for_each(ps, &output->state_cur->plane_list, link)
		ps->complete = true;

	drm_output_state_free(output->state_last);
	output->state_last = NULL;

	/* Clean up dummy framebuffer after page flip */
	if (output->fb_dummy) {
		drm_fb_unref(output->fb_dummy);
		output->fb_dummy = NULL;
	}

	if (output->destroy_pending) {
		output->destroy_pending = false;
		output->disable_pending = false;
		output->dpms_off_pending = false;
		output->mode_switch_pending = false;
		drm_output_destroy(&output->base);
		return;
	} else if (output->disable_pending) {
		output->disable_pending = false;
		output->dpms_off_pending = false;
		output->mode_switch_pending = false;
		weston_output_disable(&output->base);
		return;
	} else if (output->dpms_off_pending) {
		struct drm_pending_state *pending = drm_pending_state_alloc(device);
		output->dpms_off_pending = false;
		output->mode_switch_pending = false;
		drm_output_get_disable_state(pending, output);
		drm_pending_state_apply_sync(pending);
	} else if (output->mode_switch_pending) {
		output->mode_switch_pending = false;
		drm_output_apply_mode(output);
	}
	if (output->state_cur->dpms == WESTON_DPMS_OFF &&
	    output->base.repaint_status != REPAINT_AWAITING_COMPLETION) {
		/* DPMS can happen to us either in the middle of a repaint
		 * cycle (when we have painted fresh content, only to throw it
		 * away for DPMS off), or at any other random point. If the
		 * latter is true, then we cannot go through finish_frame,
		 * because the repaint machinery does not expect this. */
		return;
	}

	if (output->state_cur->tear)
		flags |= WESTON_FINISH_FRAME_TEARING;

	ts.tv_sec = sec;
	ts.tv_nsec = usec * 1000;

	if (output->state_cur->dpms != WESTON_DPMS_OFF)
		weston_output_finish_frame(&output->base, &ts, flags);
	else
		weston_output_finish_frame(&output->base, NULL,
					   WP_PRESENTATION_FEEDBACK_INVALID);
}

static struct drm_fb *
drm_output_render_pixman(struct drm_output_state *state,
			 pixman_region32_t *damage)
{
	struct drm_output *output = state->output;
	struct weston_compositor *ec = output->base.compositor;

	output->current_image ^= 1;

	ec->renderer->repaint_output(&output->base, damage,
				     output->renderbuffer[output->current_image]);

	return drm_fb_ref(output->dumb[output->current_image]);
}

void
drm_output_render(struct drm_output_state *state)
{
	struct drm_output *output = state->output;
	struct drm_device *device = output->device;
	struct weston_compositor *c = output->base.compositor;
	struct drm_plane_state *scanout_state;
	struct drm_plane *scanout_plane = output->scanout_handle->plane;
	struct drm_property_info *damage_info =
		&scanout_plane->props[WDRM_PLANE_FB_DAMAGE_CLIPS];
	struct drm_mode *mode;
	struct drm_fb *fb;
	enum wdrm_plane_blend blend_mode = WDRM_PLANE_BLEND__COUNT;
	pixman_region32_t damage, scanout_damage;
	pixman_box32_t *rects;
	int n_rects;

	scanout_state = drm_output_state_get_plane(state, scanout_plane);
	weston_assert_ptr_null(c, scanout_state->fb);

	switch (output->base.fb_alpha_encoding) {
	case WESTON_OUTPUT_FB_ALPHA_PREMULT:
		blend_mode = WDRM_PLANE_BLEND_PREMULT;
		break;
	case WESTON_OUTPUT_FB_ALPHA_STRAIGHT:
		blend_mode = WDRM_PLANE_BLEND_COVERAGE;
		break;
	}
	weston_assert_enum_ne(c, blend_mode, WDRM_PLANE_BLEND__COUNT);
	weston_assert_true(c, drm_plane_supports_blend_mode(scanout_plane, blend_mode));
	scanout_state->blend_mode = blend_mode;

	pixman_region32_init(&damage);

	weston_output_flush_damage_for_primary_plane(&output->base, &damage);

	/*
	 * If we don't have any damage on the primary plane, and we already
	 * have a renderer buffer active, we can reuse it; else we pass
	 * the damaged region into the renderer to re-render the affected
	 * area. But, we still have to call the renderer anyway if any screen
	 * capture is pending, otherwise the capture will not complete.
	 */
	if (!pixman_region32_not_empty(&damage) &&
	    wl_list_empty(&output->base.frame_signal.listener_list) &&
	    !weston_output_has_renderer_capture_tasks(&output->base) &&
	    scanout_plane->state_cur->fb &&
	    (scanout_plane->state_cur->fb->type == BUFFER_GBM_SURFACE ||
	     scanout_plane->state_cur->fb->type == BUFFER_PIXMAN_DUMB ||
	     scanout_plane->state_cur->fb->type == BUFFER_DMABUF_BACKEND)) {
		fb = drm_fb_ref(scanout_plane->state_cur->fb);
	} else if (c->renderer->type == WESTON_RENDERER_PIXMAN) {
		fb = drm_output_render_pixman(state, &damage);
	} else if (c->renderer->type == WESTON_RENDERER_GL) {
		fb = drm_output_render_gl(state, &damage);
	} else if (c->renderer->type == WESTON_RENDERER_VULKAN) {
		fb = drm_output_render_vulkan(state, &damage);
	} else assert(0);

	if (!fb) {
		drm_plane_state_put_back(scanout_state);
		goto out;
	}

	scanout_state->fb = fb;
	scanout_state->handle = output->scanout_handle;

	scanout_state->src_x = 0;
	scanout_state->src_y = 0;
	scanout_state->src_w = fb->width << 16;
	scanout_state->src_h = fb->height << 16;

	mode = to_drm_mode(output->base.current_mode);
	scanout_state->dest_x = 0;
	scanout_state->dest_y = 0;
	scanout_state->dest_w = mode->mode_info.hdisplay;
	scanout_state->dest_h = mode->mode_info.vdisplay;

	scanout_state->zpos = scanout_plane->zpos_min;

	/* Don't bother calculating plane damage if the plane doesn't support it */
	if (damage_info->prop_id == 0)
		goto out;

	pixman_region32_init(&scanout_damage);

	weston_region_global_to_output(&scanout_damage,
				       &output->base,
				       &damage);

	assert(scanout_state->damage_blob_id == 0);

	rects = pixman_region32_rectangles(&scanout_damage, &n_rects);

	/*
	 * If this function fails, the blob id should still be 0.
	 * This tells the kernel there is no damage information, which means
	 * that it will consider the whole plane damaged. While this may
	 * affect efficiency, it should still produce correct results.
	 */
	drmModeCreatePropertyBlob(device->kms_device->fd, rects,
				  sizeof(*rects) * n_rects,
				  &scanout_state->damage_blob_id);

	pixman_region32_fini(&scanout_damage);
out:
	pixman_region32_fini(&damage);
}

static uint32_t
drm_connector_get_possible_crtcs_mask(struct drm_connector *connector)
{
	struct drm_device *device = connector->device;
	uint32_t possible_crtcs = 0;
	drmModeConnector *conn = connector->conn;
	drmModeEncoder *encoder;
	int i;

	for (i = 0; i < conn->count_encoders; i++) {
		encoder = drmModeGetEncoder(device->kms_device->fd,
					    conn->encoders[i]);
		if (!encoder)
			continue;

		possible_crtcs |= encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);
	}

	return possible_crtcs;
}

static struct drm_writeback *
drm_output_find_compatible_writeback(struct drm_output *output,
				     const struct pixel_format_info *pixel_format)
{
	struct drm_crtc *crtc;
	struct drm_writeback *wb;
	bool in_use;
	uint32_t possible_crtcs;

	wl_list_for_each(wb, &output->device->writeback_connector_list, link) {
		/* Another output may be using the writeback connector. */
		in_use = false;
		wl_list_for_each(crtc, &output->device->crtc_list, link) {
			if (crtc->output && crtc->output->wb_state &&
			    crtc->output->wb_state->wb == wb) {
				in_use = true;
				break;
			}
		}
		if (in_use)
			continue;

		/* Is the writeback connector compatible with the CRTC? */
		possible_crtcs =
			drm_connector_get_possible_crtcs_mask(&wb->connector);
		if (!(possible_crtcs & (1 << output->crtc->pipe)))
			continue;

		/* Does the wb supports the format? */
		if (!weston_drm_format_array_find_format(&wb->formats,
							 pixel_format->format))
			continue;

		return wb;
	}

	return NULL;
}

static struct drm_writeback_state *
drm_writeback_state_alloc(void)
{
	struct drm_writeback_state *state;

	state = zalloc(sizeof *state);
	if (!state)
		return NULL;

	state->state = DRM_OUTPUT_WB_SCREENSHOT_OFF;
	state->out_fence_fd = -1;
	wl_array_init(&state->referenced_fbs);

	return state;
}

static void
drm_writeback_state_free(struct weston_compositor *c,
			 struct drm_writeback_state *state)
{
	struct drm_fb **fb;

	/* Capture task must be retired before freeing the state. */
	weston_assert_ptr_null(c, state->ct);

	if (state->out_fence_fd >= 0)
		close(state->out_fence_fd);

	/* Unref framebuffer that was given to save the content of the writeback */
	if (state->fb)
		drm_fb_unref(state->fb);

	/* Unref framebuffers that were in use in the same commit of the one with
	 * the writeback setup */
	wl_array_for_each(fb, &state->referenced_fbs)
		drm_fb_unref(*fb);
	wl_array_release(&state->referenced_fbs);

	free(state);
}

static void
drm_writeback_state_ct_destroy_handler(struct wl_listener *listener, void *data)
{
	struct drm_writeback_state *state =
		container_of(listener, struct drm_writeback_state,
			     ct_destroy_listener);

	/**
	 * Capture task was retired, so drop it from the state. The state is
	 * destroyed once the wb job completes.
	 */
	state->ct = NULL;
	wl_list_remove(&state->ct_destroy_listener.link);
}

static void
drm_output_pick_writeback_capture_task(struct drm_output *output)
{
	struct weston_capture_task *ct;
	struct weston_buffer *buffer;
	struct drm_writeback *wb;
	char *msg;
	int32_t width = output->base.current_mode->width;
	int32_t height = output->base.current_mode->height;
	const struct weston_drm_format_array *writeback_formats =
		weston_output_get_writeback_formats(&output->base);

	assert(output->device->atomic_modeset);

	ct = weston_output_pull_capture_task(&output->base,
					     WESTON_OUTPUT_CAPTURE_SOURCE_WRITEBACK,
					     width, height, NULL, writeback_formats);
	if (!ct)
		return;

	if (output->wb_state) {
		str_printf(&msg, "drm: another writeback task already in progress");
		goto err;
	}

	if (output->base.disable_planes > 0) {
		str_printf(&msg, "drm: KMS planes usage is disabled for now, " \
			   "so writeback capture tasks are rejected");
		goto err;
	}

	buffer = weston_capture_task_get_buffer(ct);

	wb = drm_output_find_compatible_writeback(output, buffer->pixel_format);
	if (!wb) {
		str_printf(&msg,
			   "drm: could not find writeback connector for output");
		goto err;
	}

	output->wb_state = drm_writeback_state_alloc();
	if (!output->wb_state) {
		str_printf(&msg,
			   "drm: failed to allocate memory for writeback state");
		goto err;
	}

	if (buffer->type == WESTON_BUFFER_SHM) {
		output->wb_state->fb = drm_fb_create_dumb(output->device, width,
							  height,
							  buffer->pixel_format->format);
		if (!output->wb_state->fb) {
			str_printf(&msg,
				   "drm: failed to create dumb buffer for " \
				   "writeback state");
			goto err_fb;
		}
	}
#ifdef BUILD_DRM_GBM
	else if (buffer->type == WESTON_BUFFER_DMABUF) {
		uint32_t failure_reasons = 0;
		output->wb_state->fb = drm_fb_get_from_dmabuf(buffer->dmabuf,
							      output->device,
							      &failure_reasons);
		if (!output->wb_state->fb) {
			str_printf(&msg,
				   "drm: failed to attach dma buffer from " \
				   "client for writeback state: %s",
				   weston_plane_failure_reasons_to_str(failure_reasons));
			goto err_fb;
		}
	}
#endif
	else {
		str_printf(&msg, "drm: Invalid buffer type");
		goto err_fb;
	}

	output->wb_state->output = output;
	output->wb_state->wb = wb;
	output->wb_state->state = DRM_OUTPUT_WB_SCREENSHOT_PREPARE_COMMIT;
	output->wb_state->ct = ct;

	output->wb_state->ct_destroy_listener.notify = drm_writeback_state_ct_destroy_handler;
	weston_capture_task_add_destroy_listener(ct, &output->wb_state->ct_destroy_listener);

	return;

err_fb:
	free(output->wb_state);
	output->wb_state = NULL;
err:
	weston_capture_task_retire_failed(ct, msg);
	free(msg);
}

#ifdef BUILD_DRM_GBM
/**
 * Update the image for the current cursor surface
 *
 * @param plane_state DRM cursor plane state
 * @param ev Source view for cursor
 */
static void
cursor_bo_update(struct drm_output *output, struct weston_paint_node *pnode)
{
	struct drm_device *device = output->device;
	struct gbm_bo *bo = output->gbm_cursor_fb[output->current_cursor]->bo;
	struct weston_buffer *buffer = pnode->surface->buffer_ref.buffer;
	uint32_t buf[device->cursor_width * device->cursor_height];
	uint8_t *s;
	int i;

	assert(buffer && buffer->shm_buffer);
	assert(buffer->width <= device->cursor_width);
	assert(buffer->height <= device->cursor_height);

	memset(buf, 0, sizeof buf);

	s = wl_shm_buffer_get_data(buffer->shm_buffer);

	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (i = 0; i < buffer->height; i++)
		memcpy(buf + i * device->cursor_width,
		       s + i * buffer->stride,
		       buffer->width * 4);
	wl_shm_buffer_end_access(buffer->shm_buffer);

	if (bo) {
		if (gbm_bo_write(bo, buf, sizeof buf) < 0)
			weston_log("failed update cursor: %s\n", strerror(errno));
	} else {
		memcpy(output->gbm_cursor_fb[output->current_cursor]->map,
		       buf, sizeof buf);
	}
}
#else
static void
cursor_bo_update(struct drm_output *output, struct weston_paint_node *pnode)
{
}
#endif

static void
drm_output_prepare_repaint(struct weston_output *output_base)
{
	struct drm_output *output = to_drm_output(output_base);

	output->device->will_repaint = true;
}

static int
drm_output_repaint(struct weston_output *output_base)
{
	struct weston_compositor *compositor = output_base->compositor;
	struct drm_output *output = to_drm_output(output_base);
	struct drm_output_state *state = NULL;
	struct drm_plane_state *scanout_state;
	struct drm_plane_state *cursor_state = NULL;
	struct drm_plane *cursor_plane = NULL;
	struct drm_pending_state *pending_state;
	struct drm_device *device;

	assert(output);
	assert(!output->is_virtual);

	device = output->device;
	pending_state = device->repaint_data;
	assert(pending_state);

	if (output->disable_pending || output->destroy_pending)
		goto err;

	assert(!output->state_last);

	/* assign_planes() is always called before a repaint, so we must have a
	 * valid output state here. */
	state = drm_pending_state_get_output(pending_state, output);
	weston_assert_ptr_not_null(compositor, state);

	if (output->cursor_handle) {
		cursor_plane = output->cursor_handle->plane;
		cursor_state = drm_output_state_get_existing_plane(state,
								   cursor_plane);
	}

	if (cursor_state && cursor_state->fb) {
		pixman_region32_t damage;
		struct drm_fb *old_fb = cursor_state->fb;
		struct weston_paint_node *cursor_node;

		assert(cursor_state->handle->plane == cursor_plane);
		assert(old_fb->type == BUFFER_CURSOR);

		pixman_region32_init(&damage);
		cursor_node = weston_output_flush_damage_for_plane(&output->base,
								   &cursor_plane->base,
								   &damage);
		if (pixman_region32_not_empty(&damage)) {
			output->current_cursor++;
			output->current_cursor =
				output->current_cursor %
					ARRAY_LENGTH(output->gbm_cursor_fb);
			cursor_bo_update(output, cursor_node);
		}
		pixman_region32_fini(&damage);

		cursor_state->fb =
			drm_fb_ref(output->gbm_cursor_fb[output->current_cursor]);
		drm_fb_unref(old_fb);
	}


	if (output_base->allow_protection)
		state->protection = output_base->desired_protection;
	else
		state->protection = WESTON_HDCP_DISABLE;

	if (drm_output_ensure_hdr_output_metadata_blob(output) < 0)
		goto err;

	if (device->atomic_modeset)
		drm_output_pick_writeback_capture_task(output);

	/* Skip the renderer if our mode allows it */
	if (state->mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY)
		return 0;

	drm_output_render(state);
	scanout_state = drm_output_state_get_plane(state,
						   output->scanout_handle->plane);
	if (!scanout_state || !scanout_state->fb)
		goto err;

	return 0;

err:
	drm_output_state_free(state);
	return -1;
}

/* Determine the type of vblank synchronization to use for the output.
 *
 * The pipe parameter indicates which CRTC is in use.  Knowing this, we
 * can determine which vblank sequence type to use for it.  Traditional
 * cards had only two CRTCs, with CRTC 0 using no special flags, and
 * CRTC 1 using DRM_VBLANK_SECONDARY.  The first bit of the pipe
 * parameter indicates this.
 *
 * Bits 1-5 of the pipe parameter are 5 bit wide pipe number between
 * 0-31.  If this is non-zero it indicates we're dealing with a
 * multi-gpu situation and we need to calculate the vblank sync
 * using DRM_BLANK_HIGH_CRTC_MASK.
 */
static unsigned int
drm_waitvblank_pipe(struct drm_crtc *crtc)
{
	if (crtc->pipe > 1)
		return (crtc->pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) &
				DRM_VBLANK_HIGH_CRTC_MASK;
	else if (crtc->pipe > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static int
drm_output_start_repaint_loop(struct weston_output *output_base)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_pending_state *pending_state;
	struct drm_plane *scanout_plane = output->scanout_handle->plane;
	struct drm_device *device = output->device;
	struct drm_backend *backend = device->backend;
	struct weston_compositor *compositor = backend->compositor;
	struct timespec ts, tnow;
	uint32_t flags = WP_PRESENTATION_FEEDBACK_INVALID;
	int ret;
	drmVBlank vbl = {
		.request.type = DRM_VBLANK_RELATIVE,
		.request.sequence = 0,
		.request.signal = 0,
	};

	if (output->disable_pending || output->destroy_pending)
		return 0;

	/* Need to smash all state in from scratch; current timings might not
	 * be what we want, page flip might not work, etc.
	 */
	if (device->recovery_status == DRM_RECOVERY_WAIT_FOR_IDLE) {
		/* We'll become part of a scheduled recovery after any
		 * outstanding operations complete.
		 */
		if (device->atomic_completes_pending)
			return 0;

		drm_device_recovery_schedule(device);
	}

	if (device->recovery_status == DRM_RECOVERY_SCHEDULED)
		goto finish_frame;

	if (!scanout_plane->state_cur->fb) {
		/* We can't page flip if there's no mode set */
		goto finish_frame;
	}

	assert(scanout_plane->state_cur->handle->output == output);

	/* If we're tearing, we've been generating timestamps from the
	 * presentation clock that don't line up with the msc timestamps,
	 * and could be more recent than the latest msc, which would cause
	 * an assert() later.
	 */
	if (output->state_cur->tear) {
		flags |= WESTON_FINISH_FRAME_TEARING;
		goto finish_frame;
	}

	/* Try to get current msc and timestamp via instant query */
	vbl.request.type |= drm_waitvblank_pipe(output->crtc);
	ret = drmWaitVBlank(device->kms_device->fd, &vbl);

	/* Error ret or zero timestamp means failure to get valid timestamp */
	if ((ret == 0) && (vbl.reply.tval_sec > 0 || vbl.reply.tval_usec > 0)) {
		bool stale_timestamp = false;

		ts.tv_sec = vbl.reply.tval_sec;
		ts.tv_nsec = vbl.reply.tval_usec * 1000;

		/* Between Linux 3.16 and Linux 4.1 there was a bug that
		 * could result in a stale timestamp being returned. We
		 * can catch that by checking if the timestamp we have
		 * is older than 1 refresh duration since now, and use a
		 * page flip to start the repaint loop.
		 *
		 * However, if we're using VRR, the time since the last
		 * vblank could be the display's longest possible frame
		 * time, which is longer than refresh_nsec. That looks
		 * exactly like the bug we need to work around here, and
		 * the page flip workaround would result in an unnecessary
		 * delay.
		 *
		 * The workaround can cause other unexpected delays when
		 * starting the repaint loop.
		 *
		 * We keep the workaround in place, based on the presence of
		 * DRM_CAP_CRTC_IN_VBLANK_EVENT, which was introduced in
		 * Linux 4.12, to keep things working for Very Old Kernels.
		 *
		 * Anyone needing precise frame timings is encouraged to
		 * upgrade.
		 */
		if (output->backend->stale_timestamp_workaround) {
			struct timespec vbl2now;
			int64_t refresh_nsec;

			weston_compositor_read_presentation_clock(compositor,
								  &tnow);
			timespec_sub(&vbl2now, &tnow, &ts);
			refresh_nsec =
				millihz_to_nsec(output->base.current_mode->refresh);
			if (timespec_to_nsec(&vbl2now) > refresh_nsec)
				stale_timestamp = true;
		}

		if (!stale_timestamp) {
			drm_output_update_msc(output, vbl.reply.sequence);
			weston_output_finish_frame(output_base, &ts, flags);
			return 0;
		}
	}

	/* Immediate query didn't provide valid timestamp.
	 * Use pageflip fallback.
	 */

	assert(!output->page_flip_pending);
	assert(!output->state_last);

	pending_state = drm_pending_state_alloc(device);
	drm_output_state_duplicate(output->state_cur, pending_state,
				   DRM_OUTPUT_STATE_PRESERVE_PLANES);

	ret = drm_pending_state_apply(pending_state);
	if (ret != 0) {
		weston_log("applying repaint-start state failed: %s\n",
			   strerror(errno));
		if (ret == -EACCES || ret == -EBUSY)
			return ret;
		goto finish_frame;
	}

	return 0;

finish_frame:
	/* if we cannot page-flip, immediately finish frame */
	weston_output_finish_frame(output_base, NULL, flags);
	return 0;
}

static void
drm_repaint_begin_device(struct drm_device *device)
{
	struct drm_backend *b = device->backend;
	struct drm_pending_state *pending_state;

	device->will_repaint = false;
	pending_state = drm_pending_state_alloc(device);
	device->repaint_data = pending_state;

	if (weston_log_scope_is_enabled(b->debug))
		drm_debug(b, "[repaint] Beginning repaint (%s); pending_state %p\n",
			  device->kms_device->filename, device->repaint_data);
}

/**
 * Begin a new repaint cycle
 *
 * Called by the core compositor at the beginning of a repaint cycle. Creates
 * a new pending_state structure to own any output state created by individual
 * output repaint functions until the repaint is flushed or cancelled.
 */
static void
drm_repaint_begin(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct drm_device *device;

	wl_list_for_each(device, &b->kms_list, link) {
		if (device->will_repaint)
			drm_repaint_begin_device(device);
	}
}

static void
drm_repaint_flush_device(struct drm_device *device)
{
	struct drm_backend *b = device->backend;
	struct drm_pending_state *pending_state;
	struct weston_output *base;
	int ret;
	bool failed_reuse = false;

	pending_state = device->repaint_data;
	if (!pending_state)
		return;

	ret = drm_pending_state_apply(pending_state);
	if (ret != 0)
		weston_log("repaint-flush failed: %s\n", strerror(errno));

	drm_debug(b, "[repaint] flushed (%s) pending_state %p\n",
		  device->kms_device->filename, pending_state);
	device->repaint_data = NULL;

	if (ret == 0)
		return;

	wl_list_for_each(base, &b->compositor->output_list, link) {
		struct drm_output *tmp = to_drm_output(base);
		if (!base->will_repaint || !tmp || tmp->device != device)
			continue;

		/* We shouldn't be failing at all when using a previous state,
		 * and when we do it can lead to choppy frame scheduling.
		 * Keep track of any failures and if we have a few, just give
		 * up on ever reusing state.
		 */
		if (tmp->reused_state) {
			failed_reuse = true;
			device->reused_state_failures++;
		}

		/* Even if we weren't reusing state, we can't reuse this one
		 * next repaint, so make sure we don't try.
		 */
		tmp->force_rebuild_state = true;
	}

	if (failed_reuse)
		drm_debug(b, "[repaint] failed with reused state, will rebuild and try again.\n");

	wl_list_for_each(base, &b->compositor->output_list, link) {
		struct drm_output *tmp = to_drm_output(base);
		if (!base->will_repaint || !tmp || tmp->device != device)
			continue;

		if (ret == -EBUSY || failed_reuse)
			weston_output_schedule_repaint_restart(base);
		else
			weston_output_schedule_repaint_reset(base);
	}
}

/**
 * Flush a repaint set
 *
 * Called by the core compositor when a repaint cycle has been completed
 * and should be flushed. Frees the pending state, transitioning ownership
 * of the output state from the pending state, to the update itself. When
 * the update completes (see drm_output_update_complete), the output
 * state will be freed.
 */
static void
drm_repaint_flush(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct drm_device *device;
	FILE *dbg;

	WESTON_TRACE_FUNC();

	wl_list_for_each(device, &b->kms_list, link)
		drm_repaint_flush_device(device);

	dbg = weston_log_scope_stream(b->debug);
	if (dbg) {
		weston_compositor_print_scene_graph(b->compositor, dbg);
		fflush(dbg);
	}
}

static void
drm_repaint_cancel_device(struct drm_device *device)
{
	struct drm_backend *b = device->backend;
	struct drm_pending_state *pending_state;

	device->will_repaint = false;
	pending_state = device->repaint_data;
	if (pending_state) {
		drm_pending_state_free(pending_state);
		drm_debug(b, "[repaint] cancel pending_state %p\n", pending_state);
		device->repaint_data = NULL;
	}
}

/**
 * Cancel a repaint set
 *
 * Called by the core compositor when a repaint has finished, so the data
 * held across the repaint cycle should be discarded.
 */
static void
drm_repaint_cancel(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct drm_device *device;

	wl_list_for_each(device, &b->kms_list, link)
		drm_repaint_cancel_device(device);
}

static int
drm_output_init_pixman(struct drm_output *output, struct drm_backend *b);
static void
drm_output_fini_pixman(struct drm_output *output);

static int
drm_output_switch_mode(struct weston_output *output_base, struct weston_mode *mode)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_mode *drm_mode;

	assert(output);

	drm_mode = drm_output_choose_mode(output, mode);
	if (!drm_mode) {
		weston_log("%s: invalid resolution %dx%d\n",
			   output_base->name, mode->width, mode->height);
		return -1;
	}

	if (&drm_mode->base == output->base.current_mode)
		return 0;

	output->base.current_mode->flags = 0;

	output->base.current_mode = &drm_mode->base;
	output->base.current_mode->flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	if (output->page_flip_pending || output->atomic_complete_pending) {
		output->mode_switch_pending = true;
		return 0;
	}

	return drm_output_apply_mode(output);
}

static int
drm_output_apply_mode(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_size fb_size;

	/* XXX: This drops our current buffer too early, before we've started
	 *      displaying it. Ideally this should be much more atomic and
	 *      integrated with a full repaint cycle, rather than doing a
	 *      sledgehammer modeswitch first, and only later showing new
	 *      content.
	 */
	drm_device_recovery_required(device);

	fb_size.width = output->base.current_mode->width;
	fb_size.height = output->base.current_mode->height;

	if (!weston_renderer_resize_output(&output->base, &fb_size, NULL))
		return -1;

	if (b->compositor->renderer->type == WESTON_RENDERER_GL) {
		drm_output_fini_egl(output);
		if (drm_output_init_egl(output, b) < 0) {
			weston_log("failed to init output egl state with "
				   "new mode");
			return -1;
		}
	} else if (b->compositor->renderer->type == WESTON_RENDERER_VULKAN) {
		drm_output_fini_vulkan(output);
		if (drm_output_init_vulkan(output, b) < 0) {
			weston_log("failed to init output vulkan state with "
				   "new mode");
			return -1;
		}
	}

	if (device->atomic_modeset)
		weston_output_update_capture_info(&output->base,
						  WESTON_OUTPUT_CAPTURE_SOURCE_WRITEBACK,
						  output->base.current_mode->width,
						  output->base.current_mode->height,
						  NULL,
						  weston_output_get_writeback_formats(&output->base));

	return 0;
}

static int
init_pixman(struct drm_backend *b)
{
	return weston_compositor_init_renderer(b->compositor,
					       WESTON_RENDERER_PIXMAN, NULL);
}

/**
 * Create a drm_plane for a hardware plane
 *
 * Creates one drm_plane structure for a hardware plane, and initialises its
 * properties and formats.
 *
 * This function does not add the plane to the list of usable planes in Weston
 * itself; the caller is responsible for this.
 *
 * Call drm_plane_destroy to clean up the plane.
 *
 * @sa drm_output_find_special_plane
 * @param device DRM device
 * @param kplane DRM plane to create
 */
static struct drm_plane *
drm_plane_create(struct drm_device *device, const drmModePlane *kplane)
{
	struct drm_backend *b = device->backend;
	struct weston_compositor *compositor = b->compositor;
	struct drm_plane *plane, *tmp;
	drmModeObjectProperties *props;
	const uint64_t *zpos_range_values;
	const uint64_t *alpha_range_values;

	plane = zalloc(sizeof(*plane));
	if (!plane) {
		weston_log("%s: out of memory\n", __func__);
		return NULL;
	}

	plane->device = device;
	plane->state_cur = drm_plane_state_alloc(NULL, plane);
	plane->state_cur->complete = true;
	plane->possible_crtcs = kplane->possible_crtcs;
	plane->plane_id = kplane->plane_id;
	plane->crtc_id = kplane->crtc_id;

	weston_drm_format_array_init(&plane->formats);

	props = drmModeObjectGetProperties(device->kms_device->fd, kplane->plane_id,
					   DRM_MODE_OBJECT_PLANE);
	if (!props) {
		weston_log("couldn't get plane properties\n");
		goto err;
	}

	drm_property_info_populate(device, plane_props, plane->props,
				   WDRM_PLANE__COUNT, props);
	plane->type =
		drm_property_get_value(&plane->props[WDRM_PLANE_TYPE],
				       props,
				       WDRM_PLANE_TYPE__COUNT);

	plane->can_scale =
		drm_property_has_feature(&plane->props[WDRM_PLANE_FEATURE],
					 props,
					 WDRM_PLANE_FEATURE_SCALE);
	if (getenv("WESTON_DRM_DISABLE_PLANE_SCALE"))
		plane->can_scale = false;

	zpos_range_values =
		drm_property_get_range_values(&plane->props[WDRM_PLANE_ZPOS],
					      props);

	if (zpos_range_values) {
		plane->zpos_min = zpos_range_values[0];
		plane->zpos_max = zpos_range_values[1];
	} else {
		plane->zpos_min = DRM_PLANE_ZPOS_INVALID_PLANE;
		plane->zpos_max = DRM_PLANE_ZPOS_INVALID_PLANE;
	}

	alpha_range_values =
		drm_property_get_range_values(&plane->props[WDRM_PLANE_ALPHA],
					      props);

	if (alpha_range_values) {
		plane->alpha_min = (uint16_t) alpha_range_values[0];
		plane->alpha_max = (uint16_t) alpha_range_values[1];
	} else {
		plane->alpha_min = DRM_PLANE_ALPHA_OPAQUE;
		plane->alpha_max = DRM_PLANE_ALPHA_OPAQUE;
	}

	if (drm_plane_populate_formats(plane, kplane, props,
				       device->fb_modifiers) < 0) {
		drmModeFreeObjectProperties(props);
		goto err;
	}

	drm_plane_populate_color_pipelines(plane, props);

	drmModeFreeObjectProperties(props);

	if (plane->type == WDRM_PLANE_TYPE__COUNT)
		goto err_props;

	weston_plane_init(&plane->base, compositor);

	wl_list_for_each(tmp, &device->plane_list, link) {
		if (tmp->zpos_max < plane->zpos_max) {
			wl_list_insert(tmp->link.prev, &plane->link);
			break;
		}
	}
	if (plane->link.next == NULL)
		wl_list_insert(device->plane_list.prev, &plane->link);

	return plane;

err_props:
	drm_property_info_free(plane->props, WDRM_PLANE__COUNT);
err:
	weston_drm_format_array_fini(&plane->formats);
	drm_plane_state_free(plane->state_cur, true);
	free(plane);
	return NULL;
}

/**
 * Find, or create, a special-purpose plane
 *
 * @param device DRM device
 * @param output Output to use for plane
 * @param type Type of plane
 */
static struct drm_plane *
drm_output_find_special_plane(struct drm_device *device,
			      struct drm_output *output,
			      enum wdrm_plane_type type)
{
	struct drm_backend *b = device->backend;
	struct drm_plane *plane;

	wl_list_for_each(plane, &device->plane_list, link) {
		struct weston_output *base;
		bool found_elsewhere = false;

		if (plane->type != type)
			continue;
		if (!drm_plane_is_available(plane, output))
			continue;

		/* On some platforms, primary/cursor planes can roam
		 * between different CRTCs, so make sure we don't claim the
		 * same plane for two outputs. */
		wl_list_for_each(base, &b->compositor->output_list, link) {
			struct drm_output *tmp = to_drm_output(base);
			if (!tmp)
				continue;

			if ((tmp->cursor_handle &&
			     tmp->cursor_handle->plane == plane) ||
			    tmp->scanout_handle->plane == plane) {
				found_elsewhere = true;
				break;
			}
		}

		if (found_elsewhere)
			continue;

		/* If a plane already has a CRTC selected and it is not our
		 * output's CRTC, then do not select this plane. We cannot
		 * switch away a plane from a CTRC when active. */
		if ((type == WDRM_PLANE_TYPE_PRIMARY) &&
		    (plane->crtc_id != 0) &&
		    (plane->crtc_id != output->crtc->crtc_id))
			continue;

		plane->possible_crtcs = (1 << output->crtc->pipe);
		return plane;
	}

	return NULL;
}

/**
 * Destroy one DRM plane
 *
 * Destroy a DRM plane, removing it from screen and releasing its retained
 * buffers in the process. The counterpart to drm_plane_create.
 *
 * @param plane Plane to deallocate (will be freed)
 */
static void
drm_plane_destroy(struct drm_plane *plane)
{
	struct drm_device *device = plane->device;

	if (plane->type == WDRM_PLANE_TYPE_OVERLAY)
		drmModeSetPlane(device->kms_device->fd, plane->plane_id,
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	drm_plane_state_free(plane->state_cur, true);
	drm_property_info_free(plane->props, WDRM_PLANE__COUNT);
	drm_plane_release_color_pipelines(plane);
	weston_plane_release(&plane->base);
	weston_drm_format_array_fini(&plane->formats);
	wl_list_remove(&plane->link);
	free(plane);
}

void
drm_plane_destroy_handle(struct drm_plane_handle *handle)
{
	wl_list_remove(&handle->link);
	free(handle);
}

struct drm_plane_handle *
drm_plane_create_handle(struct drm_plane *plane, struct drm_output *output)
{
	struct drm_plane_handle *handle = xzalloc(sizeof(*handle));

	handle->output = output;
	handle->plane = plane;

	wl_list_init(&handle->link);

	return handle;
}

/**
 * Initialise hardware planes
 *
 * Walk the list of provided DRM planes, and add overlay planes.
 *
 * Call destroy_planes to free these planes.
 *
 * @param device DRM device
 */
static void
create_planes(struct drm_device *device, drmModeRes *resources)
{
	drmModePlaneRes *kplane_res;
	drmModePlane *kplane;
	struct drm_plane *drm_plane;
	struct drm_crtc *drm_crtc;
	uint32_t i;
	uint32_t next_plane_idx = 0;
	uint32_t num_primary = 0, crtc_pipe;

	kplane_res = drmModeGetPlaneResources(device->kms_device->fd);

	if (!kplane_res) {
		weston_log("failed to get plane resources: %s\n",
			strerror(errno));
		return;
	}

	for (i = 0; i < kplane_res->count_planes; i++) {
		kplane = drmModeGetPlane(device->kms_device->fd, kplane_res->planes[i]);
		if (!kplane)
			continue;

		drm_plane = drm_plane_create(device, kplane);
		drmModeFreePlane(kplane);

		/**
		 * Assume that the Nth primary plane is meant for the Nth CRTC.
		 * See:
		 * https://lore.kernel.org/dri-devel/20200807090706.GA2352366@phenom.ffwll.local/
		 */
		if (drm_plane->type == WDRM_PLANE_TYPE_PRIMARY) {
			num_primary++;
			crtc_pipe = num_primary - 1;
			drm_crtc = drm_crtc_find(device,
						 resources->crtcs[crtc_pipe]);
			assert(drm_crtc);
			drm_crtc->primary_plane_id = drm_plane->plane_id;
		}
	}

	wl_list_for_each (drm_plane, &device->plane_list, link)
		drm_plane->plane_idx = next_plane_idx++;

	drmModeFreePlaneResources(kplane_res);
}

/**
 * Clean up hardware planes
 *
 * The counterpart to create_planes.
 *
 * @param device DRM device
 */
static void
destroy_planes(struct drm_device *device)
{
	struct drm_plane *plane, *next;

	wl_list_for_each_safe(plane, next, &device->plane_list, link)
		drm_plane_destroy(plane);
}

/* returns a value between 0-255 range, where higher is brighter */
static uint32_t
drm_get_backlight(struct drm_head *head)
{
	long brightness, max_brightness, norm;

	brightness = backlight_get_brightness(head->backlight);
	max_brightness = backlight_get_max_brightness(head->backlight);

	/* convert it on a scale of 0 to 255 */
	norm = (brightness * 255)/(max_brightness);

	return (uint32_t) norm;
}

/* values accepted are between 0-255 range */
static void
drm_set_backlight(struct weston_output *output_base, uint32_t value)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_head *head;
	long max_brightness, new_brightness;

	if (value > 255)
		return;

	wl_list_for_each(head, &output->base.head_list, base.output_link) {
		if (!head->backlight)
			return;

		max_brightness = backlight_get_max_brightness(head->backlight);

		/* get denormalized value */
		new_brightness = (value * max_brightness) / 255;

		backlight_set_brightness(head->backlight, new_brightness);
	}
}

static void
drm_output_init_backlight(struct drm_output *output)
{
	struct weston_head *base;
	struct drm_head *head;

	output->base.set_backlight = NULL;

	wl_list_for_each(base, &output->base.head_list, output_link) {
		head = to_drm_head(base);

		if (head->backlight) {
			weston_log("Initialized backlight for head '%s', device %s\n",
				   head->base.name, head->backlight->path);

			if (!output->base.set_backlight) {
				output->base.set_backlight = drm_set_backlight;
				output->base.backlight_current =
							drm_get_backlight(head);
			}
		}
	}
}

/**
 * Power output on or off
 *
 * The DPMS/power level of an output is used to switch it on or off. This
 * is DRM's hook for doing so, which can called either as part of repaint,
 * or independently of the repaint loop.
 *
 * If we are called as part of repaint, we simply set the relevant bit in
 * state and return.
 *
 * This function is never called on a virtual output.
 */
static void
drm_set_dpms(struct weston_output *output_base, enum dpms_enum level)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_device *device = output->device;
	struct drm_pending_state *pending_state = device->repaint_data;
	struct drm_output_state *state;
	int ret;

	assert(output);
	assert(!output->is_virtual);

	if (output->state_cur->dpms == level)
		return;

	/* If we're being called during the repaint loop, then this is
	 * simple: discard any previously-generated state, and create a new
	 * state where we disable everything. When we come to flush, this
	 * will be applied.
	 *
	 * However, we need to be careful: we can be called whilst another
	 * output is in its repaint cycle (pending_state exists), but our
	 * output still has an incomplete state application outstanding.
	 * In that case, we need to wait until that completes. */
	if (pending_state && !output->state_last) {
		/* The repaint loop already sets DPMS on; we don't need to
		 * explicitly set it on here, as it will already happen
		 * whilst applying the repaint state. */
		if (level == WESTON_DPMS_ON)
			return;

		state = drm_pending_state_get_output(pending_state, output);
		if (state)
			drm_output_state_free(state);
		state = drm_output_get_disable_state(pending_state, output);
		return;
	}

	/* As we throw everything away when disabling, just send us back through
	 * a repaint cycle. */
	if (level == WESTON_DPMS_ON) {
		if (output->dpms_off_pending)
			output->dpms_off_pending = false;
		weston_output_schedule_repaint(output_base);
		return;
	}

	/* If we've already got a request in the pipeline, then we need to
	 * park our DPMS request until that request has quiesced. */
	if (output->state_last) {
		output->dpms_off_pending = true;
		return;
	}

	pending_state = drm_pending_state_alloc(device);
	drm_output_get_disable_state(pending_state, output);
	ret = drm_pending_state_apply_sync(pending_state);
	if (ret != 0)
		weston_log("drm_set_dpms: couldn't disable output?\n");
}

static const char * const connector_type_names[] = {
	[DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
	[DRM_MODE_CONNECTOR_VGA]         = "VGA",
	[DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
	[DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
	[DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
	[DRM_MODE_CONNECTOR_Composite]   = "Composite",
	[DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
	[DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
	[DRM_MODE_CONNECTOR_Component]   = "Component",
	[DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
	[DRM_MODE_CONNECTOR_DisplayPort] = "DP",
	[DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
	[DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
	[DRM_MODE_CONNECTOR_TV]          = "TV",
	[DRM_MODE_CONNECTOR_eDP]         = "eDP",
	[DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
	[DRM_MODE_CONNECTOR_DSI]         = "DSI",
	[DRM_MODE_CONNECTOR_DPI]         = "DPI",
};

/** Create a name given a DRM connector
 *
 * \param con The DRM connector whose type and id form the name.
 * \return A newly allocate string, or NULL on error. Must be free()'d
 * after use.
 *
 * The name does not identify the DRM display device.
 */
static char *
make_connector_name(const drmModeConnector *con)
{
	char *name;
	const char *type_name = NULL;
	int ret;

	if (con->connector_type < ARRAY_LENGTH(connector_type_names))
		type_name = connector_type_names[con->connector_type];

	if (!type_name)
		type_name = "UNNAMED";

	ret = asprintf(&name, "%s-%d", type_name, con->connector_type_id);
	if (ret < 0)
		return NULL;

	return name;
}

static bool
drm_rb_discarded_cb(weston_renderbuffer_t rb, void *data)
{
	struct drm_output *output = (struct drm_output *) data;
	struct weston_renderer *renderer = output->base.compositor->renderer;
	const struct pixel_format_info *pfmt = output->format;
	int w = output->base.current_mode->width;
	int h = output->base.current_mode->height;
	struct drm_fb *dumb;
	size_t i;

	assert(renderer->type == WESTON_RENDERER_PIXMAN);

	for (i = 0; i < ARRAY_LENGTH(output->renderbuffer); i++) {
		if (rb == output->renderbuffer[i]) {
			drm_fb_unref(output->dumb[i]);
			output->dumb[i] = NULL;
			renderer->destroy_renderbuffer(output->renderbuffer[i]);
			output->renderbuffer[i] = NULL;

			dumb = drm_fb_create_dumb(output->device, w, h,
						  pfmt->format);
			if (!dumb)
				break;
			rb = renderer->create_renderbuffer(&output->base, pfmt,
							   dumb->map,
							   dumb->strides[0],
							   drm_rb_discarded_cb,
							   output);
			if (!rb) {
				drm_fb_unref(dumb);
				break;
			}

			output->dumb[i] = dumb;
			output->renderbuffer[i] = rb;
			return true;
		}
	}

	assert(i != ARRAY_LENGTH(output->renderbuffer));

	weston_log("failed to reload pixman dumb and render buffers");
	return false;
}

static bool
drm_output_pick_format_pixman(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;

	/* Any other value of eotf_mode requires color management, which is also
	 * necessary to have from_blend_to_output_by_backend set. Color
	 * management is unsupported by Pixman renderer. */
	assert(!output->base.from_blend_to_output_by_backend);
	assert(output->base.eotf_mode == WESTON_EOTF_MODE_SDR);

	if (!b->format->pixman_format) {
		weston_log("Error: failed to pick format for output '%s', format %s unsupported by Pixman.\n",
			   output->base.name, b->format->drm_format_name);
		return false;
	}

	output->format = b->format;

	if (output->has_underlay && (output->format->bits.a == 0)) {
		weston_log("Disabling underlay planes: "
			   "output '%s' with format %s does not have alpha channel, "
			   "which is required to support underlay planes.\n",
			   output->base.name, output->format->drm_format_name);
		output->has_underlay = false;
	}

	return true;
}

static int
drm_output_init_pixman(struct drm_output *output, struct drm_backend *b)
{
	struct weston_renderer *renderer = output->base.compositor->renderer;
	const struct pixman_renderer_interface *pixman = renderer->pixman;
	struct drm_device *device = output->device;
	int w = output->base.current_mode->width;
	int h = output->base.current_mode->height;
	struct pixman_renderer_output_options options;
	unsigned int i;

	if (!output->format && !drm_output_pick_format_pixman(output))
		return -1;

	options.format = output->format;
	options.use_shadow = b->use_pixman_shadow;
	options.fb_size.width = w;
	options.fb_size.height = h;

	if (pixman->output_create(&output->base, &options) < 0)
		goto err;

	/* FIXME error checking */
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		output->dumb[i] = drm_fb_create_dumb(device, w, h,
						     options.format->format);
		if (!output->dumb[i])
			goto err;

		output->renderbuffer[i] =
			renderer->create_renderbuffer(&output->base,
						      options.format,
						      output->dumb[i]->map,
						      output->dumb[i]->strides[0],
						      drm_rb_discarded_cb,
						      output);
		if (!output->renderbuffer[i])
			goto err;
	}

	weston_log("DRM: output %s %s shadow framebuffer.\n", output->base.name,
		   b->use_pixman_shadow ? "uses" : "does not use");

	return 0;

err:
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		if (output->dumb[i])
			drm_fb_unref(output->dumb[i]);
		if (output->renderbuffer[i])
			renderer->destroy_renderbuffer(output->renderbuffer[i]);

		output->dumb[i] = NULL;
		output->renderbuffer[i] = NULL;
	}
	pixman->output_destroy(&output->base);

	return -1;
}

static void
drm_output_fini_pixman(struct drm_output *output)
{
	struct weston_renderer *renderer = output->base.compositor->renderer;
	struct drm_backend *b = output->backend;
	unsigned int i;

	/* Destroying the Pixman surface will destroy all our buffers,
	 * regardless of refcount. Ensure we destroy them here. */
	if (!b->compositor->shutting_down && output->scanout_handle &&
	    output->scanout_handle->plane->state_cur->fb &&
	    output->scanout_handle->plane->state_cur->fb->type == BUFFER_PIXMAN_DUMB) {
		drm_plane_reset_state(output->scanout_handle->plane);
	}

	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		renderer->destroy_renderbuffer(output->renderbuffer[i]);
		drm_fb_unref(output->dumb[i]);
		output->dumb[i] = NULL;
		output->renderbuffer[i] = NULL;
	}

	renderer->pixman->output_destroy(&output->base);
}

static void
setup_output_seat_constraint(struct drm_backend *b,
			     struct weston_output *output,
			     const char *s)
{
	if (strcmp(s, "") != 0) {
		struct weston_pointer *pointer;
		struct udev_seat *seat;

		seat = udev_seat_get_named(&b->input, s);
		if (!seat)
			return;

		seat->base.output = output;
		seat->has_output = true;

		pointer = weston_seat_get_pointer(&seat->base);
		if (pointer)
			pointer->pos = weston_pointer_clamp(pointer,
							    pointer->pos);
	}
}

static int
drm_output_attach_head(struct weston_output *output_base,
		       struct weston_head *head_base)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_backend *b = output->backend;
	struct drm_device *device = b->drm;
	struct drm_head *head = to_drm_head(head_base);

	if (wl_list_length(&output_base->head_list) >= MAX_CLONED_CONNECTORS)
		return -1;

	wl_list_remove(&head->disable_head_link);
	wl_list_init(&head->disable_head_link);

	if (!output_base->enabled)
		return 0;

	/* XXX: ensure the configuration will work.
	 * This is actually impossible without major infrastructure
	 * work. */

	/* Need to go through modeset to add connectors. */
	/* XXX: Ideally we'd do this per-output, not globally. */
	/* XXX: Doing it globally, what guarantees another output's update
	 * will not clear the flag before this output is updated?
	 */
	drm_device_recovery_required(device);

	return 0;
}

static void
drm_output_detach_head(struct weston_output *output_base,
		       struct weston_head *head_base)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_backend *b = output->backend;
	struct drm_device *device = b->drm;
	struct drm_head *head = to_drm_head(head_base);

	if (!output_base->enabled)
		return;

	/* Drop connectors that should no longer be driven on next repaint. */
	wl_list_remove(&head->disable_head_link);
	wl_list_insert(&output->disable_head, &head->disable_head_link);

	if (!wl_list_length(&output_base->head_list))
		return;

	/* XXX: ensure the configuration will work.
	 * This is actually impossible without major infrastructure
	 * work. */

	/* Need to go through modeset to add connectors. */
	/* XXX: Ideally we'd do this per-output, not globally. */
	/* XXX: Doing it globally, what guarantees another output's update
	 * will not clear the flag before this output is updated?
	 */
	drm_device_recovery_required(device);
}

static const struct weston_drm_format_array *
drm_output_get_writeback_formats(struct weston_output *output_base)
{
	struct weston_compositor *compositor = output_base->compositor;
	struct drm_output *output = to_drm_output(output_base);

	weston_assert_ptr_not_null(compositor, output->crtc);

	return &output->crtc->writeback_formats;
}

int
parse_gbm_format(const char *s, const struct pixel_format_info *default_format,
		 const struct pixel_format_info **format)
{
	if (s == NULL) {
		*format = default_format;

		return 0;
	}

	/* GBM formats and DRM formats are identical. */
	*format = pixel_format_get_info_by_drm_name(s);
	if (!*format) {
		weston_log("fatal: unrecognized pixel format: %s\n", s);

		return -1;
	}

	return 0;
}

static int
drm_head_read_current_setup(struct drm_head *head, struct drm_device *device)
{
	int drm_fd = device->kms_device->fd;
	drmModeConnector *conn = head->connector.conn;
	drmModeEncoder *encoder;
	drmModeCrtc *crtc;

	/* Get the current mode on the crtc that's currently driving
	 * this connector. */
	encoder = drmModeGetEncoder(drm_fd, conn->encoder_id);
	if (encoder != NULL) {
		head->inherited_crtc_id = encoder->crtc_id;

		crtc = drmModeGetCrtc(drm_fd, encoder->crtc_id);
		drmModeFreeEncoder(encoder);

		if (crtc == NULL)
			return -1;
		if (crtc->mode_valid)
			head->inherited_mode = crtc->mode;
		drmModeFreeCrtc(crtc);
	}

	/* Get the current max_bpc that's currently configured to
	 * this connector. */
	head->inherited_max_bpc = drm_property_get_value(
		&head->connector.props[WDRM_CONNECTOR_MAX_BPC],
		head->connector.props_drm, 0);

	return 0;
}

static void
drm_output_set_gbm_format(struct weston_output *base,
			  const char *gbm_format)
{
	struct drm_output *output = to_drm_output(base);

	if (parse_gbm_format(gbm_format, NULL, &output->format) == -1)
		output->format = NULL;
}

static void
drm_output_set_seat(struct weston_output *base,
		    const char *seat)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = output->backend;

	setup_output_seat_constraint(b, &output->base,
				     seat ? seat : "");
}

static void
drm_output_set_max_bpc(struct weston_output *base, unsigned max_bpc)
{
	struct drm_output *output = to_drm_output(base);

	assert(output);
	assert(!output->base.enabled);

	output->max_bpc = max_bpc;
}

static const struct { const char *name; uint32_t token; } content_types[] = {
	{ "no data",  WDRM_CONTENT_TYPE_NO_DATA },
	{ "graphics", WDRM_CONTENT_TYPE_GRAPHICS },
	{ "photo",    WDRM_CONTENT_TYPE_PHOTO },
	{ "cinema",   WDRM_CONTENT_TYPE_CINEMA },
	{ "game",     WDRM_CONTENT_TYPE_GAME },
};

static int
drm_output_set_content_type(struct weston_output *base,
			    const char *content_type)
{
	unsigned int i;
	struct drm_output *output = to_drm_output(base);

	if (content_type == NULL) {
		output->content_type = WDRM_CONTENT_TYPE_NO_DATA;
		return 0;
	}

	for (i = 0; i < ARRAY_LENGTH(content_types); i++)
		if (strcmp(content_types[i].name, content_type) == 0) {
			output->content_type = content_types[i].token;
			return 0;
		}

	weston_log("Error: unknown content-type for output %s: \"%s\"\n",
		   base->name, content_type);
	output->content_type = WDRM_CONTENT_TYPE_NO_DATA;
	return -1;
}

static int
drm_output_init_legacy_gamma_size(struct drm_output *output)
{
	struct drm_device *device = output->device;
	drmModeCrtc *crtc;

	assert(output->base.compositor);
	assert(output->crtc);
	crtc = drmModeGetCrtc(device->kms_device->fd, output->crtc->crtc_id);
	if (!crtc)
		return -1;

	output->legacy_gamma_size = crtc->gamma_size;

	drmModeFreeCrtc(crtc);

	return 0;
}

static struct weston_vec3f *
lut_3x1d_from_blend_to_output(struct weston_compositor *compositor,
			      struct weston_color_transform *xform,
			      size_t len_lut, char **err_msg)
{
	/**
	 * We expect steps to be valid for blend-to-output, as LittleCMS is
	 * always able to optimize such xform. If that's invalid, we'd need to
	 * use to_clut() to offload the xform, but the DRM API currently only
	 * supports us programming a LUT after blending.
	 */
	if (!xform->steps_valid) {
		str_printf(err_msg, "xform color steps are invalid");
		return NULL;
	}

	/**
	 * We expect blend-to-output to be composed of pre-curve only. We could
	 * handle a post-curve as well (merging the pre-curve and post-curve),
	 * but that's not necessary.
	 */
	if (xform->post_curve.type != WESTON_COLOR_CURVE_TYPE_IDENTITY ||
	    xform->mapping.type != WESTON_COLOR_MAPPING_TYPE_IDENTITY) {
		str_printf(err_msg, "xform unexpectedly has more steps than pre-curve");
		return NULL;
	}

	/**
	 * No need to craft LUT 3x1D from identity. But there shouldn't be a
	 * blend-to-output xform like this in first place.
	 */
	weston_assert_u32_ne(compositor, xform->pre_curve.type,
					 WESTON_COLOR_CURVE_TYPE_IDENTITY);

	return weston_color_curve_to_3x1D_LUT(compositor, xform,
					      WESTON_COLOR_CURVE_STEP_PRE,
					      WESTON_COLOR_PRECISION_CARELESS,
					      len_lut, err_msg);
}

static int
drm_output_pick_blend_to_output(struct drm_output *output)
{
	struct weston_compositor *compositor = output->base.compositor;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	const struct drm_colorop_3x1d_lut_blob *colorop_lut;
	struct weston_color_transform *xform;
	enum weston_color_curve_step curve_step;
	size_t lut_len;
	struct weston_vec3f *cm_lut;
	char *err_msg;

	/* Check if there's actually something to offload. */
	weston_assert_ptr_not_null(compositor, output->base.color_outcome);
	xform = output->base.color_outcome->from_blend_to_output;
	if (!xform)
		return 0;

	lut_len = output->crtc->lut_size;
	if (lut_len == 0) {
		drm_debug(b, "[output] can't offload blend-to-output: GAMMA_LUT_SIZE unsupported\n");
		return -1;
	}

	/**
	 * For now we expect blend-to-output to be composed of pre-curve only,
	 * so lut_3x1d_from_blend_to_output() will return a LUT it creates from
	 * the xform pre-curve.
	 */
	curve_step = WESTON_COLOR_CURVE_STEP_PRE;

	/**
	 * First let's check if the LUT has already been cached. If that's the
	 * case, we make use of it.
	 */
	colorop_lut = drm_colorop_3x1d_lut_blob_search(device, xform, curve_step,
						       DRM_COLOROP_3X1D_LUT_BLOB_QUANTIZATION_U16,
						       lut_len);
	if (colorop_lut) {
		output->blend_to_output_xform = colorop_lut;
		return 0;
	}

	cm_lut = lut_3x1d_from_blend_to_output(compositor, xform, lut_len, &err_msg);
	if (!cm_lut) {
		drm_debug(b, "[output] failed to create 3x1D LUT for blend-to-output: %s\n",
			     err_msg);
		free(err_msg);
		return -1;
	}

	output->blend_to_output_xform =
		drm_colorop_3x1d_lut_blob_create(device, xform, curve_step,
						 DRM_COLOROP_3X1D_LUT_BLOB_QUANTIZATION_U16,
						 cm_lut, lut_len);
	free(cm_lut);
	if (!output->blend_to_output_xform) {
		drm_debug(b, "[output] failed to create colorop 3x1D LUT");
		return -1;
	}

	return 0;
}

enum writeback_screenshot_state
drm_output_get_writeback_state(struct drm_output *output)
{
	if (!output->wb_state)
		return DRM_OUTPUT_WB_SCREENSHOT_OFF;

	return output->wb_state->state;
}

static int
drm_crtc_num_planes(struct drm_device *device, struct drm_crtc *crtc)
{
	struct drm_plane *plane;
	int count = 0;

	wl_list_for_each(plane, &device->plane_list, link)
		if (plane->possible_crtcs & (1 << crtc->pipe))
			count++;

	return count;
}

/** Pick a CRTC that might be able to drive all attached connectors
 *
 * @param output The output whose attached heads to include.
 * @return CRTC object to pick, or NULL on failure or not found.
 */
static struct drm_crtc *
drm_output_pick_crtc(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct drm_backend *backend = device->backend;
	struct weston_compositor *compositor = backend->compositor;
	struct weston_head *base;
	struct drm_head *head;
	struct drm_crtc *crtc;
	struct drm_crtc *best_crtc = NULL;
	struct drm_crtc *fallback_crtc = NULL;
	struct drm_crtc *existing_crtc[32];
	uint32_t possible_crtcs = 0xffffffff;
	unsigned n = 0;
	uint32_t crtc_id;
	unsigned int i;
	bool match;
	int best_crtc_num_planes = 0;
	int num_planes;

	/* This algorithm ignores drmModeEncoder::possible_clones restriction,
	 * because it is more often set wrong than not in the kernel. */

	/* Accumulate a mask of possible crtcs and find existing routings. */
	wl_list_for_each(base, &output->base.head_list, output_link) {
		head = to_drm_head(base);

		possible_crtcs &=
			drm_connector_get_possible_crtcs_mask(&head->connector);

		crtc_id = head->inherited_crtc_id;
		if (crtc_id > 0 && n < ARRAY_LENGTH(existing_crtc))
			existing_crtc[n++] = drm_crtc_find(device, crtc_id);
	}

	/* Find a crtc that could drive each connector individually at least,
	 * and prefer existing routings. */
	wl_list_for_each(crtc, &device->crtc_list, link) {

		/* Could the crtc not drive each connector? */
		if (!(possible_crtcs & (1 << crtc->pipe)))
			continue;

		/* Is the crtc already in use? */
		if (crtc->output)
			continue;

		/* Try to preserve the existing CRTC -> connector routing;
		 * it makes initialisation faster, and also since we have a
		 * very dumb picking algorithm, may preserve a better
		 * choice. */
		for (i = 0; i < n; i++) {
			if (existing_crtc[i] == crtc)
				return crtc;
		}

		/* Check if any other head had existing routing to this CRTC.
		 * If they did, this is not the best CRTC as it might be needed
		 * for another output we haven't enabled yet. */
		match = false;
		wl_list_for_each(base, &compositor->head_list, compositor_link) {
			head = to_drm_head(base);
			if (!head)
				continue;

			if (head->base.output == &output->base)
				continue;

			if (weston_head_is_enabled(&head->base))
				continue;

			if (head->inherited_crtc_id == crtc->crtc_id) {
				match = true;
				break;
			}
		}

		num_planes = drm_crtc_num_planes(device, crtc);

		if (!match && num_planes > best_crtc_num_planes) {
			best_crtc_num_planes = num_planes;
			best_crtc = crtc;
		}

		fallback_crtc = crtc;
	}

	if (best_crtc)
		return best_crtc;

	if (fallback_crtc)
		return fallback_crtc;

	/* Likely possible_crtcs was empty due to asking for clones,
	 * but since the DRM documentation says the kernel lies, let's
	 * pick one crtc anyway. Trial and error is the only way to
	 * be sure if something doesn't work. */

	/* First pick any existing assignment. */
	for (i = 0; i < n; i++) {
		crtc = existing_crtc[i];
		if (!crtc->output)
			return crtc;
	}

	/* Otherwise pick any available crtc. */
	wl_list_for_each(crtc, &device->crtc_list, link) {
		if (!crtc->output)
			return crtc;
	}

	return NULL;
}

static bool
drm_crtc_populate_writeback_formats(struct drm_crtc *crtc)
{
	struct drm_writeback *wb;
	uint32_t possible_crtcs;
	int ret;

	weston_drm_format_array_init(&crtc->writeback_formats);

	wl_list_for_each(wb, &crtc->device->writeback_connector_list, link) {
		/* Ignore wb's that are incompatible with the CRTC. */
		possible_crtcs =
			drm_connector_get_possible_crtcs_mask(&wb->connector);
		if (!(possible_crtcs & (1 << crtc->pipe)))
			continue;

		ret = weston_drm_format_array_join(&crtc->writeback_formats, &wb->formats);
		if (ret < 0) {
			weston_drm_format_array_fini(&crtc->writeback_formats);
			return false;
		}
	}

	return true;
}

/** Create an "empty" drm_crtc. It will only set its ID, pipe and props. After
 * all, it adds the object to the DRM-backend CRTC list.
 */
static struct drm_crtc *
drm_crtc_create(struct drm_device *device, uint32_t crtc_id, uint32_t pipe)
{
	struct drm_crtc *crtc;
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(device->kms_device->fd, crtc_id,
					   DRM_MODE_OBJECT_CRTC);
	if (!props) {
		weston_log("failed to get CRTC properties\n");
		return NULL;
	}

	crtc = zalloc(sizeof(*crtc));
	if (!crtc)
		goto ret;

	crtc->device = device;
	crtc->crtc_id = crtc_id;
	crtc->pipe = pipe;

	if (!drm_crtc_populate_writeback_formats(crtc)) {
		free(crtc);
		crtc = NULL;
		goto ret;
	}

	drm_property_info_populate(device, crtc_props, crtc->props_crtc,
				   WDRM_CRTC__COUNT, props);

	crtc->lut_size =
		drm_property_get_value(&crtc->props_crtc[WDRM_CRTC_GAMMA_LUT_SIZE],
				       props, 0);

	crtc->background_color =
		drm_property_get_value(&crtc->props_crtc[WDRM_CRTC_BACKGROUND_COLOR],
				       props, 0);

	/* Add it to the last position of the DRM-backend CRTC list */
	wl_list_insert(device->crtc_list.prev, &crtc->link);

ret:
	drmModeFreeObjectProperties(props);
	return crtc;
}

/** Destroy a drm_crtc object that was created with drm_crtc_create(). It will
 * also remove it from the DRM-backend CRTC list.
 */
static void
drm_crtc_destroy(struct drm_crtc *crtc)
{
	assert(!crtc->output);

	wl_list_remove(&crtc->link);
	drm_property_info_free(crtc->props_crtc, WDRM_CRTC__COUNT);
	weston_drm_format_array_fini(&crtc->writeback_formats);
	free(crtc);
}

/** Find all CRTCs of the fd and create drm_crtc objects for them.
 *
 * The CRTCs are saved in a list of the drm_backend and will keep there until
 * the fd gets closed.
 *
 * @param device The DRM device structure.
 * @param resources The DRM resources, it is taken with drmModeGetResources
 * @return 0 on success (at least one CRTC in the list), -1 on failure.
 */
static int
drm_backend_create_crtc_list(struct drm_device *device, drmModeRes *resources)
{
	struct drm_crtc *crtc, *crtc_tmp;
	int i;

	/* Iterate through all CRTCs */
	for (i = 0; i < resources->count_crtcs; i++) {

		/* Let's create an object for the CRTC and add it to the list */
		crtc = drm_crtc_create(device, resources->crtcs[i], i);
		if (!crtc)
			goto err;
	}

	return 0;

err:
	wl_list_for_each_safe(crtc, crtc_tmp, &device->crtc_list, link)
		drm_crtc_destroy(crtc);
	return -1;
}


/** Populates scanout and cursor planes for the output. Also sets the topology
 * of the planes by adding them to the plane stacking list.
 */
static int
drm_output_init_planes(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct weston_compositor *wc = output->base.compositor;
	struct drm_plane *plane, *scanout_plane, *cursor_plane;
	struct drm_plane_handle *handle;
	uint64_t primary_plane_zpos_min;

	scanout_plane =	drm_output_find_special_plane(device, output,
						      WDRM_PLANE_TYPE_PRIMARY);
	if (!scanout_plane) {
		weston_log("Failed to find primary plane for output %s\n",
			   output->base.name);
		return -1;
	}
	primary_plane_zpos_min = scanout_plane->zpos_min;

	/**
	 * If only coverage blend mode supported by primary plane, renderers
	 * should produce straight alpha fb's.
	 */
	if (!drm_plane_supports_blend_mode(scanout_plane, WDRM_PLANE_BLEND_PREMULT)) {
		if (!drm_plane_supports_blend_mode(scanout_plane, WDRM_PLANE_BLEND_COVERAGE)) {
			weston_log("Error: primary plane must support either premult "
				   "or coverage alpha blend mode.\n");
			return -1;
		}
		output->base.fb_alpha_encoding = WESTON_OUTPUT_FB_ALPHA_STRAIGHT;
	}

	/* Failing to find a cursor plane is not fatal, as we'll fall back
	 * to software cursor. */
	cursor_plane =
		drm_output_find_special_plane(device, output,
					      WDRM_PLANE_TYPE_CURSOR);

	wl_list_for_each(plane, &device->plane_list, link) {
		struct drm_plane_handle *handle;

		if (!(plane->possible_crtcs & (1 << output->crtc->pipe)))
			continue;

		handle = drm_plane_create_handle(plane, output);

		if (plane == scanout_plane) {
			output->scanout_handle = handle;
			continue;
		}

		if (plane == cursor_plane) {
			output->cursor_handle = handle;
			continue;
		}
		wl_list_insert(&output->plane_handle_list, &handle->link);
	}

	assert(output->scanout_handle);
	assert(!cursor_plane || output->cursor_handle);

	output->has_underlay = false;
	wl_list_for_each(handle, &output->plane_handle_list, link) {
		plane = handle->plane;

		if (plane->zpos_min < primary_plane_zpos_min &&
		    plane->zpos_max >= primary_plane_zpos_min) {
			handle->subtype = PLANE_SUBTYPE_BOTH;
			output->has_underlay = true;
		} else if (plane->zpos_min < primary_plane_zpos_min &&
			   plane->zpos_max < primary_plane_zpos_min) {
			handle->subtype = PLANE_SUBTYPE_UNDERLAY_ONLY;
			output->has_underlay = true;
		} else {
			handle->subtype = PLANE_SUBTYPE_OVERLAY_ONLY;
		}

	}

	/**
	 * Depending on what the hardware supports, we must feed the primary
	 * plane with straight alpha fb's. If renderer can't do that, we can
	 * still support straight alpha outputs by disabling underlays (as the
	 * primary plane won't get blended with something below it).
	 */
	if (output->base.fb_alpha_encoding == WESTON_OUTPUT_FB_ALPHA_STRAIGHT &&
	    !wc->renderer->can_render_straight_alpha(wc) &&
	    output->has_underlay) {
		weston_log("Disabling underlay planes for output '%s': the primary plane does not support\n" \
			   "framebuffers with pre-multiplied alpha and the chosen renderer cannot produce\n" \
			   "straight alpha on this system.\n",
			   output->base.name);
		output->has_underlay = false;
		output->base.fb_alpha_encoding = WESTON_OUTPUT_FB_ALPHA_PREMULT;
	}

	return 0;
}

/** The opposite of drm_output_init_planes(). First of all it removes the planes
 * from the plane stacking list. After all it sets the planes of the output as NULL.
 */
static void
drm_output_deinit_planes(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct drm_plane_handle *handle, *next_handle;

	if (output->cursor_handle) {
		/* Turn off hardware cursor */
		drmModeSetCursor(device->kms_device->fd, output->crtc->crtc_id, 0, 0, 0);
	}

	/* With universal planes, the planes are allocated at startup,
	 * freed at shutdown, and live on the plane list in between.
	 * We want the planes to  continue to exist and be freed up
	 * for other outputs.
	 */
	if (output->cursor_handle) {
		drm_plane_reset_state(output->cursor_handle->plane);
		drm_plane_destroy_handle(output->cursor_handle);
		output->cursor_handle = NULL;
	}

	if (output->scanout_handle) {
		drm_plane_reset_state(output->scanout_handle->plane);
		drm_plane_destroy_handle(output->scanout_handle);
		output->scanout_handle = NULL;
	}

	wl_list_for_each_safe(handle, next_handle,
			      &output->plane_handle_list, link)
		drm_plane_destroy_handle(handle);
}

static struct weston_drm_format_array *
get_scanout_formats(struct drm_device *device)
{
	struct weston_compositor *ec = device->backend->compositor;
	const struct weston_drm_format_array *renderer_formats;
	struct weston_drm_format_array *scanout_formats, union_planes_formats;
	struct drm_plane *plane;
	int ret;

	/* If we got here it means that dma-buf feedback is supported and that
	 * the renderer has formats/modifiers to expose. */
	assert(ec->renderer->get_supported_dmabuf_formats != NULL);
	renderer_formats = ec->renderer->get_supported_dmabuf_formats(ec);

	scanout_formats = zalloc(sizeof(*scanout_formats));
	if (!scanout_formats) {
		weston_log("%s: out of memory\n", __func__);
		return NULL;
	}

	weston_drm_format_array_init(&union_planes_formats);
	weston_drm_format_array_init(scanout_formats);

	/* Compute the union of the format/modifiers of the KMS planes */
	wl_list_for_each(plane, &device->plane_list, link) {
		/* The scanout formats are used by the dma-buf feedback. But for
		 * now cursor planes do not support dma-buf buffers, only wl_shm
		 * buffers. So we skip cursor planes here. */
		if (plane->type == WDRM_PLANE_TYPE_CURSOR)
			continue;

		ret = weston_drm_format_array_join(&union_planes_formats,
						   &plane->formats);
		if (ret < 0)
			goto err;
	}

	/* Compute the intersection between the union of format/modifiers of KMS
	 * planes and the formats supported by the renderer */
	ret = weston_drm_format_array_replace(scanout_formats,
					      renderer_formats);
	if (ret < 0)
		goto err;

	ret = weston_drm_format_array_intersect(scanout_formats,
						&union_planes_formats);
	if (ret < 0)
		goto err;

	weston_drm_format_array_fini(&union_planes_formats);

	return scanout_formats;

err:
	weston_drm_format_array_fini(&union_planes_formats);
	weston_drm_format_array_fini(scanout_formats);
	free(scanout_formats);
	return NULL;
}

/** Pick a CRTC and reserve it for the output.
 *
 * On failure, the output remains without a CRTC.
 *
 * @param output The output with no CRTC associated.
 * @return 0 on success, -1 on failure.
 */
static int
drm_output_attach_crtc(struct drm_output *output)
{
	output->crtc = drm_output_pick_crtc(output);
	if (!output->crtc) {
		weston_log("Output '%s': No available CRTCs.\n",
			   output->base.name);
		return -1;
	}

	/* Reserve the CRTC for the output */
	output->crtc->output = output;

	return 0;
}

/** Release reservation of the CRTC.
 *
 * Make the CRTC free to be reserved and used by another output.
 *
 * @param output The output that will release its CRTC.
 */
static void
drm_output_detach_crtc(struct drm_output *output)
{
	struct drm_crtc *crtc = output->crtc;

	crtc->output = NULL;
	output->crtc = NULL;

	/* HACK: This is done here instead of in kms.c for the master mode */
	drmModeSetCrtc(crtc->device->kms_device->fd,
		       crtc->crtc_id, 0, 0, 0, NULL, 0, NULL);
}

static bool
should_wait_drm_events(struct drm_device *device)
{
	struct weston_output *base;
	struct drm_output *output;

	wl_list_for_each(base, &device->backend->compositor->output_list, link) {
		/* We only care about outputs related to the DRM backend. */
		output = to_drm_output(base);
		if (!output)
			continue;

		/* We only care about the outputs on our device. */
		if (output->device != device)
			continue;

		if (output->destroy_pending || output->disable_pending)
			return true;
	}

	return false;
}

static int
drm_output_enable(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	int ret;

	assert(output);
	assert(!output->is_virtual);

	/* TODO: drop this hack when we rework the output configuration API. For
	 * now we need this because the frontend may call
	 * weston_output_destroy() or weston_output_disable() but it may have a
	 * pending page flip. In such case the destruction is delayed, but the
	 * frontend can't tell that. Until the flip completes, the DRM objects
	 * of the card (CRTC, planes, etc) won't be available, as they will
	 * still be attached to the output marked to be destroyed/disabled. So
	 * if the frontend calls weston_output_enable() for an output right
	 * after weston_output_destroy() or weston_output_disable(), it might
	 * not find DRM objects available and fail. So we spin here until the
	 * flip completes and the output gets destroyed/disabled and release the
	 * DRM objects. */
	while (should_wait_drm_events(device))
		on_drm_input(device->kms_device->fd, 0 /* unused mask */, device);

	output->connector_colorspace = wdrm_colorspace_from_output(&output->base);
	if (output->connector_colorspace == WDRM_COLORSPACE__COUNT)
		return -1;

	output->connector_color_format = wdrm_color_format_from_output(&output->base);
	if (output->connector_color_format == WDRM_COLOR_FORMAT__COUNT)
		return -1;

	ret = drm_output_attach_crtc(output);
	if (ret < 0)
		return -1;

	ret = drm_output_init_planes(output);
	if (ret < 0)
		goto err_crtc;

	if (drm_output_init_legacy_gamma_size(output) < 0)
		goto err_planes;

	output->base.from_blend_to_output_by_backend = b->offload_blend_to_output;
	if (output->base.from_blend_to_output_by_backend &&
	    drm_output_pick_blend_to_output(output) < 0)
		goto err_planes;

	if (b->pageflip_timeout)
		drm_output_pageflip_timer_create(output);

	if (b->compositor->renderer->type == WESTON_RENDERER_PIXMAN) {
		if (drm_output_init_pixman(output, b) < 0) {
			weston_log("Failed to init output pixman state\n");
			goto err_planes;
		}
	} else if (b->compositor->renderer->type == WESTON_RENDERER_VULKAN) {
		if (drm_output_init_vulkan(output, b) < 0) {
			weston_log("Failed to init output vulkan state\n");
			goto err_planes;
		}
	} else if (drm_output_init_egl(output, b) < 0) {
		weston_log("Failed to init output gl state\n");
		goto err_planes;
	}

	drm_output_init_backlight(output);

	output->base.start_repaint_loop = drm_output_start_repaint_loop;
	output->base.prepare_repaint = drm_output_prepare_repaint;
	output->base.repaint = drm_output_repaint;
	output->base.assign_planes = drm_assign_planes;
	output->base.set_dpms = drm_set_dpms;
	output->base.switch_mode = drm_output_switch_mode;

	if (device->atomic_modeset)
		weston_output_update_capture_info(base,
						  WESTON_OUTPUT_CAPTURE_SOURCE_WRITEBACK,
						  base->current_mode->width,
						  base->current_mode->height,
						  NULL,
						  weston_output_get_writeback_formats(&output->base));

	weston_log("Output %s (crtc %d) video modes:\n",
		   output->base.name, output->crtc->crtc_id);
	drm_output_print_modes(output);

	return 0;

err_planes:
	drm_output_deinit_planes(output);
err_crtc:
	drm_output_detach_crtc(output);
	return -1;
}

static void
drm_output_deinit(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = output->backend;
	struct drm_device *device = output->device;
	struct drm_pending_state *pending;

	if (!b->compositor->shutting_down) {
		pending = drm_pending_state_alloc(device);
		drm_output_get_disable_state(pending, output);
		drm_pending_state_apply_sync(pending);
	}

	/*
	 * Remove all potential drm_fb references to GBM BOs, so that the
	 * renderer tear-down can destroy the originating GBM/Vulkan
	 * surface without leaving dangling drm_fb pointers.
	 */
	drm_output_deinit_planes(output);

	if (b->compositor->renderer->type == WESTON_RENDERER_PIXMAN)
		drm_output_fini_pixman(output);
	else if (b->compositor->renderer->type == WESTON_RENDERER_VULKAN)
		drm_output_fini_vulkan(output);
	else
		drm_output_fini_egl(output);

	drm_output_detach_crtc(output);

	output->blend_to_output_xform = NULL;
	output->base.from_blend_to_output_by_backend = false;

	if (output->hdr_output_metadata_blob_id) {
		drmModeDestroyPropertyBlob(device->kms_device->fd,
					   output->hdr_output_metadata_blob_id);
		output->hdr_output_metadata_blob_id = 0;
	}
}

void
drm_output_destroy(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_device *device = output->device;
	struct drm_head *head, *tmp;

	assert(output);
	assert(!output->is_virtual);

	wl_list_for_each_safe(head, tmp, &output->disable_head,
			      disable_head_link) {
		wl_list_remove(&head->disable_head_link);
		wl_list_init(&head->disable_head_link);
	}

	if (output->page_flip_pending || output->atomic_complete_pending) {
		if (!base->compositor->shutting_down) {
			/* We are not shutting down, so we can wait for flip
			 * completion. */
			output->destroy_pending = true;
			weston_log("delaying output destruction because of a " \
				   "pending flip, wait until it completes\n");
			return;
		} else {
			weston_log("destroying output %s (id %u) with a pending " \
				   "flip, but as we are shutting down we can't " \
				   "wait to destroy it when the flip completes... " \
				   "destroying it now\n", base->name, base->id);
		}
	}

	if (output->base.enabled)
		drm_output_deinit(&output->base);

	drm_mode_list_destroy(device, &output->base.mode_list);

	if (output->pageflip_timer)
		wl_event_source_remove(output->pageflip_timer);

	weston_output_release(&output->base);

	assert(!output->state_last);
	drm_output_state_free(output->state_cur);

	assert(output->hdr_output_metadata_blob_id == 0);

	wl_list_remove(&output->disable_head);

	free(output);
}

static int
drm_output_disable(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);

	assert(output);
	assert(!output->is_virtual);

	if (output->page_flip_pending || output->atomic_complete_pending) {
		output->disable_pending = true;
		return -1;
	}

	weston_log("Disabling output %s\n", output->base.name);

	if (output->base.enabled)
		drm_output_deinit(&output->base);

	output->disable_pending = false;

	return 0;
}

/*
 * This function converts the protection status from drm values to
 * weston_hdcp_protection status. The drm values as read from the connector
 * properties "Content Protection" and "HDCP Content Type" need to be converted
 * to appropriate weston values, that can be sent to a client application.
 */
static int
get_weston_protection_from_drm(enum wdrm_content_protection_state protection,
			       enum wdrm_hdcp_content_type type,
			       enum weston_hdcp_protection *weston_protection)

{
	if (protection >= WDRM_CONTENT_PROTECTION__COUNT)
		return -1;
	if (protection == WDRM_CONTENT_PROTECTION_DESIRED ||
	    protection == WDRM_CONTENT_PROTECTION_UNDESIRED) {
		*weston_protection = WESTON_HDCP_DISABLE;
		return 0;
	}
	if (type >= WDRM_HDCP_CONTENT_TYPE__COUNT)
		return -1;
	if (type == WDRM_HDCP_CONTENT_TYPE0) {
		*weston_protection = WESTON_HDCP_ENABLE_TYPE_0;
		return 0;
	}
	if (type == WDRM_HDCP_CONTENT_TYPE1) {
		*weston_protection = WESTON_HDCP_ENABLE_TYPE_1;
		return 0;
	}
	return -1;
}

/**
 * Get current content-protection status for a given head.
 *
 * @param head drm_head, whose protection is to be retrieved
 * @return protection status in case of success, -1 otherwise
 */
static enum weston_hdcp_protection
drm_head_get_current_protection(struct drm_head *head)
{
	drmModeObjectProperties *props = head->connector.props_drm;
	struct drm_property_info *info;
	enum wdrm_content_protection_state protection;
	enum wdrm_hdcp_content_type type;
	enum weston_hdcp_protection weston_hdcp = WESTON_HDCP_DISABLE;

	info = &head->connector.props[WDRM_CONNECTOR_CONTENT_PROTECTION];
	protection = drm_property_get_value(info, props,
					    WDRM_CONTENT_PROTECTION__COUNT);

	if (protection == WDRM_CONTENT_PROTECTION__COUNT)
		return WESTON_HDCP_DISABLE;

	info = &head->connector.props[WDRM_CONNECTOR_HDCP_CONTENT_TYPE];
	type = drm_property_get_value(info, props,
				      WDRM_HDCP_CONTENT_TYPE__COUNT);

	/*
	 * In case of platforms supporting HDCP1.4, only property
	 * 'Content Protection' is exposed and not the 'HDCP Content Type'
	 * for such cases HDCP Type 0 should be considered as the content-type.
	 */

	if (type == WDRM_HDCP_CONTENT_TYPE__COUNT)
		type = WDRM_HDCP_CONTENT_TYPE0;

	if (get_weston_protection_from_drm(protection, type,
					   &weston_hdcp) == -1) {
		weston_log("Invalid drm protection:%d type:%d, for head:%s connector-id:%d\n",
			   protection, type, head->base.name,
			   head->connector.connector_id);
		return WESTON_HDCP_DISABLE;
	}

	return weston_hdcp;
}

static int
drm_connector_update_properties(struct drm_connector *connector)
{
	struct drm_device *device = connector->device;
	drmModeObjectProperties *props;

	props = drmModeObjectGetProperties(device->kms_device->fd,
					   connector->connector_id,
					   DRM_MODE_OBJECT_CONNECTOR);
	if (!props) {
		weston_log("Error: failed to get connector properties\n");
		return -1;
	}

	if (connector->props_drm)
		drmModeFreeObjectProperties(connector->props_drm);
	connector->props_drm = props;

	return 0;
}

/** Replace connector data and monitor information
 *
 * @param connector The drm_connector object to be updated.
 * @param conn The connector data to be owned by the drm_connector, must match
 * the current drm_connector ID.
 * @return 0 on success, -1 on failure.
 *
 * Takes ownership of @c connector on success, not on failure.
 */
static int
drm_connector_assign_connector_info(struct drm_connector *connector,
				    drmModeConnector *conn)
{
	struct drm_device *device = connector->device;

	assert(connector->conn != conn);
	assert(connector->connector_id == conn->connector_id);

	if (drm_connector_update_properties(connector) < 0)
		return -1;

	if (connector->conn)
		drmModeFreeConnector(connector->conn);
	connector->conn = conn;

	drm_property_info_free(connector->props, WDRM_CONNECTOR__COUNT);
	drm_property_info_populate(device, connector_props, connector->props,
				   WDRM_CONNECTOR__COUNT, connector->props_drm);
	return 0;
}

static void
drm_connector_init(struct drm_device *device, struct drm_connector *connector,
		   uint32_t connector_id)
{
	connector->device = device;
	connector->connector_id = connector_id;
	connector->conn = NULL;
	connector->props_drm = NULL;
}

static const char *
vrr_mode_to_str(enum weston_vrr_mode vrr_mode)
{
	switch (vrr_mode) {
	case WESTON_VRR_MODE_NONE:		return "(none)";
	case WESTON_VRR_MODE_GAME:		return "Game";
	}
	return "???";
}

static void
drm_connector_fini(struct drm_connector *connector)
{
	drmModeFreeConnector(connector->conn);
	drmModeFreeObjectProperties(connector->props_drm);
	drm_property_info_free(connector->props, WDRM_CONNECTOR__COUNT);
}

static char *
weston_vrr_mask_to_str(uint32_t mask)
{
	return bits_to_str(mask, vrr_mode_to_str);
}

static void
drm_head_log_info(struct drm_head *head, const char *msg)
{
	char *str;

	if (head->base.connected) {
		weston_log("DRM: head '%s' %s, connector %d is connected, "
			   "EDID make '%s', model '%s', serial '%s'\n",
			   head->base.name, msg, head->connector.connector_id,
			   head->base.make, head->base.model,
			   head->base.serial_number ?: "");
		str = weston_eotf_mask_to_str(head->base.supported_eotf_mask);
		if (str) {
			weston_log_continue(STAMP_SPACE
					    "Supported EOTF modes: %s\n",
					    str);
		}
		free(str);

		str = weston_colorimetry_mask_to_str(head->base.supported_colorimetry_mask);
		if (str) {
			weston_log_continue(STAMP_SPACE
					    "Supported colorimetry modes: %s\n",
					    str);
		}
		free(str);

		str = weston_vrr_mask_to_str(head->base.supported_vrr_mode_mask);
		if (str) {
			weston_log_continue(STAMP_SPACE
					    "Supported VRR modes: (none), %s\n",
					    str);
		}
		free(str);

		if (head->base.underscan_supported) {
			weston_log_continue(STAMP_SPACE
					    "Underscan supported.\n");
			weston_log_continue(STAMP_SPACE
					    "\tunderscan-hborder max: %d\n",
					    head->base.underscan_hborder_max);
			weston_log_continue(STAMP_SPACE
					    "\tunderscan-vborder max: %d\n",
					    head->base.underscan_vborder_max);
		}

		str = weston_color_format_mask_to_str(head->base.supported_color_format_mask);
		if (str) {
			weston_log_continue(STAMP_SPACE
					    "Supported color formats: %s\n",
					    str);
		}
		free(str);
	} else {
		weston_log("DRM: head '%s' %s, connector %d is disconnected.\n",
			   head->base.name, msg, head->connector.connector_id);
	}
}

/** Update connector and monitor information
 *
 * @param head The head to update.
 * @param conn The DRM connector object.
 * @returns 0 on success, -1 on failure.
 *
 * Updates monitor information and connection status. This may schedule a
 * heads changed call to the user.
 *
 * Takes ownership of @c connector on success, not on failure.
 */
static int
drm_head_update_info(struct drm_head *head, drmModeConnector *conn)
{
	int ret;

	ret = drm_connector_assign_connector_info(&head->connector, conn);
	if (ret == 0) {
		update_head_from_connector(head);
		weston_head_set_content_protection_status(&head->base,
						drm_head_get_current_protection(head));
	}

	return ret;
}

/** Update writeback connector
 *
 * @param writeback The writeback to update.
 * @param conn DRM connector object.
 * @returns 0 on success, -1 on failure.
 *
 * Takes ownership of @c connector on success, not on failure.
 */
static int
drm_writeback_update_info(struct drm_writeback *writeback, drmModeConnector *conn)
{
	int ret;

	ret = drm_connector_assign_connector_info(&writeback->connector, conn);

	return ret;
}

/**
 * Create a Weston head for a connector
 *
 * Given a DRM connector, create a matching drm_head structure and add it
 * to Weston's head list.
 *
 * @param device DRM device structure
 * @param conn DRM connector object
 * @param drm_device udev device pointer
 * @returns 0 on success, -1 on failure
 *
 * Takes ownership of @c connector on success, not on failure.
 */
static int
drm_head_create(struct drm_device *device, drmModeConnector *conn,
		struct udev_device *drm_device)
{
	struct drm_backend *backend = device->backend;
	struct drm_head *head;
	char *name;
	int ret;

	head = zalloc(sizeof *head);
	if (!head)
		return -1;

	drm_connector_init(device, &head->connector, conn->connector_id);

	name = make_connector_name(conn);
	if (!name)
		goto err;

	weston_head_init(&head->base, name);
	free(name);

	head->base.backend = &backend->base;

	wl_list_init(&head->disable_head_link);

	ret = drm_head_update_info(head, conn);
	if (ret < 0)
		goto err_update;

	head->backlight = backlight_init(drm_device, conn->connector_type);
	if (head->backlight && head->backlight->max_brightness == 0) {
		weston_log("Failed to retreive a valid value for max_brightness"
			   " from connector %d. Backlight disabled\n",
			   head->connector.connector_id);
		backlight_destroy(head->backlight);
		head->backlight = NULL;
	}

	switch(conn->connector_type) {
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_eDP:
	case DRM_MODE_CONNECTOR_DSI:
	case DRM_MODE_CONNECTOR_DPI:
		weston_head_set_internal(&head->base);
		break;
	default:
		break;
	}

	if (drm_head_read_current_setup(head, device) < 0) {
		weston_log("Failed to retrieve current mode from connector %d.\n",
			   head->connector.connector_id);
		/* Not fatal. */
	}

	weston_compositor_add_head(backend->compositor, &head->base);
	drm_head_log_info(head, "found");

	return 0;

err_update:
	weston_head_release(&head->base);
err:
	drm_connector_fini(&head->connector);
	free(head);
	return -1;
}

static void
drm_head_destroy(struct weston_head *base)
{
	struct drm_head *head = to_drm_head(base);

	assert(head);

	drm_free_display_info(&head->base.display_info);
	weston_head_release(&head->base);

	drm_connector_fini(&head->connector);

	if (head->backlight)
		backlight_destroy(head->backlight);

	wl_list_remove(&head->disable_head_link);

	free(head->display_data);
	free(head);
}

static struct drm_device *
drm_device_find_by_output(struct weston_compositor *compositor, const char *name)
{
	struct drm_device *device = NULL;
	struct weston_head *base = NULL;
	struct drm_head *head;
	const char *tmp;

	while ((base = weston_compositor_iterate_heads(compositor, base))) {
		tmp = weston_head_get_name(base);
		if (strcmp(name, tmp) != 0)
			continue;
		head = to_drm_head(base);
		device = head->connector.device;
		break;
	}

	return device;
}

/**
 * Create a Weston output structure
 *
 * Create an "empty" drm_output. This is the implementation of
 * weston_backend::create_output.
 *
 * Creating an output is usually followed by drm_output_attach_head()
 * and drm_output_enable() to make use of it.
 *
 * @param backend The backend instance.
 * @param name Name for the new output.
 * @returns The output, or NULL on failure.
 */
static struct weston_output *
drm_output_create(struct weston_backend *backend, const char *name)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct drm_device *device;
	struct drm_output *output;

	device = drm_device_find_by_output(b->compositor, name);
	if (!device)
		return NULL;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;

	output->device = device;
	output->crtc = NULL;

	wl_list_init(&output->disable_head);

	output->max_bpc = 16;
#ifdef BUILD_DRM_GBM
	output->gbm_bo_flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
#endif

	weston_output_init(&output->base, b->compositor, name);

	output->base.enable = drm_output_enable;
	output->base.destroy = drm_output_destroy;
	output->base.disable = drm_output_disable;
	output->base.attach_head = drm_output_attach_head;
	output->base.detach_head = drm_output_detach_head;

	output->base.get_writeback_formats = drm_output_get_writeback_formats;

	output->backend = b;

	output->destroy_pending = false;
	output->disable_pending = false;

	output->state_cur = drm_output_state_alloc(output);

	wl_list_init(&output->plane_handle_list);

	weston_compositor_add_pending_output(&output->base, b->compositor);

	return &output->base;
}

static void
pixman_copy_screenshot(uint32_t *dst, uint32_t *src, int dst_stride,
		       int src_stride, int pixman_format, int width, int height)
{
	pixman_image_t *pixman_dst;
	pixman_image_t *pixman_src;

	pixman_src = pixman_image_create_bits(pixman_format,
					      width, height,
					      src, src_stride);
	pixman_dst = pixman_image_create_bits(pixman_format,
					      width, height,
					      dst, dst_stride);
	assert(pixman_src);
	assert(pixman_dst);

	pixman_image_composite32(PIXMAN_OP_SRC,
				 pixman_src,     /* src */
				 NULL,           /* mask */
				 pixman_dst,     /* dst */
				 0, 0,           /* src_x, src_y */
				 0, 0,           /* mask_x, mask_y */
				 0, 0,           /* dst_x, dst_y */
				 width, height); /* width, height */

	pixman_image_unref(pixman_src);
	pixman_image_unref(pixman_dst);
}

static void
drm_writeback_success_screenshot(struct drm_writeback_state *state)
{
	struct drm_output *output = state->output;
	struct weston_compositor *c = output->base.compositor;
	struct weston_buffer *buffer;
	int width, height;
	int dst_stride, src_stride;
	uint32_t *src, *dst;

	/**
	 * Capture task already retired, see
	 * drm_writeback_state_ct_destroy_handler(). Here we destroy the wb
	 * state.
	 */
	if (!state->ct)
		goto destroy_state;

	buffer = weston_capture_task_get_buffer(state->ct);

	if (buffer->type == WESTON_BUFFER_SHM) {
		src = state->fb->map;
		src_stride = state->fb->strides[0];

		dst = wl_shm_buffer_get_data(buffer->shm_buffer);
		dst_stride = buffer->stride;

		width = state->fb->width;
		height = state->fb->height;

		wl_shm_buffer_begin_access(buffer->shm_buffer);
		pixman_copy_screenshot(dst, src, dst_stride, src_stride,
				       buffer->pixel_format->pixman_format,
				       width, height);
		wl_shm_buffer_end_access(buffer->shm_buffer);
	}

	weston_capture_task_retire_complete(state->ct);
	state->ct = NULL;

destroy_state:
	drm_writeback_state_free(c, state);
	output->wb_state = NULL;
}

void
drm_writeback_fail_screenshot(struct drm_writeback_state *state,
			      const char *err_msg)
{
	struct drm_output *output = state->output;
	struct weston_compositor *c = output->base.compositor;

	/**
	 * Capture task already retired, see
	 * drm_writeback_state_ct_destroy_handler(). Here we destroy the wb
	 * state.
	 */
	if (!state->ct)
		goto destroy_state;

	weston_capture_task_retire_failed(state->ct, err_msg);
	state->ct = NULL;

destroy_state:
	drm_writeback_state_free(c, state);
	output->wb_state = NULL;
}

static int
drm_writeback_save_callback(int fd, uint32_t mask, void *data)
{
	struct drm_writeback_state *state = data;

	wl_event_source_remove(state->wb_source);
	close(fd);

	drm_writeback_success_screenshot(state);

	return 0;
}

static bool
drm_writeback_has_finished(struct drm_writeback_state *state)
{
	struct pollfd pollfd;
	int ret;

	pollfd.fd = state->out_fence_fd;
	pollfd.events = POLLIN;

	while ((ret = poll(&pollfd, 1, 0)) == -1 && errno == EINTR)
		continue;

	if (ret < 0) {
		drm_writeback_fail_screenshot(state, "drm: polling wb fence failed");
		return true;
	} else if (ret > 0) {
		/* fence already signaled, simply save the screenshot */
		drm_writeback_success_screenshot(state);
		return true;
	}

	/* poll() returned 0, what means that out fence was not signalled yet */
	return false;
}

/**
 * Try to complete writeback screenshot
 *
 * After submitting a writeback task with an atomic commit, call this function
 * to complete the screenshot. If the writeback result is already available, the
 * screenshot is completed immediately. Otherwise, drm_writeback_save_callback()
 * is scheduled to finish it later.
 *
 * @param state The writeback task state.
 * @return true if the screenshot was completed immediately, false if deferred.
 */
bool
drm_writeback_try_complete(struct drm_writeback_state *state)
{
	struct weston_compositor *ec = state->output->base.compositor;
	struct wl_event_loop *event_loop;

	if (state->state == DRM_OUTPUT_WB_SCREENSHOT_WAITING_SIGNAL)
		return false;

	if (state->state == DRM_OUTPUT_WB_SCREENSHOT_CHECK_FENCE) {
		if (drm_writeback_has_finished(state))
			return true;

		/* The writeback has not finished yet. So add callback that gets
		 * called when the sync fd of the writeback job gets signalled.
		 * We need to wait for that to resume the repaint loop. */
		event_loop = wl_display_get_event_loop(ec->wl_display);
		state->wb_source =
			wl_event_loop_add_fd(event_loop, state->out_fence_fd,
					     WL_EVENT_READABLE,
					     drm_writeback_save_callback, state);
		if (!state->wb_source) {
			drm_writeback_fail_screenshot(state, "drm: out of memory");
			return true;
		}

		state->state = DRM_OUTPUT_WB_SCREENSHOT_WAITING_SIGNAL;

		return false;
	}

	weston_assert_not_reached(ec, "drm_writeback_try_complete() called without a wb task submitted");
}

void
drm_writeback_reference_planes(struct drm_writeback_state *state,
			       struct wl_list *plane_state_list)
{
	struct drm_plane_state *plane_state;
	struct drm_fb **fb;

	wl_list_for_each(plane_state, plane_state_list, link) {
		if (!plane_state->fb)
			continue;
		fb = wl_array_add(&state->referenced_fbs, sizeof(*fb));
		*fb = drm_fb_ref(plane_state->fb);
	}
}

static int
drm_writeback_populate_formats(struct drm_writeback *wb)
{
	struct drm_property_info *info = wb->connector.props;
	drmModeObjectProperties *props = wb->connector.props_drm;
	uint64_t blob_id;
	drmModePropertyBlobPtr blob;
	uint32_t *blob_formats;
	unsigned int i;
	int ret = 0;

	blob_id = drm_property_get_value(&info[WDRM_CONNECTOR_WRITEBACK_PIXEL_FORMATS],
					 props, 0);
	if (blob_id == 0)
		return -1;

	blob = drmModeGetPropertyBlob(wb->device->kms_device->fd, blob_id);
	if (!blob)
		return -1;

	blob_formats = blob->data;

	for (i = 0; i < blob->length / sizeof(uint32_t); i++) {
		struct weston_drm_format *fmt;

		if (!pixel_format_get_info(blob_formats[i]))
			continue;

		fmt = weston_drm_format_array_add_format(&wb->formats,
							 blob_formats[i]);
		if (fmt == NULL ||
		    weston_drm_format_add_modifier(fmt,
						   DRM_FORMAT_MOD_LINEAR) < 0) {
			ret = -1;
			break;
		}
	}

	drmModeFreePropertyBlob(blob);

	return ret;
}

/**
 * Create a Weston writeback for a writeback connector
 *
 * Given a DRM connector of type writeback, create a matching drm_writeback
 * structure and add it to Weston's writeback list.
 *
 * @param device DRM device structure
 * @param conn DRM connector object of type writeback
 * @returns 0 on success, -1 on failure
 *
 * Takes ownership of @c connector on success, not on failure.
 */
static int
drm_writeback_create(struct drm_device *device, drmModeConnector *conn)
{
	struct drm_writeback *writeback;
	int ret;

	writeback = zalloc(sizeof *writeback);
	assert(writeback);

	writeback->device = device;

	drm_connector_init(device, &writeback->connector, conn->connector_id);

	ret = drm_writeback_update_info(writeback, conn);
	if (ret < 0)
		goto err;

	weston_drm_format_array_init(&writeback->formats);
	ret = drm_writeback_populate_formats(writeback);
	if (ret < 0)
		goto err_formats;

	wl_list_insert(&device->writeback_connector_list, &writeback->link);
	return 0;

err_formats:
	weston_drm_format_array_fini(&writeback->formats);
err:
	drm_connector_fini(&writeback->connector);
	free(writeback);
	return -1;
}

static void
drm_writeback_destroy(struct drm_writeback *writeback)
{
	drm_connector_fini(&writeback->connector);
	weston_drm_format_array_fini(&writeback->formats);
	wl_list_remove(&writeback->link);

	free(writeback);
}

/** Given the DRM connector object of a connector, create drm_head or
 * drm_writeback object (depending on the type of connector) for it.
 *
 * The object is then added to the DRM-backend list of heads or writebacks.
 *
 * @param device The DRM device structure
 * @param conn The DRM connector object
 * @param drm_device udev device pointer
 * @return 0 on success, -1 on failure
 */
static int
drm_backend_add_connector(struct drm_device *device, drmModeConnector *conn,
			  struct udev_device *drm_device)
{
	int ret;

	if (conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		ret = drm_writeback_create(device, conn);
		if (ret < 0)
			weston_log("DRM: failed to create writeback for connector %d.\n",
				   conn->connector_id);
	} else {
		ret = drm_head_create(device, conn, drm_device);
		if (ret < 0)
			weston_log("DRM: failed to create head for connector %d.\n",
				   conn->connector_id);
	}

	return ret;
}

/** Find all connectors of the fd and create drm_head or drm_writeback objects
 * (depending on the type of connector they are) for each of them
 *
 * These objects are added to the DRM-backend lists of heads and writebacks.
 *
 * @param device The DRM device structure
 * @param drm_device udev device pointer
 * @param resources The DRM resources, it is taken with drmModeGetResources
 * @return 0 on success, -1 on failure
 */
static int
drm_backend_discover_connectors(struct drm_device *device,
				struct udev_device *drm_device,
				drmModeRes *resources)
{
	drmModeConnector *conn;
	int i, ret;

	device->min_width  = resources->min_width;
	device->max_width  = resources->max_width;
	device->min_height = resources->min_height;
	device->max_height = resources->max_height;

	for (i = 0; i < resources->count_connectors; i++) {
		uint32_t connector_id = resources->connectors[i];

		conn = drmModeGetConnector(device->kms_device->fd, connector_id);
		if (!conn)
			continue;

		ret = drm_backend_add_connector(device, conn, drm_device);
		if (ret < 0)
			drmModeFreeConnector(conn);
	}

	return 0;
}

static bool
resources_has_connector(drmModeRes *resources, uint32_t connector_id)
{
	for (int i = 0; i < resources->count_connectors; i++) {
		if (resources->connectors[i] == connector_id)
			return true;
	}

	return false;
}

static void
drm_backend_update_connector(struct drm_device *device,
			     struct udev_device *drm_device,
			     uint32_t connector_id)
{
	struct drm_backend *b = device->backend;
	drmModeConnector *conn;
	struct drm_head *head;
	struct drm_writeback *writeback;
	int ret;

	conn = drmModeGetConnector(device->kms_device->fd, connector_id);
	if (!conn)
		return;

	head = drm_head_find_by_connector(b, device, connector_id);
	writeback = drm_writeback_find_by_connector(device, connector_id);

	/* Connector can't be owned by both a head and a writeback, so
	 * one of the searches must fail. */
	assert(head == NULL || writeback == NULL);

	if (head) {
		ret = drm_head_update_info(head, conn);
		if (head->base.device_changed) {
			drm_head_log_info(head, "updated");
		}

		/* a no change in weston_head::device_changed but with
		 * connected status still on, means we got here through a udev
		 * HOTPLUG event and we further got interrupted by another
		 * HOTPLUG event.
		 *
		 * When this happens mark the state as invalid to allow to
		 * connector/output to be enabled on a next flip; otherwise we
		 * reach a point where the kernel had the connector disabled
		 * but we Weston has it enabled, finishing finally with Weston
		 * not doing anything to re-enable the output */
		if (!head->base.device_changed && head->base.connected) {
			drm_debug(b, "\t[CONN:%d] Invalid state detected.\n",
				  connector_id);

			drm_device_recovery_required(device);
		}
	} else if (writeback) {
		ret = drm_writeback_update_info(writeback, conn);
	} else {
		ret = drm_backend_add_connector(device, conn, drm_device);
	}

	if (ret < 0)
		drmModeFreeConnector(conn);
}

static void
drm_backend_update_connectors_post_destroy(struct drm_device *device,
					   drmModeRes *resources)
{
	struct drm_backend *b = device->backend;
	struct weston_head *base, *base_next;
	struct drm_head *head;
	struct drm_writeback *writeback, *writeback_next;
	uint32_t connector_id;

	if (!resources)
		return;

	/* Destroy head objects of connectors (except writeback connectors) that
	 * have disappeared. */
	wl_list_for_each_safe(base, base_next,
			      &b->compositor->head_list, compositor_link) {
		head = to_drm_head(base);
		if (!head)
			continue;
		connector_id = head->connector.connector_id;

		if (head->connector.device != device)
			continue;

		if (resources_has_connector(resources, connector_id))
			continue;

		weston_log("DRM: head '%s' (connector %d) disappeared.\n",
			   head->base.name, connector_id);
		drm_head_destroy(base);
	}

	/* Destroy writeback objects of writeback connectors that have
	 * disappeared. */
	wl_list_for_each_safe(writeback, writeback_next,
			      &b->drm->writeback_connector_list, link) {
		connector_id = writeback->connector.connector_id;

		if (resources_has_connector(resources, connector_id))
			continue;

		weston_log("DRM: writeback connector (connector %d) disappeared.\n",
			   connector_id);
		drm_writeback_destroy(writeback);
	}
}

static void
drm_backend_update_connectors(struct drm_device *device)
{
	struct udev_device *drm_device;
	drmModeRes *resources;
	int i;

	drm_device = device->kms_device->udev_device;

	resources = drmModeGetResources(device->kms_device->fd);
	if (!resources) {
		weston_log("drmModeGetResources failed\n");
		return;
	}

	for (i = 0; i < resources->count_connectors; i++) {
		uint32_t connector_id = resources->connectors[i];
		drm_backend_update_connector(device, drm_device, connector_id);
	}

	drm_backend_update_connectors_post_destroy(device, resources);
	drmModeFreeResources(resources);
}

static int
udev_event_is_hotplug(struct drm_backend *b, struct udev_device *udev_device)
{
	const char *val;

	drm_debug(b, "[udev] HOTPLUG event\n");

	val = udev_device_get_property_value(udev_device, "HOTPLUG");
	if (!val)
		return 0;

	return strcmp(val, "1") == 0;
}

/**
 * Process debounced hotplug update
 * @param b DRM backend instance
 *
 * Checks if sufficient time has passed since last update.
 * If yes: processes immediately; if no: schedules timer.
 */
static void
drm_hotplug_update(struct drm_backend *b)
{
	struct timespec now;
	int64_t now_ms, next_ms;

	/* Skip if update already scheduled */
	if (b->pending_hotplug_update)
		return;

	next_ms = b->last_hotplug_update_ms + DRM_HOTPLUG_DEBOUNCE_MS;

	weston_compositor_read_presentation_clock(b->compositor, &now);
	now_ms = timespec_to_msec(&now);

	if (next_ms <= now_ms) {
		/* Time window passed: process immediately */
		struct drm_device *device_iter;
		wl_list_for_each(device_iter, &b->kms_list, link)
			drm_backend_update_connectors(device_iter);
		b->last_hotplug_update_ms = now_ms;
	} else {
		/* Too soon: schedule delayed processing */
		b->pending_hotplug_update = true;
		wl_event_source_timer_update(b->hotplug_update_timer,
					     next_ms - now_ms);
	}
}

static int
hotplug_update_handler(void *data)
{
	struct drm_backend *b = data;
	b->pending_hotplug_update = false;
	drm_hotplug_update(b);
	return 0;
}

static int
udev_drm_event(int fd, uint32_t mask, void *data)
{
	struct drm_backend *b = data;
	struct udev_device *event;

	event = udev_monitor_receive_device(b->udev_monitor);
	if (udev_event_is_hotplug(b, event))
		drm_hotplug_update(b);

	udev_device_unref(event);
	return 1;
}

static void
drm_shutdown(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct weston_compositor *ec = b->compositor;
	struct weston_output *output_base;
	struct drm_output *output;

	udev_input_destroy(&b->input);

	wl_event_source_remove(b->hotplug_update_timer);
	wl_event_source_remove(b->udev_drm_source);
	wl_event_source_remove(b->perf_page_flips_stats.pageflip_timer_counter);

	/* We are shutting down. This function destroy the planes with
	 * destroy_planes() and then calls weston_compositor_shutdown(), which
	 * will lead to calls to drm_output_destroy(). We are destroying the
	 * plane and its state_cur in drm_plane_destroy(), and that removes
	 * state_cur from the output->state_cur list, but not from the
	 * state_last list.
	 *
	 * This was not an issue because we'd leak the drm_output (and so
	 * output->last_state) during shutdown with a pending page flip. And if
	 * we were no shutting down, we'd always wait until flip completion
	 * before destroying an output (meaning that
	 * drm_output_update_complete() would be called and output->state_last
	 * would be freed and set to NULL).
	 *
	 * But now we don't leak the drm_output during shutdown with a pending
	 * flip anymore. So we must destroy output->state_last for every output
	 * before destroying the planes with destroy_planes(), otherwise we'd
	 * leave output->state_last referring to a freed plane, leading to
	 * issues when trying to free it at a later point.
	 */
	wl_list_for_each(output_base, &ec->output_list, link) {
		output = to_drm_output(output_base);
		if (output && (output->page_flip_pending || output->atomic_complete_pending)) {
			drm_output_state_free(output->state_last);
			output->state_last = NULL;
		}
	}

	weston_log_scope_destroy(b->debug);
	b->debug = NULL;
}

static void
drm_kms_device_destroy(struct drm_kms_device *kms_device)
{
	if (!kms_device)
		return;

	if (kms_device->fd >= 0)
		weston_launcher_close(kms_device->fd_owner, kms_device->fd);
	udev_device_unref(kms_device->udev_device);
	free(kms_device->filename);
	free(kms_device);
}

static void
drm_device_destroy(struct drm_device *device)
{
	struct weston_compositor *ec = device->backend->compositor;
	struct drm_crtc *crtc, *crtc_tmp;
	struct drm_writeback *writeback, *writeback_tmp;

	wl_list_remove(&device->link);

	destroy_planes(device);

	wl_list_for_each_safe(crtc, crtc_tmp, &device->crtc_list, link)
		drm_crtc_destroy(crtc);

	wl_list_for_each_safe(writeback, writeback_tmp,
			      &device->writeback_connector_list, link)
		drm_writeback_destroy(writeback);

	weston_assert_list_empty(ec, &device->drm_colorop_3x1d_lut_blob_list);
	weston_assert_list_empty(ec, &device->drm_colorop_clut_blob_list);
	weston_assert_list_empty(ec, &device->drm_colorop_matrix_blob_list);

	if (device->drm_event_source)
		wl_event_source_remove(device->drm_event_source);

	drm_kms_device_destroy(device->kms_device);
	hash_table_destroy(device->gem_handle_refcnt);
	free(device);
}

static void
drm_backend_destroy_all_drm_devices(struct drm_backend *b)
{
	struct drm_device *device, *next;

	wl_list_for_each_safe(device, next, &b->kms_list, link)
		drm_device_destroy(device);
}

void
drm_destroy(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct weston_compositor *ec = b->compositor;
	struct weston_head *base, *next;

	wl_list_remove(&b->base.link);

	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link) {
		if (to_drm_head(base))
			drm_head_destroy(base);
	}

#ifdef BUILD_DRM_GBM
	if (b->gbm)
		gbm_device_destroy(b->gbm);
#endif

	drm_backend_destroy_all_drm_devices(b);

	udev_monitor_unref(b->udev_monitor);
	udev_unref(b->udev);

	weston_launcher_destroy(ec->launcher);

	free(b);
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct weston_compositor *compositor = data;
	struct drm_backend *b =
		container_of(listener, struct drm_backend, session_listener);
	struct drm_device *device = b->drm;
	struct weston_output *output;

	if (compositor->session_active) {
		weston_log("activating session\n");
		weston_compositor_wake(compositor);
		drm_device_recovery_required(device);
		udev_input_enable(&b->input);
	} else {
		weston_log("deactivating session\n");
		udev_input_disable(&b->input);

		weston_compositor_offscreen(compositor);

		/* If we have a repaint scheduled (either from a
		 * pending pageflip or the idle handler), make sure we
		 * cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attempts at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output, &compositor->output_list, link)
			if (to_drm_output(output))
				output->repaint_needed = false;
	}
}


/**
 * Handle KMS GPU being added/removed
 *
 * If the device being added/removed is the KMS device, we activate/deactivate
 * the compositor session.
 *
 * @param backend The DRM backend instance.
 * @param devnum The device being added/removed.
 * @param added Whether the device is being added (or removed)
 */
static void
drm_device_changed(struct weston_backend *backend,
		dev_t devnum, bool added)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct weston_compositor *compositor = b->compositor;
	struct drm_device *device = b->drm;

	if (!device->kms_device || device->kms_device->devnum != devnum ||
	    compositor->session_active == added)
		return;

	compositor->session_active = added;
	wl_signal_emit(&compositor->session_signal, compositor);
}

/**
 * Determines whether or not a device is capable of modesetting.
 */
static struct drm_kms_device *
drm_kms_device_create(struct weston_launcher *launcher,
		      struct udev_device *udev_device)
{
	const char *filename = udev_device_get_devnode(udev_device);
	const char *sysnum = udev_device_get_sysnum(udev_device);
	dev_t devnum = udev_device_get_devnum(udev_device);
	struct drm_kms_device *kms_device;
	drmModeRes *res;
	int id = -1, fd;

	if (!filename)
		return NULL;

	fd = weston_launcher_open(launcher, filename, O_RDWR);
	if (fd < 0)
		return NULL;

	res = drmModeGetResources(fd);
	if (!res)
		goto out_fd;

	if (res->count_crtcs <= 0 || res->count_connectors <= 0 ||
	    res->count_encoders <= 0)
		goto out_res;

	if (sysnum)
		id = atoi(sysnum);
	if (!sysnum || id < 0) {
		weston_log("couldn't get sysnum for device %s\n", filename);
		goto out_res;
	}

	kms_device = xzalloc(sizeof *kms_device);

	kms_device->fd_owner = launcher;
	kms_device->fd = fd;
	kms_device->id = id;
	kms_device->filename = strdup(filename);
	kms_device->devnum = devnum;
	kms_device->udev_device = udev_device_ref(udev_device);

	drmModeFreeResources(res);

	return kms_device;

out_res:
	drmModeFreeResources(res);
out_fd:
	weston_launcher_close(launcher, fd);
	return NULL;
}

/*
 * Find primary GPU
 * Some systems may have multiple DRM devices attached to a single seat. This
 * function loops over all devices and tries to find a PCI device with the
 * boot_vga sysfs attribute set to 1.
 * If no such device is found, the first DRM device reported by udev is used.
 * Devices are also vetted to make sure they are are capable of modesetting,
 * rather than pure render nodes (GPU with no display), or pure
 * memory-allocation devices (VGEM).
 */
static struct drm_kms_device *
find_primary_gpu(struct weston_launcher *launcher,
		 struct udev *udev,
		 const char *seat)
{
	struct drm_kms_device *chosen_kms_device = NULL;
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	const char *path, *device_seat, *id;
	struct udev_device *dev, *pci;

	e = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(e, "drm");
	udev_enumerate_add_match_sysname(e, "card[0-9]*");

	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		struct drm_kms_device *kms_device;
		bool is_boot_vga = false;

		path = udev_list_entry_get_name(entry);
		dev = udev_device_new_from_syspath(udev, path);
		if (!dev)
			continue;
		device_seat = udev_device_get_property_value(dev, "ID_SEAT");
		if (!device_seat)
			device_seat = default_seat;
		if (strcmp(device_seat, seat)) {
			udev_device_unref(dev);
			continue;
		}

		pci = udev_device_get_parent_with_subsystem_devtype(dev,
								"pci", NULL);
		if (pci) {
			id = udev_device_get_sysattr_value(pci, "boot_vga");
			if (id && !strcmp(id, "1"))
				is_boot_vga = true;
		}

		/* If we already have a modesetting-capable device, and this
		 * device isn't our boot-VGA device, we aren't going to use
		 * it. */
		if (!is_boot_vga && chosen_kms_device) {
			udev_device_unref(dev);
			continue;
		}

		/* Make sure this device is actually capable of modesetting */
		kms_device = drm_kms_device_create(launcher, dev);
		udev_device_unref(dev);

		if (!kms_device)
			continue;

		/* There can only be one boot_vga device, and we try to use it
		 * at all costs. */
		if (is_boot_vga) {
			drm_kms_device_destroy(chosen_kms_device);
			chosen_kms_device = kms_device;
			break;
		}

		/* Per the (!is_boot_vga && chosen_kms_device) test above, we only
		 * trump existing saved devices with boot-VGA devices, so if
		 * we end up here, this must be the first device we've seen. */
		assert(!chosen_kms_device);
		chosen_kms_device = kms_device;
	}

	udev_enumerate_unref(e);
	return chosen_kms_device;
}

static struct drm_kms_device *
open_specific_drm_device(struct weston_launcher *launcher,
			 struct udev *udev,
			 const char *name)
{
	struct udev_device *udev_device;
	struct drm_kms_device *kms_device;

	udev_device = udev_device_new_from_subsystem_sysname(udev, "drm", name);
	if (!udev_device) {
		weston_log("ERROR: could not open DRM device '%s'\n", name);
		return NULL;
	}

	kms_device = drm_kms_device_create(launcher, udev_device);
	udev_device_unref(udev_device);

	if (!kms_device) {
		weston_log("ERROR: DRM device '%s' is not a KMS device.\n", name);
		return NULL;
	}

	return kms_device;
}

static void
planes_binding(struct weston_keyboard *keyboard, const struct timespec *time,
	       uint32_t key, void *data)
{
	struct drm_backend *b = data;
	struct drm_device *device = b->drm;

	switch (key) {
	case KEY_C:
		device->cursors_are_broken ^= true;
		break;
	case KEY_V:
		/* We don't support hardware planes usage with legacy KMS. */
		if (device->atomic_modeset)
			device->disable_client_buffer_scanout ^= true;
		break;
	default:
		break;
	}
}

/** Create a live DRM KMS device initialized for use
 *
 * \param backend The backend.
 * \param kms_device The opened DRM KMS device to initialize.
 * \return The new initialized DRM KMS device, or NULL on failure.
 *
 * On success, the ownership of kms_device is taken. On failure, it is not,
 * and the caller must take care of disposing it.
 */
static struct drm_device *
drm_device_create(struct drm_backend *backend,
		  struct drm_kms_device *kms_device)
{
	struct weston_compositor *compositor = backend->compositor;
	struct drm_device *device;
	struct wl_event_loop *loop;
	drmModeRes *res;

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;
	device->recovery_status = DRM_RECOVERY_SCHEDULED;
	device->kms_device = kms_device;
	device->backend = backend;
	device->gem_handle_refcnt = hash_table_create();

	if (init_kms_caps(device) < 0) {
		weston_log("failed to initialize kms\n");
		goto err;
	}

	res = drmModeGetResources(device->kms_device->fd);
	if (!res) {
		weston_log("Failed to get drmModeRes\n");
		goto err;
	}

	loop = wl_display_get_event_loop(compositor->wl_display);
	device->drm_event_source =
		wl_event_loop_add_fd(loop, device->kms_device->fd,
				     WL_EVENT_READABLE, on_drm_input, device);

	wl_list_init(&device->drm_colorop_3x1d_lut_blob_list);
	wl_list_init(&device->drm_colorop_clut_blob_list);
	wl_list_init(&device->drm_colorop_matrix_blob_list);

	wl_list_init(&device->writeback_connector_list);
	if (drm_backend_discover_connectors(device, device->kms_device->udev_device, res) < 0) {
		weston_log("Failed to create heads for %s\n", device->kms_device->filename);
		goto err_res;
	}

	wl_list_init(&device->crtc_list);
	if (drm_backend_create_crtc_list(device, res) == -1) {
		weston_log("Failed to create CRTC list for DRM-backend\n");
		goto err_res;
	}

	wl_list_init(&device->plane_list);
	create_planes(device, res);

	/* 'compute' faked zpos values in case HW doesn't expose any */
	drm_backend_create_faked_zpos(device);

	wl_list_insert(backend->kms_list.prev, &device->link);

	drmModeFreeResources(res);

	return device;

err_res:
	drmModeFreeResources(res);
err:
	return NULL;
}

static void
open_additional_devices(struct drm_backend *backend, const char *cards)
{
	char *tokenize = strdup(cards);
	char *card = strtok(tokenize, ",");

	while (card) {
		struct drm_kms_device *kms_device;
		struct drm_device *device = NULL;

		kms_device = open_specific_drm_device(backend->compositor->launcher,
						      backend->udev, card);
		if (kms_device)
			device = drm_device_create(backend, kms_device);
		if (!device) {
			weston_log("unable to use card %s\n", card);
			drm_kms_device_destroy(kms_device);
			goto next;
		}

		weston_log("adding secondary device %s\n",
			   device->kms_device->filename);

next:
		card = strtok(NULL, ",");
	}

	free(tokenize);
}

static const struct weston_drm_output_api api = {
	drm_output_set_mode,
	drm_output_set_gbm_format,
	drm_output_set_seat,
	drm_output_set_max_bpc,
	drm_output_set_content_type,
};

static struct drm_backend *
drm_backend_create(struct weston_compositor *compositor,
		   struct weston_drm_backend_config *config)
{
	struct drm_backend *b;
	struct drm_kms_device *main_kms_device;
	struct drm_device *device;
	struct wl_event_loop *loop;
	const char *seat_id = default_seat;
	const char *session_seat;
	struct weston_drm_format_array *scanout_formats;
	const char *buf;
	int ret;

	session_seat = getenv("XDG_SEAT");
	if (session_seat)
		seat_id = session_seat;

	if (config->seat_id)
		seat_id = config->seat_id;

	weston_log("initializing drm backend\n");

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	buf = getenv("WESTON_DRM_VIRTUAL_SIZE");
	if (buf)
		sscanf(buf, "%dx%d", &b->virtual_width, &b->virtual_height);

	buf = getenv("WESTON_DRM_MASTER");
	if (buf && buf[0] == '1')
		b->master = true;

	wl_list_init(&b->kms_list);

	b->compositor = compositor;
	b->pageflip_timeout = config->pageflip_timeout;
	b->use_pixman_shadow = config->use_pixman_shadow;
	b->offload_blend_to_output = config->offload_blend_to_output;
	b->disable_drm_state_reuse = config->disable_drm_state_reuse;

	b->debug = weston_compositor_add_log_scope(compositor, "drm-backend",
						   "Debug messages from DRM/KMS backend\n",
						   drm_backend_pageflip_counter_timer_arm_cb,
						   drm_backend_pageflip_counter_timer_disable_cb,
						   b);

	wl_list_insert(&compositor->backend_list, &b->base.link);

	if (parse_gbm_format(config->gbm_format,
			     pixel_format_get_info(DRM_FORMAT_XRGB8888),
			     &b->format) < 0)
		goto err_compositor;

	/* Check if we run drm-backend using a compatible launcher */
	compositor->launcher = weston_launcher_connect(compositor, seat_id, true);
	if (compositor->launcher == NULL) {
		weston_log("fatal: your system should either provide the "
			   "logind D-Bus API, or use seatd.\n");
		goto err_compositor;
	}

	b->udev = udev_new();
	if (b->udev == NULL) {
		weston_log("failed to initialize udev context\n");
		goto err_launcher;
	}

	b->session_listener.notify = session_notify;
	wl_signal_add(&compositor->session_signal, &b->session_listener);

	if (config->specific_device) {
		main_kms_device = open_specific_drm_device(compositor->launcher,
							      b->udev,
							      config->specific_device);
	} else {
		main_kms_device = find_primary_gpu(compositor->launcher,
						      b->udev, seat_id);
	}
	if (!main_kms_device) {
		weston_log("no drm device found\n");
		goto err_udev;
	}

	device = drm_device_create(b, main_kms_device);
	if (device) {
		main_kms_device = NULL;
	} else {
		weston_log("Could not initialize DRM device '%s'\n",
			   main_kms_device->filename);
		drm_kms_device_destroy(main_kms_device);
		goto err_udev;
	}
	b->drm = device;

	if (config->additional_devices)
		open_additional_devices(b, config->additional_devices);

	/* GL renderer is the default whenever it is enabled.
	 * Only on a build without GL but with Vulkan, Vulkan is picked
	 * as the default. Otherwise, pick pixman as the default */
	if (config->renderer == WESTON_RENDERER_AUTO) {
#if defined(ENABLE_EGL)
		config->renderer = WESTON_RENDERER_GL;
#elif defined(ENABLE_VULKAN)
		config->renderer = WESTON_RENDERER_VULKAN;
#else
		config->renderer = WESTON_RENDERER_PIXMAN;
#endif
	}

	switch (config->renderer) {
	case WESTON_RENDERER_PIXMAN:
		if (init_pixman(b) < 0) {
			weston_log("failed to initialize pixman renderer\n");
			goto err_drm_device;
		}
		break;
	case WESTON_RENDERER_GL:
		if (init_egl(b) < 0) {
			weston_log("failed to initialize egl\n");
			goto err_drm_device;
		}
		break;
	case WESTON_RENDERER_VULKAN:
		if (init_vulkan(b) < 0) {
			weston_log("failed to initialize vulkan\n");
			goto err_drm_device;
		}
		break;
	default:
		weston_log("unsupported renderer for DRM backend\n");
		goto err_drm_device;
	}

	b->base.shutdown = drm_shutdown;
	b->base.destroy = drm_destroy;
	b->base.repaint_begin = drm_repaint_begin;
	b->base.repaint_flush = drm_repaint_flush;
	b->base.repaint_cancel = drm_repaint_cancel;
	b->base.create_output = drm_output_create;
	b->base.device_changed = drm_device_changed;
	b->base.can_scanout_dmabuf = drm_can_scanout_dmabuf;

	weston_setup_vt_switch_bindings(compositor);

	if (udev_input_init(&b->input,
			    compositor, b->udev, seat_id,
			    config->configure_device) < 0) {
		weston_log("failed to create input devices\n");
		goto err_drm_device;
	}

	device->cursors_are_broken |= config->use_sw_cursor;

	/* A this point we have some idea of whether or not we have a working
	 * cursor plane. */
	if (!device->cursors_are_broken)
		compositor->capabilities |= WESTON_CAP_CURSOR_PLANE;

	if (compositor->cursor_size) {
		device->cursor_width = compositor->cursor_size;
		device->cursor_height = compositor->cursor_size;
	}

	b->udev_monitor = udev_monitor_new_from_netlink(b->udev, "udev");
	if (b->udev_monitor == NULL) {
		weston_log("failed to initialize udev monitor\n");
		goto err_udev_input;
	}
	udev_monitor_filter_add_match_subsystem_devtype(b->udev_monitor,
							"drm", NULL);
	loop = wl_display_get_event_loop(compositor->wl_display);
	b->udev_drm_source =
		wl_event_loop_add_fd(loop,
				     udev_monitor_get_fd(b->udev_monitor),
				     WL_EVENT_READABLE, udev_drm_event, b);

	if (udev_monitor_enable_receiving(b->udev_monitor) < 0) {
		weston_log("failed to enable udev-monitor receiving\n");
		goto err_udev_monitor;
	}

	weston_compositor_add_debug_binding(compositor, KEY_O,
					    planes_binding, b);
	weston_compositor_add_debug_binding(compositor, KEY_C,
					    planes_binding, b);
	weston_compositor_add_debug_binding(compositor, KEY_V,
					    planes_binding, b);

	if (compositor->renderer->import_dmabuf) {
		if (compositor->default_dmabuf_feedback) {
			/* We were able to create the compositor's default
			 * dma-buf feedback in the renderer, that means that the
			 * table was already created and populated with
			 * renderer's format/modifier pairs. So now we must
			 * compute the scanout formats indices in the table */
			scanout_formats = get_scanout_formats(b->drm);
			if (!scanout_formats)
				goto err_udev_monitor;
			ret = weston_dmabuf_feedback_format_table_set_scanout_indices(compositor->dmabuf_feedback_format_table,
										      scanout_formats);
			weston_drm_format_array_fini(scanout_formats);
			free(scanout_formats);
			if (ret < 0)
				goto err_udev_monitor;
		}
		if (weston_direct_display_setup(compositor) < 0)
			weston_log("Error: initializing direct-display "
				   "support failed.\n");
	}

	if (compositor->capabilities & WESTON_CAP_EXPLICIT_SYNC) {
		if (linux_explicit_synchronization_setup(compositor) < 0)
			weston_log("Error: initializing explicit "
				   " synchronization support failed.\n");
	}

	if (device->atomic_modeset)
		if (weston_compositor_enable_content_protection(compositor) < 0)
			weston_log("Error: initializing content-protection "
				   "support failed.\n");

	ret = weston_plugin_api_register(compositor, WESTON_DRM_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		weston_log("Failed to register output API.\n");
		goto err_udev_monitor;
	}

	ret = drm_backend_init_virtual_output_api(compositor);
	if (ret < 0) {
		weston_log("Failed to register virtual output API.\n");
		goto err_udev_monitor;
	}

	drm_backend_pageflip_counter_timer_create(b, DEFAULT_FRAME_RATE_INTERVAL);

	if (weston_log_scope_is_enabled(b->debug))
		drm_backend_pageflip_counter_timer_arm(b);

	b->hotplug_update_timer =
		wl_event_loop_add_timer(loop, hotplug_update_handler, b);

	return b;

err_udev_monitor:
	wl_event_source_remove(b->udev_drm_source);
	udev_monitor_unref(b->udev_monitor);
err_udev_input:
	udev_input_destroy(&b->input);
err_drm_device:
	drm_backend_destroy_all_drm_devices(b);
err_udev:
	udev_unref(b->udev);
err_launcher:
	weston_launcher_destroy(compositor->launcher);
err_compositor:
	weston_log_scope_destroy(b->debug);
	wl_list_remove(&b->base.link);
#ifdef BUILD_DRM_GBM
	if (b->gbm)
		gbm_device_destroy(b->gbm);
#endif
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_drm_backend_config *config)
{
	config->renderer = WESTON_RENDERER_AUTO;
	config->use_pixman_shadow = true;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct drm_backend *b;
	struct weston_drm_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_DRM_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_drm_backend_config)) {
		weston_log("drm backend config structure is invalid\n");
		return -1;
	}

	if (compositor->renderer) {
		weston_log("drm backend must be the primary backend\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = drm_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	return 0;
}
