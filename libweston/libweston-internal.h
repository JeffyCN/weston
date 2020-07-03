/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2017, 2018 General Electric Company
 * Copyright © 2012, 2017-2019, 2021 Collabora, Ltd.
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

#ifndef LIBWESTON_INTERNAL_H
#define LIBWESTON_INTERNAL_H

/*
 * This is the internal (private) part of libweston. All symbols found here
 * are, and should be only (with a few exceptions) used within the internal
 * parts of libweston.  Notable exception(s) include a few files in tests/ that
 * need access to these functions, and those remoting/. Those will require some
 * further fixing as to avoid including this private header.
 *
 * Eventually, these symbols should reside naturally into their own scope. New
 * features should either provide their own (internal) header or use this one.
 */

#include <libweston/libweston.h>
#include <assert.h>
#include "color.h"

#define DEFAULT_FRAME_RATE_INTERVAL 1 /* seconds */

/* compositor <-> renderer interface */

/** Opaque pointer to renderbuffer data.
 */
typedef void *weston_renderbuffer_t;

/** Callback emitted when a renderbuffer is discarded
 *
 * \param renderbuffer The renderbuffer being discarded.
 * \param user_data User data.
 * \return true on success, false otherwise.
 *
 * A renderbuffer can be discarded by the renderer on various occasions, such as
 * when the output is resized. Before signal emission, the renderer releases
 * most allocated resources and marks it as stale. It is kept in the renderer's
 * renderbuffer list until destruction with destroy_renderbuffer(), which can
 * safely be called from the \c discarded callback.
 */
typedef bool (*weston_renderbuffer_discarded_func)(weston_renderbuffer_t renderbuffer,
						   void *user_data);

struct weston_renderer_options {
};

struct linux_dmabuf_memory {
	struct dmabuf_attributes *attributes;

	void (*destroy)(struct linux_dmabuf_memory *dmabuf);
};

enum weston_renderer_border_side {
	WESTON_RENDERER_BORDER_TOP = 0,
	WESTON_RENDERER_BORDER_LEFT = 1,
	WESTON_RENDERER_BORDER_RIGHT = 2,
	WESTON_RENDERER_BORDER_BOTTOM = 3,
};

struct weston_renderer {
	void (*repaint_output)(struct weston_output *output,
			       pixman_region32_t *output_damage,
			       weston_renderbuffer_t renderbuffer);

	/** See weston_renderer_resize_output()
	 *
	 * \return True for success, false for leaving the output in a mess.
	 */
	bool (*resize_output)(struct weston_output *output,
			      const struct weston_size *fb_size,
			      const struct weston_geometry *area);

	void (*flush_damage)(struct weston_paint_node *pnode);
	void (*attach)(struct weston_paint_node *pnode);
	void (*destroy)(struct weston_compositor *ec);

	/** See weston_surface_copy_content() */
	int (*surface_copy_content)(struct weston_surface *surface,
				    void *target, size_t size,
				    int src_x, int src_y,
				    int width, int height);

	/** See weston_compositor_import_dmabuf() */
	bool (*import_dmabuf)(struct weston_compositor *ec,
			      struct linux_dmabuf_buffer *buffer);

	const struct weston_drm_format_array *
			(*get_supported_dmabuf_formats)(struct weston_compositor *ec);

	bool (*fill_buffer_info)(struct weston_compositor *ec,
				 struct weston_buffer *buffer);

	void (*buffer_init)(struct weston_compositor *ec,
			    struct weston_buffer *buffer);

