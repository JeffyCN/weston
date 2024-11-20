/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012-2018, 2021 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
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

#include <libweston/libweston.h>
#include <libweston/commit-timing.h>
#include <libweston/fifo.h>
#include "libweston-internal.h"

#include "backend.h"
#include "color-representation.h"
#include "pixel-formats.h"
#include "shared/fd-util.h"
#include "shared/timespec-util.h"
#include "shared/weston-assert.h"
#include "shared/xalloc.h"
#include <sys/timerfd.h>
#include "timeline.h"
#include "weston-trace.h"

/* Deferred content updates:
 *
 * In the absence of readiness constraints, weston will apply content updates
 * as they're delivered from clients via wl_surface.commit() requests.
 *
 * When readiness constraints exist, we must instead store the content update
 * for later application. This is done by creating a weston_content_update,
 * which is a wrapper for a weston_surface_state to be applied to a
 * weston_surface.
 *
 * The weston_content_update is then added to a weston_transaction, which is
 * an atomic group of content updates that can only be applied when the
 * entire set is ready.
 *
 * The transactions themselves are stored within weston_transaction_queues,
 * which contain an ordered sequence of transactions, each of which depends
 * on the one before it in the list. Only the head transaction can be
 * considered for application.
 *
 * The compositor holds a list of these queues, and will consider the head
 * of each list any time transactions are applied (which must happen
 * immediately before we "latch" content for a repaint, and some time
 * after that repaint clears but before outputs are selected for the
 * next repaint)
 */
struct weston_transaction_queue {
	struct wl_list link; /* weston_compositor::transaction_queue_list */
	struct wl_list transaction_list; /* weston_transaction::link */
};

struct weston_transaction {
	struct weston_transaction_queue *queue;
	uint64_t flow_id;
	struct wl_list link; /* weston_transaction_queue::transaction_list */
	struct wl_list content_update_list; /* weston_content_update::link */
};

struct weston_content_update {
	struct weston_transaction *transaction;
	struct weston_surface *surface;
	struct weston_surface_state state;
	struct wl_listener surface_destroy_listener;
	struct wl_list link; /* weston_transaction::content_update_list */
};

static void
weston_surface_apply(struct weston_surface *surface,
		     struct weston_surface_state *state);

uint32_t
weston_surface_visibility_mask(struct weston_surface *surface)
{
	struct weston_view *view;
	uint32_t visibility_mask;

	/* Assume the surface is visible on any output without up to date
	 * visibility information.
	 */
	visibility_mask = surface->output_visibility_dirty_mask;

	/* We can skip the loop if it's dirty everywhere */
	if (visibility_mask == surface->output_mask)
		return visibility_mask;

	wl_list_for_each(view, &surface->views, surface_link)
		visibility_mask |= view->output_visibility_mask;

	return visibility_mask;
}

static void
weston_surface_dirty_paint_nodes(struct weston_surface *surface,
				 enum weston_paint_node_status status)
{
	struct weston_paint_node *node;

	wl_list_for_each(node, &surface->paint_node_list, surface_link) {
		assert(node->surface == surface);

		node->status |= status;
	}
}

void
weston_surface_state_init(struct weston_surface *surface,
			  struct weston_surface_state *state)
{
	state->flow_id = 0;
	state->status = WESTON_SURFACE_CLEAN;
	state->buffer_ref.buffer = NULL;
	state->buf_offset = weston_coord_surface(0, 0, surface);

	pixman_region32_init(&state->damage_surface);
	pixman_region32_init(&state->damage_buffer);
	pixman_region32_init(&state->opaque);
	region_init_infinite(&state->input);

	wl_list_init(&state->frame_callback_list);
	wl_list_init(&state->feedback_list);

	state->buffer_viewport.buffer.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	state->buffer_viewport.buffer.scale = 1;
	state->buffer_viewport.buffer.src_width = wl_fixed_from_int(-1);
	state->buffer_viewport.surface.width = -1;

	state->acquire_fence_fd = -1;

	state->desired_protection = WESTON_HDCP_DISABLE;
	state->protection_mode = WESTON_SURFACE_PROTECTION_MODE_RELAXED;

	state->color_profile = NULL;
	state->render_intent = NULL;

	weston_reset_color_representation(&state->color_representation);

	state->fifo_barrier = false;
	state->fifo_wait = false;

	state->update_time.valid = false;
	state->update_time.satisfied = false;
	state->update_time.time.tv_sec = 0;
	state->update_time.time.tv_nsec = 0;
}

