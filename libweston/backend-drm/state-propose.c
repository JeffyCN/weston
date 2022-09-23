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
#include "color-representation.h"
#include "linux-dmabuf.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "shared/string-helpers.h"
#include "shared/weston-assert.h"

static const char *const drm_output_propose_state_mode_as_string[] = {
	[DRM_OUTPUT_PROPOSE_STATE_INVALID] = "invalid(uninitialized) state",
	[DRM_OUTPUT_PROPOSE_STATE_MIXED] = "mixed state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR] = "renderer-and-cursor state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY] = "renderer-only state",
	[DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY]	= "plane-only state"
};

static const char *const drm_output_propose_reused_state_mode_as_string[] = {
	[DRM_OUTPUT_PROPOSE_STATE_INVALID] = "reused invalid(uninitialized) state",
	[DRM_OUTPUT_PROPOSE_STATE_MIXED] = "reused mixed state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR] = "reused renderer-and-cursor state",
	[DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY] = "reused renderer-only state",
	[DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY]	= "reused plane-only state",
};

static const char *
drm_propose_state_mode_to_string(enum drm_output_propose_state_mode mode)
{
	bool reuse = mode & DRM_OUTPUT_PROPOSE_STATE_REUSE;

	mode &= ~DRM_OUTPUT_PROPOSE_STATE_REUSE;
	if (mode < 0 || mode >= ARRAY_LENGTH(drm_output_propose_state_mode_as_string))
		return " unknown compositing mode";

	if (reuse)
		return drm_output_propose_reused_state_mode_as_string[mode];
	else
		return drm_output_propose_state_mode_as_string[mode];
}

static bool
drm_mixed_mode_check_underlay(enum drm_output_propose_state_mode mode,
                              struct drm_plane_state *scanout_state,
                              uint64_t zpos)
{
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		assert(scanout_state != NULL);
		if (scanout_state->zpos >= zpos)
			return true;
	}

	return false;
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
drm_output_try_paint_node_on_plane(struct drm_plane_handle *handle,
				   struct drm_output_state *output_state,
				   struct weston_paint_node *pnode,
				   enum drm_output_propose_state_mode mode,
				   struct drm_fb *fb, uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct weston_surface *surface = pnode->surface;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct drm_plane *plane = handle->plane;
	struct drm_plane_state *state = NULL;

	assert(!device->disable_client_buffer_scanout);
	assert(output == handle->output);
	assert(device->atomic_modeset);
	assert(fb);
	assert(mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY ||
	       (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED &&
	        plane->type == WDRM_PLANE_TYPE_OVERLAY));

	state = drm_output_state_get_plane(output_state, plane);
	/* we can't have a 'pending' framebuffer as never set one before reaching here */
	assert(!state->fb);
	assert(handle->plane == state->plane);
	state->handle = handle;

	drm_plane_state_coords_for_paint_node(state, pnode, zpos);

	/* We hold one reference for the lifetime of this function; from
	 * calling drm_fb_get_from_paint_node() in
	 * drm_output_prepare_plane_view(), so, we take another reference
	 * here to live within the state. */
	state->paint_node = pnode;
	state->fb = drm_fb_ref(fb);
	state->in_fence_fd = surface->acquire_fence_fd;

	if (fb->format && fb->format->color_model == COLOR_MODEL_YUV) {
		struct weston_color_representation color_rep;
		const struct weston_color_matrix_coef_info *matrix_coef_info;
		const struct weston_color_quant_range_info *quant_range_info;

		color_rep =
			weston_fill_color_representation(&surface->color_representation,
							 fb->format);
		matrix_coef_info =
			weston_color_matrix_coef_info_get(color_rep.matrix_coefficients);
		assert(matrix_coef_info);
		assert(matrix_coef_info->wdrm != WDRM_PLANE_COLOR_ENCODING__COUNT);

		quant_range_info =
			weston_color_quant_range_info_get(color_rep.quant_range);
		assert(quant_range_info);
		assert(quant_range_info->wdrm != WDRM_PLANE_COLOR_RANGE__COUNT);


		if (plane->props[WDRM_PLANE_COLOR_ENCODING].prop_id == 0) {
			if (matrix_coef_info->wdrm != WDRM_PLANE_COLOR_ENCODING_DEFAULT) {
				drm_debug(b, "\t\t\t[paint node] not placing paint node %s on plane %lu: "
					  "non-default color encoding not supported\n",
					  pnode->internal_name, (unsigned long) plane->plane_id);
				goto out;
			}
		} else if (!drm_plane_supports_color_encoding(plane,
							      matrix_coef_info->wdrm)) {
			drm_debug(b, "\t\t\t[paint node] not placing paint node %s on plane %lu: "
				  "color encoding not supported\n", pnode->internal_name,
				  (unsigned long) plane->plane_id);
			goto out;
		}

		if (plane->props[WDRM_PLANE_COLOR_RANGE].prop_id == 0) {
			if (quant_range_info->wdrm != WDRM_PLANE_COLOR_RANGE_DEFAULT) {
				drm_debug(b, "\t\t\t[paint node] not placing paint node %s on plane %lu: "
					  "non-default color range not supported\n",
					  pnode->internal_name, (unsigned long) plane->plane_id);
				goto out;
			}
		} else if (!drm_plane_supports_color_range(plane,
							   quant_range_info->wdrm)) {
			drm_debug(b, "\t\t\t[paint node] not placing paint node %s on plane %lu: "
				  "color range not supported\n", pnode->internal_name,
				  (unsigned long) plane->plane_id);
			goto out;
		}

		state->color_encoding = matrix_coef_info->wdrm;
		state->color_range = quant_range_info->wdrm;
	}

	/* In planes-only mode, we don't have an incremental state to
	 * test against, so we just hope it'll work. */
	if (mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY &&
	    drm_pending_state_test(output_state->pending_state) != 0) {
		drm_debug(b, "\t\t\t[paint node] not placing paint node %s on plane %lu: "
		             "atomic test failed\n",
			  pnode->internal_name, (unsigned long) plane->plane_id);
		goto out;
	}

	drm_debug(b, "\t\t\t[paint node] provisionally placing paint node %s on plane %lu\n",
		  pnode->internal_name, (unsigned long) plane->plane_id);

	/* Take a reference on the buffer so that we don't release it
	 * back to the client until we're done with it; cursor buffers
	 * don't require a reference since we copy them. */
	assert(state->fb_ref.buffer.buffer == NULL);
	assert(state->fb_ref.release.buffer_release == NULL);
	weston_buffer_reference(&state->fb_ref.buffer,
				surface->buffer_ref.buffer,
				BUFFER_MAY_BE_ACCESSED);
	weston_buffer_release_reference(&state->fb_ref.release,
					surface->buffer_release_ref.buffer_release);

	return state;

out:
	drm_plane_state_put_back(state);
	return NULL;
}

