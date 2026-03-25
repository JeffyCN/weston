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


#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef BUILD_DRM_GBM
#include <gbm.h>
#endif
#include <libudev.h>

#include <libweston/libweston.h>
#include <libweston/backend-drm.h>
#include <libweston/weston-log.h>
#include "output-capture.h"
#include "shared/helpers.h"
#include "shared/weston-drm-fourcc.h"
#include "libinput-seat.h"
#include "backend.h"
#include "libweston-internal.h"
#include "drm-kms-enums.h"

#ifndef GBM_BO_USE_CURSOR
#define GBM_BO_USE_CURSOR GBM_BO_USE_CURSOR_64X64
#endif

#ifndef GBM_BO_USE_LINEAR
#define GBM_BO_USE_LINEAR (1 << 4)
#endif

#ifndef DRM_PLANE_ZPOS_INVALID_PLANE
#define DRM_PLANE_ZPOS_INVALID_PLANE	0xffffffffffffffffULL
#endif

#ifndef DRM_PLANE_ALPHA_OPAQUE
#define DRM_PLANE_ALPHA_OPAQUE	0xffffUL
#endif

#ifndef MAX_DMABUF_PLANES
#define MAX_DMABUF_PLANES 4
#endif

#define DRM_MAX_REUSE_FAILURES 10

/**
 * A small wrapper to print information into the 'drm-backend' debug scope.
 *
 * The following conventions are used to print variables:
 *
 *  - fixed uint32_t values, including Weston object IDs such as weston_output
 *    IDs, DRM object IDs such as CRTCs or properties, and GBM/DRM formats:
 *      "%lu (0x%lx)" (unsigned long) value, (unsigned long) value
 *
 *  - fixed uint64_t values, such as DRM property values (including object IDs
 *    when used as a value):
 *      "%llu (0x%llx)" (unsigned long long) value, (unsigned long long) value
 *
 *  - non-fixed-width signed int:
 *      "%d" value
 *
 *  - non-fixed-width unsigned int:
 *      "%u (0x%x)" value, value
 *
 *  - non-fixed-width unsigned long:
 *      "%lu (0x%lx)" value, value
 *
 * Either the integer or hexadecimal forms may be omitted if it is known that
 * one representation is not useful (e.g. width/height in hex are rarely what
 * you want).
 *
 * This is to avoid implicit widening or narrowing when we use fixed-size
 * types: uint32_t can be resolved by either unsigned int or unsigned long
 * on a 32-bit system but only unsigned int on a 64-bit system, with uint64_t
 * being unsigned long long on a 32-bit system and unsigned long on a 64-bit
 * system. To avoid confusing side effects, we explicitly cast to the widest
 * possible type and use a matching format specifier.
 */
#define DO_DRM_DEBUG_COMPLEX(scope, ...) \
	weston_log_scope_printf(scope, __VA_ARGS__)

#define DO_DRM_DEBUG(scope, string) \
	weston_log_scope_puts(scope, string)

#define drm_debug(b, format, ...)				\
	DO_DRM_DEBUG ## __VA_OPT__(_COMPLEX)			\
	((b)->debug, format __VA_OPT__(,) __VA_ARGS__)

#define MAX_CLONED_CONNECTORS 4

/* Minimum interval between hotplug update requests (ms) to avoid glitches */
#define DRM_HOTPLUG_DEBOUNCE_MS	1000

/**
 * Represents the values of an enum-type KMS property
 */
struct drm_property_enum_info {
	const char *name; /**< name as string (static, not freed) */
	bool valid; /**< true if value is supported; ignore if false */
	uint64_t value; /**< raw value */
};

/**
 * Holds information on a DRM property, including its ID and the enum
 * values it holds.
 *
 * DRM properties are allocated dynamically, and maintained as DRM objects
 * within the normal object ID space; they thus do not have a stable ID
 * to refer to. This includes enum values, which must be referred to by
 * integer values, but these are not stable.
 *
 * drm_property_info allows a cache to be maintained where Weston can use
 * enum values internally to refer to properties, with the mapping to DRM
 * ID values being maintained internally.
 */
