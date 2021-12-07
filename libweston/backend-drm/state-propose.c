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

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libweston/libweston.h>
#include <libweston/backend-drm.h>
#include <libweston/pixel-formats.h>

#include "drm-internal.h"

#include "color.h"
#include "linux-dmabuf.h"
#include "presentation-time-server-protocol.h"

enum drm_output_propose_state_mode {
	DRM_OUTPUT_PROPOSE_STATE_MIXED, /**< mix renderer & planes */
	DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY, /**< only assign to renderer & cursor */
	DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY, /**< no renderer use, only planes */
};

static const char *const drm_output_propose_state_mode_as_string[] = {
	[DRM_OUTPUT_PROPOSE_STATE_MIXED] = "mixed state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY] = "render-only state",
	[DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY]	= "plane-only state"
};

static const char *
drm_propose_state_mode_to_string(enum drm_output_propose_state_mode mode)
{
	if (mode < 0 || mode >= ARRAY_LENGTH(drm_output_propose_state_mode_as_string))
		return " unknown compositing mode";

	return drm_output_propose_state_mode_as_string[mode];
}

static bool
drm_output_check_plane_has_view_assigned(struct drm_plane *plane,
                                         struct drm_output_state *output_state)
{
	struct drm_plane_state *ps;
	wl_list_for_each(ps, &output_state->plane_list, link) {
		if (ps->plane == plane && ps->fb)
			return true;
	}
	return false;
}