#ifdef BUILD_DRM_GBM
static struct drm_plane_state *
drm_output_prepare_cursor_paint_node(struct drm_output_state *output_state,
				     struct weston_paint_node *pnode,
				     uint64_t zpos)
{
	struct drm_output *output = output_state->output;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_compositor *compositor = b->compositor;
	struct drm_plane_handle *handle = output->cursor_handle;
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	const char *p_name;

	assert(!device->cursors_are_broken);
	assert(handle);

	plane = handle->plane;

	assert(plane);
	assert(plane->state_cur->complete);
	assert(!plane->state_cur->handle ||
	       plane->state_cur->handle->output == output);

	p_name = drm_output_get_handle_type_name(handle);

	/* We use GBM to import SHM buffers. */
	assert(b->gbm);

	plane_state = drm_output_state_get_plane(output_state, plane);
	assert(!plane_state->fb);

	/* We can't scale with the legacy API, and we don't try to account for
	 * simple cropping/translation in cursor_bo_update. */
	plane_state->handle = handle;
	drm_plane_state_coords_for_paint_node(plane_state, pnode, zpos);

	/* Apply cursor size scaling */
	if (compositor->cursor_size) {
		float scale =
			(float)compositor->cursor_size / pnode->surface->width;
		plane_state->dest_w *= scale;
		plane_state->dest_h *= scale;
	}

	/* Rockchip hw constraint: cursor min size 4x4 */
	if ((plane_state->src_w >> 16) < 4 ||
	    (plane_state->src_h >> 16) < 4) {
		drm_debug(b, "\t\t\t\t[%s] not assigning paint node %s to %s plane "
			  "(cursor too small)\n",
			  p_name, pnode->internal_name, p_name);
		goto err;
	}

	/* Check crop/scaling support for non-atomic modeset */
	if (plane_state->src_x != 0 || plane_state->src_y != 0 ||
	    plane_state->src_w > (unsigned) device->cursor_width << 16 ||
	    plane_state->src_h > (unsigned) device->cursor_height << 16 ||
	    plane_state->src_w != plane_state->dest_w << 16 ||
	    plane_state->src_h != plane_state->dest_h << 16) {
		if (!device->atomic_modeset)
			goto err_crop_scale;

		/* Rockchip hw constraint: max 8x scaling */
		if (!plane->can_scale ||
		    plane_state->dest_w > 8 * (plane_state->src_w >> 16) ||
		    plane_state->dest_w * 8 < (plane_state->src_w >> 16) ||
		    plane_state->dest_h > 8 * (plane_state->src_h >> 16) ||
		    plane_state->dest_h * 8 < (plane_state->src_h >> 16))
			goto err_crop_scale;

		goto out;
	}

	/* The cursor API is somewhat special: in cursor_bo_update(), we upload
	 * a buffer which is always cursor_width x cursor_height, even if the
	 * surface we want to promote is actually smaller than this. Manually
	 * mangle the plane state to deal with this. */
	plane_state->src_w = device->cursor_width << 16;
	plane_state->src_h = device->cursor_height << 16;
	plane_state->src_w -= plane_state->src_x;
	plane_state->src_h -= plane_state->src_y;
	plane_state->dest_w = plane_state->src_w >> 16;
	plane_state->dest_h = plane_state->src_h >> 16;

out:
	plane_state->paint_node = pnode;
	/* We always test with cursor fb 0. There are two potential fbs, and
	 * they are identically allocated for cursor use specifically, so if
	 * one works the other almost certainly should as well.
	 *
	 * Later when we determine if the cursor needs an update, we'll
	 * select the correct fb to use.
	 */
	plane_state->fb = drm_fb_ref(output->gbm_cursor_fb[0]);

	drm_debug(b, "\t\t\t\t[%s] provisionally assigned paint node %s to cursor\n",
		  p_name, pnode->internal_name);

	return plane_state;
err_crop_scale:
	drm_debug(b, "\t\t\t\t[%s] not assigning paint node %s to %s plane "
		  "(positioning requires cropping or scaling)\n",
		  p_name, pnode->internal_name, p_name);
err:
	drm_plane_state_put_back(plane_state);
	return NULL;
}
#else
static struct drm_plane_state *
drm_output_prepare_cursor_paint_node(struct drm_output_state *output_state,
				     struct weston_paint_node *node,
				     uint64_t zpos)
{
	return NULL;
}
#endif

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

static const char *
action_needed_to_str(enum actions_needed_dmabuf_feedback action_needed)
{
      switch(action_needed) {
      case ACTION_NEEDED_ADD_SCANOUT_TRANCHE:
              return "add scanout tranche";
      case ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE:
              return "remove scanout tranche";
      case ACTION_NEEDED_NONE:
              return "no action needed";
      default:
              assert(0);
      }
}

static void
dmabuf_feedback_maybe_update(struct drm_device *device,
			     struct weston_paint_node *pnode)
{
	struct weston_dmabuf_feedback *dmabuf_feedback = pnode->surface->dmabuf_feedback;
	struct weston_dmabuf_feedback_tranche *scanout_tranche;
	struct drm_backend *b = device->backend;
	dev_t scanout_dev = device->kms_device->devnum;
	uint32_t scanout_flags = ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
	enum actions_needed_dmabuf_feedback action_needed = ACTION_NEEDED_NONE;
	struct timespec current_time, delta_time;
	const time_t MAX_TIME_SECONDS = 2;
	uint32_t try_view_on_plane_failure_reasons = pnode->try_view_on_plane_failure_reasons;

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
					scanout_dev, scanout_flags, SCANOUT_PREF);
		scanout_tranche->active = false;
	}

	/* Direct scanout won't happen even if client re-allocates using params
	 * from the scanout tranche, so keep only the renderer tranche. */
	if (try_view_on_plane_failure_reasons & (FAILURE_REASONS_FORCE_RENDERER |
						 FAILURE_REASONS_NO_PLANES_AVAILABLE |
						 FAILURE_REASONS_INADEQUATE_CONTENT_PROTECTION |
						 FAILURE_REASONS_INCOMPATIBLE_TRANSFORM |
						 FAILURE_REASONS_NO_BUFFER |
						 FAILURE_REASONS_BUFFER_TOO_BIG |
						 FAILURE_REASONS_BUFFER_TYPE |
						 FAILURE_REASONS_GLOBAL_ALPHA |
						 FAILURE_REASONS_NO_GBM |
						 FAILURE_REASONS_NO_COLOR_TRANSFORM |
						 FAILURE_REASONS_SOLID_SURFACE |
						 FAILURE_REASONS_OCCLUDED_BY_RENDERER |
						 FAILURE_REASONS_OUTPUT_COLOR_EFFECT)) {
		action_needed = ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE;
	} else if (try_view_on_plane_failure_reasons & (FAILURE_REASONS_ADD_FB_FAILED |
		/* Direct scanout may be possible if client re-allocates using
		 * the params from the scanout tranche. */
							FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE |
							FAILURE_REASONS_DMABUF_MODIFIER_INVALID |
							FAILURE_REASONS_GBM_BO_IMPORT_FAILED |
							FAILURE_REASONS_GBM_BO_GET_HANDLE_FAILED)) {
		action_needed = ACTION_NEEDED_ADD_SCANOUT_TRANCHE;
	} else if (try_view_on_plane_failure_reasons == FAILURE_REASONS_NONE) {
		/* Direct scanout is already possible, so include the scanout
		 * tranche. */
		action_needed = ACTION_NEEDED_ADD_SCANOUT_TRANCHE;
	}

	/* No actions needed, so disarm timer and return */
	if (action_needed == ACTION_NEEDED_NONE ||
	    (action_needed == ACTION_NEEDED_ADD_SCANOUT_TRANCHE && scanout_tranche->active) ||
	    (action_needed == ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE && !scanout_tranche->active)) {
		dmabuf_feedback->action_needed = ACTION_NEEDED_NONE;
		return;
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
		return;
	} else {
		/* Timer is already on and the action needed when it was set to on does
		 * not conflict with the most recent needed action we've detected. If
		 * more than MAX_TIME_SECONDS has passed, we need to resend the dma-buf
		 * feedback. Otherwise, return and leave the timer running. */
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		delta_time.tv_sec = current_time.tv_sec -
				    dmabuf_feedback->timer.tv_sec;
		if (delta_time.tv_sec < MAX_TIME_SECONDS)
			return;
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
		     "dma-buf feedback for surface of paint node %s: %s\n",
		     pnode->internal_name,
		     action_needed_to_str(action_needed));
	weston_dmabuf_feedback_send_all(b->compositor, dmabuf_feedback,
					b->compositor->dmabuf_feedback_format_table);

	/* Set the timer to off */
	dmabuf_feedback->action_needed = ACTION_NEEDED_NONE;
}