void
weston_surface_state_fini(struct weston_surface_state *state)
{
	struct wl_resource *cb, *next;

	state->flow_id = 0;
	wl_resource_for_each_safe(cb, next, &state->frame_callback_list)
		wl_resource_destroy(cb);

	weston_presentation_feedback_discard_list(&state->feedback_list);

	pixman_region32_fini(&state->input);
	pixman_region32_fini(&state->opaque);
	pixman_region32_fini(&state->damage_surface);
	pixman_region32_fini(&state->damage_buffer);

	weston_buffer_reference(&state->buffer_ref, NULL,
				BUFFER_WILL_NOT_BE_ACCESSED);

	fd_clear(&state->acquire_fence_fd);
	weston_buffer_release_reference(&state->buffer_release_ref, NULL);

	weston_color_profile_unref(state->color_profile);
	state->color_profile = NULL;
	state->render_intent = NULL;
}

static enum weston_surface_status
weston_surface_attach(struct weston_surface *surface,
		      struct weston_surface_state *state,
		      enum weston_surface_status status)
{
	WESTON_TRACE_FUNC_FLOW(&surface->flow_id);
	struct weston_buffer *buffer = state->buffer_ref.buffer;
	struct weston_buffer *old_buffer = surface->buffer_ref.buffer;
	enum weston_paint_node_status pnode_changes = WESTON_PAINT_NODE_CLEAN;

	if (!buffer) {
		if (weston_surface_is_mapped(surface)) {
			weston_surface_unmap(surface);
			/* This is the unmapping commit */
			surface->is_unmapping = true;
			status |= WESTON_SURFACE_DIRTY_BUFFER;
			status |= WESTON_SURFACE_DIRTY_BUFFER_PARAMS;
			status |= WESTON_SURFACE_DIRTY_SIZE;
		}

		weston_buffer_reference(&surface->buffer_ref, NULL,
					BUFFER_WILL_NOT_BE_ACCESSED);

		surface->width_from_buffer = 0;
		surface->height_from_buffer = 0;

		return status;
	}

	pnode_changes |= WESTON_PAINT_NODE_BUFFER_DIRTY;

	/* Recalculate the surface size if the buffer dimensions or the
	 * surface transforms (viewport, rotation/mirror, scale) have
	 * changed. */
	if (!old_buffer ||
	    buffer->width != old_buffer->width ||
	    buffer->height != old_buffer->height ||
	    (status & WESTON_SURFACE_DIRTY_SIZE)) {
		struct weston_buffer_viewport *vp = &state->buffer_viewport;
		int32_t old_width = surface->width_from_buffer;
		int32_t old_height = surface->height_from_buffer;
		bool size_ok;

		size_ok = convert_buffer_size_by_transform_scale(&surface->width_from_buffer,
								 &surface->height_from_buffer,
								 buffer, vp);
		weston_assert_true(surface->compositor, size_ok);

		if (surface->width_from_buffer != old_width ||
		    surface->height_from_buffer != old_height) {
			status |= WESTON_SURFACE_DIRTY_SIZE;
		}
	}

	/* The buffer was destroyed (!old_buffer->resource), so set the
	 * BUFFER_PARAMS_DIRTY bit.
	 *
	 * This is because backends can opportunistically reuse plane state
	 * if only the buffer has changed, but a buffer changing from invalid
	 * to valid needs action.
	 */
	if (old_buffer && !old_buffer->resource)
		pnode_changes |= WESTON_PAINT_NODE_BUFFER_PARAMS_DIRTY;

	if (!old_buffer ||
	    buffer->pixel_format != old_buffer->pixel_format ||
	    buffer->format_modifier != old_buffer->format_modifier) {
		surface->is_opaque = pixel_format_is_opaque(buffer->pixel_format);
		status |= WESTON_SURFACE_DIRTY_BUFFER_PARAMS;
		pnode_changes |= WESTON_PAINT_NODE_BUFFER_PARAMS_DIRTY;
	}

	status |= WESTON_SURFACE_DIRTY_BUFFER;
	weston_surface_dirty_paint_nodes(surface, pnode_changes);

	weston_buffer_reference(&surface->buffer_ref, buffer,
				BUFFER_MAY_BE_ACCESSED);