struct drm_property_info {
	const char *name; /**< name as string (static, not freed) */
	uint32_t prop_id; /**< KMS property object ID */
	uint32_t flags;
	unsigned int num_enum_values; /**< number of enum values */
	struct drm_property_enum_info *enum_values; /**< array of enum values */
	unsigned int num_range_values;
	uint64_t range_values[2];
};

/**
 * We use this to keep track of actions we need to do with the dma-buf feedback
 * in order to keep it up-to-date with the info we get from the DRM-backend.
 */
enum actions_needed_dmabuf_feedback {
	ACTION_NEEDED_NONE = 0,
	ACTION_NEEDED_ADD_SCANOUT_TRANCHE = (1 << 0),
	ACTION_NEEDED_REMOVE_SCANOUT_TRANCHE = (1 << 1),
};

enum drm_plane_subtype {
	PLANE_SUBTYPE_OVERLAY_ONLY = 0,
	PLANE_SUBTYPE_UNDERLAY_ONLY = 1,
	PLANE_SUBTYPE_BOTH = 2,
};

enum drm_recovery_status {
	DRM_RECOVERY_UNNECESSARY = 0,
	DRM_RECOVERY_WAIT_FOR_IDLE = 1,
	DRM_RECOVERY_SCHEDULED = 2,
	DRM_RECOVERY_APPLIED = 3,
};

struct drm_kms_device {
	int id;
	char *filename;
	dev_t devnum;
	struct udev_device *udev_device;

	int fd;
	struct weston_launcher *fd_owner;
};

struct drm_device {
	struct drm_backend *backend;

	/* owned */
	struct drm_kms_device *kms_device;
	struct wl_event_source *drm_event_source;

	/* Track the GEM handles if the device does not have a gbm device, which
	 * tracks the handles for us.
	 */
	struct hash_table *gem_handle_refcnt;

	/* drm_crtc::link */
	struct wl_list crtc_list;

	struct wl_list plane_list;

	/* drm_writeback::link */
	struct wl_list writeback_connector_list;

	bool will_repaint;

	enum drm_recovery_status recovery_status;

	int32_t atomic_completes_pending;

	bool atomic_modeset;

	bool tearing_supported;

	bool aspect_ratio_supported;

	int32_t cursor_width;
	int32_t cursor_height;

	bool cursors_are_broken;
	bool disable_client_buffer_scanout;

	void *repaint_data;

	bool fb_modifiers;

	/* we need these parameters in order to not fail drmModeAddFB2()
	 * due to out of bounds dimensions, and then mistakenly set
	 * sprites_are_broken:
	 */
	int min_width, max_width;
	int min_height, max_height;

	/* drm_backend::kms_list */
	struct wl_list link;

	/* struct drm_colorop_3x1d_lut::link  */
	struct wl_list drm_colorop_3x1d_lut_list;

	int reused_state_failures;
};

struct drm_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct udev *udev;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_drm_source;

	struct drm_device *drm;
	/* drm_device::link */
	struct wl_list kms_list;
	struct gbm_device *gbm;
	struct wl_listener session_listener;
	const struct pixel_format_info *format;

	bool use_pixman_shadow;

	bool offload_blend_to_output;

	struct udev_input input;

	uint32_t pageflip_timeout;

	struct weston_log_scope *debug;

	struct {
		uint32_t frame_counter_interval;
		struct wl_event_source *pageflip_timer_counter;
		bool timer_armed;
	} perf_page_flips_stats;

	/* True if we need a workaround for some very old kernels */
	bool stale_timestamp_workaround;

	/* Timer for debouncing hotplug events */
	struct wl_event_source *hotplug_update_timer;
	/* Flag for pending debounced update */
	bool pending_hotplug_update;
	/* Timestamp of last processed update */
	int64_t last_hotplug_update_ms;
};

struct drm_mode {
	struct weston_mode base;
	drmModeModeInfo mode_info;
	uint32_t blob_id;
};