static void
try_pnode_on_cursor_plane(struct drm_output *output, struct weston_paint_node *pnode)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_buffer *buffer = pnode->surface->buffer_ref.buffer;
	struct drm_plane *cursor_plane = NULL;

	if (output->cursor_handle)
		cursor_plane = output->cursor_handle->plane;

	if (!cursor_plane || device->cursors_are_broken) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_BUFFER_TYPE;
		/* SHM buffers can only be placed on a cursor plane, so if
		 * cursors aren't available skip all the following tests,
		 * we already have the only failure reason that matters.
		 */
		return;
	}

	/* Even though this is a SHM buffer, pixel_format stores
	 * the format code as DRM FourCC */
	if (buffer->pixel_format->format != DRM_FORMAT_ARGB8888) {
		drm_debug(b, "\t\t\t\t[paint node] not placing paint node %s on "
			     "plane; SHM buffers must be ARGB8888 for "
			     "cursor\n", pnode->internal_name);
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE;
	}

	if (buffer->width > device->cursor_width ||
	    buffer->height > device->cursor_height) {
		drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s to plane "
			     "(buffer (%dx%d) too large for cursor plane)\n",
			     pnode->internal_name, buffer->width, buffer->height);
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_BUFFER_TOO_BIG;
	}

	if (!drm_paint_node_transform_supported(pnode, cursor_plane))
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_INCOMPATIBLE_TRANSFORM;
}

static bool
view_with_region_matches_output_entirely(struct weston_paint_node *pnode,
					 const pixman_region32_t *background_region,
					 struct weston_output *output)
{
	pixman_region32_t combined_region;
	pixman_box32_t *extents;
	bool res = true;

	pixman_region32_init(&combined_region);

	pixman_region32_union(&combined_region,
			      background_region,
			      weston_paint_node_get_opaque_region (pnode));

	/* Check for holes in the region */
	if (pixman_region32_n_rects (&combined_region) != 1) {
		pixman_region32_fini(&combined_region);
		return false;
	}

	extents = pixman_region32_extents(&combined_region);

	if (extents->x1 != (int32_t)output->pos.c.x ||
	    extents->y1 != (int32_t)output->pos.c.y ||
	    extents->x2 != (int32_t)output->pos.c.x + output->width ||
	    extents->y2 != (int32_t)output->pos.c.y + output->height)
		res = false;

	pixman_region32_fini(&combined_region);

	return res;
}

static bool
pnode_can_use_plane(struct drm_output_state *output_state,
		    struct drm_plane_handle *handle,
		    struct weston_paint_node *pnode)
{
	const char *p_name = drm_output_get_handle_type_name(handle);
	struct drm_output *output = output_state->output;
	struct drm_plane *plane = handle->plane;
	struct drm_backend *b = output->backend;

	if (!drm_plane_is_available(plane, output))
		return false;

	if (drm_output_check_plane_has_view_assigned(plane, output_state)) {
		drm_debug(b, "\t\t\t\t[plane] not trying plane %d: "
			     "another view already assigned\n",
			     plane->plane_id);
		return false;
	}

	/* if view has alpha check if this plane supports plane alpha */
	if (pnode->view_alpha != 1.0f && plane->alpha_max == plane->alpha_min) {
		drm_debug(b, "\t\t\t\t[plane] not trying plane %d:"
			     "plane-alpha not supported\n",
			     plane->plane_id);
		return false;
	}

	/* If the surface buffer has an in-fence fd, but the plane doesn't
	 * support fences, we can't place the buffer on this plane. */
	if (pnode->surface->acquire_fence_fd >= 0 &&
	    plane->props[WDRM_PLANE_IN_FENCE_FD].prop_id == 0) {
		drm_debug(b, "\t\t\t\t[%s] not placing paint node %s on %s: "
		          "no in-fence support\n",
			  p_name, pnode->internal_name, p_name);
		return false;
	}

	return true;
}