	/* Early attach to reduce repaint latency */
	if (!wl_list_empty(&surface->paint_node_list)) {
		struct weston_paint_node *pnode =
			wl_container_of(surface->paint_node_list.next,
					pnode, surface_link);
		surface->compositor->renderer->attach(pnode);
	}

	return status;
}

static void
weston_surface_apply_subsurface_order(struct weston_surface *surface)
{
	struct weston_compositor *comp = surface->compositor;
	struct weston_subsurface *sub;
	struct weston_view *view;

	wl_list_for_each_reverse(sub, &surface->subsurface_list_pending,
				 parent_link_pending) {
		wl_list_remove(&sub->parent_link);
		wl_list_insert(&surface->subsurface_list, &sub->parent_link);
		wl_list_for_each(view, &sub->surface->views, surface_link)
			weston_view_geometry_dirty(view);
	}
	weston_assert_true(comp, comp->view_list_needs_rebuild);
}

/* Translate pending damage in buffer co-ordinates to surface
 * co-ordinates and union it with a pixman_region32_t.
 * This should only be called after the buffer is attached.
 */
static void
apply_damage_buffer(pixman_region32_t *dest,
		    struct weston_surface *surface,
		    struct weston_surface_state *state)
{
	struct weston_buffer *buffer = surface->buffer_ref.buffer;
	pixman_region32_t buffer_damage;

	/* wl_surface.damage_buffer needs to be clipped to the buffer,
	 * translated into surface co-ordinates and unioned with
	 * any other surface damage.
	 * None of this makes sense if there is no buffer though.
	 */
	if (!buffer || !pixman_region32_not_empty(&state->damage_buffer))
		return;

	pixman_region32_intersect_rect(&state->damage_buffer,
				       &state->damage_buffer,
				       0, 0,
				       buffer->width, buffer->height);
	pixman_region32_init(&buffer_damage);
	weston_matrix_transform_region(&buffer_damage,
				       &surface->buffer_to_surface_matrix,
				       &state->damage_buffer);
	pixman_region32_union(dest, dest, &buffer_damage);
	pixman_region32_fini(&buffer_damage);
}

static void
weston_surface_set_desired_protection(struct weston_surface *surface,
				      enum weston_hdcp_protection protection)
{
	struct weston_paint_node *pnode;

	if (surface->desired_protection == protection)
		return;

	surface->desired_protection = protection;

	wl_list_for_each(pnode, &surface->paint_node_list, surface_link) {
		if (pixman_region32_not_empty(&pnode->visible))
			weston_output_damage(pnode->output);
	}
}

static void
weston_surface_set_protection_mode(struct weston_surface *surface,
				   enum weston_surface_protection_mode p_mode)
{
	struct content_protection *cp = surface->compositor->content_protection;
	struct protected_surface *psurface;

	surface->protection_mode = p_mode;
	wl_list_for_each(psurface, &cp->protected_list, link) {
		if (!psurface || psurface->surface != surface)
			continue;
		weston_protected_surface_send_event(psurface,
						    surface->current_protection);
	}
}

static bool
weston_surface_status_invalidates_visibility(enum weston_surface_status status)
{
	return status & WESTON_SURFACE_DIRTY_SIZE ||
	       status & WESTON_SURFACE_DIRTY_POS ||
	       status & WESTON_SURFACE_DIRTY_BUFFER_PARAMS ||
	       status & WESTON_SURFACE_DIRTY_SUBSURFACE_CONFIG;
}

static enum weston_surface_status
weston_surface_apply_state(struct weston_surface *surface,
			   struct weston_surface_state *state)
{
	WESTON_TRACE_FUNC_FLOW(&state->flow_id);
	struct weston_view *view;
	pixman_region32_t opaque;
	enum weston_surface_status status = state->status;

	assert(!surface->compositor->latched);

	surface->flow_id = state->flow_id;
	state->flow_id = 0;

	/* wl_surface.set_buffer_transform */
	/* wl_surface.set_buffer_scale */
	/* wp_viewport.set_source */
	/* wp_viewport.set_destination */
	surface->buffer_viewport = state->buffer_viewport;

	/* wp_presentation.feedback */
	weston_presentation_feedback_discard_list(&surface->feedback_list);

	/* wp_color_representation_v1.set_alpha_mode */
	/* wp_color_representation_v1.set_coefficients_and_range */
	/* wp_color_representation_v1.set_chroma_location */
	surface->color_representation = state->color_representation;