static struct drm_plane_state *
drm_output_prepare_overlay_view(struct drm_plane *plane,
				struct drm_output_state *output_state,
				struct weston_view *ev,
				enum drm_output_propose_state_mode mode,
				struct drm_fb *fb, uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct weston_compositor *ec = output->base.compositor;
	struct drm_backend *b = to_drm_backend(ec);
	struct drm_plane_state *state = NULL;
	int ret;

	assert(!b->sprites_are_broken);
	assert(b->atomic_modeset);

	if (!fb) {
		drm_debug(b, "\t\t\t\t[overlay] not placing view %p on overlay: "
			     " couldn't get fb\n", ev);
		return NULL;
	}

	state = drm_output_state_get_plane(output_state, plane);
	/* we can't have a 'pending' framebuffer as never set one before reaching here */
	assert(!state->fb);

	state->ev = ev;
	state->output = output;

	if (!drm_plane_state_coords_for_view(state, ev, zpos)) {
		drm_debug(b, "\t\t\t\t[overlay] not placing view %p on overlay: "
			     "unsuitable transform\n", ev);
		drm_plane_state_put_back(state);
		state = NULL;
		goto out;
	}

	/* If the surface buffer has an in-fence fd, but the plane
	 * doesn't support fences, we can't place the buffer on this
	 * plane. */
	if (ev->surface->acquire_fence_fd >= 0 &&
	     plane->props[WDRM_PLANE_IN_FENCE_FD].prop_id == 0) {
		drm_debug(b, "\t\t\t\t[overlay] not placing view %p on overlay: "
			     "no in-fence support\n", ev);
		drm_plane_state_put_back(state);
		state = NULL;
		goto out;
	}

	/* We hold one reference for the lifetime of this function; from
	 * calling drm_fb_get_from_view() in drm_output_prepare_plane_view(),
	 * so, we take another reference here to live within the state. */
	state->fb = drm_fb_ref(fb);

	state->in_fence_fd = ev->surface->acquire_fence_fd;

	/* In planes-only mode, we don't have an incremental state to
	 * test against, so we just hope it'll work. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY) {
		drm_debug(b, "\t\t\t[overlay] provisionally placing "
			     "view %p on overlay %lu in planes-only mode\n",
			  ev, (unsigned long) plane->plane_id);
		goto out;
	}

	ret = drm_pending_state_test(output_state->pending_state);
	if (ret == 0) {
		drm_debug(b, "\t\t\t[overlay] provisionally placing "
			     "view %p on overlay %d in mixed mode\n",
			  ev, plane->plane_id);
		goto out;
	}

	drm_debug(b, "\t\t\t[overlay] not placing view %p on overlay %lu "
		     "in mixed mode: kernel test failed\n",
		  ev, (unsigned long) plane->plane_id);

	drm_plane_state_put_back(state);
	state = NULL;

out:
	return state;
}

#ifdef BUILD_DRM_GBM
/**
 * Update the image for the current cursor surface
 *
 * @param plane_state DRM cursor plane state
 * @param ev Source view for cursor
 */
static void
cursor_bo_update(struct drm_plane_state *plane_state, struct weston_view *ev)
{
	struct drm_backend *b = plane_state->plane->backend;
	struct gbm_bo *bo = plane_state->fb->bo;
	struct weston_buffer *buffer = ev->surface->buffer_ref.buffer;
	uint32_t buf[b->cursor_width * b->cursor_height];
	int32_t stride;
	uint8_t *s;
	int i;

	assert(buffer && buffer->shm_buffer);
	assert(buffer->shm_buffer == wl_shm_buffer_get(buffer->resource));
	assert(buffer->width <= b->cursor_width);
	assert(buffer->height <= b->cursor_height);

	memset(buf, 0, sizeof buf);
	stride = wl_shm_buffer_get_stride(buffer->shm_buffer);
	s = wl_shm_buffer_get_data(buffer->shm_buffer);

	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (i = 0; i < buffer->height; i++)
		memcpy(buf + i * b->cursor_width,
		       s + i * stride,
		       buffer->width * 4);
	wl_shm_buffer_end_access(buffer->shm_buffer);

	if (gbm_bo_write(bo, buf, sizeof buf) < 0)
		weston_log("failed update cursor: %s\n", strerror(errno));
}

static struct drm_plane_state *
drm_output_prepare_cursor_view(struct drm_output_state *output_state,
			       struct weston_view *ev, uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct drm_plane *plane = output->cursor_plane;
	struct drm_plane_state *plane_state;
	bool needs_update = false;
	const char *p_name = drm_output_get_plane_type_name(plane);

	assert(!b->cursors_are_broken);

	if (!plane)
		return NULL;

	if (!plane->state_cur->complete)
		return NULL;

	if (plane->state_cur->output && plane->state_cur->output != output)
		return NULL;

	/* We use GBM to import SHM buffers. */
	if (b->gbm == NULL)
		return NULL;

	plane_state = drm_output_state_get_plane(output_state, plane);

	if (plane_state && plane_state->fb)
		return NULL;

	/* We can't scale with the legacy API, and we don't try to account for
	 * simple cropping/translation in cursor_bo_update. */
	plane_state->output = output;
	if (!drm_plane_state_coords_for_view(plane_state, ev, zpos)) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     "unsuitable transform\n", p_name, ev, p_name);
		goto err;
	}

	if (plane_state->src_x != 0 || plane_state->src_y != 0 ||
	    plane_state->src_w > (unsigned) b->cursor_width << 16 ||
	    plane_state->src_h > (unsigned) b->cursor_height << 16 ||
	    plane_state->src_w != plane_state->dest_w << 16 ||
	    plane_state->src_h != plane_state->dest_h << 16) {
		drm_debug(b, "\t\t\t\t[%s] not assigning view %p to %s plane "
			     "(positioning requires cropping or scaling)\n",
			     p_name, ev, p_name);
		goto err;
	}

	/* Since we're setting plane state up front, we need to work out
	 * whether or not we need to upload a new cursor. We can't use the
	 * plane damage, since the planes haven't actually been calculated
	 * yet: instead try to figure it out directly. KMS cursor planes are
	 * pretty unique here, in that they lie partway between a Weston plane
	 * (direct scanout) and a renderer. */
	if (ev != output->cursor_view ||
	    pixman_region32_not_empty(&ev->surface->damage)) {
		output->current_cursor++;
		output->current_cursor =
			output->current_cursor %
				ARRAY_LENGTH(output->gbm_cursor_fb);
		needs_update = true;
	}

	drm_output_set_cursor_view(output, ev);
	plane_state->ev = ev;

	plane_state->fb =
		drm_fb_ref(output->gbm_cursor_fb[output->current_cursor]);

	if (needs_update) {
		drm_debug(b, "\t\t\t\t[%s] copying new content to cursor BO\n", p_name);
		cursor_bo_update(plane_state, ev);
	}

	/* The cursor API is somewhat special: in cursor_bo_update(), we upload
	 * a buffer which is always cursor_width x cursor_height, even if the
	 * surface we want to promote is actually smaller than this. Manually
	 * mangle the plane state to deal with this. */
	plane_state->src_w = b->cursor_width << 16;
	plane_state->src_h = b->cursor_height << 16;
	plane_state->dest_w = b->cursor_width;
	plane_state->dest_h = b->cursor_height;

	drm_debug(b, "\t\t\t\t[%s] provisionally assigned view %p to cursor\n",
		  p_name, ev);

	return plane_state;

err:
	drm_plane_state_put_back(plane_state);
	return NULL;
}
#else
static struct drm_plane_state *
drm_output_prepare_cursor_view(struct drm_output_state *output_state,
			       struct weston_view *ev, uint64_t zpos)
{
	return NULL;
}
#endif