	/** Create a renderbuffer
	 *
	 * \param output The output to render.
	 * \param format The renderbuffer pixel format.
	 * \param buffer The destination buffer, or \c NULL.
	 * \param stride The destination \c buffer stride in bytes, or 0.
	 * \param discarded_cb The callback emitted on a discarded event, or
	 * NULL.
	 * \param user_data User data passed to \c discarded_cb.
	 * \return An opaque renderbuffer pointer, or \c NULL on failure.
	 *
	 * This function creates a renderbuffer of the requested format. The
	 * renderer can then use it to repaint the \c output into the specified
	 * destination \c buffer, which must be the same size as the \c output
	 * (including borders), or into an internal buffer if \c NULL.
	 *
	 * Backends should provide a \c discarded_cb callback in order to
	 * properly handle renderbuffer lifetime.
	 *
	 * See repaint_output().
	 */
	weston_renderbuffer_t
	(*create_renderbuffer)(struct weston_output *output,
			       const struct pixel_format_info *format,
			       void *buffer,
			       int stride,
			       weston_renderbuffer_discarded_func discarded_cb,
			       void *user_data);

	/** Create a renderbuffer from a DMABUF
	 *
	 * \param output The output to render.
	 * \param dmabuf The destination DMABUF, ownership is transferred to the
	 * renderbuffer.
	 * \param discarded_cb The callback emitted on a discarded event, or
	 * NULL.
	 * \param user_data User data passed to \c discarded_cb.
	 * \return An opaque renderbuffer pointer, or \c NULL on failure.
	 *
	 * This function creates a renderbuffer from a DMABUF. The renderer can
	 * then use it to repaint the output into the specified destination \c
	 * dmabuf, which must be the same size as the \c output (including
	 * borders).
	 *
	 * Backends should provide a \c discarded_cb callback in order to
	 * properly handle renderbuffer lifetime.
	 *
	 * See repaint_output().
	 */
	weston_renderbuffer_t
	(*create_renderbuffer_dmabuf)(struct weston_output *output,
				      struct linux_dmabuf_memory *dmabuf,
				      weston_renderbuffer_discarded_func discarded_cb,
				      void *user_data);

	/** Destroy a renderbuffer
	 *
	 * \param renderbuffer The renderbuffer to destroy.
	 *
	 * This function destroys a \c renderbuffer.
	 */
	void (*destroy_renderbuffer)(weston_renderbuffer_t renderbuffer);

	/** Allocate a DMABUF that can be imported as renderbuffer
	 *
	 * \param renderer The renderer that allocated the DMABUF
	 * \param width The width of the allocated DMABUF
	 * \param height The height of the allocated DMABUF
	 * \param format The pixel format of the allocated DMABUF
	 * \param modifiers The suggested modifiers for the allocated DMABUF
	 * \param count The number of suggested modifiers for the allocated DMABUF
	 * \return A linux_dmabuf_memory object that may be imported as renderbuffer
	 *
	 * Request a DMABUF from the renderer. The returned DMABUF can be
	 * imported into the renderer as a renderbuffer and exported to other
	 * processes.
	 */
	struct linux_dmabuf_memory *
			(*dmabuf_alloc)(struct weston_renderer *renderer,
					unsigned int width, unsigned int height,
					uint32_t format,
					const uint64_t *modifiers, unsigned int count);

	/** Checks if renderer is able to produce fb's with straight alpha
	 * encoding (i.e. not pre-multiplied by alpha).
	 *
	 * \param wc The Weston compositor instance.
	 * \return True if renderer is capable, false otherwise.
	 */
	bool (*can_render_straight_alpha)(struct weston_compositor *wc);

	enum weston_renderer_type type;
	const struct gl_renderer_interface *gl;
	const struct vulkan_renderer_interface *vulkan;
	const struct pixman_renderer_interface *pixman;

	/* Sets the output border.
	 *
	 * The side specifies the side for which we are setting the border.
	 * The width and height are the width and height of the border.
	 * The tex_width patemeter specifies the width of the actual
	 * texture; this may be larger than width if the data is not
	 * tightly packed.
	 *
	 * The top and bottom textures will extend over the sides to the
	 * full width of the bordered window.  The right and left edges,
	 * however, will extend only to the top and bottom of the
	 * compositor surface.  This is demonstrated by the picture below:
	 *
	 * +-----------------------+
	 * |          TOP          |
	 * +-+-------------------+-+
	 * | |                   | |
	 * |L|                   |R|
	 * |E|                   |I|
	 * |F|                   |G|
	 * |T|                   |H|
	 * | |                   |T|
	 * | |                   | |
	 * +-+-------------------+-+
	 * |        BOTTOM         |
	 * +-----------------------+
	 */
	void (*output_set_border)(struct weston_output *output,
				  enum weston_renderer_border_side side,
				  int32_t width, int32_t height,
				  int32_t tex_width, unsigned char *data);

};