	/* wl_surface.attach */
	if (status & WESTON_SURFACE_DIRTY_BUFFER) {
		/* zwp_surface_synchronization_v1.set_acquire_fence */
		fd_move(&surface->acquire_fence_fd,
			&state->acquire_fence_fd);
		/* zwp_surface_synchronization_v1.get_release */
		weston_buffer_release_move(&surface->buffer_release_ref,
					   &state->buffer_release_ref);

		status |= weston_surface_attach(surface, state, status);
	} else if (status & WESTON_SURFACE_DIRTY_SIZE && surface->buffer_ref.buffer) {
		bool size_ok;

		size_ok = convert_buffer_size_by_transform_scale(&surface->width_from_buffer,
								 &surface->height_from_buffer,
								 surface->buffer_ref.buffer,
								 &state->buffer_viewport);
		weston_assert_true(surface->compositor, size_ok);
	}
	weston_buffer_reference(&state->buffer_ref, NULL,
				BUFFER_WILL_NOT_BE_ACCESSED);
	assert(state->acquire_fence_fd == -1);
	assert(state->buffer_release_ref.buffer_release == NULL);

	if (status & WESTON_SURFACE_DIRTY_SIZE) {
		weston_surface_build_buffer_matrix(surface,
						   &surface->surface_to_buffer_matrix);
		weston_matrix_invert(&surface->buffer_to_surface_matrix,
				     &surface->surface_to_buffer_matrix);
		weston_surface_dirty_paint_nodes(surface,
						 WESTON_PAINT_NODE_VIEW_DIRTY);
		weston_surface_update_size(surface);
	}

	if ((status & (WESTON_SURFACE_DIRTY_BUFFER | WESTON_SURFACE_DIRTY_SIZE |
		       WESTON_SURFACE_DIRTY_POS)) &&
	     surface->committed)
		surface->committed(surface, state->buf_offset);

	state->buf_offset = weston_coord_surface(0, 0, surface);

	/* wl_surface.damage and wl_surface.damage_buffer; only valid
	 * in the same cycle as wl_surface.commit */
	if (status & WESTON_SURFACE_DIRTY_BUFFER) {
		TL_POINT(surface->compositor, TLP_CORE_COMMIT_DAMAGE,
			TLP_SURFACE(surface), TLP_END);

		pixman_region32_union(&surface->damage, &surface->damage,
				      &state->damage_surface);

		apply_damage_buffer(&surface->damage, surface, state);
		surface->frame_commit_counter++;

		pixman_region32_intersect_rect(&surface->damage,
					       &surface->damage,
					       0, 0,
					       surface->width, surface->height);
	}
	pixman_region32_clear(&state->damage_buffer);
	pixman_region32_clear(&state->damage_surface);

	/* wl_surface.set_opaque_region */
	if (status & (WESTON_SURFACE_DIRTY_SIZE |
		      WESTON_SURFACE_DIRTY_BUFFER_PARAMS)) {
		pixman_region32_init(&opaque);
		pixman_region32_intersect_rect(&opaque, &state->opaque,
					       0, 0,
					       surface->width, surface->height);

		if (!pixman_region32_equal(&opaque, &surface->opaque)) {
			pixman_region32_copy(&surface->opaque, &opaque);
			wl_list_for_each(view, &surface->views, surface_link) {
				weston_view_geometry_dirty_internal(view);
				weston_view_update_transform(view);
			}
		}

		pixman_region32_fini(&opaque);
	}

	/* wl_surface.set_input_region */
	if (status & (WESTON_SURFACE_DIRTY_SIZE | WESTON_SURFACE_DIRTY_INPUT)) {
		pixman_region32_intersect_rect(&surface->input, &state->input,
					       0, 0,
					       surface->width, surface->height);
	}

	/* wl_surface.frame */
	wl_list_insert_list(&surface->frame_callback_list,
			    &state->frame_callback_list);
	wl_list_init(&state->frame_callback_list);

	/* XXX:
	 * What should happen with a feedback request, if there
	 * is no wl_buffer attached for this commit?
	 */

	/* presentation.feedback */
	wl_list_insert_list(&surface->feedback_list,
			    &state->feedback_list);
	wl_list_init(&state->feedback_list);

	/* weston_protected_surface.enforced/relaxed */
	if (surface->protection_mode != state->protection_mode)
		weston_surface_set_protection_mode(surface,
						   state->protection_mode);

	/* weston_protected_surface.set_type */
	weston_surface_set_desired_protection(surface, state->desired_protection);