static struct drm_plane_state *
drm_output_prepare_scanout_view(struct drm_output_state *output_state,
				struct weston_view *ev,
				enum drm_output_propose_state_mode mode,
				struct drm_fb *fb, uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct drm_plane *scanout_plane = output->scanout_plane;
	struct drm_plane_state *state;
	const char *p_name = drm_output_get_plane_type_name(scanout_plane);

	assert(!b->sprites_are_broken);
	assert(b->atomic_modeset);
	assert(mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY);

	/* Check the view spans exactly the output size, calculated in the
	 * logical co-ordinate space. */
	if (!weston_view_matches_output_entirely(ev, &output->base)) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     " view does not match output entirely\n",
			     p_name, ev, p_name);
		return NULL;
	}

	/* If the surface buffer has an in-fence fd, but the plane doesn't
	 * support fences, we can't place the buffer on this plane. */
	if (ev->surface->acquire_fence_fd >= 0 &&
	    scanout_plane->props[WDRM_PLANE_IN_FENCE_FD].prop_id == 0) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     "no in-fence support\n", p_name, ev, p_name);
		return NULL;
	}

	if (!fb) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     " couldn't get fb\n", p_name, ev, p_name);
		return NULL;
	}

	state = drm_output_state_get_plane(output_state, scanout_plane);

	/* The only way we can already have a buffer in the scanout plane is
	 * if we are in mixed mode, or if a client buffer has already been
	 * placed into scanout. The former case will never call into here,
	 * and in the latter case, the view must have been marked as occluded,
	 * meaning we should never have ended up here. */
	assert(!state->fb);

	/* take another reference here to live within the state */
	state->fb = drm_fb_ref(fb);
	state->ev = ev;
	state->output = output;
	if (!drm_plane_state_coords_for_view(state, ev, zpos)) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     "unsuitable transform\n", p_name, ev, p_name);
		goto err;
	}

	if (state->dest_x != 0 || state->dest_y != 0 ||
	    state->dest_w != (unsigned) output->base.current_mode->width ||
	    state->dest_h != (unsigned) output->base.current_mode->height) {
		drm_debug(b, "\t\t\t\t[%s] not placing view %p on %s: "
			     " invalid plane state\n", p_name, ev, p_name);
		goto err;
	}

	state->in_fence_fd = ev->surface->acquire_fence_fd;

	/* In plane-only mode, we don't need to test the state now, as we
	 * will only test it once at the end. */
	return state;

err:
	drm_plane_state_put_back(state);
	return NULL;
}

static struct drm_plane_state *
drm_output_try_view_on_plane(struct drm_plane *plane,
			     struct drm_output_state *state,
			     struct weston_view *ev,
			     enum drm_output_propose_state_mode mode,
			     struct drm_fb *fb, uint64_t zpos)
{
	struct drm_backend *b = state->pending_state->backend;
	struct weston_output *wet_output = &state->output->base;
	bool view_matches_entire_output, scanout_has_view_assigned;
	struct drm_plane *scanout_plane = state->output->scanout_plane;
	struct drm_plane_state *ps = NULL;
	const char *p_name = drm_output_get_plane_type_name(plane);
	struct weston_surface *surface = ev->surface;
	enum {
		NO_PLANES,	/* generic err-handle */
		NO_PLANES_ACCEPTED,
		PLACED_ON_PLANE,
	} availability = NO_PLANES;

	/* sanity checks in case we over/underflow zpos or pass incorrect
	 * values */
	assert(zpos <= plane->zpos_max ||
	       zpos != DRM_PLANE_ZPOS_INVALID_PLANE);

	switch (plane->type) {
	case WDRM_PLANE_TYPE_CURSOR:
		ps = drm_output_prepare_cursor_view(state, ev, zpos);
		if (ps)
			availability = PLACED_ON_PLANE;
		break;
	case WDRM_PLANE_TYPE_OVERLAY:
		/* do not attempt to place it in the overlay if we don't have
		 * anything in the scanout/primary and the view doesn't cover
		 * the entire output  */
		view_matches_entire_output =
			weston_view_matches_output_entirely(ev, wet_output);
		scanout_has_view_assigned =
			drm_output_check_plane_has_view_assigned(scanout_plane,
								 state);

		if (view_matches_entire_output && !scanout_has_view_assigned) {
			availability = NO_PLANES_ACCEPTED;
			goto out;
		}

		ps = drm_output_prepare_overlay_view(plane, state, ev, mode,
						     fb, zpos);
		if (ps)
			availability = PLACED_ON_PLANE;
		break;
	case WDRM_PLANE_TYPE_PRIMARY:
		ps = drm_output_prepare_scanout_view(state, ev, mode,
						     fb, zpos);
		if (ps)
			availability = PLACED_ON_PLANE;
		break;
	default:
		assert(0);
		break;
	}

out:
	switch (availability) {
	case NO_PLANES:
		/* set initial to this catch-all case, such that
		 * prepare_cursor/overlay/scanout() should have/contain the
		 * reason for failling */
		break;
	case NO_PLANES_ACCEPTED:
		drm_debug(b, "\t\t\t\t[plane] plane %d refusing to "
			     "place view %p in %s\n",
			     plane->plane_id, ev, p_name);
		break;
	case PLACED_ON_PLANE:
		/* Take a reference on the buffer so that we don't release it
		 * back to the client until we're done with it; cursor buffers
		 * don't require a reference since we copy them. */
		assert(ps->fb_ref.buffer.buffer == NULL);
		assert(ps->fb_ref.release.buffer_release == NULL);
		if (ps->plane->type == WDRM_PLANE_TYPE_CURSOR) {
			assert(ps->fb->type == BUFFER_CURSOR);
		} else if (fb->type == BUFFER_CLIENT || fb->type == BUFFER_DMABUF) {
			assert(ps->fb == fb);
			weston_buffer_reference(&ps->fb_ref.buffer,
						surface->buffer_ref.buffer);
			weston_buffer_release_reference(&ps->fb_ref.release,
							surface->buffer_release_ref.buffer_release);
		}
		break;
	}