static struct drm_plane_state *
drm_output_find_plane_for_paint_node(struct drm_output_state *state,
				     struct weston_paint_node *pnode,
				     enum drm_output_propose_state_mode mode,
				     struct drm_plane_state *scanout_state,
				     const pixman_region32_t *background_region,
				     uint64_t current_lowest_zpos_overlay,
				     uint64_t current_lowest_zpos_underlay,
				     bool need_underlay)
{
	struct drm_output *output = state->output;
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct drm_plane_state *ps = NULL;
	struct drm_plane_handle *handle;
	struct weston_buffer *buffer;
	struct drm_fb *fb = NULL;
	uint64_t current_lowest_zpos = need_underlay ?
	                               current_lowest_zpos_underlay :
	                               current_lowest_zpos_overlay;

	uint32_t possible_plane_mask = 0;
	uint32_t fb_failure_reasons = 0;
	bool any_candidate_picked = false;

	/* renderer-only mode, so no view assignments to planes */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_FORCE_RENDERER;
		return NULL;
	}

	/* filter out non-cursor views in renderer-and-cursor mode */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR &&
	    !pnode->on_cursor_layer) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_FORCE_RENDERER;
		return NULL;
	}

	/* check view for valid buffer, doesn't make sense to even try */
	if (!weston_paint_node_has_valid_buffer(pnode)) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_NO_BUFFER;
		return NULL;
	}

	buffer = pnode->surface->buffer_ref.buffer;
	if (pnode->draw_solid) {
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_SOLID_SURFACE;
	} else if (buffer->type == WESTON_BUFFER_SHM) {
		struct drm_plane_handle *cursor_handle = output->cursor_handle;
		struct drm_plane *cursor_plane = NULL;
		uint64_t zpos;

		if (!output->cursor_handle) {
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_PLANES_REJECTED;
			return NULL;
		}

		cursor_plane = output->cursor_handle->plane;

		try_pnode_on_cursor_plane(output, pnode);

		if (pnode->try_view_on_plane_failure_reasons == FAILURE_REASONS_NONE &&
		    pnode_can_use_plane(state, cursor_handle, pnode)) {
			if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
				zpos = cursor_plane->zpos_max;
			else
				zpos = MIN(current_lowest_zpos - 1, cursor_plane->zpos_max);

			ps = drm_output_prepare_cursor_paint_node(state, pnode, zpos);
		}
		if (!ps)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_PLANES_REJECTED;
		return ps;
	} else {
		struct drm_plane *scanout_plane = output->scanout_handle->plane;

		if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR) {
			drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s "
				     "to plane: renderer-and-cursor mode\n",
				     pnode->internal_name);
			return NULL;
		}

		if (drm_paint_node_transform_supported(pnode, scanout_plane))
			possible_plane_mask |= 1 << scanout_plane->plane_idx;

		wl_list_for_each(handle, &output->plane_handle_list, link) {
			struct drm_plane *plane = handle->plane;

			if (drm_paint_node_transform_supported(pnode, plane))
				possible_plane_mask |= 1 << plane->plane_idx;
		}

		if (!possible_plane_mask)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_INCOMPATIBLE_TRANSFORM;

		fb = drm_fb_get_from_paint_node(state, pnode, &fb_failure_reasons);
		if (fb) {
			possible_plane_mask &= fb->plane_mask;
		} else {
			FILE *dbg = weston_log_scope_stream(b->debug);

			if (dbg) {
				fputs("\t\t\t[paint node] couldn't get FB for paint node: ", dbg);
				bits_to_str_stream(fb_failure_reasons,
						   weston_plane_failure_reasons_to_str, dbg);
				fputs("\n", dbg);
			}
			pnode->try_view_on_plane_failure_reasons |= fb_failure_reasons;
		}
	}

	/* if the view covers the whole output, put it in the scanout plane,
	 * not overlay */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY) {
		struct drm_plane_handle *scanout_handle = output->scanout_handle;
		struct drm_plane *scanout_plane = scanout_handle->plane;
		bool scanout_has_view_assigned;
		bool view_matches_entire_output;
		bool scanout_plane_possible;
		bool may_use_scanout_plane;

		scanout_has_view_assigned =
			drm_output_check_plane_has_view_assigned(output->scanout_handle->plane,
								 state);
		view_matches_entire_output =
			view_with_region_matches_output_entirely(pnode,
								 background_region,
								 &output->base);

		may_use_scanout_plane = !scanout_has_view_assigned && view_matches_entire_output;

		scanout_plane_possible = possible_plane_mask & (1 << output->scanout_handle->plane->plane_idx) &&
					 pnode_can_use_plane(state, scanout_handle, pnode);

		if (may_use_scanout_plane && scanout_plane_possible && fb) {
			uint64_t zpos;

			if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
				zpos = scanout_plane->zpos_max;
			else
				zpos = MIN(current_lowest_zpos - 1, scanout_plane->zpos_max);

			ps = drm_output_try_paint_node_on_plane(scanout_handle,
								state, pnode, mode,
								fb, zpos);
			if (ps) {
				drm_fb_unref(fb);
				return ps;
			}
		}
		/* Maybe we can still build a planes only state without
		 * the scanout plane at all.
		 */
	}

	/* assemble a list with possible candidates */
	wl_list_for_each(handle, &output->plane_handle_list, link) {
		struct drm_plane *plane = handle->plane;
		const char *p_name = drm_output_get_handle_type_name(handle);
		uint64_t zpos;
		bool mm_underlay_only = false;

		weston_assert_enum(b->compositor,
				   plane->type, WDRM_PLANE_TYPE_OVERLAY);

		if (possible_plane_mask == 0)
			break;

		if (!(possible_plane_mask & (1 << plane->plane_idx)))
			continue;

		possible_plane_mask &= ~(1 << plane->plane_idx);
		mm_underlay_only =
			drm_mixed_mode_check_underlay(mode, scanout_state, plane->zpos_max);

		weston_assert_false(b->compositor,
				    mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR);

		/* for alpha views, avoid placing them on the hardware
		 * planes that are below the primary plane. */
		if (mm_underlay_only && !pnode->is_fully_opaque)
			continue;

		if (!pnode_can_use_plane(state, handle, pnode))
			continue;

		/* The paint node isn't occluded by the renderer, so it doesn't
		 * unconditionally need an underlay plane. However, we may
		 * only have underlay planes available, so it could use one
		 * anyway.
		 */
		if (!need_underlay && output->has_underlay) {
			uint64_t tmp_next_lowest_zpos;
			if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
				tmp_next_lowest_zpos = plane->zpos_max;
			else
				tmp_next_lowest_zpos = current_lowest_zpos - 1;
			if (drm_mixed_mode_check_underlay(mode, scanout_state, tmp_next_lowest_zpos)) {
				drm_debug(b, "\t\t\t\t[plane] could not use overlay planes, "
				             "attempting to find underlay plane\n");
				current_lowest_zpos = current_lowest_zpos_underlay;
			}
		}

		if (plane->zpos_min >= current_lowest_zpos) {
			drm_debug(b, "\t\t\t\t[plane] not trying plane %d: "
				     "plane's minimum zpos (%"PRIu64") above "
				     "current lowest zpos (%"PRIu64")\n",
				     plane->plane_id, plane->zpos_min,
				     current_lowest_zpos);
			continue;
		}

		if (!output->has_underlay && mm_underlay_only) {
			drm_debug(b, "\t\t\t\t[plane] not adding plane %d to "
				     "candidate list: plane is below the primary "
				     "plane and backend format (%s) is opaque, "
				     "hole on primary plane will not work\n",
				     plane->plane_id, b->format->drm_format_name);

			continue;
		}

		if (current_lowest_zpos == DRM_PLANE_ZPOS_INVALID_PLANE)
			zpos = plane->zpos_max;
		else
			zpos = MIN(current_lowest_zpos - 1, plane->zpos_max);

		any_candidate_picked = true;
		drm_debug(b, "\t\t\t\t[plane] plane %d picked "
			     "from candidate list, type: %s\n",
			     plane->plane_id, p_name);

		if (fb)
			ps = drm_output_try_paint_node_on_plane(handle, state,
								pnode, mode,
								fb, zpos);

		if (ps) {
			/* Check if this ps is underlay plane, if so, the view
			 * needs through hole on primary plane. */
			pnode->need_hole =
				drm_mixed_mode_check_underlay(mode,
							      scanout_state,
							      ps->zpos);

			drm_debug(b, "\t\t\t\t[paint node] paint node %s has been placed to "
				     "%s plane as an %s with computed zpos "
				     "%"PRIu64"\n",
				     pnode->internal_name, p_name,
				     pnode->need_hole ? "underlay" : "overlay",
				     zpos);
			break;
		}

		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_PLANES_REJECTED;
	}

	if (!any_candidate_picked)
		pnode->try_view_on_plane_failure_reasons |=
			FAILURE_REASONS_NO_PLANES_AVAILABLE;

	/* if we have a plane state, it has its own ref to the fb; if not then
	 * we drop ours here */
	drm_fb_unref(fb);
	return ps;
}

static bool
is_paint_node_solid_opaque_untransformed(struct weston_paint_node *pnode)
{
	return pnode->draw_solid && pnode->is_fully_opaque &&
	       pnode->valid_transform &&
	       (pnode->surf_xform_valid && !pnode->surf_xform.transform);
}