	/* color_management_surface_v1_interface.set_image_description or
	 * color_management_surface_v1_interface.unset_image_description */
	weston_surface_set_color_profile(surface, state->color_profile,
					 state->render_intent);

	wl_signal_emit(&surface->commit_signal, surface);

	if (status & WESTON_SURFACE_DIRTY_SUBSURFACE_CONFIG)
		weston_surface_apply_subsurface_order(surface);

	/* Surface is now quiescent */
	surface->is_unmapping = false;
	surface->is_mapping = false;

	if (state->fifo_barrier)
		weston_fifo_surface_set_barrier(surface);
	state->fifo_barrier = false;

	if (weston_surface_status_invalidates_visibility(status))
		surface->output_visibility_dirty_mask |= surface->output_mask;

	/* If we have a target time and a driving output, we can try to use
	 * VRR to move the display time to hit it. If a repaint is already
	 * scheduled, then its exact time was used to satisfy our time
	 * constraint, so don't mess with it.
	 *
	 * We also need to make sure that if a bunch of updates become ready
	 * all at once, that we keep forced_present monotonic, so nothing
	 * is presented early.
	 */
	if (state->update_time.valid && surface->output &&
	    surface->output->repaint_status != REPAINT_SCHEDULED) {
		if (!surface->output->forced_present.valid ||
		    timespec_sub_to_nsec(&state->update_time.time,
					 &surface->output->forced_present.time) > 0) {
			surface->output->forced_present = state->update_time;
		}
	}

	weston_commit_timing_clear_target(&state->update_time);

	state->status = WESTON_SURFACE_CLEAN;

	return status;
}

static void
weston_subsurface_parent_apply(struct weston_subsurface *sub)
{
	struct weston_view *view;

	if (sub->position.changed) {
		wl_list_for_each(view, &sub->surface->views, surface_link)
			weston_view_set_rel_position(view,
						     sub->position.offset);

		sub->position.changed = false;
	}

	if (sub->effectively_synchronized)
		weston_surface_apply(sub->surface, &sub->cached);
}


/**
 * \param surface  The surface to be repainted
 * \param status The weston_surface_status last applied
 *
 * Marks the output(s) that the surface is shown on as needing to be
 * repainted. Tries to avoid repaints on occluded surfaces when
 * possible by checking surface status dirty bits.
 *
 * See weston_output_schedule_repaint().
 */
static void
weston_surface_schedule_repaint(struct weston_surface *surface,
				enum weston_surface_status status)
{
	struct weston_output *output;
	uint32_t visible_mask;

	if (surface->output && surface->fifo_barrier)
		weston_output_schedule_repaint(surface->output);

	if (status == WESTON_SURFACE_CLEAN)
		return;

	visible_mask = weston_surface_visibility_mask(surface);
	wl_list_for_each(output, &surface->compositor->output_list, link) {
		if (visible_mask & (1u << output->id))
			weston_output_schedule_repaint(output);
	}
}

static void
weston_surface_apply(struct weston_surface *surface,
		     struct weston_surface_state *state)
{
	WESTON_TRACE_FUNC_FLOW(&state->flow_id);
	struct weston_subsurface *sub;
	enum weston_surface_status status;

	status = weston_surface_apply_state(surface, state);

	weston_surface_schedule_repaint(surface, status);

	wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
		if (sub->surface != surface)
			weston_subsurface_parent_apply(sub);
	}
}

static void
weston_surface_state_merge_from(struct weston_surface_state *dst,
				struct weston_surface_state *src,
				struct weston_surface *surface)
{
	WESTON_TRACE_FUNC_FLOW(&dst->flow_id);
	src->flow_id = 0;


	/*
	 * If this commit would cause the surface to move by the
	 * attach(dx, dy) parameters, the old damage region must be
	 * translated to correspond to the new surface coordinate system
	 * origin.
	 */
	if (surface->pending.status & WESTON_SURFACE_DIRTY_POS) {
		pixman_region32_translate(&dst->damage_surface,
					  -src->buf_offset.c.x,
					  -src->buf_offset.c.y);
	}
	pixman_region32_union(&dst->damage_surface,
			      &dst->damage_surface,
			      &src->damage_surface);
	pixman_region32_clear(&src->damage_surface);

	pixman_region32_union(&dst->damage_buffer,
			      &dst->damage_buffer,
			      &src->damage_buffer);
	pixman_region32_clear(&src->damage_buffer);