	return ps;
}

static void
drm_output_check_zpos_plane_states(struct drm_output_state *state)
{
	struct drm_plane_state *ps;

	wl_list_for_each(ps, &state->plane_list, link) {
		struct wl_list *next_node = ps->link.next;
		bool found_dup = false;

		/* skip any plane that is not enabled */
		if (!ps->fb)
			continue;

		assert(ps->zpos != DRM_PLANE_ZPOS_INVALID_PLANE);

		/* find another plane with the same zpos value */
		if (next_node == &state->plane_list)
			break;

		while (next_node && next_node != &state->plane_list) {
			struct drm_plane_state *ps_next;

			ps_next = container_of(next_node,
					       struct drm_plane_state,
					       link);

			if (ps->zpos == ps_next->zpos) {
				found_dup = true;
				break;
			}
			next_node = next_node->next;
		}

		/* this should never happen so exit hard in case
		 * we screwed up that bad */
		assert(!found_dup);
	}
}

static bool
dmabuf_feedback_maybe_update(struct drm_backend *b, struct weston_view *ev,
			     uint32_t try_view_on_plane_failure_reasons)
{
	struct weston_dmabuf_feedback *dmabuf_feedback = ev->surface->dmabuf_feedback;
	struct weston_dmabuf_feedback_tranche *scanout_tranche;
	dev_t scanout_dev = b->drm.devnum;
	uint32_t scanout_flags = ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
	uint32_t action_needed = ACTION_NEEDED_NONE;
	struct timespec current_time, delta_time;
	const time_t MAX_TIME_SECONDS = 2;

	/* Find out what we need to do with the dma-buf feedback */
	if (try_view_on_plane_failure_reasons & FAILURE_REASONS_FORCE_RENDERER)
		action_needed |= ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE;
	if (try_view_on_plane_failure_reasons &
		(FAILURE_REASONS_ADD_FB_FAILED |
		 FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE |
		 FAILURE_REASONS_DMABUF_MODIFIER_INVALID))
		action_needed |= ACTION_NEEDED_ADD_SCANOUT_TRANCHE;

	assert(action_needed != (ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE |
				 ACTION_NEEDED_ADD_SCANOUT_TRANCHE));

	/* Look for scanout tranche. If not found, add it but in disabled mode
	 * (we still don't know if we'll have to send it to clients). This
	 * simplifies the code. */
	scanout_tranche =
		weston_dmabuf_feedback_find_tranche(dmabuf_feedback, scanout_dev,
						    scanout_flags, SCANOUT_PREF);
	if (!scanout_tranche) {
		scanout_tranche =
			weston_dmabuf_feedback_tranche_create(dmabuf_feedback,
					b->compositor->dmabuf_feedback_format_table,
					scanout_dev, scanout_flags,
					SCANOUT_PREF);
		scanout_tranche->active = false;
	}

	/* No actions needed, so disarm timer and return */
	if (action_needed == ACTION_NEEDED_NONE ||
	    (action_needed == ACTION_NEEDED_ADD_SCANOUT_TRANCHE &&
	     scanout_tranche->active) ||
	    (action_needed == ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE &&
	     !scanout_tranche->active)) {
		dmabuf_feedback->action_needed = ACTION_NEEDED_NONE;
		return false;
	}

	/* We hit this if:
	 *
	 * 1. timer is still off, or
	 * 2. the action needed when it was set to on does not match the most
	 *    recent needed action we've detected.
	 *
	 * So we reset the timestamp, set the timer to on it with the most
	 * recent needed action, return and leave the timer running. */
	if (dmabuf_feedback->action_needed == ACTION_NEEDED_NONE ||
	    dmabuf_feedback->action_needed != action_needed) {
		clock_gettime(CLOCK_MONOTONIC, &dmabuf_feedback->timer);
		dmabuf_feedback->action_needed = action_needed;
		return false;
	/* Timer is already on and the action needed when it was set to on does
	 * not conflict with the most recent needed action we've detected. If
	 * more than MAX_TIME_SECONDS has passed, we need to resend the dma-buf
	 * feedback. Otherwise, return and leave the timer running. */
	} else {
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		delta_time.tv_sec = current_time.tv_sec -
				    dmabuf_feedback->timer.tv_sec;
		if (delta_time.tv_sec < MAX_TIME_SECONDS)
			return false;
	}

	/* If we got here it means that the timer has triggered, so we have
	 * pending actions with the dma-buf feedback. So we update and resend
	 * them. */
	if (action_needed == ACTION_NEEDED_ADD_SCANOUT_TRANCHE)
		scanout_tranche->active = true;
	else if (action_needed == ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE)
		scanout_tranche->active = false;
	else
		assert(0);

	drm_debug(b, "\t[repaint] Need to update and resend the "
		     "dma-buf feedback for surface of view %p\n", ev);
	weston_dmabuf_feedback_send_all(dmabuf_feedback,
					b->compositor->dmabuf_feedback_format_table);

	/* Set the timer to off */
	dmabuf_feedback->action_needed = ACTION_NEEDED_NONE;

	return true;
}