enum drm_fb_type {
	BUFFER_INVALID = 0, /**< never used */
	BUFFER_CLIENT, /**< directly sourced from client */
	BUFFER_DMABUF, /**< imported from linux_dmabuf client */
	BUFFER_DMABUF_BACKEND, /**< imported from dmabuf renderbuffer */
	BUFFER_PIXMAN_DUMB, /**< internal Pixman rendering */
	BUFFER_GBM_SURFACE, /**< internal EGL rendering */
	BUFFER_CURSOR, /**< internal cursor buffer */
};

struct drm_fb {
	enum drm_fb_type type;

	struct drm_backend *backend;
	struct drm_device *scanout_device;

	int refcnt;

	uint32_t fb_id, size;
	uint32_t handles[MAX_DMABUF_PLANES];
	uint32_t strides[MAX_DMABUF_PLANES];
	uint32_t offsets[MAX_DMABUF_PLANES];
	int num_planes;
	const struct pixel_format_info *format;
	uint64_t modifier;
	int width, height;
	int fd;

	uint32_t plane_mask;

	/* Used by gbm fbs */
	struct gbm_bo *bo;
	struct gbm_surface *gbm_surface;

	/* Used when direct-display extension is turned on for that dmabuf */
	bool direct_display;
	int fds[MAX_DMABUF_PLANES];
	 /* tracks how many fds we've dup'ed */
	int num_duped_fds;

	/* Used by dumb fbs */
	void *map;
};

struct drm_buffer_fb {
	struct drm_fb *fb;
	enum try_view_on_plane_failure_reasons failure_reasons;
	struct drm_device *device;
	struct wl_list link;
};

struct drm_fb_private {
	struct wl_list buffer_fb_list;
	struct wl_listener buffer_destroy_listener;
};

/**
 * Pending state holds one or more drm_output_state structures, collected from
 * performing repaint. This pending state is transient, and only lives between
 * beginning a repaint group and flushing the results: after flush, each
 * output state will complete and be retired separately.
 */
struct drm_pending_state {
	struct drm_device *device;
	struct wl_list output_list;
};

enum drm_output_propose_state_mode {
	DRM_OUTPUT_PROPOSE_STATE_INVALID = 0, /**< Invalid state */
	DRM_OUTPUT_PROPOSE_STATE_MIXED, /**< mix renderer & planes */
	DRM_OUTPUT_PROPOSE_STATE_RENDERER_AND_CURSOR, /**< only assign to renderer & cursor plane */
	DRM_OUTPUT_PROPOSE_STATE_RENDERER_ONLY, /**< only assign to renderer */
	DRM_OUTPUT_PROPOSE_STATE_PLANES_ONLY, /**< no renderer use, only planes */
	DRM_OUTPUT_PROPOSE_STATE_REUSE = 128, /**< bit indicates reuse prior state with new buffers */
};

/*
 * Output state holds the dynamic state for one Weston output, i.e. a KMS CRTC,
 * plus >= 1 each of encoder/connector/plane. Since everything but the planes
 * is currently statically assigned per-output, we mainly use this to track
 * plane state.
 *
 * pending_state is set when the output state is owned by a pending_state,
 * i.e. when it is being constructed and has not yet been applied. When the
 * output state has been applied, the owning pending_state is freed.
 */
struct drm_output_state {
	struct drm_pending_state *pending_state;
	struct drm_output *output;
	enum drm_output_propose_state_mode mode;
	struct wl_list link;
	enum dpms_enum dpms;
	enum weston_hdcp_protection protection;
	struct wl_list plane_list;
	bool tear;
	bool planes_enabled;
};

/**
 * Plane state holds the dynamic state for a plane: where it is positioned,
 * and which buffer it is currently displaying.
 *
 * The plane state is owned by an output state, except when setting an initial
 * state. See drm_output_state for notes on state object lifetime.
 */
struct drm_plane_state {
	struct drm_plane *plane;
	struct drm_plane_handle *handle;
	struct drm_output_state *output_state;

	struct drm_fb *fb;
	struct {
		struct weston_buffer_reference buffer;
		struct weston_buffer_release_reference release;
	} fb_ref;