static bool
lower_solid_views_to_background_region(struct drm_output *output,
				       struct wl_array *visible_pnodes,
				       struct weston_paint_node **last_visible_pnode,
				       pixman_region32_t *background_region)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_paint_node **visible_pnode;
	struct wl_array visible_pnodes_new;
	struct weston_solid_buffer_values background_region_color = {0};

	wl_array_init(&visible_pnodes_new);
	wl_array_for_each(visible_pnode, visible_pnodes) {
		struct weston_paint_node *pnode = *visible_pnode;
		struct weston_paint_node **visible_pnode_new;
		pixman_region32_t tmp;

		drm_debug(b, "\t\t\t[paint node] evaluating paint node %s for scene"
			  "-graph optimization on output %s (%lu)\n",
			  pnode->internal_name, output->base.name,
			  (unsigned long) output->base.id);

		if (!pnode->draw_solid) {
			/* Bail if parts of the view need to be occluded by the
			 * background region as this would generally require a
			 * solid-color plane on a higher z-pos.
			 * Note: A special case that could be optimized in the future
			 * is if the visible region of the view is a rectangle. In that
			 * case we could crop the plane. */
			pixman_region32_init(&tmp);
			pixman_region32_intersect(&tmp,
						  &pnode->clipped_view,
						  background_region);
			if (pixman_region32_not_empty(&tmp)) {
				drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s to "
					  "a plane (occluded by solid buffer).\n",
					  pnode->internal_name);
				pixman_region32_fini(&tmp);
				goto error;
			}
			pixman_region32_fini(&tmp);

			visible_pnode_new = wl_array_add(&visible_pnodes_new,
							 sizeof(pnode));
			*visible_pnode_new = pnode;
			*last_visible_pnode = pnode;
			continue;
		}

		if (!is_paint_node_solid_opaque_untransformed(pnode)) {
			drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s to "
                                  "a plane (solid buffer not opaque or requiring "
				  "a color transform).\n",
				  pnode->internal_name);
			goto error;
		}

		if (!drm_crtc_supports_background_color(output->crtc) &&
		    (pnode->solid.r != 0.0f || pnode->solid.g != 0.0f ||
		     pnode->solid.b != 0.0f)) {
			drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s to "
                                  "a plane (non-opaque-black solid buffer not "
				  "supported).\n",
				  pnode->internal_name);
			goto error;
		}

		if (pixman_region32_not_empty(background_region) &&
		    (background_region_color.r != pnode->solid.r ||
		     background_region_color.g != pnode->solid.g ||
		     background_region_color.b != pnode->solid.b)) {
			drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s to "
				  "a plane (multiple solid buffer buffers with "
				  "different colors).\n",
				  pnode->internal_name);
			goto error;
		}

		drm_debug(b, "\t\t\t\t[paint node] lowering paint node %s to background " \
			  "(opaque solid buffer r %f g %f b %f " \
			  "a %f).\n",
			  pnode->internal_name, pnode->solid.r, pnode->solid.g,
			  pnode->solid.b, pnode->solid.a);

		pixman_region32_union(background_region,
				      background_region,
				      &pnode->visible);

		/* Fixate the background_region color */
		background_region_color.a = 1.0f;
		background_region_color.r = pnode->solid.r;
		background_region_color.g = pnode->solid.g;
		background_region_color.b = pnode->solid.b;

		if (drm_crtc_supports_background_color(output->crtc)) {
			uint64_t a16, r16, g16, b16;

			a16 = 0xffff;
			r16 = 0xffff * background_region_color.r;
			g16 = 0xffff * background_region_color.g;
			b16 = 0xffff * background_region_color.b;

			output->crtc->background_color =
				a16 << 48 | r16 << 32 | g16 << 16 | b16;
		}
	}

	wl_array_release(visible_pnodes);
	*visible_pnodes = visible_pnodes_new;
	return true;

error:
	wl_array_release(&visible_pnodes_new);
	output->crtc->background_color = 0;
	return false;
}

static void
debug_propose_fail(struct drm_output *output,
		   enum drm_output_propose_state_mode mode,
		   const char *reason)
{
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	const char *mode_str = drm_propose_state_mode_to_string(mode);

	drm_debug(b, "\t\t[state] cannot propose %s "
	             "for output %s (%lu): %s\n",
		  mode_str,
		  output->base.name,
		  (unsigned long) output->base.id,
		  reason);
}

static struct drm_output_state *
drm_output_propose_state_try_reuse(struct weston_output *output_base,
				   struct drm_pending_state *pending_state,
				   enum drm_output_propose_state_mode mode)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_compositor *compositor = b->compositor;
	struct weston_paint_node *pnode;
	struct drm_output_state *state;

	if (output->force_rebuild_state) {
		debug_propose_fail(output, mode, "previous state failed");
		return NULL;
	}

	/* These states are where we end up when we can't use planes
	 * at all, sometimes simply because we don't have a renderer
	 * fb yet. In that case we'd immediately get into a better
	 * state on the next repaint, even with no scene graph
	 * changes. But if we allow reuse we're stuck in a bad state
	 * waiting for a scene graph change to move us out of it.
	 *
	 * Just disable this optimization for these suboptimal states.
	 */
	if (output->state_cur->mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY ||
	    output->state_cur->mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR) {
		debug_propose_fail(output, mode, "probable fallback state");
		return NULL;
	}

	if (output->state_cur->mode == DRM_OUTPUT_PROPOSE_STATE_INVALID) {
		debug_propose_fail(output, mode, "no previous state");
		return NULL;
	}

	if (output_base->paint_node_changes & ~WESTON_PAINT_NODE_BUFFER_DIRTY) {
		debug_propose_fail(output, mode, "state is outdated");
		return NULL;
	}

	if (output->state_cur->planes_enabled != !output_base->disable_planes) {
		debug_propose_fail(output, mode, "planes_enabled changed");
		return NULL;
	}

	state = drm_output_state_duplicate(output->state_cur,
					   pending_state,
					   DRM_OUTPUT_STATE_PRESERVE_PLANES);
	if (!state) {
		debug_propose_fail(output, mode, "could not clone prior state");
		return NULL;
	}

	wl_list_for_each(pnode, &output_base->paint_node_z_order_list,
			 z_order_link) {
		enum try_view_on_plane_failure_reasons reasons;
		struct drm_plane_state *pstate;
		struct drm_plane *plane;
		struct drm_fb *fb;

		/* we don't care about renderer views */
		if (pnode->plane == &output_base->primary_plane) {
			drm_debug(b, "\t\t[reuse] ignoring paint node %s on renderer plane\n", pnode->internal_name);
			continue;
		}
		plane = (struct drm_plane *) pnode->plane;
		pstate = drm_output_state_get_existing_plane(state, plane);
		weston_assert_ptr_not_null(compositor, pstate);
		pstate->paint_node = pnode;

		/* cursor is handled out of band */
		if (plane->type == WDRM_PLANE_TYPE_CURSOR) {
			drm_debug(b, "\t\t[reuse] ignoring cursor plane for paint node %s\n", pnode->internal_name);
			continue;
		}

		drm_debug(b, "\t\t[reuse] paint node %s has plane %d\n",
			  pnode->internal_name, plane->plane_id);

		/* FIXME: If we get here, there should be a valid weston_buffer, and that's
		 * all we should ever need at this point. If the buffer is deleted while
		 * attached to a surface right now weston_paint_node_has_valid_buffer() sees the
		 * buffer as invalid. We don't want that to be the case, but we also don't
		 * want paint node DIRTY bits to track that. This will be cleaned up in the
		 * future by chasing down remaining bugs in buffer lifecycle management, at
		 * which point we replace this with a weston_assert() instead.
		 */
		if (!weston_paint_node_has_valid_buffer(pnode)) {
			drm_debug(b, "\t\t[reuse] paint node %s no longer has a valid buffer\n",
				  pnode->internal_name);
			drm_output_state_free(state);
			return NULL;
		}

		fb = drm_fb_get_from_paint_node(state, pnode, &reasons);
		if (!fb) {
			char *fr_str = bits_to_str(pnode->try_view_on_plane_failure_reasons,
						   weston_plane_failure_reasons_to_str);
			char *msg;

			str_printf(&msg, "couldn't get new FB: %s", fr_str);
			debug_propose_fail(output, mode, msg);
			free(msg);
			free(fr_str);
			drm_output_state_free(state);
			return NULL;
		}

		drm_fb_unref(pstate->fb);
		pstate->fb = fb;

		pstate->in_fence_fd = pnode->surface->acquire_fence_fd;
		drm_debug(b, "\t\t[reuse] successfully stole away pnode %s to reused plane\n", pnode->internal_name);
		/* XXX: When we set non-default color states in DRM, make sure they match */
		weston_buffer_reference(&pstate->fb_ref.buffer,
					pnode->surface->buffer_ref.buffer,
					BUFFER_MAY_BE_ACCESSED);
		weston_buffer_release_reference(&pstate->fb_ref.release,
						pnode->surface->buffer_release_ref.buffer_release);
	}

	if (device->reused_state_failures > DRM_MAX_REUSE_FAILURES) {
		drm_debug(b, "\t\t[reuse] must test due to high number of failures\n");
		if (drm_pending_state_test(pending_state) != 0) {
			debug_propose_fail(output, mode, "atomic test not OK");
			drm_output_state_free(state);
			return NULL;
		}
	}

	/* In any state with the renderer, we need to unreference and remove
	 * the previous renderer fb.
	 */
	if (state->mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY) {
		struct drm_plane *scanout_plane = output->scanout_handle->plane;
		struct drm_plane_state *pstate =
			drm_output_state_get_existing_plane(state, scanout_plane);

		/* drm_output_repaint expects to see this */
		drm_debug(b, "\t\t[reuse] dropped reference on renderer fb %p\n", pstate->fb);
		weston_assert_true(compositor,
				   pstate->fb->type == BUFFER_GBM_SURFACE ||
				   pstate->fb->type == BUFFER_PIXMAN_DUMB ||
				   pstate->fb->type == BUFFER_DMABUF_BACKEND);
		drm_fb_unref(pstate->fb);
		pstate->fb = NULL;
	}

	output->reused_state = true;
	return state;
}