	dst->render_intent = src->render_intent;
	weston_color_profile_unref(dst->color_profile);
	dst->color_profile =
		weston_color_profile_ref(src->color_profile);

	weston_presentation_feedback_discard_list(&dst->feedback_list);

	if (src->status & WESTON_SURFACE_DIRTY_BUFFER) {
		weston_buffer_reference(&dst->buffer_ref,
					src->buffer_ref.buffer,
					src->buffer_ref.buffer ?
						BUFFER_MAY_BE_ACCESSED :
						BUFFER_WILL_NOT_BE_ACCESSED);
		/* zwp_surface_synchronization_v1.set_acquire_fence */
		fd_move(&dst->acquire_fence_fd,
			&src->acquire_fence_fd);
		/* zwp_surface_synchronization_v1.get_release */
		weston_buffer_release_move(&dst->buffer_release_ref,
					   &src->buffer_release_ref);
	}
	dst->desired_protection = src->desired_protection;
	dst->protection_mode = src->protection_mode;
	assert(src->acquire_fence_fd == -1);
	assert(src->buffer_release_ref.buffer_release == NULL);
	dst->buf_offset = weston_coord_surface_add(dst->buf_offset,
						   src->buf_offset);

	dst->buffer_viewport.buffer = src->buffer_viewport.buffer;
	dst->buffer_viewport.surface = src->buffer_viewport.surface;

	dst->color_representation = src->color_representation;

	weston_buffer_reference(&src->buffer_ref,
				NULL, BUFFER_WILL_NOT_BE_ACCESSED);

	src->buf_offset = weston_coord_surface(0, 0, surface);

	pixman_region32_copy(&dst->opaque, &src->opaque);

	pixman_region32_copy(&dst->input, &src->input);

	wl_list_insert_list(&dst->frame_callback_list,
			    &src->frame_callback_list);
	wl_list_init(&src->frame_callback_list);

	wl_list_insert_list(&dst->feedback_list,
			    &src->feedback_list);
	wl_list_init(&src->feedback_list);

	dst->fifo_barrier = src->fifo_barrier;
	src->fifo_barrier = false;
	dst->fifo_wait = src->fifo_wait;
	src->fifo_wait = false;

	dst->update_time = src->update_time;
	weston_commit_timing_clear_target(&src->update_time);

	dst->status |= src->status;
	src->status = WESTON_SURFACE_CLEAN;
}

static struct weston_transaction_queue *
weston_surface_find_parent_transaction_queue(struct weston_compositor *comp,
					     struct weston_surface *surface)
{
	struct weston_transaction_queue *tq;
	struct weston_transaction *tr;
	struct weston_content_update *cu;

	wl_list_for_each(tq, &comp->transaction_queue_list, link) {
		wl_list_for_each(tr, &tq->transaction_list, link)
			wl_list_for_each(cu, &tr->content_update_list, link) {
				if (cu->surface == surface)
					return tq;
			}
	}

	return NULL;
}

static void
weston_content_update_fini(struct weston_content_update *cu)
{
	wl_list_remove(&cu->link);
	weston_surface_state_fini(&cu->state);
	wl_list_remove(&cu->surface_destroy_listener.link);
	free(cu);
}

static void
content_update_surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_content_update *cu = container_of(listener,
							struct weston_content_update,
							surface_destroy_listener);
	struct weston_transaction *tr = cu->transaction;
	struct weston_transaction_queue *tq = tr->queue;

	weston_content_update_fini(cu);

	/* If we were the last update in the transaction, remove it */
	if (wl_list_empty(&tr->content_update_list)) {
		wl_list_remove(&tr->link);
		free(tr);
	}

	/* If removing a transaction emptied a list, remove that too */
	if (wl_list_empty(&tq->transaction_list)) {
		wl_list_remove(&tq->link);
		free(tq);
	}
}

static void
weston_transaction_add_content_update(struct weston_transaction *tr,
				      struct weston_surface *surface,
				      struct weston_surface_state *state)
{
	struct weston_content_update *cu;

	cu = xzalloc(sizeof *cu);
	cu->transaction = tr;
	/* Since surfaces don't maintain a list of transactions they're on, we
	 * can either have the surface destructor walk all transaction lists
	 * to remove any content updates for a destroyed surface, or hook
	 * the surface_destroy signal.
	 *
	 * The latter is a little easier, so set that up.
	 */
	cu->surface_destroy_listener.notify = content_update_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &cu->surface_destroy_listener);
	cu->surface = surface;
	weston_surface_state_init(surface, &cu->state);

	cu->state.flow_id = state->flow_id;
	weston_surface_state_merge_from(&cu->state, state, surface);

	wl_list_insert(&tr->content_update_list, &cu->link);
}