	struct weston_paint_node *paint_node; /**< maintained for drm_assign_planes only */

	int32_t src_x, src_y;
	uint32_t src_w, src_h;
	int32_t dest_x, dest_y;
	uint32_t dest_w, dest_h;

	uint32_t rotation;

	uint64_t zpos;
	uint16_t alpha;

	enum wdrm_plane_color_encoding color_encoding;
	enum wdrm_plane_color_range color_range;

	bool complete;

	/* We don't own the fd, so we shouldn't close it */
	int in_fence_fd;

	uint32_t damage_blob_id; /* damage to kernel */

	struct wl_list link; /* drm_output_state::plane_list */
};

/**
 * A plane represents one buffer, positioned within a CRTC, and stacked
 * relative to other planes on the same CRTC.
 *
 * Each CRTC has a 'primary plane', which use used to display the classic
 * framebuffer contents, as accessed through the legacy drmModeSetCrtc
 * call (which combines setting the CRTC's actual physical mode, and the
 * properties of the primary plane).
 *
 * The cursor plane also has its own alternate legacy API.
 *
 * Other planes are used opportunistically to display content we do not
 * wish to blit into the primary plane. These non-primary/cursor planes
 * are referred to as 'sprites'.
 */
struct drm_plane {
	struct weston_plane base;

	struct drm_device *device;

	enum wdrm_plane_type type;

	uint32_t possible_crtcs;
	uint32_t plane_id;
	uint32_t plane_idx;
	uint32_t crtc_id;

	struct drm_property_info props[WDRM_PLANE__COUNT];

	/* The last state submitted to the kernel for this plane. */
	struct drm_plane_state *state_cur;

	uint64_t zpos_min;
	uint64_t zpos_max;

	uint16_t alpha_min;
	uint16_t alpha_max;

	struct wl_list link;

	struct weston_drm_format_array formats;
};

struct drm_plane_handle {
	struct drm_output *output;
	struct drm_plane *plane;

	/* Whether this plane supports overlay, underlay, or both */
	enum drm_plane_subtype subtype;

	struct wl_list link; /* drm_output::plane_handle_list */
};

struct drm_connector {
	struct drm_device *device;

	drmModeConnector *conn;
	uint32_t connector_id;

	drmModeObjectProperties *props_drm;

	/* Holds the properties for the connector */
	struct drm_property_info props[WDRM_CONNECTOR__COUNT];
};

enum writeback_screenshot_state {
	/* No writeback connector screenshot ongoing. */
	DRM_OUTPUT_WB_SCREENSHOT_OFF,
	/* Screenshot client just triggered a writeback connector screenshot.
         * Now we need to prepare an atomic commit that will make DRM perform
         * the writeback operation. */
	DRM_OUTPUT_WB_SCREENSHOT_PREPARE_COMMIT,
	/* The atomic commit with writeback setup has been committed. After the
	 * commit is handled by DRM it will give us a sync fd that gets
	 * signalled when the writeback is done. */
	DRM_OUTPUT_WB_SCREENSHOT_CHECK_FENCE,
	/* The atomic commit completed and we received the sync fd from the
	 * kernel. We've polled to check if the writeback was over, but it
	 * wasn't. Now we must stop the repaint loop and wait until the
	 * writeback is complete, because we can't commit with KMS objects
	 * (CRTC, planes, etc) that are in used by the writeback job. */
	DRM_OUTPUT_WB_SCREENSHOT_WAITING_SIGNAL,
};

struct drm_writeback_state {
	struct drm_writeback *wb;
	struct drm_output *output;

	enum writeback_screenshot_state state;
	struct weston_capture_task *ct;
	struct wl_listener ct_destroy_listener;

	struct drm_fb *fb;
	int32_t out_fence_fd;
	struct wl_event_source *wb_source;

	/* Reference to fb's being used by the writeback job. These are all the
	 * framebuffers in every drm_plane_state of the output state that we've
	 * used to request the writeback job */
	struct wl_array referenced_fbs;
};

struct drm_writeback {
	/* drm_device::writeback_connector_list */
	struct wl_list link;