static struct drm_output_state *
drm_output_propose_state(struct weston_output *output_base,
			 struct drm_pending_state *pending_state,
			 enum drm_output_propose_state_mode mode)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct weston_paint_node *pnode;
	struct drm_output_state *state;
	struct drm_plane_state *scanout_state = NULL;

	struct weston_paint_node **visible_pnode;
	struct weston_paint_node *last_visible_pnode = NULL;
	struct wl_array visible_pnodes;

	pixman_region32_t renderer_region;
	pixman_region32_t background_region;
	pixman_region32_t obscured_region;

	int ret;
	/* Record the current lowest zpos of the overlay planes */
	uint64_t current_lowest_zpos_overlay = DRM_PLANE_ZPOS_INVALID_PLANE;
	/* Record the current lowest zpos of the underlay plane */
	uint64_t current_lowest_zpos_underlay = DRM_PLANE_ZPOS_INVALID_PLANE;

	output->reused_state = false;

	if (mode & DRM_OUTPUT_PROPOSE_STATE_REUSE) {
		return drm_output_propose_state_try_reuse(output_base,
							  pending_state,
							  mode);
	}

	assert(!output->state_last);
	state = drm_output_state_duplicate(output->state_cur,
					   pending_state,
					   DRM_OUTPUT_STATE_CLEAR_PLANES);
	state->mode = mode;
	state->dpms = WESTON_DPMS_ON;
	state->planes_enabled = !output_base->disable_planes;

	/* Start with the assumption that we're going to do a tearing commit,
	 * if the hardware supports it and we're not compositing with the
	 * renderer.
	 * As soon as anything in the scene graph wants to be presented without
	 * tearing, or a test fails, drop the tear flag. */
	state->tear = device->tearing_supported &&
		      mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY;

	/* We implement mixed mode by progressively creating and testing
	 * incremental states, of scanout + overlay + cursor. Since we
	 * walk our views top to bottom, the scanout plane is last, however
	 * we always need it in our scene for the test modeset to be
	 * meaningful. To do this, we steal a reference to the last
	 * renderer framebuffer we have, if we think it's basically
	 * compatible. If we don't have that, then we conservatively fall
	 * back to only using the renderer for this repaint. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		struct drm_plane *plane = output->scanout_handle->plane;
		struct drm_fb *scanout_fb = plane->state_cur->fb;

		if (!scanout_fb ||
		    (scanout_fb->type != BUFFER_GBM_SURFACE &&
		     scanout_fb->type != BUFFER_PIXMAN_DUMB &&
		     scanout_fb->type != BUFFER_DMABUF_BACKEND)) {
			debug_propose_fail(output, mode,
					   "no previous renderer fb");
			drm_output_state_free(state);
			return NULL;
		}

		if (scanout_fb->width != output_base->current_mode->width ||
		    scanout_fb->height != output_base->current_mode->height) {
			debug_propose_fail(output, mode,
					   "previous fb has different size");
			drm_output_state_free(state);
			return NULL;
		}

		scanout_state = drm_plane_state_duplicate(state,
							  plane->state_cur);
		/* assign the primary the lowest zpos value */
		scanout_state->zpos = plane->zpos_min;
		/* Set the initial lowest zpos used for the underlay plane
		 * (assuming a capable platform) to the zpos of the primary
		 * plane, matching the lowest possible value. As we parse views
		 * from top to bottom we also need a start-up point for
		 * underlays, below this initial lowest zpos value. */
		current_lowest_zpos_underlay = scanout_state->zpos;
		drm_debug(b, "\t\t[state] using renderer FB ID %lu for mixed "
			     "mode for output %s (%lu)\n",
			  (unsigned long) scanout_fb->fb_id, output->base.name,
			  (unsigned long) output->base.id);
		drm_debug(b, "\t\t[state] scanout will use for zpos %"PRIu64"\n",
				scanout_state->zpos);
	}

	/* Build an array of paint nodes that will be visible on screen. Doing
	 * so before assigning them to hardware planes or the renderer allows
	 * us to apply optimizations. */
	wl_array_init(&visible_pnodes);
	wl_list_for_each(pnode, &output->base.paint_node_z_order_list,
			 z_order_link) {
		pnode->try_view_on_plane_failure_reasons = FAILURE_REASONS_NONE;

		drm_debug(b, "\t\t\t[paint node] evaluating paint node %s for scene-graph "
		             "building on output %s (%lu)\n",
		          pnode->internal_name, output->base.name,
			  (unsigned long) output->base.id);

		/* Cannot show anything without a color transform. */
		if (!pnode->surf_xform_valid) {
			drm_debug(b, "\t\t\t\t[paint node] ignoring paint node %s "
				     "(color transform failed)\n",
				     pnode->internal_name);
			continue;
		}

		if (pnode->is_fully_transparent) {
			drm_debug(b, "\t\t\t\t[paint node] ignoring paint node %s " \
				  "(fully transparent)\n", pnode->internal_name);
			continue;
		}

		/* if the view is completely occluded then ignore that
		 * view; includes the case where occluded_region covers
		 * the entire output */
		if (!pixman_region32_not_empty(&pnode->visible)) {
			drm_debug(b, "\t\t\t\t[paint node] ignoring paint node %s "
				     "(occluded on our output)\n",
				     pnode->internal_name);
			continue;
		}

		visible_pnode = wl_array_add(&visible_pnodes, sizeof(pnode));
		*visible_pnode = pnode;
	}

	/* renderer_region contains the total region which which will be
	 * covered by the renderer and underlay region. */
	pixman_region32_init(&renderer_region);

	pixman_region32_init(&obscured_region);

	/* background_region contains the area that is covered by opaque
	 * solid views. If they are black this area can be fully ignored in
	 * PLANES_ONLY mode according to the DRM spec:
	 *
	 * "Unless explicitly specified (via CRTC property or otherwise), the
	 * active area of a CRTC will be black by default. This means portions
	 * of the active area which are not covered by a plane will be black,
	 * and alpha blending of any planes with the CRTC background will blend
	 * with black at the lowest zpos."
	 *
	 * See https://dri.freedesktop.org/docs/drm/gpu/drm-kms.html#plane-abstraction
	 *
	 * For other colors the same applies if the BACKGROUND_COLOR DRM
	 * property is supported.
	 *
	 * All said views can thus be ignored during plane assignment.
	 */
	pixman_region32_init(&background_region);

	if (mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY &&
	    !lower_solid_views_to_background_region(output,
						    &visible_pnodes,
						    &last_visible_pnode,
						    &background_region))
		goto err_region;

	/* Assign paint nodes to planes. */
	wl_array_for_each(visible_pnode, &visible_pnodes) {
		struct weston_paint_node *pnode = *visible_pnode;
		struct drm_plane_state *ps = NULL;
		bool need_underlay = false;
		pixman_region32_t tmp;
		bool renderer_ok = (mode != DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY);

		drm_debug(b, "\t\t\t[paint node] evaluating paint node %s for plane "
		             "assignment on output %s (%lu)\n",
			  pnode->internal_name, output->base.name,
			  (unsigned long) output->base.id);

		if (!b->gbm)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_NO_GBM;

		if (!weston_paint_node_has_valid_buffer(pnode))
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_NO_BUFFER;

		if (pnode->draw_solid)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_SOLID_SURFACE;

		if (pnode->output->color_effect)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_OUTPUT_COLOR_EFFECT;

		if (pnode->surf_xform.transform != NULL ||
		    !pnode->surf_xform.identity_pipeline)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_NO_COLOR_TRANSFORM;

		/* Since we process views from top to bottom, we know that if
		 * the view intersects the calculated renderer region, it must
		 * be part of, or occluded by, it, and cannot go on an overlay
		 * plane. */
		pixman_region32_init(&tmp);
		pixman_region32_intersect(&tmp, &renderer_region,
					  &pnode->clipped_view);
		if (pixman_region32_not_empty(&tmp)) {
			if (output->has_underlay) {
				need_underlay = true;
			} else {
				pnode->try_view_on_plane_failure_reasons |=
					FAILURE_REASONS_OCCLUDED_BY_RENDERER;
				drm_debug(b, "\t\t\t\t[paint node] not assigning paint node %s to a "
					     "plane (occluded by renderer views), current lowest "
					     "zpos change to %"PRIu64"\n", pnode->internal_name,
					     current_lowest_zpos_underlay);
			}
		}
		pixman_region32_fini(&tmp);

		/* If need_underlay, but view contains alpha, then it needs to
		 * be rendered. Only fully-opaque views can go on an underlay.
		 */
		if (need_underlay && !pnode->is_fully_opaque)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_OCCLUDED_BY_RENDERER;

		/* In case of enforced mode of content-protection do not
		 * assign planes for a protected surface on an unsecured output.
		 */
		if (pnode->censored)
			pnode->try_view_on_plane_failure_reasons |=
				FAILURE_REASONS_INADEQUATE_CONTENT_PROTECTION;

		if (pnode->surface->tear_control)
			state->tear &= pnode->surface->tear_control->may_tear;
		else
			state->tear = 0;

		/* Now try to place it on a plane if we can. */
		if (!pnode->try_view_on_plane_failure_reasons) {
			pixman_region32_t obscured_or_background_region;

			drm_debug(b, "\t\t\t[plane] started with zpos %"PRIu64"\n",
				      need_underlay ? current_lowest_zpos_underlay :
				      current_lowest_zpos_overlay);

			pixman_region32_init(&obscured_or_background_region);
			if (pnode == last_visible_pnode) {
				pixman_region32_union(&obscured_or_background_region,
						      &background_region,
						      &obscured_region);
				if (pixman_region32_not_empty (&obscured_or_background_region))
					drm_debug(b, "\t\t\t[plane] adding background region\n");
			}

			ps = drm_output_find_plane_for_paint_node(state, pnode, mode,
								  scanout_state,
								  &obscured_or_background_region,
								  current_lowest_zpos_overlay,
								  current_lowest_zpos_underlay,
								  need_underlay);

			pixman_region32_fini(&obscured_or_background_region);
		}

		if (ps) {
			if (mode == DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY &&
			    ps->plane->type == WDRM_PLANE_TYPE_OVERLAY) {
				pixman_region32_union(&obscured_region,
						      &obscured_region,
						      weston_paint_node_get_opaque_region (pnode));
			}

			if (drm_mixed_mode_check_underlay(mode, scanout_state, ps->zpos))
				current_lowest_zpos_underlay = ps->zpos;
			else
				current_lowest_zpos_overlay = ps->zpos;
			drm_debug(b, "\t\t\t[plane] next overlay zpos to use %"PRIu64","
			             " next underlay zpos to use %"PRIu64"\n",
			             current_lowest_zpos_overlay,
			             current_lowest_zpos_underlay);
		} else if (!ps && !renderer_ok) {
			drm_debug(b, "\t\t[paint node] failing state generation: "
				      "placing paint node %s to renderer not allowed\n",
				  pnode->internal_name);
			goto err_region;
		} else if (!ps) {
			FILE *dbg = weston_log_scope_stream(b->debug);

			if (dbg) {
				fprintf(dbg, "\t\t\t\t[paint node] paint node %s will be placed on the renderer: ",
					pnode->internal_name);
				bits_to_str_stream(pnode->try_view_on_plane_failure_reasons,
						   weston_plane_failure_reasons_to_str, dbg);
				fputs("\n", dbg);
			}
		}

		if (!ps || drm_mixed_mode_check_underlay(mode, scanout_state, ps->zpos)) {
			/* visible contains the area that's going to be visible
			 * on screen; add this to the renderer region */
			pixman_region32_union(&renderer_region,
					      &renderer_region,
					      &pnode->visible);
		}
	}

	pixman_region32_fini(&renderer_region);
	pixman_region32_fini(&obscured_region);
	pixman_region32_fini(&background_region);
	wl_array_release(&visible_pnodes);

	/* In renderer-only and renderer-and-cursor modes, we can't test the
	 * state as we don't have a renderer buffer yet. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY ||
	    mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR)
		return state;

	/* check if we have invalid zpos values, like duplicate(s) */
	drm_output_check_zpos_plane_states(state);

	/* Check to see if this state will actually work. */
	ret = drm_pending_state_test(state->pending_state);
	if (ret != 0) {
		debug_propose_fail(output, mode, "atomic test not OK");
		goto err;
	}

	/* Counterpart to duplicating scanout state at the top of this
	 * function: if we have taken a renderer framebuffer and placed it in
	 * the pending state in order to incrementally test overlay planes,
	 * remove it now. */
	if (mode == DRM_OUTPUT_PROPOSE_STATE_MIXED) {
		assert(scanout_state->fb->type == BUFFER_GBM_SURFACE ||
		       scanout_state->fb->type == BUFFER_PIXMAN_DUMB ||
		       scanout_state->fb->type == BUFFER_DMABUF_BACKEND);
		drm_plane_state_put_back(scanout_state);
	}
	return state;