struct weston_tearing_control {
	struct weston_surface *surface;
	bool may_tear;
};

/** A client tracker
 *
 * This life time is tied to the wl_client.
 */
struct weston_client {
	struct wl_listener wl_client_destroy_listener;
	uint64_t internal_id;
	char *internal_name;

	uint64_t internal_id_counter;
};

bool
weston_renderer_resize_output(struct weston_output *output,
			      const struct weston_size *fb_size,
			      const struct weston_geometry *area);

static inline void
check_compositing_area(const struct weston_size *fb_size,
		       const struct weston_geometry *area)
{
	assert(fb_size);
	assert(fb_size->width > 0);
	assert(fb_size->height > 0);

	assert(area);
	assert(area->x >= 0);
	assert(area->width > 0);
	assert(area->x <= fb_size->width - area->width);
	assert(area->y >= 0);
	assert(area->height > 0);
	assert(area->y <= fb_size->height - area->height);
}

/* weston_buffer */

void
weston_buffer_send_server_error(struct weston_buffer *buffer,
				const char *msg);
void
weston_buffer_reference(struct weston_buffer_reference *ref,
			struct weston_buffer *buffer,
			enum weston_buffer_reference_type type);

void
weston_buffer_release_move(struct weston_buffer_release_reference *dest,
			   struct weston_buffer_release_reference *src);

void
weston_buffer_release_reference(struct weston_buffer_release_reference *ref,
				struct weston_buffer_release *buf_release);

/* weston_bindings */
void
weston_binding_list_destroy_all(struct wl_list *list);

/* weston_compositor */

void
touch_calibrator_mode_changed(struct weston_compositor *compositor);

int
noop_renderer_init(struct weston_compositor *ec);

void
weston_compositor_schedule_heads_changed(struct weston_compositor *compositor);

void
weston_compositor_add_head(struct weston_compositor *compositor,
			   struct weston_head *head);
void
weston_compositor_add_pending_output(struct weston_output *output,
				     struct weston_compositor *compositor);
bool
weston_compositor_import_dmabuf(struct weston_compositor *compositor,
				struct linux_dmabuf_buffer *buffer);
bool
weston_compositor_dmabuf_can_scanout(struct weston_compositor *compositor,
					struct linux_dmabuf_buffer *buffer);
void
weston_compositor_offscreen(struct weston_compositor *compositor);

void
weston_compositor_print_scene_graph(struct weston_compositor *ec, FILE *fp);

void
weston_compositor_read_presentation_clock(
			struct weston_compositor *compositor,
			struct timespec *ts);

int
weston_compositor_init_renderer(struct weston_compositor *compositor,
				enum weston_renderer_type renderer_type,
				const struct weston_renderer_options *options);

int
weston_compositor_run_axis_binding(struct weston_compositor *compositor,
				   struct weston_pointer *pointer,
				   const struct timespec *time,
				   const struct weston_pointer_axis_event *event);
void
weston_compositor_run_button_binding(struct weston_compositor *compositor,
				     struct weston_pointer *pointer,
				     const struct timespec *time,
				     uint32_t button,
				     enum wl_pointer_button_state value);
int
weston_compositor_run_debug_binding(struct weston_compositor *compositor,
				    struct weston_keyboard *keyboard,
				    const struct timespec *time,
				    uint32_t key,
				    enum wl_keyboard_key_state state);
void
weston_compositor_run_key_binding(struct weston_compositor *compositor,
				  struct weston_keyboard *keyboard,
				  const struct timespec *time,
				  uint32_t key,
				  enum wl_keyboard_key_state state);
void
weston_compositor_run_modifier_binding(struct weston_compositor *compositor,
				       struct weston_keyboard *keyboard,
				       enum weston_keyboard_modifier modifier,
				       enum wl_keyboard_key_state state);