	struct drm_device *device;
	struct drm_connector connector;

	struct weston_drm_format_array formats;
};

struct drm_colorop_3x1d_lut {
	/* drm_device::drm_colorop_3x1d_lut_list */
	struct wl_list link;
	struct drm_device *device;

	uint64_t lut_size;

	struct weston_color_transform *xform;
	struct wl_listener destroy_listener;

	uint32_t blob_id;
};

struct drm_head {
	struct weston_head base;
	struct drm_connector connector;

	struct backlight *backlight;

	drmModeModeInfo inherited_mode;	/**< Original mode on the connector */
	uint32_t inherited_max_bpc;	/**< Original max_bpc on the connector */
	uint32_t inherited_crtc_id;	/**< Original CRTC assignment */

	/* drm_output::disable_head */
	struct wl_list disable_head_link;

	void *display_data;             /**< EDID or DisplayID blob */
	size_t display_data_len;        /**< bytes */
};

struct drm_crtc {
	/* drm_device::crtc_list */
	struct wl_list link;
	struct drm_device *device;

	/* The output driven by the CRTC */
	struct drm_output *output;

	uint32_t crtc_id; /* object ID to pass to DRM functions */
	int pipe; /* index of CRTC in resource array / bitmasks */

	uint32_t primary_plane_id; /* ID of the corresponding primary plane */

	/* Holds the properties for the CRTC */
	struct drm_property_info props_crtc[WDRM_CRTC__COUNT];

	/* CRTC prop WDRM_CRTC_GAMMA_LUT_SIZE */
	uint32_t lut_size;

	/* CRTC prop WDRM_CRTC_BACKGROUND_COLOR */
	uint64_t background_color;

	/* Union of formats of all compatible writeback connectors */
	struct weston_drm_format_array writeback_formats;
};

struct drm_output {
	struct weston_output base;
	struct drm_backend *backend;
	struct drm_device *device;
	struct drm_crtc *crtc;

	/* drm_head::disable_head_link */
	struct wl_list disable_head;

	bool page_flip_pending;
	bool atomic_complete_pending;
	bool destroy_pending;
	bool disable_pending;
	bool dpms_off_pending;
	bool mode_switch_pending;

	/* List of hardware planes this output can use, excluding the special
	 * cursor and scanout planes. */
	struct wl_list plane_handle_list;

	/* True, if underlay planes exist. */
	bool has_underlay;

	uint32_t gbm_cursor_handle[2];
	struct drm_fb *gbm_cursor_fb[2];
	struct drm_plane_handle *cursor_handle;
	int current_cursor;

	struct gbm_surface *gbm_surface;
	struct linux_dmabuf_memory *linux_dmabuf_memory[2];
	const struct pixel_format_info *format;
	uint32_t gbm_bo_flags;

	uint32_t hdr_output_metadata_blob_id;
	uint64_t ackd_color_outcome_serial;

	unsigned max_bpc;
	enum wdrm_colorspace connector_colorspace;

	bool legacy_gamma_not_supported;
	uint16_t legacy_gamma_size;
	struct drm_colorop_3x1d_lut *blend_to_output_xform;

	/* Plane being displayed directly on the CRTC */
	struct drm_plane_handle *scanout_handle;

	/* The last state submitted to the kernel for this CRTC. */
	struct drm_output_state *state_cur;
	/* The previously-submitted state, where the hardware has not
	 * yet acknowledged completion of state_cur. */
	struct drm_output_state *state_last;

	/* only set when a writeback screenshot is ongoing */
	struct drm_writeback_state *wb_state;

	struct drm_fb *dumb[2];
	weston_renderbuffer_t renderbuffer[2];
	int current_image;

	struct wl_event_source *pageflip_timer;

	/* how many page flips */
	uint32_t page_flips_counted;

	/* how many page flips / interval */
	float page_flips_per_timer_interval;

	bool is_virtual;
	void (*virtual_destroy)(struct weston_output *base);

	submit_frame_cb virtual_submit_frame;

	enum wdrm_content_type content_type;