static struct drm_plane_state *
drm_output_prepare_plane_view(struct drm_output_state *state,
			      struct weston_view *ev,
			      enum drm_output_propose_state_mode mode,
			      struct drm_plane_state *scanout_state,
			      uint64_t current_lowest_zpos,
			      uint32_t *try_view_on_plane_failure_reasons)
{
	struct drm_output *output = state->output;
	struct drm_backend *b = to_drm_backend(output->base.compositor);

	struct drm_plane_state *ps = NULL;
	struct drm_plane *plane;

	struct weston_buffer *buffer;
	struct wl_shm_buffer *shmbuf;
	struct drm_fb *fb = NULL;

	uint32_t possible_plane_mask = 0;

	/* check view for valid buffer, doesn't make sense to even try */
	if (!weston_view_has_valid_buffer(ev))
		return ps;

	buffer = ev->surface->buffer_ref.buffer;
	shmbuf = wl_shm_buffer_get(buffer->resource);
	if (shmbuf) {
		if (!output->cursor_plane || b->cursors_are_broken)
			return NULL;

		if (wl_shm_buffer_get_format(shmbuf) != WL_SHM_FORMAT_ARGB8888) {
			drm_debug(b, "\t\t\t\t[view] not placing view %p on "
			             "plane; SHM buffers must be ARGB8888 for "
				     "cursor view", ev);
			return NULL;
		}

		if (buffer->width > b->cursor_width ||
		    buffer->height > b->cursor_height) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
				     "(buffer (%dx%d) too large for cursor plane)",
				     ev, buffer->width, buffer->height);
			return NULL;
		}

		possible_plane_mask = (1 << output->cursor_plane->plane_idx);
	} else {
		if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p "
				     "to plane: renderer-only mode\n", ev);
			return NULL;
		}

		fb = drm_fb_get_from_view(state, ev, try_view_on_plane_failure_reasons);
		if (!fb)
			return NULL;

		possible_plane_mask = fb->plane_mask;
	}

	/* assemble a list with possible candidates */
	wl_list_for_each(plane, &b->plane_list, link) {
		const char *p_name = drm_output_get_plane_type_name(plane);
		uint64_t zpos;

		if (possible_plane_mask == 0)
			break;

		if (!(possible_plane_mask & (1 << plane->plane_idx)))
			continue;

		possible_plane_mask &= ~(1 << plane->plane_idx);

		if (plane->type == WDRM_PLANE_TYPE_CURSOR &&
		    (plane != output->cursor_plane || !shmbuf)) {
			continue;
		}

		if (plane->type == WDRM_PLANE_TYPE_PRIMARY &&
		    (plane != output->scanout_plane ||
		     mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY)) {
			continue;
		}

		if (!drm_plane_is_available(plane, output))
			continue;

		if (drm_output_check_plane_has_view_assigned(plane, state)) {
			drm_debug(b, "\t\t\t\t[plane] not trying plane %d: "
				     "another view already assigned",
				     plane->plane_id);
			continue;
		}

		if (plane->zpos_min >= current_lowest_zpos) {
			drm_debug(b, "\t\t\t\t[plane] not trying plane %d: "
				     "plane's minimum zpos (%"PRIu64") above "
				     "current lowest zpos (%"PRIu64")\n",
				     plane->plane_id, plane->zpos_min,
				     current_lowest_zpos);
			continue;
		}

		if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
			assert(scanout_state != NULL);
			if (scanout_state->zpos >= plane->zpos_max) {
				drm_debug(b, "\t\t\t\t[plane] not adding plane %d to "
					     "candidate list: primary's zpos "
					     "value (%"PRIu64") higher than "
					     "plane's maximum value (%"PRIu64")\n",
					     plane->plane_id, scanout_state->zpos,
					     plane->zpos_max);
				continue;
			}
		}

		if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
			zpos = plane->zpos_max;
		else
			zpos = MIN(current_lowest_zpos - 1, plane->zpos_max);

		drm_debug(b, "\t\t\t\t[plane] plane %d picked "
			     "from candidate list, type: %s\n",
			     plane->plane_id, p_name);

		ps = drm_output_try_view_on_plane(plane, state, ev,
						  mode, fb, zpos);
		if (ps) {
			drm_debug(b, "\t\t\t\t[view] view %p has been placed to "
				     "%s plane with computed zpos %"PRIu64"\n",
				     ev, p_name, zpos);
			break;
		}
	}

	/* if we have a plane state, it has its own ref to the fb; if not then
	 * we drop ours here */
	drm_fb_unref(fb);
	return ps;
}