static void
weston_surface_create_transaction(struct weston_compositor *comp,
				  struct weston_surface *surface,
				  struct weston_surface_state *state)
{
	uint64_t transaction_flow_id = 0;
	WESTON_TRACE_FUNC_FLOW(&transaction_flow_id);

	struct weston_transaction *tr;
	struct weston_transaction_queue *parent;
	bool need_schedule = false;

	tr = xzalloc(sizeof *tr);
	tr->flow_id = transaction_flow_id;
	wl_list_init(&tr->content_update_list);

	weston_transaction_add_content_update(tr, surface, state);

	/* Figure out if we need to be blocked behind an existing transaction */
	parent = weston_surface_find_parent_transaction_queue(comp, surface);
	if (!parent) {
		/* We weren't blocked by any existing transactions, so set up
		 * a new list so content updates for this surface can block
		 * behind us in the future
		 */
		parent = xzalloc(sizeof *parent);
		wl_list_init(&parent->transaction_list);
		wl_list_insert(&comp->transaction_queue_list, &parent->link);
		need_schedule = true;
	}
	tr->queue = parent;
	wl_list_insert(parent->transaction_list.prev, &tr->link);

	if (need_schedule)
		weston_repaint_timer_arm(comp);
}

static bool
weston_surface_state_ready(struct weston_surface *surface,
			   struct weston_surface_state *state)
{
	if (!weston_fifo_surface_state_ready(surface, state))
		return false;

	if (!weston_commit_timing_surface_state_ready(surface, state))
		return false;

	return true;
}

void
weston_surface_commit(struct weston_surface *surface)
{
	WESTON_TRACE_FUNC_FLOW(&surface->pending.flow_id);
	struct weston_compositor *comp = surface->compositor;
	struct weston_subsurface *sub = weston_surface_to_subsurface(surface);
	struct weston_surface_state *state = &surface->pending;
	struct weston_transaction_queue *tq;

	if (sub) {
		weston_surface_state_merge_from(&sub->cached,
						state,
						surface);
		if (sub->effectively_synchronized)
			return;

		state = &sub->cached;
	}

	/* Check if this surface is a member of a transaction list already.
	 * If it is, we're not ready to apply this state, so we'll have
	 * to make a new transaction and wait until we are.
	 *
	 * For now, we don't have a way to combine multiple content updates
	 * in a single transaction, so these effectively become per surface
	 * update streams.
	 */
	tq = weston_surface_find_parent_transaction_queue(comp, surface);
	if (tq || !weston_surface_state_ready(surface, state)) {
		weston_surface_create_transaction(comp, surface, state);
		return;
	}

	weston_surface_apply(surface, state);
}

/** Recursively update effectively_synchronized state for a subsurface tree
 *
 * \param sub Subsurface to start from
 *
 * From wayland.xml :
 *   Even if a sub-surface is in desynchronized mode, it will behave as
 *   in synchronized mode, if its parent surface behaves as in
 *   synchronized mode. This rule is applied recursively throughout the
 *   tree of surfaces.
 *
 * In Weston, we call a surface "effectively synchronized" if it is either
 * synchronized, or is forced to "behave as in synchronized mode" by a
 * parent surface that is effectively synchronized.
 *
 * Calling weston_subsurface_update_effectively_synchronized on a subsurface
 * will update the tree of subsurfaces to have accurate
 * effectively_synchronized state below that point, by walking all descendants
 * and combining their state with their immediate parent's state.
 *
 * Since every subsurface starts off synchronized, they also start off
 * effectively synchronized, so we only need to call this function in response
 * to synchronization changes from protocol requests (set_sync, set_desync) to
 * keep the subsurface tree state up to date.
 */