	bool reused_state;
	bool force_rebuild_state;
};

void
drm_destroy(struct weston_backend *backend);

static inline struct drm_head *
to_drm_head(struct weston_head *base)
{
	if (base->backend->destroy != drm_destroy)
		return NULL;
	return container_of(base, struct drm_head, base);
}

void
drm_device_recovery_schedule(struct drm_device *device);

void
drm_device_recovery_required(struct drm_device *device);

void
drm_device_recovery_complete(struct drm_device *device);

void
drm_writeback_reference_planes(struct drm_writeback_state *state,
			       struct wl_list *plane_state_list);
bool
drm_writeback_try_complete(struct drm_writeback_state *state);
void
drm_writeback_fail_screenshot(struct drm_writeback_state *state,
			      const char *err_msg);
enum writeback_screenshot_state
drm_output_get_writeback_state(struct drm_output *output);

void
drm_output_destroy(struct weston_output *output_base);
void
drm_virtual_output_destroy(struct weston_output *output_base);

static inline struct drm_output *
to_drm_output(struct weston_output *base)
{
	if (
#ifdef BUILD_DRM_VIRTUAL
	    base->destroy != drm_virtual_output_destroy &&
#endif
	    base->destroy != drm_output_destroy)
		return NULL;
	return container_of(base, struct drm_output, base);
}

static inline struct drm_backend *
to_drm_backend(struct weston_compositor *base)
{
	struct weston_backend *backend;

	wl_list_for_each(backend, &base->backend_list, link) {
		if (backend->destroy == drm_destroy)
			return container_of(backend, struct drm_backend, base);
	}

	return NULL;
}

static inline struct drm_mode *
to_drm_mode(struct weston_mode *base)
{
	return container_of(base, struct drm_mode, base);
}

static inline const char *
drm_output_get_plane_type_name_internal(struct drm_plane *p, struct drm_plane_handle *h)
{
	assert(!p || !h);

	if (h)
		p = h->plane;

	switch (p->type) {
	case WDRM_PLANE_TYPE_PRIMARY:
		return "primary";
	case WDRM_PLANE_TYPE_CURSOR:
		return "cursor";
	case WDRM_PLANE_TYPE_OVERLAY:
		if (!h)
			return "overlay(no subtype)";

		switch (h->subtype) {
		case PLANE_SUBTYPE_OVERLAY_ONLY:
			return "overlay";
		case PLANE_SUBTYPE_UNDERLAY_ONLY:
			return "underlay";
		case PLANE_SUBTYPE_BOTH:
			return "over/underlay";
		}
		// fall through
	default:
		assert(0);
		break;
	}
}

static inline const char *
drm_output_get_plane_type_name(struct drm_plane *p)
{
	return drm_output_get_plane_type_name_internal(p, NULL);
}

static inline const char *
drm_output_get_handle_type_name(struct drm_plane_handle *h)
{
	return drm_output_get_plane_type_name_internal(NULL, h);
}

struct drm_crtc *
drm_crtc_find(struct drm_device *device, uint32_t crtc_id);

bool
drm_crtc_supports_background_color(struct drm_crtc *crtc);

struct drm_head *
drm_head_find_by_connector(struct drm_backend *backend, struct drm_device *device, uint32_t connector_id);

void
drm_free_display_info(struct di_info **display_info);

uint64_t
drm_rotation_from_output_transform(struct drm_plane *plane,
				   enum wl_output_transform ot);

static inline bool
drm_paint_node_transform_supported(struct weston_paint_node *node, struct drm_plane *plane)
{
	/* if false, the transform doesn't map to any of the standard
	 * (ie: 90 degree) output transformations. */
	if (!node->valid_transform)
		return false;

	if (drm_rotation_from_output_transform(plane, node->transform) == 0)
		return false;

	return true;
}

int
drm_mode_ensure_blob(struct drm_device *device, struct drm_mode *mode);

struct drm_mode *
drm_output_choose_mode(struct drm_output *output,
		       struct weston_mode *target_mode);
void
update_head_from_connector(struct drm_head *head);