err_region:
	pixman_region32_fini(&renderer_region);
	pixman_region32_fini(&obscured_region);
	pixman_region32_fini(&background_region);
	wl_array_release(&visible_pnodes);
err:
	drm_output_state_free(state);
	return NULL;
}

void
drm_assign_planes(struct weston_output *output_base)
{
	struct drm_output *output = to_drm_output(output_base);
	struct drm_device *device = output->device;
	struct drm_backend *b = device->backend;
	struct drm_pending_state *pending_state = device->repaint_data;
	struct drm_output_state *state;
	struct drm_plane_state *plane_state;
	struct drm_writeback_state *wb_state = output->wb_state;
	struct weston_paint_node *pnode;
	struct weston_plane *primary = &output_base->primary_plane;
	enum drm_output_propose_state_mode mode;

	assert(output);

	drm_debug(b, "\t[repaint] preparing state for output %s (%lu)\n",
		  output_base->name, (unsigned long) output_base->id);

	drm_debug(b, "\t[repaint] trying to reuse prior %s\n",
		  drm_propose_state_mode_to_string(output->state_cur->mode));

	mode = DRM_OUTPUT_PROPOSE_STATE_REUSE | output->state_cur->mode;
	state = drm_output_propose_state(output_base, pending_state, mode);
	if (!state)
		drm_debug(b, "\t[repaint] could not reuse prior state\n");

	if (!state && !device->disable_client_buffer_scanout &&
	    !output_base->disable_planes && !output->is_virtual && b->gbm &&
	    !output_base->mirroring) {
		drm_debug(b, "\t[repaint] trying planes-only build state\n");
		mode = DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY;
		state = drm_output_propose_state(output_base, pending_state, mode);
		if (!state) {
			drm_debug(b, "\t[repaint] could not build planes-only "
				     "state, trying mixed\n");
			mode = DRM_OUTPUT_PROPOSE_STATE_MIXED;
			state = drm_output_propose_state(output_base,
							 pending_state,
							 mode);
		}
	} else if (!state) {
		drm_debug(b, "\t[state] no hardware plane support\n");
	}

	/* We can enter this block in two situations:
	 * 1. If we didn't enter the last block (for some reason we can't use planes)
	 * 2. If we entered but both the planes-only and the mixed modes didn't work */
	if (!state) {
		if (output_base->disable_planes)
			mode = DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY;
		else
			mode = DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR;

		drm_debug(b, "\t[repaint] could not build state with planes, "
			     "trying %s\n",
			     (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) ?
			     "renderer-only" : "renderer-and-cursor");

		state = drm_output_propose_state(output_base, pending_state,
						 mode);
		/* If renderer/renderer-and-cursor mode failed and we are in a
		 * writeback screenshot, let's abort the writeback screenshot
		 * and try again. */
		if (!state && drm_output_get_writeback_state(output) != DRM_OUTPUT_WB_SCREENSHOT_OFF) {
			drm_debug(b, "\t[repaint] could not build %s "
				     "state, trying without writeback setup\n",
				     (mode == DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY) ?
				     "renderer-only" : "renderer-and-cursor");
			drm_writeback_fail_screenshot(wb_state, "drm: failed to propose state");
			state = drm_output_propose_state(output_base, pending_state,
							 mode);
		}
	}

	assert(state);
	assert(state->planes_enabled == !output_base->disable_planes);

	output->force_rebuild_state = false;

	drm_debug(b, "\t[repaint] Using %s composition\n",
		  drm_propose_state_mode_to_string(mode));

	wl_list_for_each(pnode, &output->base.paint_node_z_order_list,
			 z_order_link) {
		struct weston_surface *surface = pnode->surface;
		struct drm_plane *target_plane = NULL;
		struct drm_plane_handle *target_handle = NULL;

		/* Update dmabuf-feedback if needed */
		if (surface->dmabuf_feedback)
			dmabuf_feedback_maybe_update(device, pnode);

		/* Test whether this buffer can ever go into a plane:
		 * non-shm, or small enough to be a cursor.  */
		surface->keep_buffer = false;
		if (weston_paint_node_has_valid_buffer(pnode)) {
			struct weston_buffer *buffer =
				surface->buffer_ref.buffer;
			if (buffer->type == WESTON_BUFFER_DMABUF ||
			    buffer->type == WESTON_BUFFER_RENDERER_OPAQUE)
				surface->keep_buffer = true;
			else if (buffer->type == WESTON_BUFFER_SHM &&
				 (surface->width <= device->cursor_width &&
				  surface->height <= device->cursor_height))
				surface->keep_buffer = true;
		}

		/* This is a bit unpleasant, but lacking a temporary place to
		 * hang a plane off the paint node, we have to do a nested walk.
		 * Our first-order iteration has to be planes rather than
		 * nodes, because otherwise we won't reset nodes which were
		 * previously on planes to being on the primary plane. */
		wl_list_for_each(plane_state, &state->plane_list, link) {
			if (plane_state->paint_node == pnode) {
				plane_state->paint_node = NULL;
				target_handle = plane_state->handle;
				break;
			}
		}

		if (target_handle)
			target_plane = target_handle->plane;

		if (target_plane) {
			drm_debug(b, "\t[repaint] paint node %s on %s plane %lu\n",
				  pnode->internal_name,
				  drm_output_get_handle_type_name(target_handle),
				  (unsigned long) target_plane->plane_id);
			weston_paint_node_move_to_plane(pnode, &target_plane->base);
		} else {
			drm_debug(b, "\t[repaint] paint node %s using renderer "
				     "composition\n", pnode->internal_name);
			weston_paint_node_move_to_plane(pnode, primary);
			pnode->need_hole = false;
		}

		if (!target_plane ||
		    target_plane->type == WDRM_PLANE_TYPE_CURSOR) {
			/* cursor plane & renderer involve a copy */
			pnode->psf_flags = 0;
		} else {
			/* All other planes are a direct scanout of a
			 * single client buffer.
			 */
			pnode->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
		}
	}

	if (drm_output_get_writeback_state(output) == DRM_OUTPUT_WB_SCREENSHOT_PREPARE_COMMIT)
		drm_writeback_reference_planes(wb_state, &state->plane_list);
}