void
weston_compositor_run_touch_binding(struct weston_compositor *compositor,
				    struct weston_touch *touch,
				    const struct timespec *time,
				    int touch_type);
void
weston_compositor_run_tablet_tool_binding(struct weston_compositor *compositor,
					  struct weston_tablet_tool *tool,
					  uint32_t button, uint32_t state_w);
void
weston_compositor_set_touch_mode_normal(struct weston_compositor *compositor);

void
weston_compositor_set_touch_mode_calib(struct weston_compositor *compositor);

void
weston_compositor_xkb_destroy(struct weston_compositor *ec);

int
weston_input_init(struct weston_compositor *compositor);

void
weston_input_bind_output(struct weston_compositor *compositor,
			 const char *output_name, const char *match);

/* weston_output */

void
weston_output_disable_planes_incr(struct weston_output *output);

void
weston_output_disable_planes_decr(struct weston_output *output);

void
weston_output_set_single_mode(struct weston_output *output,
			      struct weston_mode *target);

/* weston_plane */

void
weston_plane_init(struct weston_plane *plane, struct weston_compositor *ec);

void
weston_plane_release(struct weston_plane *plane);

/* weston_seat */

struct clipboard *
clipboard_create(struct weston_seat *seat);

void
weston_seat_init(struct weston_seat *seat, struct weston_compositor *ec,
		 const char *seat_name);

void
weston_seat_repick(struct weston_seat *seat);

void
weston_seat_release(struct weston_seat *seat);

void
weston_seat_send_selection(struct weston_seat *seat, struct wl_client *client);

int
weston_seat_init_pointer(struct weston_seat *seat);

int
weston_seat_init_keyboard(struct weston_seat *seat, struct xkb_keymap *keymap);

int
weston_seat_init_touch(struct weston_seat *seat);

void
weston_seat_release_keyboard(struct weston_seat *seat);

void
weston_seat_release_pointer(struct weston_seat *seat);

void
weston_seat_release_touch(struct weston_seat *seat);

struct weston_tablet *
weston_seat_add_tablet(struct weston_seat *seat);
struct weston_tablet_tool *
weston_seat_add_tablet_tool(struct weston_seat *seat);
void
weston_seat_release_tablet_tool(struct weston_tablet_tool *tablet_tool);
void
weston_seat_release_tablet(struct weston_tablet *tablet);

void
weston_seat_update_keymap(struct weston_seat *seat, struct xkb_keymap *keymap);

void
wl_data_device_set_keyboard_focus(struct weston_seat *seat);

/* weston_pointer */

struct weston_coord_global
weston_pointer_clamp(struct weston_pointer *pointer,
		     struct weston_coord_global pos);

void
weston_pointer_set_default_grab(struct weston_pointer *pointer,
			        const struct weston_pointer_grab_interface *interface);

void
weston_pointer_constraint_destroy(struct weston_pointer_constraint *constraint);

/* weston_keyboard */
bool
weston_keyboard_has_focus_resource(struct weston_keyboard *keyboard);

/* weston_touch */

struct weston_touch_device *
weston_touch_create_touch_device(struct weston_touch *touch,
				 const char *syspath,
				 void *backend_data,
				 const struct weston_touch_device_ops *ops,
				 weston_touch_device_set_output_func_t set_output);

void
weston_touch_device_destroy(struct weston_touch_device *device);

bool
weston_touch_has_focus_resource(struct weston_touch *touch);

int
weston_touch_start_drag(struct weston_touch *touch,
			struct weston_data_source *source,
			struct weston_surface *icon,
			struct wl_client *client);


/* weston_touch_device */

bool
weston_touch_device_can_calibrate(const struct weston_touch_device *device);

/* weston_tablet */

void
weston_tablet_manager_init(struct weston_compositor *ec);

struct weston_tablet *
weston_tablet_create(void);

void
weston_tablet_destroy(struct weston_tablet *tablet);

/* weston_tablet_tool */

struct weston_tablet_tool *
weston_tablet_tool_create(void);