void
drm_mode_list_destroy(struct drm_device *device, struct wl_list *mode_list);

void
drm_output_print_modes(struct drm_output *output);

int
drm_output_set_mode(struct weston_output *base,
		    enum weston_drm_backend_output_mode mode,
		    const char *modeline);

void
drm_property_info_populate(struct drm_device *device,
		           const struct drm_property_info *src,
			   struct drm_property_info *info,
			   unsigned int num_infos,
			   drmModeObjectProperties *props);
uint64_t
drm_property_get_value(struct drm_property_info *info,
		       const drmModeObjectProperties *props,
		       uint64_t def);
uint64_t *
drm_property_get_range_values(struct drm_property_info *info,
			      const drmModeObjectProperties *props);
int
drm_plane_populate_formats(struct drm_plane *plane, const drmModePlane *kplane,
			   const drmModeObjectProperties *props,
			   const bool use_modifiers);
void
drm_property_info_free(struct drm_property_info *info, int num_props);

extern struct drm_property_enum_info plane_type_enums[];
extern const struct drm_property_info plane_props[];
extern struct drm_property_enum_info dpms_state_enums[];
extern struct drm_property_enum_info content_protection_enums[];
extern struct drm_property_enum_info hdcp_content_type_enums[];
extern const struct drm_property_info connector_props[];
extern const struct drm_property_info crtc_props[];

int
init_kms_caps(struct drm_device *device);

int
drm_pending_state_test(struct drm_pending_state *pending_state);
int
drm_pending_state_apply(struct drm_pending_state *pending_state);
int
drm_pending_state_apply_sync(struct drm_pending_state *pending_state);

void
drm_output_update_msc(struct drm_output *output, unsigned int seq);
void
drm_output_update_complete(struct drm_output *output, uint32_t flags,
			   unsigned int sec, unsigned int usec);
int
on_drm_input(int fd, uint32_t mask, void *data);

struct drm_fb *
drm_fb_ref(struct drm_fb *fb);
void
drm_fb_unref(struct drm_fb *fb);

struct drm_fb *
drm_fb_create_dumb(struct drm_device *device, int width, int height,
		   uint32_t format);

struct drm_fb *
drm_fb_get_from_dmabuf(struct linux_dmabuf_buffer *dmabuf,
		       struct drm_device *device, bool is_opaque,
		       uint32_t *try_view_on_plane_failure_reasons);
struct drm_fb *
drm_fb_get_from_bo(struct gbm_bo *bo, struct drm_device *device,
		   bool is_opaque, enum drm_fb_type type);

int
drm_output_ensure_hdr_output_metadata_blob(struct drm_output *output);

enum wdrm_colorspace
wdrm_colorspace_from_output(struct weston_output *output);

#ifdef BUILD_DRM_GBM
extern struct drm_fb *
drm_fb_get_from_paint_node(struct drm_output_state *state,
			   struct weston_paint_node *pnode,
			   uint32_t *try_view_on_plane_failure_reasons);

extern bool
drm_can_scanout_dmabuf(struct weston_backend *backend,
		       struct linux_dmabuf_buffer *dmabuf);

struct drm_fb *
drm_fb_get_from_dmabuf_attributes(struct dmabuf_attributes *attributes,
				  struct drm_device *device, bool is_opaque,
				  bool direct_display, bool is_internal,
				  uint32_t *try_view_on_plane_failure_reasons);
#else
static inline struct drm_fb *
drm_fb_get_from_paint_node(struct drm_output_state *state,
			   struct weston_paint_node *pnode,
			   uint32_t *try_view_on_plane_failure_reasons)
{
	return NULL;
}
static inline bool
drm_can_scanout_dmabuf(struct weston_backend *backend,
		       struct linux_dmabuf_buffer *dmabuf)
{
	return false;
}
#endif

struct drm_pending_state *
drm_pending_state_alloc(struct drm_device *device);
void
drm_pending_state_free(struct drm_pending_state *pending_state);
struct drm_output_state *
drm_pending_state_get_output(struct drm_pending_state *pending_state,
			     struct drm_output *output);