static struct drm_output_state *
drm_output_propose_state(struct weston_output *output_base,
			 struct drm_pending_state *pending_state,
			 enum drm_output_propose_state_mode mode)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct weston_paint_node *pnode;
	struct drm_output_state *state;
	struct drm_plane_state *scanout_state = NULL;

	pixman_region32_t renderer_region;
	pixman_region32_t occluded_region;

	bool renderer_ok = (mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY);
	int ret;
	uint64_t current_lowest_zpos = DRM_PLANE_ZPOS_INVALID_PLANE;

	assert(!output->state_last);
	state = drm_output_state_duplicate(output->state_cur,
					   pending_state,
					   DRM_OUTPUT_STATE_CLEAR_PLANES);

	/* We implement mixed mode by progressively creating and testing
	 * incremental states, of scanout + overlay + cursor. Since we
	 * walk our views top to bottom, the scanout plane is last, however
	 * we always need it in our scene for the test modeset to be
	 * meaningful. To do this, we steal a reference to the last
	 * renderer framebuffer we have, if we think it's basically
	 * compatible. If we don't have that, then we conservatively fall
	 * back to only using the renderer for this repaint. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		struct drm_plane *plane = output->scanout_plane;
		struct drm_fb *scanout_fb = plane->state_cur->fb;

		if (!scanout_fb ||
		    (scanout_fb->type != BUFFER_GBM_SURFACE &&
		     scanout_fb->type != BUFFER_PIXMAN_DUMB)) {
			drm_debug(b, "\t\t[state] cannot propose mixed mode: "
			             "for output %s (%lu): no previous renderer "
			             "fb\n",
				  output->base.name,
				  (unsigned long) output->base.id);
			drm_output_state_free(state);
			return NULL;
		}

		if (scanout_fb->width != output_base->current_mode->width ||
		    scanout_fb->height != output_base->current_mode->height) {
			drm_debug(b, "\t\t[state] cannot propose mixed mode "
			             "for output %s (%lu): previous fb has "
				     "different size\n",
				  output->base.name,
				  (unsigned long) output->base.id);
			drm_output_state_free(state);
			return NULL;
		}

		scanout_state = drm_plane_state_duplicate(state,
							  plane->state_cur);
		/* assign the primary the lowest zpos value */
		scanout_state->zpos = plane->zpos_min;
		drm_debug(b, "\t\t[state] using renderer FB ID %lu for mixed "
			     "mode for output %s (%lu)\n",
			  (unsigned long) scanout_fb->fb_id, output->base.name,
			  (unsigned long) output->base.id);
		drm_debug(b, "\t\t[state] scanout will use for zpos %"PRIu64"\n",
				scanout_state->zpos);
	}

	/* - renderer_region contains the total region which which will be
	 *   covered by the renderer
	 * - occluded_region contains the total region which which will be
	 *   covered by the renderer and hardware planes, where the view's
	 *   visible-and-opaque region is added in both cases (the view's
	 *   opaque region  accumulates there for each view); it is being used
	 *   to skip the view, if it is completely occluded; includes the
	 *   situation where occluded_region covers entire output's region.
	 */
	pixman_region32_init(&renderer_region);
	pixman_region32_init(&occluded_region);

	wl_list_for_each(pnode, &output->base.paint_node_z_order_list,
			 z_order_link) {
		struct weston_view *ev = pnode->view;
		struct drm_plane_state *ps = NULL;
		bool force_renderer = false;
		pixman_region32_t clipped_view;
		pixman_region32_t surface_overlap;
		bool totally_occluded = false;

		drm_debug(b, "\t\t\t[view] evaluating view %p for "
		             "output %s (%lu)\n",
		          ev, output->base.name,
			  (unsigned long) output->base.id);

		/* If this view doesn't touch our output at all, there's no
		 * reason to do anything with it. */
		/* TODO: turn this into assert once z_order_list is pruned. */
		if (!(ev->output_mask & (1u << output->base.id))) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p "
			             "(not on our output)\n", ev);
			continue;
		}

		/* Cannot show anything without a color transform. */
		if (!pnode->surf_xform_valid) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p "
			             "(color transform failed)\n", ev);
			continue;
		}

		/* Ignore views we know to be totally occluded. */
		pixman_region32_init(&clipped_view);
		pixman_region32_intersect(&clipped_view,
					  &ev->transform.boundingbox,
					  &output->base.region);

		pixman_region32_init(&surface_overlap);
		pixman_region32_subtract(&surface_overlap, &clipped_view,
					 &occluded_region);
		/* if the view is completely occluded then ignore that
		 * view; includes the case where occluded_region covers
		 * the entire output */
		totally_occluded = !pixman_region32_not_empty(&surface_overlap);
		if (totally_occluded) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p "
			             "(occluded on our output)\n", ev);
			pixman_region32_fini(&surface_overlap);
			pixman_region32_fini(&clipped_view);
			continue;
		}

		/* We only assign planes to views which are exclusively present
		 * on our output. */
		if (ev->output_mask != (1u << output->base.id)) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(on multiple outputs)\n", ev);
			force_renderer = true;
		}

		if (!b->gbm) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(GBM not available)\n", ev);
			force_renderer = true;
		}

		if (!weston_view_has_valid_buffer(ev)) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(no buffer available)\n", ev);
			force_renderer = true;
		}

		if (pnode->surf_xform.transform != NULL ||
		    !pnode->surf_xform.identity_pipeline) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(requires color transform)\n", ev);
			force_renderer = true;
		}

		/* Since we process views from top to bottom, we know that if
		 * the view intersects the calculated renderer region, it must
		 * be part of, or occluded by, it, and cannot go on a plane. */
		pixman_region32_intersect(&surface_overlap, &renderer_region,
					  &clipped_view);
		if (pixman_region32_not_empty(&surface_overlap)) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
			             "(occluded by renderer views)\n", ev);
			force_renderer = true;
		}
		pixman_region32_fini(&surface_overlap);

		/* In case of enforced mode of content-protection do not
		 * assign planes for a protected surface on an unsecured output.
		 */
		if (ev->surface->protection_mode == WESTON_SURFACE_PROTECTION_MODE_ENFORCED &&
		    ev->surface->desired_protection > output_base->current_protection) {
			drm_debug(b, "\t\t\t\t[view] not assigning view %p to plane "
				     "(enforced protection mode on unsecured output)\n", ev);
			force_renderer = true;
		}

		/* Now try to place it on a plane if we can. */
		if (!force_renderer) {
			drm_debug(b, "\t\t\t[plane] started with zpos %"PRIu64"\n",
				      current_lowest_zpos);
			ps = drm_output_prepare_plane_view(state, ev, mode,
							   scanout_state,
							   current_lowest_zpos,
							   &pnode->try_view_on_plane_failure_reasons);
			/* If we were able to place the view in a plane, set
			 * failure reasons to none. */
			if (ps)
				pnode->try_view_on_plane_failure_reasons =
					FAILURE_REASONS_NONE;
		} else {
			/* We are forced to place the view in the renderer, set
			 * the failure reason accordingly. */
			pnode->try_view_on_plane_failure_reasons =
				FAILURE_REASONS_FORCE_RENDERER;
		}

		if (ps) {
			current_lowest_zpos = ps->zpos;
			drm_debug(b, "\t\t\t[plane] next zpos to use %"PRIu64"\n",
				      current_lowest_zpos);
		} else if (!ps && !renderer_ok) {
			drm_debug(b, "\t\t[view] failing state generation: "
				      "placing view %p to renderer not allowed\n",
				  ev);
			pixman_region32_fini(&clipped_view);
			goto err_region;
		} else if (!ps) {
			/* clipped_view contains the area that's going to be
			 * visible on screen; add this to the renderer region */
			pixman_region32_union(&renderer_region,
					      &renderer_region,
					      &clipped_view);

			drm_debug(b, "\t\t\t\t[view] view %p will be placed "
				     "on the renderer\n", ev);
		}

		/* Opaque areas of our clipped view occlude areas behind it;
		 * however, anything not in the opaque region (which is the
		 * entire clipped area if the whole view is known to be
		 * opaque) does not necessarily occlude what's behind it, as
		 * it could be alpha-blended. */
		if (!weston_view_is_opaque(ev, &clipped_view))
			pixman_region32_intersect(&clipped_view,
						  &clipped_view,
						  &ev->transform.opaque);
		pixman_region32_union(&occluded_region,
				      &occluded_region,
				      &clipped_view);

		pixman_region32_fini(&clipped_view);
	}

	pixman_region32_fini(&renderer_region);
	pixman_region32_fini(&occluded_region);

	/* In renderer-only mode, we can't test the state as we don't have a
	 * renderer buffer yet. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY)
		return state;

	/* check if we have invalid zpos values, like duplicate(s) */
	drm_output_check_zpos_plane_states(state);

	/* Check to see if this state will actually work. */
	ret = drm_pending_state_test(state->pending_state);
	if (ret != 0) {
		drm_debug(b, "\t\t[view] failing state generation: "
			     "atomic test not OK\n");
		goto err;
	}

	/* Counterpart to duplicating scanout state at the top of this
	 * function: if we have taken a renderer framebuffer and placed it in
	 * the pending state in order to incrementally test overlay planes,
	 * remove it now. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		assert(scanout_state->fb->type == BUFFER_GBM_SURFACE ||
		       scanout_state->fb->type == BUFFER_PIXMAN_DUMB);
		drm_plane_state_put_back(scanout_state);
	}
	return state;

err_region:
	pixman_region32_fini(&renderer_region);
	pixman_region32_fini(&occluded_region);
err:
	drm_output_state_free(state);
	return NULL;
}

void
drm_assign_planes(struct weston_output *output_base, void *repaint_data)
{
	struct drm_backend *b = to_drm_backend(output_base->compositor);
	struct drm_pending_state *pending_state = repaint_data;
	struct drm_output *output = to_drm_output(output_base);
	struct drm_output_state *state = NULL;
	struct drm_plane_state *plane_state;
	struct weston_paint_node *pnode;
	struct weston_plane *primary = &output_base->compositor->primary_plane;
	enum drm_output_propose_state_mode mode = DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY;

	drm_debug(b, "\t[repaint] preparing state for output %s (%lu)\n",
		  output_base->name, (unsigned long) output_base->id);

	if (!b->sprites_are_broken && !output->virtual && b->gbm) {
		drm_debug(b, "\t[repaint] trying planes-only build state\n");
		state = drm_output_propose_state(output_base, pending_state, mode);
		if (!state) {
			drm_debug(b, "\t[repaint] could not build planes-only "
				     "state, trying mixed\n");
			mode = DRM_OUTPUT_PROPOSE_STATE_MIXED;
			state = drm_output_propose_state(output_base,
							 pending_state,
							 mode);
		}
		if (!state) {
			drm_debug(b, "\t[repaint] could not build mixed-mode "
				     "state, trying renderer-only\n");
		}
	} else {
		drm_debug(b, "\t[state] no overlay plane support\n");
	}

	if (!state) {
		mode = DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY;
		state = drm_output_propose_state(output_base, pending_state,
						 mode);
	}

	assert(state);
	drm_debug(b, "\t[repaint] Using %s composition\n",
		  drm_propose_state_mode_to_string(mode));

	wl_list_for_each(pnode, &output->base.paint_node_z_order_list,
			 z_order_link) {
		struct weston_view *ev = pnode->view;
		struct drm_plane *target_plane = NULL;

		/* If this view doesn't touch our output at all, there's no
		 * reason to do anything with it. */
		/* TODO: turn this into assert once z_order_list is pruned. */
		if (!(ev->output_mask & (1u << output->base.id)))
			continue;

		/* Update dmabuf-feedback if needed */
		if (ev->surface->dmabuf_feedback)
			dmabuf_feedback_maybe_update(b, ev,
						     pnode->try_view_on_plane_failure_reasons);
		pnode->try_view_on_plane_failure_reasons = FAILURE_REASONS_NONE;

		/* Test whether this buffer can ever go into a plane:
		 * non-shm, or small enough to be a cursor.
		 *
		 * Also, keep a reference when using the pixman renderer.
		 * That makes it possible to do a seamless switch to the GL
		 * renderer and since the pixman renderer keeps a reference
		 * to the buffer anyway, there is no side effects.
		 */
		if (b->use_pixman ||
		    (weston_view_has_valid_buffer(ev) &&
		    (!wl_shm_buffer_get(ev->surface->buffer_ref.buffer->resource) ||
		     (ev->surface->width <= b->cursor_width &&
		      ev->surface->height <= b->cursor_height))))
			ev->surface->keep_buffer = true;
		else
			ev->surface->keep_buffer = false;

		/* This is a bit unpleasant, but lacking a temporary place to
		 * hang a plane off the view, we have to do a nested walk.
		 * Our first-order iteration has to be planes rather than
		 * views, because otherwise we won't reset views which were
		 * previously on planes to being on the primary plane. */
		wl_list_for_each(plane_state, &state->plane_list, link) {
			if (plane_state->ev == ev) {
				plane_state->ev = NULL;
				target_plane = plane_state->plane;
				break;
			}
		}

		if (target_plane) {
			drm_debug(b, "\t[repaint] view %p on %s plane %lu\n",
				  ev, plane_type_enums[target_plane->type].name,
				  (unsigned long) target_plane->plane_id);
			weston_view_move_to_plane(ev, &target_plane->base);
		} else {
			drm_debug(b, "\t[repaint] view %p using renderer "
				     "composition\n", ev);
			weston_view_move_to_plane(ev, primary);
		}

		if (!target_plane ||
		    target_plane->type == WDRM_PLANE_TYPE_CURSOR) {
			/* cursor plane & renderer involve a copy */
			ev->psf_flags = 0;
		} else {
			/* All other planes are a direct scanout of a
			 * single client buffer.
			 */
			ev->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
		}
	}

	/* We rely on output->cursor_view being both an accurate reflection of
	 * the cursor plane's state, but also being maintained across repaints
	 * to avoid unnecessary damage uploads, per the comment in
	 * drm_output_prepare_cursor_view. In the event that we go from having
	 * a cursor view to not having a cursor view, we need to clear it. */
	if (output->cursor_view) {
		plane_state =
			drm_output_state_get_existing_plane(state,
							    output->cursor_plane);
		if (!plane_state || !plane_state->fb)
			drm_output_set_cursor_view(output, NULL);
	}
}

static void
drm_output_handle_cursor_view_destroy(struct wl_listener *listener, void *data)
{
	struct drm_output *output =
		container_of(listener, struct drm_output,
			     cursor_view_destroy_listener);

	drm_output_set_cursor_view(output, NULL);
}

/** Set the current cursor view used for an output.
 *
 * Ensure the stored value will be properly cleared if the view is destroyed.
 * The stored cursor view helps avoid unnecessary uploads of cursor data to
 * cursor plane buffer objects (see drm_output_prepare_cursor_view).
 */
void
drm_output_set_cursor_view(struct drm_output *output, struct weston_view *ev)
{
	if (output->cursor_view == ev)
		return;

	if (output->cursor_view)
		wl_list_remove(&output->cursor_view_destroy_listener.link);

	output->cursor_view = ev;

	if (ev) {
		output->cursor_view_destroy_listener.notify =
			drm_output_handle_cursor_view_destroy;
		wl_signal_add(&ev->destroy_signal,
			      &output->cursor_view_destroy_listener);
	}
}