static void
weston_subsurface_update_effectively_synchronized(struct weston_subsurface *sub)
{
	bool parent_e_sync = false;
	struct weston_subsurface *child;
	struct weston_surface *surf = sub->surface;
	WESTON_TRACE_FUNC_FLOW(&surf->flow_id);

	if (sub->parent) {
		struct weston_subsurface *parent;

		parent = weston_surface_to_subsurface(sub->parent);
		if (parent)
			parent_e_sync = parent->effectively_synchronized;
	}

	/* This subsurface will be effectively synchronized if it is
	 * explicitly synchronized, or if a parent surface is effectively
	 * synchronized.
	 *
	 * Since we're called for every protocol driven change, and update
	 * recursively at that point, we know that the immediate parent
	 * state is always up to date, so we only have to test that here.
	 */
	sub->effectively_synchronized = parent_e_sync || sub->synchronized;

	wl_list_for_each(child, &surf->subsurface_list, parent_link) {
		if (child->surface == surf)
			continue;

		weston_subsurface_update_effectively_synchronized(child);
	}
}

void
weston_subsurface_set_synchronized(struct weston_subsurface *sub, bool sync)
{
	WESTON_TRACE_FUNC_FLOW(&sub->surface->flow_id);
	bool old_e_sync = sub->effectively_synchronized;

	if (sub->synchronized == sync)
		return;

	sub->synchronized = sync;

	weston_subsurface_update_effectively_synchronized(sub);

	/* If sub became effectively desynchronized, flush */
	if (old_e_sync && !sub->effectively_synchronized)
		weston_surface_apply(sub->surface, &sub->cached);
}

static void
apply_transaction(struct weston_transaction *transaction)
{
	WESTON_TRACE_FUNC_FLOW(&transaction->flow_id);
	struct weston_content_update *cu, *tmp;

	wl_list_remove(&transaction->link);

	wl_list_for_each_safe(cu, tmp, &transaction->content_update_list, link) {
		weston_surface_apply(cu->surface, &cu->state);
		weston_content_update_fini(cu);
	}

	free(transaction);
}

static bool
transaction_ready(struct weston_transaction *transaction)
{
	WESTON_TRACE_FUNC_FLOW(&transaction->flow_id);
	struct weston_content_update *cu;

	/* Every content update within the transaction must be ready
	 * for the transaction to be applied.
	 */
	wl_list_for_each(cu, &transaction->content_update_list, link)
		if (!weston_surface_state_ready(cu->surface, &cu->state))
			return false;

	return true;
}

void
weston_compositor_apply_transactions(struct weston_compositor *compositor)
{
	WESTON_TRACE_FUNC();
	struct weston_transaction_queue *tq, *tq_tmp;
	struct weston_transaction *tr, *tr_tmp;

	assert(!compositor->latched);

	/* The compositor has a list of transaction queues. These queues are
	 * independent streams of transactions, and the head of a queue blocks
	 * every transaction after it. We must consider (only) each queue head.
	 */
	wl_list_for_each_safe(tq, tq_tmp, &compositor->transaction_queue_list, link) {
		/* Walk this queue and greedily consume any that are ready.
		 * As soon as one is not, we're done with the list, as all
		 * further members are blocked.
		 */
		wl_list_for_each_safe(tr, tr_tmp, &tq->transaction_list, link) {
			if (!transaction_ready(tr))
				break;

			apply_transaction(tr);
		}

		if (wl_list_empty(&tq->transaction_list)) {
			wl_list_remove(&tq->link);
			free(tq);
		}
	}
}

/** Update output nearest commit-timing target times
 *
 * \param compositor weston_compositor
 *
 * Updates the list of upcoming deferred content updates so every output
 * with a deferred update has a stored copy of the nearest ready time.
 */
void
weston_commit_timing_update_output_targets(struct weston_compositor *compositor)
{
	struct weston_transaction_queue *tq;
	struct weston_output *output;

	weston_commit_timing_clear_target(&compositor->requested_repaint_fallback);
	wl_list_for_each(output, &compositor->output_list, link)
		weston_commit_timing_clear_target(&output->requested_present);

	wl_list_for_each(tq, &compositor->transaction_queue_list, link) {
		/* First transaction only - it blocks the rest */
		struct weston_transaction *tr = wl_container_of(tq->transaction_list.next, tr, link);
		struct weston_content_update *cu;
		struct weston_commit_timing_target *target;

		wl_list_for_each(cu, &tr->content_update_list, link) {
			if (!cu->state.update_time.valid)
				continue;
			if (cu->state.update_time.satisfied)
				continue;

			if (cu->surface->output)
				target = &cu->surface->output->requested_present;
			else
				target = &cu->surface->compositor->requested_repaint_fallback;

			if (!target->valid ||
			    timespec_sub_to_nsec(&target->time,
						 &cu->state.update_time.time) > 0)
				*target = cu->state.update_time;
		}
	}
}