/**
 * Mode for drm_output_state_duplicate.
 */
enum drm_output_state_duplicate_mode {
	DRM_OUTPUT_STATE_CLEAR_PLANES, /**< reset all planes to off */
	DRM_OUTPUT_STATE_PRESERVE_PLANES, /**< preserve plane state */
};

struct drm_output_state *
drm_output_state_alloc(struct drm_output *output);

struct drm_output_state *
drm_output_state_duplicate(struct drm_output_state *src,
			   struct drm_pending_state *pending_state,
			   enum drm_output_state_duplicate_mode plane_mode);
void
drm_output_state_free(struct drm_output_state *state);
struct drm_plane_state *
drm_output_state_get_plane(struct drm_output_state *state_output,
			   struct drm_plane *plane);
struct drm_plane_state *
drm_output_state_get_existing_plane(struct drm_output_state *state_output,
				    struct drm_plane *plane);



struct drm_plane_state *
drm_plane_state_alloc(struct drm_output_state *state_output,
		      struct drm_plane *plane);
struct drm_plane_state *
drm_plane_state_duplicate(struct drm_output_state *state_output,
			  struct drm_plane_state *src);
void
drm_plane_state_free(struct drm_plane_state *state, bool force);
void
drm_plane_state_put_back(struct drm_plane_state *state);
void
drm_plane_state_coords_for_paint_node(struct drm_plane_state *state,
				      struct weston_paint_node *node,
				      uint64_t zpos);
void
drm_plane_reset_state(struct drm_plane *plane);

void
drm_assign_planes(struct weston_output *output_base);

bool
drm_plane_is_available(struct drm_plane *plane, struct drm_output *output);

bool
drm_plane_supports_color_encoding(struct drm_plane *plane,
				  enum wdrm_plane_color_encoding encoding);

bool
drm_plane_supports_color_range(struct drm_plane *plane,
			       enum wdrm_plane_color_range range);

void
drm_output_render(struct drm_output_state *state);

int
parse_gbm_format(const char *s, const struct pixel_format_info *default_format,
		 const struct pixel_format_info **format);

struct drm_plane_handle *
drm_plane_create_handle(struct drm_plane *plane, struct drm_output *output);

void
drm_plane_destroy_handle(struct drm_plane_handle *plane);

#ifdef BUILD_DRM_VIRTUAL
extern int
drm_backend_init_virtual_output_api(struct weston_compositor *compositor);
#else
inline static int
drm_backend_init_virtual_output_api(struct weston_compositor *compositor)
{
	return 0;
}
#endif

#ifdef BUILD_DRM_GBM
int
init_egl(struct drm_backend *b);

int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b);

void
drm_output_fini_egl(struct drm_output *output);

struct drm_fb *
drm_output_render_gl(struct drm_output_state *state, pixman_region32_t *damage);

int
init_vulkan(struct drm_backend *b);

int
drm_output_init_vulkan(struct drm_output *output, struct drm_backend *b);

void
drm_output_fini_vulkan(struct drm_output *output);

struct drm_fb *
drm_output_render_vulkan(struct drm_output_state *state, pixman_region32_t *damage);

#else
inline static int
init_egl(struct drm_backend *b)
{
	weston_log("Compiled without GBM/EGL support\n");
	return -1;
}

inline static int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b)
{
	return -1;
}

inline static void
drm_output_fini_egl(struct drm_output *output)
{
}

inline static struct drm_fb *
drm_output_render_gl(struct drm_output_state *state, pixman_region32_t *damage)
{
	return NULL;
}

inline static int
init_vulkan(struct drm_backend *b)
{
	weston_log("Compiled without GBM support\n");
	return -1;
}

inline static int
drm_output_init_vulkan(struct drm_output *output, struct drm_backend *b)
{
	return -1;
}

inline static void
drm_output_fini_vulkan(struct drm_output *output)
{
}

inline static struct drm_fb *
drm_output_render_vulkan(struct drm_output_state *state, pixman_region32_t *damage)
{
	return NULL;
}

#endif