void
weston_tablet_tool_destroy(struct weston_tablet_tool *tool);

/* weston_surface */
pixman_box32_t
weston_surface_to_buffer_rect(struct weston_surface *surface,
			      pixman_box32_t rect);
void
weston_surface_to_buffer_region(struct weston_surface *surface,
				pixman_region32_t *surface_region,
				pixman_region32_t *buffer_region);

/* weston_spring */

void
weston_spring_init(struct weston_spring *spring,
		   double k, double current, double target);
int
weston_spring_done(struct weston_spring *spring);

void
weston_spring_update(struct weston_spring *spring, const struct timespec *time);

/* weston_view */

bool
weston_view_is_opaque(struct weston_view *ev, pixman_region32_t *region);

bool
weston_paint_node_has_valid_buffer(struct weston_paint_node *pnode);

bool
weston_view_takes_input_at_point(struct weston_view *view,
				 struct weston_coord_surface surf_pos);

void
weston_view_geometry_dirty_internal(struct weston_view *view);

void
weston_paint_node_move_to_plane(struct weston_paint_node *pnode,
				struct weston_plane *plane);

const pixman_region32_t *
weston_paint_node_get_opaque_region(const struct weston_paint_node *pnode);

void
weston_view_buffer_to_output_matrix(const struct weston_view *view,
				    const struct weston_output *output,
				    struct weston_matrix *matrix);

pixman_box32_t
weston_matrix_transform_rect(struct weston_matrix *matrix,
                            pixman_box32_t rect);
void
weston_matrix_transform_region(pixman_region32_t *dest,
			       struct weston_matrix *matrix,
			       pixman_region32_t *src);

/* protected_surface */
void
weston_protected_surface_send_event(struct protected_surface *psurface,
				    enum weston_hdcp_protection protection);

/* weston_drm_format */

struct weston_drm_format {
	uint32_t format;
	struct wl_array modifiers;
};

struct weston_drm_format_array {
	struct wl_array arr;
};

void
weston_drm_format_array_init(struct weston_drm_format_array *formats);

void
weston_drm_format_array_fini(struct weston_drm_format_array *formats);

int
weston_drm_format_array_replace(struct weston_drm_format_array *formats,
				const struct weston_drm_format_array *source_formats);

struct weston_drm_format *
weston_drm_format_array_add_format(struct weston_drm_format_array *formats,
				   uint32_t format);

void
weston_drm_format_array_remove_latest_format(struct weston_drm_format_array *formats);

struct weston_drm_format *
weston_drm_format_array_find_format(const struct weston_drm_format_array *formats,
				    uint32_t format);

unsigned int
weston_drm_format_array_count_pairs(const struct weston_drm_format_array *formats);

bool
weston_drm_format_array_equal(const struct weston_drm_format_array *formats_A,
			      const struct weston_drm_format_array *formats_B);

int
weston_drm_format_array_join(struct weston_drm_format_array *formats_A,
			     const struct weston_drm_format_array *formats_B);

int
weston_drm_format_array_intersect(struct weston_drm_format_array *formats_A,
				  const struct weston_drm_format_array *formats_B);

int
weston_drm_format_array_subtract(struct weston_drm_format_array *formats_A,
				 const struct weston_drm_format_array *formats_B);

int
weston_drm_format_add_modifier(struct weston_drm_format *format,
			       uint64_t modifier);

bool
weston_drm_format_has_modifier(const struct weston_drm_format *format,
			       uint64_t modifier);

const uint64_t *
weston_drm_format_get_modifiers(const struct weston_drm_format *format,
				unsigned int *count_out);

void
weston_compositor_destroy_touch_calibrator(struct weston_compositor *compositor);


/**
 * Reasons why placing a view on a plane failed. Needed by the dma-buf feedback.
 */
enum try_view_on_plane_failure_reasons {
	FAILURE_REASONS_NONE = 0,
	FAILURE_REASONS_FORCE_RENDERER = 1 << 0,
	FAILURE_REASONS_FB_FORMAT_INCOMPATIBLE = 1 << 1,
	FAILURE_REASONS_DMABUF_MODIFIER_INVALID = 1 << 2,
	FAILURE_REASONS_ADD_FB_FAILED = 1 << 3,
	FAILURE_REASONS_NO_PLANES_AVAILABLE = 1 << 4,
	FAILURE_REASONS_PLANES_REJECTED = 1 << 5,
	FAILURE_REASONS_INADEQUATE_CONTENT_PROTECTION = 1 << 6,
	FAILURE_REASONS_INCOMPATIBLE_TRANSFORM = 1 << 7,
	FAILURE_REASONS_NO_BUFFER = 1 << 8,
	FAILURE_REASONS_BUFFER_TOO_BIG = 1 << 9,
	FAILURE_REASONS_BUFFER_TYPE = 1 << 10,
	FAILURE_REASONS_GLOBAL_ALPHA = 1 << 11,
	FAILURE_REASONS_NO_GBM = 1 << 12,
	FAILURE_REASONS_GBM_BO_IMPORT_FAILED = 1 << 13,
	FAILURE_REASONS_GBM_BO_GET_HANDLE_FAILED = 1 << 14,
	FAILURE_REASONS_NO_COLOR_TRANSFORM = 1 << 15,
	FAILURE_REASONS_SOLID_SURFACE = 1 << 16,
	FAILURE_REASONS_OCCLUDED_BY_RENDERER = 1 << 17,
	FAILURE_REASONS_OUTPUT_COLOR_EFFECT = 1 << 18,
};

/**
 * paint node
 *
 * A generic data structure unique for surface-view-output combination.
 */
struct weston_paint_node {
	struct weston_trace_flow flow; /* Perfetto flow */

	/* Immutable members: */

	/* struct weston_surface::paint_node_list */
	struct wl_list surface_link;
	struct weston_surface *surface;

	/* struct weston_view::paint_node_list */
	struct wl_list view_link;
	struct weston_view *view;
	struct weston_matrix *view_transform_matrix;

	/* struct weston_output::paint_node_list */
	struct wl_list output_link;
	struct weston_output *output;

	char *internal_name;

	/* Mutable members: */

	enum weston_paint_node_status status;
	struct weston_matrix buffer_to_output_matrix;
	struct weston_matrix output_to_buffer_matrix;
	bool needs_filtering;

	/* We consider a transform to be simple if it can be
	 * represented by one of wayland's named transforms,
	 * plus translation and scale.
	 *
	 * An axis aligned box must remain axis aligned.
	 */
	bool simple_transform;
	/* Only valid if the transform is considered simple. */
	enum wl_output_transform transform;
	/* The paint node's output destination rectangle, only valid if simple_transform
	 * is true */
	struct weston_geometry output_dest;
	/* The paint node's buffer source rectangle, only valid if simple_transform
	 * is true */
	float buffer_source_x;
	float buffer_source_y;
	float buffer_source_width;
	float buffer_source_height;

	/* struct weston_output::paint_node_z_order_list */
	struct wl_list z_order_link;

	pixman_region32_t visible_previous;
	pixman_region32_t visible;
	pixman_region32_t clipped_view;
	pixman_region32_t damage; /* In global coordinates */
	struct weston_plane *plane;
	struct weston_plane *plane_next;

	struct weston_surface_color_transform surf_xform;
	bool surf_xform_valid;

	uint32_t try_view_on_plane_failure_reasons;
	bool is_fully_opaque;
	bool is_fully_blended;
	bool on_cursor_layer;

	/* Combined alpha from view and surface. */
	float alpha;

	/* This node's contents are solid, either from a solid buffer or a
	 * placeholder.
	 *
	 * Care is taken to ensure that it is correct both during
	 * assign_planes and during render, even if the value is different
	 * for each during a single repaint.
	 */
	bool draw_solid;

	/* This node's buffer or view alpha causes it to be completely
	 * transparent, so it can be optimized away or skipped for
	 * plane assignment.
	 */
	bool is_fully_transparent;

	/* censored content must not be placed on a plane, it will be rendered
	 * as a placeholder, and draw_solid must be set.
	 */
	bool censored;

	/* Only valid when draw_solid is true, this is the solid color of
	 * the paint node. It may simply be a copy of a solid buffer's
	 * values, or it may be a placeholder used to replace a buffer's
	 * content.
	 *
	 * It may change after assign_planes if plane assignment requires
	 * the renderer to use a placeholder.
	 */
	struct weston_solid_buffer_values solid;

	/* need_hole means this paint node has been placed on a plane beneath
	 * the renderer's plane, so the renderer must draw a transparent hole
	 * for the paint node.
	 *
	 * This is set in the backend assign_planes callback.
	 */
	bool need_hole;
	uint32_t psf_flags; /* presentation-feedback flags */
};

struct weston_paint_node *
weston_view_find_paint_node(struct weston_view *view,
			    struct weston_output *output);

/* others */
int
wl_data_device_manager_init(struct wl_display *display);

/* Exclusively for unit tests */

bool
weston_output_set_color_outcome(struct weston_output *output);

void
weston_surface_build_buffer_matrix(const struct weston_surface *surface,
				   struct weston_matrix *matrix);

void
weston_output_update_matrix(struct weston_output *output);

static inline void
convert_size_by_transform_scale(int32_t *width_out, int32_t *height_out,
				int32_t width, int32_t height,
				uint32_t transform,
				int32_t scale)
{
	assert(scale > 0);

	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*width_out = width / scale;
		*height_out = height / scale;
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*width_out = height / scale;
		*height_out = width / scale;
		break;
	default:
		assert(0 && "invalid transform");
	}
}

static inline bool
convert_buffer_size_by_transform_scale(int32_t *width_out, int32_t *height_out,
				       const struct weston_buffer *buf,
				       const struct weston_buffer_viewport *vp)
{
	/* Buffer dimensions must be integer multiples of the scale */
	if (buf->width % vp->buffer.scale ||
	    buf->height % vp->buffer.scale)
		return false;

	convert_size_by_transform_scale(width_out, height_out,
					buf->width, buf->height,
					vp->buffer.transform,
					vp->buffer.scale);
	if (*width_out == 0 || *height_out == 0)
		return false;

	return true;
}

/* User authentication for remote backends */

bool
weston_authenticate_user(const char *username, const char *password);

void
weston_output_copy_native_mode(struct weston_output *output,
			       struct weston_mode *mode);

static inline void
region_init_infinite(pixman_region32_t *region)
{
	pixman_region32_init_rect(region, INT32_MIN, INT32_MIN,
				  UINT32_MAX, UINT32_MAX);
}

struct weston_subsurface *
weston_surface_to_subsurface(struct weston_surface *surface);

void
weston_presentation_feedback_discard_list(struct wl_list *list);

void
weston_surface_update_size(struct weston_surface *surface);

/* Surface state helpers from surface-state.c */

void
weston_surface_commit(struct weston_surface *surface);

void
weston_subsurface_set_synchronized(struct weston_subsurface *sub, bool sync);

void
weston_surface_state_init(struct weston_surface *surface,
			  struct weston_surface_state *state);

void
weston_surface_state_fini(struct weston_surface_state *state);

const char *
weston_plane_failure_reasons_to_str(enum try_view_on_plane_failure_reasons failure_reasons);

uint32_t
weston_surface_visibility_mask(struct weston_surface *surface);

void
weston_compositor_apply_transactions(struct weston_compositor *compositor);

void
weston_repaint_timer_arm(struct weston_compositor *compositor);

struct timespec
weston_output_repaint_from_present(const struct weston_output *output,
				   const struct timespec *now,
				   const struct timespec *present_time);

void
weston_backend_set_deferred(struct weston_backend *backend);

void
weston_backend_clear_deferred(struct weston_backend *backend,
                              struct weston_compositor *compositor);

struct weston_coord_surface __attribute__ ((warn_unused_result))
weston_coord_global_to_surface_for_paint_node(const struct weston_paint_node *pnode,
					      struct weston_coord_global coord);

#endif
