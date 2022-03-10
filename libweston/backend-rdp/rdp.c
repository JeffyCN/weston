/*
 * Copyright © 2013 Hardening <rdp.effort@gmail.com>
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

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>

#include "rdp.h"

#include <winpr/input.h>
#include <winpr/ssl.h>

#include "shared/timespec-util.h"
#include <libweston/libweston.h>
#include <libweston/backend-rdp.h>
#include "pixman-renderer.h"

/* This can be removed when we bump FreeRDP dependency past 3.0.0 in the future */
#ifndef KBD_PERSIAN
#define KBD_PERSIAN 0x50429
#endif

static void
rdp_peer_refresh_rfx(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height, nrects, i;
	pixman_box32_t *region, *rects;
	uint32_t *ptr;
	RFX_RECT *rfxRect;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND cmd = { 0 };
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	Stream_Clear(context->encode_stream);
	Stream_SetPosition(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

	cmd.skipCompression = TRUE;
	cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
	cmd.destLeft = damage->extents.x1;
	cmd.destTop = damage->extents.y1;
	cmd.destRight = damage->extents.x2;
	cmd.destBottom = damage->extents.y2;
	cmd.bmp.bpp = 32;
	cmd.bmp.codecID = peer->settings->RemoteFxCodecId;
	cmd.bmp.width = width;
	cmd.bmp.height = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * (pixman_image_get_stride(image) / sizeof(uint32_t));

	rects = pixman_region32_rectangles(damage, &nrects);
	context->rfx_rects = realloc(context->rfx_rects, nrects * sizeof *rfxRect);

	for (i = 0; i < nrects; i++) {
		region = &rects[i];
		rfxRect = &context->rfx_rects[i];

		rfxRect->x = (region->x1 - damage->extents.x1);
		rfxRect->y = (region->y1 - damage->extents.y1);
		rfxRect->width = (region->x2 - region->x1);
		rfxRect->height = (region->y2 - region->y1);
	}

	rfx_compose_message(context->rfx_context, context->encode_stream, context->rfx_rects, nrects,
			(BYTE *)ptr, width, height,
			pixman_image_get_stride(image)
	);

	cmd.bmp.bitmapDataLength = Stream_GetPosition(context->encode_stream);
	cmd.bmp.bitmapData = Stream_Buffer(context->encode_stream);

	update->SurfaceBits(update->context, &cmd);
}


static void
rdp_peer_refresh_nsc(pixman_region32_t *damage, pixman_image_t *image, freerdp_peer *peer)
{
	int width, height;
	uint32_t *ptr;
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND cmd = { 0 };
	RdpPeerContext *context = (RdpPeerContext *)peer->context;

	Stream_Clear(context->encode_stream);
	Stream_SetPosition(context->encode_stream, 0);

	width = (damage->extents.x2 - damage->extents.x1);
	height = (damage->extents.y2 - damage->extents.y1);

	cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
	cmd.skipCompression = TRUE;
	cmd.destLeft = damage->extents.x1;
	cmd.destTop = damage->extents.y1;
	cmd.destRight = damage->extents.x2;
	cmd.destBottom = damage->extents.y2;
	cmd.bmp.bpp = 32;
	cmd.bmp.codecID = peer->settings->NSCodecId;
	cmd.bmp.width = width;
	cmd.bmp.height = height;

	ptr = pixman_image_get_data(image) + damage->extents.x1 +
				damage->extents.y1 * (pixman_image_get_stride(image) / sizeof(uint32_t));

	nsc_compose_message(context->nsc_context, context->encode_stream, (BYTE *)ptr,
			width, height,
			pixman_image_get_stride(image));

	cmd.bmp.bitmapDataLength = Stream_GetPosition(context->encode_stream);
	cmd.bmp.bitmapData = Stream_Buffer(context->encode_stream);

	update->SurfaceBits(update->context, &cmd);
}

static void
pixman_image_flipped_subrect(const pixman_box32_t *rect, pixman_image_t *img, BYTE *dest)
{
	int stride = pixman_image_get_stride(img);
	int h;
	int toCopy = (rect->x2 - rect->x1) * 4;
	int height = (rect->y2 - rect->y1);
	const BYTE *src = (const BYTE *)pixman_image_get_data(img);
	src += ((rect->y2-1) * stride) + (rect->x1 * 4);

	for (h = 0; h < height; h++, src -= stride, dest += toCopy)
		   memcpy(dest, src, toCopy);
}

static void
rdp_peer_refresh_raw(pixman_region32_t *region, pixman_image_t *image, freerdp_peer *peer)
{
	rdpUpdate *update = peer->update;
	SURFACE_BITS_COMMAND cmd = { 0 };
	SURFACE_FRAME_MARKER marker;
	pixman_box32_t *rect, subrect;
	int nrects, i;
	int heightIncrement, remainingHeight, top;

	rect = pixman_region32_rectangles(region, &nrects);
	if (!nrects)
		return;

	marker.frameId++;
	marker.frameAction = SURFACECMD_FRAMEACTION_BEGIN;
	update->SurfaceFrameMarker(peer->context, &marker);

	cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
	cmd.bmp.bpp = 32;
	cmd.bmp.codecID = 0;

	for (i = 0; i < nrects; i++, rect++) {
		/*weston_log("rect(%d,%d, %d,%d)\n", rect->x1, rect->y1, rect->x2, rect->y2);*/
		cmd.destLeft = rect->x1;
		cmd.destRight = rect->x2;
		cmd.bmp.width = (rect->x2 - rect->x1);

		heightIncrement = peer->settings->MultifragMaxRequestSize / (16 + cmd.bmp.width * 4);
		remainingHeight = rect->y2 - rect->y1;
		top = rect->y1;

		subrect.x1 = rect->x1;
		subrect.x2 = rect->x2;

		while (remainingHeight) {
			   cmd.bmp.height = (remainingHeight > heightIncrement) ? heightIncrement : remainingHeight;
			   cmd.destTop = top;
			   cmd.destBottom = top + cmd.bmp.height;
			   cmd.bmp.bitmapDataLength = cmd.bmp.width * cmd.bmp.height * 4;
			   cmd.bmp.bitmapData = (BYTE *)realloc(cmd.bmp.bitmapData, cmd.bmp.bitmapDataLength);

			   subrect.y1 = top;
			   subrect.y2 = top + cmd.bmp.height;
			   pixman_image_flipped_subrect(&subrect, image, cmd.bmp.bitmapData);

			   /*weston_log("*  sending (%d,%d, %d,%d)\n", subrect.x1, subrect.y1, subrect.x2, subrect.y2); */
			   update->SurfaceBits(peer->context, &cmd);

			   remainingHeight -= cmd.bmp.height;
			   top += cmd.bmp.height;
		}
	}

	free(cmd.bmp.bitmapData);

	marker.frameAction = SURFACECMD_FRAMEACTION_END;
	update->SurfaceFrameMarker(peer->context, &marker);
}

static void
rdp_peer_refresh_region(pixman_region32_t *region, freerdp_peer *peer)
{
	RdpPeerContext *context = (RdpPeerContext *)peer->context;
	struct rdp_output *output = context->rdpBackend->output;
	rdpSettings *settings = peer->settings;

	if (settings->RemoteFxCodec)
		rdp_peer_refresh_rfx(region, output->shadow_surface, peer);
	else if (settings->NSCodec)
		rdp_peer_refresh_nsc(region, output->shadow_surface, peer);
	else
		rdp_peer_refresh_raw(region, output->shadow_surface, peer);
}

static int
rdp_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);

	return 0;
}

static int
rdp_output_repaint(struct weston_output *output_base, pixman_region32_t *damage)
{
	struct rdp_output *output = container_of(output_base, struct rdp_output, base);
	struct weston_compositor *ec = output->base.compositor;
	struct rdp_peers_item *outputPeer;
	struct timespec now, target;
	int refresh_nsec = millihz_to_nsec(output_base->current_mode->refresh);
	int refresh_msec = refresh_nsec / 1000000;
	int next_frame_delta;

	/* Calculate the time we should complete this frame such that frames
	   are spaced out by the specified monitor refresh. Note that our timer
	   mechanism only has msec precision, so we won't exactly hit our
	   target refresh rate.
	 */
	weston_compositor_read_presentation_clock(ec, &now);

	timespec_add_nsec(&target, &output_base->frame_time, refresh_nsec);

	next_frame_delta = (int)timespec_sub_to_msec(&target, &now);
	if (next_frame_delta < 1 || next_frame_delta > refresh_msec) {
		next_frame_delta = refresh_msec;
	}

	pixman_renderer_output_set_buffer(output_base, output->shadow_surface);
	ec->renderer->repaint_output(&output->base, damage);

	if (pixman_region32_not_empty(damage)) {
		wl_list_for_each(outputPeer, &output->peers, link) {
			if ((outputPeer->flags & RDP_PEER_ACTIVATED) &&
					(outputPeer->flags & RDP_PEER_OUTPUT_ENABLED))
			{
				rdp_peer_refresh_region(damage, outputPeer->peer);
			}
		}
	}

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	wl_event_source_timer_update(output->finish_frame_timer, next_frame_delta);
	return 0;
}

static int
finish_frame_handler(void *data)
{
	struct rdp_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static struct weston_mode *
rdp_insert_new_mode(struct weston_output *output, int width, int height, int rate)
{
	struct weston_mode *ret;
	ret = zalloc(sizeof *ret);
	if (!ret)
		return NULL;
	ret->width = width;
	ret->height = height;
	ret->refresh = rate;
	wl_list_insert(&output->mode_list, &ret->link);
	return ret;
}

static struct weston_mode *
ensure_matching_mode(struct weston_output *output, struct weston_mode *target)
{
	struct rdp_backend *b = to_rdp_backend(output->compositor);
	struct weston_mode *local;

	wl_list_for_each(local, &output->mode_list, link) {
		if ((local->width == target->width) && (local->height == target->height))
			return local;
	}

	return rdp_insert_new_mode(output, target->width, target->height, b->rdp_monitor_refresh_rate);
}

static int
rdp_switch_mode(struct weston_output *output, struct weston_mode *target_mode)
{
	struct rdp_output *rdpOutput = container_of(output, struct rdp_output, base);
	struct rdp_backend *rdpBackend = to_rdp_backend(output->compositor);
	struct rdp_peers_item *rdpPeer;
	rdpSettings *settings;
	pixman_image_t *new_shadow_buffer;
	struct weston_mode *local_mode;
	const struct pixman_renderer_output_options options = { .use_shadow = true, };

	local_mode = ensure_matching_mode(output, target_mode);
	if (!local_mode) {
		rdp_debug(rdpBackend, "mode %dx%d not available\n", target_mode->width, target_mode->height);
		return -ENOENT;
	}

	if (local_mode == output->current_mode)
		return 0;

	output->current_mode->flags &= ~WL_OUTPUT_MODE_CURRENT;

	output->current_mode = local_mode;
	output->current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	pixman_renderer_output_destroy(output);
	pixman_renderer_output_create(output, &options);

	new_shadow_buffer = pixman_image_create_bits(PIXMAN_x8r8g8b8, target_mode->width,
			target_mode->height, 0, target_mode->width * 4);
	pixman_image_composite32(PIXMAN_OP_SRC, rdpOutput->shadow_surface, 0, new_shadow_buffer,
			0, 0, 0, 0, 0, 0, target_mode->width, target_mode->height);
	pixman_image_unref(rdpOutput->shadow_surface);
	rdpOutput->shadow_surface = new_shadow_buffer;

	wl_list_for_each(rdpPeer, &rdpOutput->peers, link) {
		settings = rdpPeer->peer->settings;
		if (settings->DesktopWidth == (UINT32)target_mode->width &&
				settings->DesktopHeight == (UINT32)target_mode->height)
			continue;

		if (!settings->DesktopResize) {
			/* too bad this peer does not support desktop resize */
			rdpPeer->peer->Close(rdpPeer->peer);
		} else {
			settings->DesktopWidth = target_mode->width;
			settings->DesktopHeight = target_mode->height;
			rdpPeer->peer->update->DesktopResize(rdpPeer->peer->context);
		}
	}
	return 0;
}

static int
rdp_output_set_size(struct weston_output *base,
		    int width, int height)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *rdpBackend = to_rdp_backend(base->compositor);
	struct weston_head *head;
	struct weston_mode *currentMode;
	struct weston_mode initMode;

	/* We can only be called once. */
	assert(!output->base.current_mode);

	wl_list_for_each(head, &output->base.head_list, output_link) {
		weston_head_set_monitor_strings(head, "weston", "rdp", NULL);

		/* This is a virtual output, so report a zero physical size.
		 * It's better to let frontends/clients use their defaults. */
		weston_head_set_physical_size(head, 0, 0);
	}

	wl_list_init(&output->peers);

	initMode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	initMode.width = width;
	initMode.height = height;
	initMode.refresh = rdpBackend->rdp_monitor_refresh_rate;
	currentMode = ensure_matching_mode(&output->base, &initMode);
	if (!currentMode)
		return -1;

	output->base.current_mode = output->base.native_mode = currentMode;

	output->base.start_repaint_loop = rdp_output_start_repaint_loop;
	output->base.repaint = rdp_output_repaint;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = rdp_switch_mode;

	return 0;
}

static int
rdp_output_enable(struct weston_output *base)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *b = to_rdp_backend(base->compositor);
	struct wl_event_loop *loop;
	const struct pixman_renderer_output_options options = {
		.use_shadow = true,
	};

	output->shadow_surface = pixman_image_create_bits(PIXMAN_x8r8g8b8,
							  output->base.current_mode->width,
							  output->base.current_mode->height,
							  NULL,
							  output->base.current_mode->width * 4);
	if (output->shadow_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		return -1;
	}

	if (pixman_renderer_output_create(&output->base, &options) < 0) {
		pixman_image_unref(output->shadow_surface);
		return -1;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer = wl_event_loop_add_timer(loop, finish_frame_handler, output);

	b->output = output;

	return 0;
}

static int
rdp_output_disable(struct weston_output *base)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *b = to_rdp_backend(base->compositor);

	if (!output->base.enabled)
		return 0;

	pixman_image_unref(output->shadow_surface);
	pixman_renderer_output_destroy(&output->base);

	wl_event_source_remove(output->finish_frame_timer);
	b->output = NULL;

	return 0;
}

static void
rdp_output_destroy(struct weston_output *base)
{
	struct rdp_output *output = to_rdp_output(base);

	rdp_output_disable(&output->base);
	weston_output_release(&output->base);

	free(output);
}

static struct weston_output *
rdp_output_create(struct weston_compositor *compositor, const char *name)
{
	struct rdp_output *output;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;

	weston_output_init(&output->base, compositor, name);

	output->base.destroy = rdp_output_destroy;
	output->base.disable = rdp_output_disable;
	output->base.enable = rdp_output_enable;
	output->base.attach_head = NULL;

	weston_compositor_add_pending_output(&output->base, compositor);

	return &output->base;
}

static int
rdp_head_create(struct weston_compositor *compositor, const char *name)
{
	struct rdp_head *head;

	head = zalloc(sizeof *head);
	if (!head)
		return -1;

	weston_head_init(&head->base, name);
	weston_head_set_connection_status(&head->base, true);
	weston_compositor_add_head(compositor, &head->base);

	return 0;
}

static void
rdp_head_destroy(struct rdp_head *head)
{
	weston_head_release(&head->base);
	free(head);
}

static void
rdp_destroy(struct weston_compositor *ec)
{
	struct rdp_backend *b = to_rdp_backend(ec);
	struct weston_head *base, *next;
	struct rdp_peers_item *rdp_peer, *tmp;
	int i;

	wl_list_for_each_safe(rdp_peer, tmp, &b->output->peers, link) {
		freerdp_peer* client = rdp_peer->peer;

		client->Disconnect(client);
		freerdp_peer_context_free(client);
		freerdp_peer_free(client);
	}

	for (i = 0; i < MAX_FREERDP_FDS; i++)
		if (b->listener_events[i])
			wl_event_source_remove(b->listener_events[i]);

	if (b->debug) {
		weston_log_scope_destroy(b->debug);
		b->debug = NULL;
	}

	if (b->verbose) {
		weston_log_scope_destroy(b->verbose);
		b->verbose = NULL;
	}

	weston_compositor_shutdown(ec);

	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link)
		rdp_head_destroy(to_rdp_head(base));

	freerdp_listener_free(b->listener);

	free(b->server_cert);
	free(b->server_key);
	free(b->rdp_key);
	free(b);
}

static
int rdp_listener_activity(int fd, uint32_t mask, void *data)
{
	freerdp_listener* instance = (freerdp_listener *)data;

	if (!(mask & WL_EVENT_READABLE))
		return 0;
	if (!instance->CheckFileDescriptor(instance)) {
		weston_log("failed to check FreeRDP file descriptor\n");
		return -1;
	}
	return 0;
}

static
int rdp_implant_listener(struct rdp_backend *b, freerdp_listener* instance)
{
	int i, fd;
	int rcount = 0;
	void* rfds[MAX_FREERDP_FDS];
	struct wl_event_loop *loop;

	if (!instance->GetFileDescriptor(instance, rfds, &rcount)) {
		weston_log("Failed to get FreeRDP file descriptor\n");
		return -1;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);
		b->listener_events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_listener_activity, instance);
	}

	for ( ; i < MAX_FREERDP_FDS; i++)
		b->listener_events[i] = 0;
	return 0;
}


static BOOL
rdp_peer_context_new(freerdp_peer* client, RdpPeerContext* context)
{
	context->item.peer = client;
	context->item.flags = RDP_PEER_OUTPUT_ENABLED;

	context->rfx_context = rfx_context_new(TRUE);
	if (!context->rfx_context)
		return FALSE;

	context->rfx_context->mode = RLGR3;
	context->rfx_context->width = client->settings->DesktopWidth;
	context->rfx_context->height = client->settings->DesktopHeight;
	rfx_context_set_pixel_format(context->rfx_context, DEFAULT_PIXEL_FORMAT);

	context->nsc_context = nsc_context_new();
	if (!context->nsc_context)
		goto out_error_nsc;

	nsc_context_set_parameters(context->nsc_context, NSC_COLOR_FORMAT, DEFAULT_PIXEL_FORMAT);
	context->encode_stream = Stream_New(NULL, 65536);
	if (!context->encode_stream)
		goto out_error_stream;

	return TRUE;

out_error_nsc:
	rfx_context_free(context->rfx_context);
out_error_stream:
	nsc_context_free(context->nsc_context);
	return FALSE;
}

static void
rdp_peer_context_free(freerdp_peer* client, RdpPeerContext* context)
{
	int i;
	if (!context)
		return;

	wl_list_remove(&context->item.link);
	for (i = 0; i < MAX_FREERDP_FDS; i++) {
		if (context->events[i])
			wl_event_source_remove(context->events[i]);
	}

	if (context->item.flags & RDP_PEER_ACTIVATED) {
		weston_seat_release_keyboard(context->item.seat);
		weston_seat_release_pointer(context->item.seat);
		weston_seat_release(context->item.seat);
		free(context->item.seat);
	}

	Stream_Free(context->encode_stream, TRUE);
	nsc_context_free(context->nsc_context);
	rfx_context_free(context->rfx_context);
	free(context->rfx_rects);
}


static int
rdp_client_activity(int fd, uint32_t mask, void *data)
{
	freerdp_peer* client = (freerdp_peer *)data;

	if (!client->CheckFileDescriptor(client)) {
		weston_log("unable to checkDescriptor for %p\n", client);
		goto out_clean;
	}
	return 0;

out_clean:
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);
	return 0;
}

static BOOL
xf_peer_capabilities(freerdp_peer* client)
{
	return TRUE;
}

struct rdp_to_xkb_keyboard_layout {
	UINT32 rdpLayoutCode;
	const char *xkbLayout;
	const char *xkbVariant;
};

/* table reversed from
	https://github.com/awakecoding/FreeRDP/blob/master/libfreerdp/locale/xkb_layout_ids.c#L811 */
static const
struct rdp_to_xkb_keyboard_layout rdp_keyboards[] = {
	{KBD_ARABIC_101, "ara", 0},
	{KBD_BULGARIAN, 0, 0},
	{KBD_CHINESE_TRADITIONAL_US, 0},
	{KBD_CZECH, "cz", 0},
	{KBD_CZECH_PROGRAMMERS, "cz", "bksl"},
	{KBD_CZECH_QWERTY, "cz", "qwerty"},
	{KBD_DANISH, "dk", 0},
	{KBD_GERMAN, "de", 0},
	{KBD_GERMAN_NEO, "de", "neo"},
	{KBD_GERMAN_IBM, "de", "qwerty"},
	{KBD_GREEK, "gr", 0},
	{KBD_GREEK_220, "gr", "simple"},
	{KBD_GREEK_319, "gr", "extended"},
	{KBD_GREEK_POLYTONIC, "gr", "polytonic"},
	{KBD_US, "us", 0},
	{KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L, "ara", "buckwalter"},
	{KBD_SPANISH, "es", 0},
	{KBD_SPANISH_VARIATION, "es", "nodeadkeys"},
	{KBD_FINNISH, "fi", 0},
	{KBD_FRENCH, "fr", 0},
	{KBD_HEBREW, "il", 0},
	{KBD_HUNGARIAN, "hu", 0},
	{KBD_HUNGARIAN_101_KEY, "hu", "standard"},
	{KBD_ICELANDIC, "is", 0},
	{KBD_ITALIAN, "it", 0},
	{KBD_ITALIAN_142, "it", "nodeadkeys"},
	{KBD_JAPANESE, "jp", 0},
	{KBD_JAPANESE_INPUT_SYSTEM_MS_IME2002, "jp", 0},
	{KBD_KOREAN, "kr", 0},
	{KBD_KOREAN_INPUT_SYSTEM_IME_2000, "kr", "kr104"},
	{KBD_DUTCH, "nl", 0},
	{KBD_NORWEGIAN, "no", 0},
	{KBD_POLISH_PROGRAMMERS, "pl", 0},
	{KBD_POLISH_214, "pl", "qwertz"},
	{KBD_ROMANIAN, "ro", 0},
	{KBD_RUSSIAN, "ru", 0},
	{KBD_RUSSIAN_TYPEWRITER, "ru", "typewriter"},
	{KBD_CROATIAN, "hr", 0},
	{KBD_SLOVAK, "sk", 0},
	{KBD_SLOVAK_QWERTY, "sk", "qwerty"},
	{KBD_ALBANIAN, 0, 0},
	{KBD_SWEDISH, "se", 0},
	{KBD_THAI_KEDMANEE, "th", 0},
	{KBD_THAI_KEDMANEE_NON_SHIFTLOCK, "th", "tis"},
	{KBD_TURKISH_Q, "tr", 0},
	{KBD_TURKISH_F, "tr", "f"},
	{KBD_URDU, "in", "urd-phonetic3"},
	{KBD_UKRAINIAN, "ua", 0},
	{KBD_BELARUSIAN, "by", 0},
	{KBD_SLOVENIAN, "si", 0},
	{KBD_ESTONIAN, "ee", 0},
	{KBD_LATVIAN, "lv", 0},
	{KBD_LITHUANIAN_IBM, "lt", "ibm"},
	{KBD_FARSI, "ir", "pes"},
	{KBD_PERSIAN, "af", "basic"},
	{KBD_VIETNAMESE, "vn", 0},
	{KBD_ARMENIAN_EASTERN, "am", 0},
	{KBD_AZERI_LATIN, 0, 0},
	{KBD_FYRO_MACEDONIAN, "mk", 0},
	{KBD_GEORGIAN, "ge", 0},
	{KBD_FAEROESE, 0, 0},
	{KBD_DEVANAGARI_INSCRIPT, 0, 0},
	{KBD_MALTESE_47_KEY, 0, 0},
	{KBD_NORWEGIAN_WITH_SAMI, "no", "smi"},
	{KBD_KAZAKH, "kz", 0},
	{KBD_KYRGYZ_CYRILLIC, "kg", "phonetic"},
	{KBD_TATAR, "ru", "tt"},
	{KBD_BENGALI, "bd", 0},
	{KBD_BENGALI_INSCRIPT, "bd", "probhat"},
	{KBD_PUNJABI, 0, 0},
	{KBD_GUJARATI, "in", "guj"},
	{KBD_TAMIL, "in", "tam"},
	{KBD_TELUGU, "in", "tel"},
	{KBD_KANNADA, "in", "kan"},
	{KBD_MALAYALAM, "in", "mal"},
	{KBD_HINDI_TRADITIONAL, "in", 0},
	{KBD_MARATHI, 0, 0},
	{KBD_MONGOLIAN_CYRILLIC, "mn", 0},
	{KBD_UNITED_KINGDOM_EXTENDED, "gb", "intl"},
	{KBD_SYRIAC, "syc", 0},
	{KBD_SYRIAC_PHONETIC, "syc", "syc_phonetic"},
	{KBD_NEPALI, "np", 0},
	{KBD_PASHTO, "af", "ps"},
	{KBD_DIVEHI_PHONETIC, 0, 0},
	{KBD_LUXEMBOURGISH, 0, 0},
	{KBD_MAORI, "mao", 0},
	{KBD_CHINESE_SIMPLIFIED_US, 0, 0},
	{KBD_SWISS_GERMAN, "ch", "de_nodeadkeys"},
	{KBD_UNITED_KINGDOM, "gb", 0},
	{KBD_LATIN_AMERICAN, "latam", 0},
	{KBD_BELGIAN_FRENCH, "be", 0},
	{KBD_BELGIAN_PERIOD, "be", "oss_sundeadkeys"},
	{KBD_PORTUGUESE, "pt", 0},
	{KBD_SERBIAN_LATIN, "rs", 0},
	{KBD_AZERI_CYRILLIC, "az", "cyrillic"},
	{KBD_SWEDISH_WITH_SAMI, "se", "smi"},
	{KBD_UZBEK_CYRILLIC, "af", "uz"},
	{KBD_INUKTITUT_LATIN, "ca", "ike"},
	{KBD_CANADIAN_FRENCH_LEGACY, "ca", "fr-legacy"},
	{KBD_SERBIAN_CYRILLIC, "rs", 0},
	{KBD_CANADIAN_FRENCH, "ca", "fr-legacy"},
	{KBD_SWISS_FRENCH, "ch", "fr"},
	{KBD_BOSNIAN, "ba", 0},
	{KBD_IRISH, 0, 0},
	{KBD_BOSNIAN_CYRILLIC, "ba", "us"},
	{KBD_UNITED_STATES_DVORAK, "us", "dvorak"},
	{KBD_PORTUGUESE_BRAZILIAN_ABNT2, "br", "abnt2"},
	{KBD_CANADIAN_MULTILINGUAL_STANDARD, "ca", "multix"},
	{KBD_GAELIC, "ie", "CloGaelach"},

	{0x00000000, 0, 0},
};

static BOOL
xf_peer_activate(freerdp_peer* client)
{
	RdpPeerContext *peerCtx;
	struct rdp_backend *b;
	struct rdp_output *output;
	rdpSettings *settings;
	rdpPointerUpdate *pointer;
	struct rdp_peers_item *peersItem;
	struct xkb_rule_names xkbRuleNames;
	struct xkb_keymap *keymap;
	struct weston_output *weston_output;
	int i;
	pixman_box32_t box;
	pixman_region32_t damage;
	char seat_name[50];
	POINTER_SYSTEM_UPDATE pointer_system;

	peerCtx = (RdpPeerContext *)client->context;
	b = peerCtx->rdpBackend;
	peersItem = &peerCtx->item;
	output = b->output;
	settings = client->settings;

	if (!settings->SurfaceCommandsEnabled) {
		weston_log("client doesn't support required SurfaceCommands\n");
		return FALSE;
	}

	if (b->force_no_compression && settings->CompressionEnabled) {
		rdp_debug(b, "Forcing compression off\n");
		settings->CompressionEnabled = FALSE;
	}

	if (output->base.width != (int)settings->DesktopWidth ||
			output->base.height != (int)settings->DesktopHeight)
	{
		if (b->no_clients_resize) {
			/* RDP peers don't dictate their resolution to weston */
			if (!settings->DesktopResize) {
				/* peer does not support desktop resize */
				weston_log("%s: client doesn't support resizing, closing connection\n", __FUNCTION__);
				return FALSE;
			} else {
				settings->DesktopWidth = output->base.width;
				settings->DesktopHeight = output->base.height;
				client->update->DesktopResize(client->context);
			}
		} else {
			/* ask weston to adjust size */
			struct weston_mode new_mode;
			struct weston_mode *target_mode;
			new_mode.width = (int)settings->DesktopWidth;
			new_mode.height = (int)settings->DesktopHeight;
			target_mode = ensure_matching_mode(&output->base, &new_mode);
			if (!target_mode) {
				weston_log("client mode not found\n");
				return FALSE;
			}
			weston_output_mode_set_native(&output->base, target_mode, 1);
			output->base.width = new_mode.width;
			output->base.height = new_mode.height;
		}
	}

	weston_output = &output->base;
	rfx_context_reset(peerCtx->rfx_context, weston_output->width, weston_output->height);
	nsc_context_reset(peerCtx->nsc_context, weston_output->width, weston_output->height);

	if (peersItem->flags & RDP_PEER_ACTIVATED)
		return TRUE;

	/* when here it's the first reactivation, we need to setup a little more */
	rdp_debug(b, "kbd_layout:0x%x kbd_type:0x%x kbd_subType:0x%x kbd_functionKeys:0x%x\n",
			settings->KeyboardLayout, settings->KeyboardType, settings->KeyboardSubType,
			settings->KeyboardFunctionKey);

	memset(&xkbRuleNames, 0, sizeof(xkbRuleNames));
	xkbRuleNames.model = "pc105";
	for (i = 0; rdp_keyboards[i].rdpLayoutCode; i++) {
		if (rdp_keyboards[i].rdpLayoutCode == settings->KeyboardLayout) {
			xkbRuleNames.layout = rdp_keyboards[i].xkbLayout;
			xkbRuleNames.variant = rdp_keyboards[i].xkbVariant;
			weston_log("%s: matching layout=%s variant=%s\n", __FUNCTION__,
					xkbRuleNames.layout, xkbRuleNames.variant);
			break;
		}
	}

	keymap = NULL;
	if (xkbRuleNames.layout) {
		keymap = xkb_keymap_new_from_names(b->compositor->xkb_context,
						   &xkbRuleNames, 0);
	}

	if (settings->ClientHostname)
		snprintf(seat_name, sizeof(seat_name), "RDP %s", settings->ClientHostname);
	else
		snprintf(seat_name, sizeof(seat_name), "RDP peer @%s", settings->ClientAddress);

	peersItem->seat = zalloc(sizeof(*peersItem->seat));
	if (!peersItem->seat) {
		xkb_keymap_unref(keymap);
		weston_log("unable to create a weston_seat\n");
		return FALSE;
	}

	weston_seat_init(peersItem->seat, b->compositor, seat_name);
	weston_seat_init_keyboard(peersItem->seat, keymap);
	xkb_keymap_unref(keymap);
	weston_seat_init_pointer(peersItem->seat);

	peersItem->flags |= RDP_PEER_ACTIVATED;

	/* disable pointer on the client side */
	pointer = client->update->pointer;
	pointer_system.type = SYSPTR_NULL;
	pointer->PointerSystem(client->context, &pointer_system);

	/* sends a full refresh */
	box.x1 = 0;
	box.y1 = 0;
	box.x2 = output->base.width;
	box.y2 = output->base.height;
	pixman_region32_init_with_extents(&damage, &box);

	rdp_peer_refresh_region(&damage, client);

	pixman_region32_fini(&damage);

	return TRUE;
}

static BOOL
xf_peer_post_connect(freerdp_peer *client)
{
	return TRUE;
}

static void
dump_mouseinput(RdpPeerContext *peerContext, UINT16 flags, UINT16 x, UINT16 y, bool is_ex)
{
	struct rdp_backend *b = peerContext->rdpBackend;

	rdp_debug_verbose(b, "RDP mouse input%s: (%d, %d): flags:%x: ", is_ex ? "_ex" : "", x, y, flags);
	if (is_ex) {
		if (flags & PTR_XFLAGS_DOWN)
			rdp_debug_verbose_continue(b, "DOWN ");
		if (flags & PTR_XFLAGS_BUTTON1)
			rdp_debug_verbose_continue(b, "XBUTTON1 ");
		if (flags & PTR_XFLAGS_BUTTON2)
			rdp_debug_verbose_continue(b, "XBUTTON2 ");
	} else {
		if (flags & PTR_FLAGS_WHEEL)
			rdp_debug_verbose_continue(b, "WHEEL ");
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			rdp_debug_verbose_continue(b, "WHEEL_NEGATIVE ");
		if (flags & PTR_FLAGS_HWHEEL)
			rdp_debug_verbose_continue(b, "HWHEEL ");
		if (flags & PTR_FLAGS_MOVE)
			rdp_debug_verbose_continue(b, "MOVE ");
		if (flags & PTR_FLAGS_DOWN)
			rdp_debug_verbose_continue(b, "DOWN ");
		if (flags & PTR_FLAGS_BUTTON1)
			rdp_debug_verbose_continue(b, "BUTTON1 ");
		if (flags & PTR_FLAGS_BUTTON2)
			rdp_debug_verbose_continue(b, "BUTTON2 ");
		if (flags & PTR_FLAGS_BUTTON3)
			rdp_debug_verbose_continue(b, "BUTTON3 ");
	}
	rdp_debug_verbose_continue(b, "\n");
}

static void
rdp_validate_button_state(RdpPeerContext *peerContext, bool pressed, uint32_t *button)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	uint32_t index;

	if (*button < BTN_LEFT || *button > BTN_EXTRA) {
		weston_log("RDP client posted invalid button event\n");
		goto ignore;
	}

	index = *button - BTN_LEFT;
	assert(index < ARRAY_LENGTH(peerContext->button_state));

	if (pressed == peerContext->button_state[index]) {
		rdp_debug_verbose(b, "%s: inconsistent button state button:%d (index:%d) pressed:%d\n",
				  __func__, *button, index, pressed);
		goto ignore;
	} else {
		peerContext->button_state[index] = pressed;
	}
	return;
ignore:
	/* ignore button input */
	*button = 0;
}

static bool
rdp_notify_wheel_scroll(RdpPeerContext *peerContext, UINT16 flags, uint32_t axis)
{
	struct weston_pointer_axis_event weston_event;
	struct rdp_backend *b = peerContext->rdpBackend;
	int ivalue;
	double value;
	struct timespec time;
	int *accumWheelRotationPrecise;
	int *accumWheelRotationDiscrete;

	/*
	* The RDP specs says the lower bits of flags contains the "the number of rotation
	* units the mouse wheel was rotated".
	*/
	ivalue = ((int)flags & 0x000000ff);
	if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
		ivalue = (0xff - ivalue) * -1;

	/*
	* Flip the scroll direction as the RDP direction is inverse of X/Wayland
	* for vertical scroll
	*/
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		ivalue *= -1;

		accumWheelRotationPrecise = &peerContext->verticalAccumWheelRotationPrecise;
		accumWheelRotationDiscrete = &peerContext->verticalAccumWheelRotationDiscrete;
	}
	else {
		accumWheelRotationPrecise = &peerContext->horizontalAccumWheelRotationPrecise;
		accumWheelRotationDiscrete = &peerContext->horizontalAccumWheelRotationDiscrete;
	}

	/*
	* Accumulate the wheel increments.
	*
	* Every 12 wheel increments, we will send an update to our Wayland
	* clients with an updated value for the wheel for smooth scrolling.
	*
	* Every 120 wheel increments, we tick one discrete wheel click.
	*
	* https://devblogs.microsoft.com/oldnewthing/20130123-00/?p=5473 explains the 120 value
	*/
	*accumWheelRotationPrecise += ivalue;
	*accumWheelRotationDiscrete += ivalue;
	rdp_debug_verbose(b, "wheel: rawValue:%d accumPrecise:%d accumDiscrete %d\n",
			  ivalue, *accumWheelRotationPrecise, *accumWheelRotationDiscrete);

	if (abs(*accumWheelRotationPrecise) >= 12) {
		value = (double)(*accumWheelRotationPrecise / 12);

		weston_event.axis = axis;
		weston_event.value = value;
		weston_event.discrete = *accumWheelRotationDiscrete / 120;
		weston_event.has_discrete = true;

		rdp_debug_verbose(b, "wheel: value:%f discrete:%d\n",
				  weston_event.value, weston_event.discrete);

		weston_compositor_get_time(&time);

		notify_axis(peerContext->item.seat, &time, &weston_event);

		*accumWheelRotationPrecise %= 12;
		*accumWheelRotationDiscrete %= 120;

		return true;
	}

	return false;
}

static BOOL
xf_mouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_output *output;
	uint32_t button = 0;
	bool need_frame = false;
	struct timespec time;

	dump_mouseinput(peerContext, flags, x, y, false);

	if (flags & PTR_FLAGS_MOVE) {
		output = peerContext->rdpBackend->output;
		if (x < output->base.width && y < output->base.height) {
			weston_compositor_get_time(&time);
			notify_motion_absolute(peerContext->item.seat, &time,
					x, y);
			need_frame = true;
		}
	}

	if (flags & PTR_FLAGS_BUTTON1)
		button = BTN_LEFT;
	else if (flags & PTR_FLAGS_BUTTON2)
		button = BTN_RIGHT;
	else if (flags & PTR_FLAGS_BUTTON3)
		button = BTN_MIDDLE;

	if (button) {
		rdp_validate_button_state(peerContext,
					  flags & PTR_FLAGS_DOWN ? true : false,
					  &button);
	}

	if (button) {
		weston_compositor_get_time(&time);
		notify_button(peerContext->item.seat, &time, button,
			(flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
		);
		need_frame = true;
	}

	/* Per RDP spec, if both PTRFLAGS_WHEEL and PTRFLAGS_HWHEEL are specified
	 * then PTRFLAGS_WHEEL takes precedent
	 */
	if (flags & PTR_FLAGS_WHEEL) {
		if (rdp_notify_wheel_scroll(peerContext, flags, WL_POINTER_AXIS_VERTICAL_SCROLL))
			need_frame = true;
	} else if (flags & PTR_FLAGS_HWHEEL) {
		if (rdp_notify_wheel_scroll(peerContext, flags, WL_POINTER_AXIS_HORIZONTAL_SCROLL))
			need_frame = true;
	}

	if (need_frame)
		notify_pointer_frame(peerContext->item.seat);

	return TRUE;
}

static BOOL
xf_extendedMouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	uint32_t button = 0;
	bool need_frame = false;
	struct rdp_output *output;
	struct timespec time;

	dump_mouseinput(peerContext, flags, x, y, true);

	if (flags & PTR_XFLAGS_BUTTON1)
		button = BTN_SIDE;
	else if (flags & PTR_XFLAGS_BUTTON2)
		button = BTN_EXTRA;

	if (button) {
		rdp_validate_button_state(peerContext,
					  flags & PTR_XFLAGS_DOWN ? true : false,
					  &button);
	}

	if (button) {
		weston_compositor_get_time(&time);
		notify_button(peerContext->item.seat, &time, button,
			      (flags & PTR_XFLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
		need_frame = true;
	}

	output = peerContext->rdpBackend->output;
	if (x < output->base.width && y < output->base.height) {
		weston_compositor_get_time(&time);
		notify_motion_absolute(peerContext->item.seat, &time, x, y);
		need_frame = true;
	}

	if (need_frame)
		notify_pointer_frame(peerContext->item.seat);

	return TRUE;
}


static BOOL
xf_input_synchronize_event(rdpInput *input, UINT32 flags)
{
	freerdp_peer *client = input->context->peer;
	RdpPeerContext *peerCtx = (RdpPeerContext *)input->context;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_output *output = peerCtx->rdpBackend->output;
	pixman_box32_t box;
	pixman_region32_t damage;

        rdp_debug_verbose(b, "RDP backend: %s ScrLk:%d, NumLk:%d, CapsLk:%d, KanaLk:%d\n",
			  __func__,
			  flags & KBD_SYNC_SCROLL_LOCK ? 1 : 0,
			  flags & KBD_SYNC_NUM_LOCK ? 1 : 0,
			  flags & KBD_SYNC_CAPS_LOCK ? 1 : 0,
			  flags & KBD_SYNC_KANA_LOCK ? 1 : 0);

	/* sends a full refresh */
	box.x1 = 0;
	box.y1 = 0;
	box.x2 = output->base.width;
	box.y2 = output->base.height;
	pixman_region32_init_with_extents(&damage, &box);

	rdp_peer_refresh_region(&damage, client);

	pixman_region32_fini(&damage);
	return TRUE;
}


static BOOL
xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code)
{
	uint32_t scan_code, vk_code, full_code;
	enum wl_keyboard_key_state keyState;
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	int notify = 0;
	struct timespec time;

	if (!(peerContext->item.flags & RDP_PEER_ACTIVATED))
		return TRUE;

	if (flags & KBD_FLAGS_DOWN) {
		keyState = WL_KEYBOARD_KEY_STATE_PRESSED;
		notify = 1;
	} else if (flags & KBD_FLAGS_RELEASE) {
		keyState = WL_KEYBOARD_KEY_STATE_RELEASED;
		notify = 1;
	}

	if (notify) {
		full_code = code;
		if (flags & KBD_FLAGS_EXTENDED)
			full_code |= KBD_FLAGS_EXTENDED;

		vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
		if (flags & KBD_FLAGS_EXTENDED)
			vk_code |= KBDEXT;

		scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);

		/*weston_log("code=%x ext=%d vk_code=%x scan_code=%x\n", code, (flags & KBD_FLAGS_EXTENDED) ? 1 : 0,
				vk_code, scan_code);*/
		weston_compositor_get_time(&time);
		notify_key(peerContext->item.seat, &time,
					scan_code - 8, keyState, STATE_UPDATE_AUTOMATIC);
	}

	return TRUE;
}

static BOOL
xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)input->context;
	struct rdp_backend *b = peerContext->rdpBackend;

	rdp_debug(b, "Client sent a unicode keyboard event (flags:0x%X code:0x%X)\n", flags, code);

	return TRUE;
}


static BOOL
xf_suppress_output(rdpContext *context, BYTE allow, const RECTANGLE_16 *area)
{
	RdpPeerContext *peerContext = (RdpPeerContext *)context;

	if (allow)
		peerContext->item.flags |= RDP_PEER_OUTPUT_ENABLED;
	else
		peerContext->item.flags &= (~RDP_PEER_OUTPUT_ENABLED);

	return TRUE;
}

static int
rdp_peer_init(freerdp_peer *client, struct rdp_backend *b)
{
	int rcount = 0;
	void *rfds[MAX_FREERDP_FDS];
	int i, fd;
	struct wl_event_loop *loop;
	rdpSettings	*settings;
	rdpInput *input;
	RdpPeerContext *peerCtx;

	client->ContextSize = sizeof(RdpPeerContext);
	client->ContextNew = (psPeerContextNew)rdp_peer_context_new;
	client->ContextFree = (psPeerContextFree)rdp_peer_context_free;
	freerdp_peer_context_new(client);

	peerCtx = (RdpPeerContext *) client->context;
	peerCtx->rdpBackend = b;

	settings = client->settings;
	/* configure security settings */
	if (b->rdp_key)
		settings->RdpKeyFile = strdup(b->rdp_key);
	if (b->tls_enabled) {
		settings->CertificateFile = strdup(b->server_cert);
		settings->PrivateKeyFile = strdup(b->server_key);
	} else {
		settings->TlsSecurity = FALSE;
	}
	settings->NlaSecurity = FALSE;

	if (!client->Initialize(client)) {
		weston_log("peer initialization failed\n");
		goto error_initialize;
	}

	settings->OsMajorType = OSMAJORTYPE_UNIX;
	settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
	settings->ColorDepth = 32;
	settings->RefreshRect = TRUE;
	settings->RemoteFxCodec = b->remotefx_codec;
	settings->NSCodec = TRUE;
	settings->FrameMarkerCommandEnabled = TRUE;
	settings->SurfaceFrameMarkerEnabled = TRUE;
	settings->HasExtendedMouseEvent = TRUE;
	settings->HasHorizontalWheel = TRUE;

	client->Capabilities = xf_peer_capabilities;
	client->PostConnect = xf_peer_post_connect;
	client->Activate = xf_peer_activate;

	client->update->SuppressOutput = (pSuppressOutput)xf_suppress_output;

	input = client->input;
	input->SynchronizeEvent = xf_input_synchronize_event;
	input->MouseEvent = xf_mouseEvent;
	input->ExtendedMouseEvent = xf_extendedMouseEvent;
	input->KeyboardEvent = xf_input_keyboard_event;
	input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

	if (!client->GetFileDescriptor(client, rfds, &rcount)) {
		weston_log("unable to retrieve client fds\n");
		goto error_initialize;
	}

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	for (i = 0; i < rcount; i++) {
		fd = (int)(long)(rfds[i]);

		peerCtx->events[i] = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				rdp_client_activity, client);
	}
	for ( ; i < MAX_FREERDP_FDS; i++)
		peerCtx->events[i] = 0;

	wl_list_insert(&b->output->peers, &peerCtx->item.link);
	return 0;

error_initialize:
	client->Close(client);
	return -1;
}


static BOOL
rdp_incoming_peer(freerdp_listener *instance, freerdp_peer *client)
{
	struct rdp_backend *b = (struct rdp_backend *)instance->param4;
	if (rdp_peer_init(client, b) < 0) {
		weston_log("error when treating incoming peer\n");
		return FALSE;
	}

	return TRUE;
}

static const struct weston_rdp_output_api api = {
	rdp_output_set_size,
};

static struct rdp_backend *
rdp_backend_create(struct weston_compositor *compositor,
		   struct weston_rdp_backend_config *config)
{
	struct rdp_backend *b;
	char *fd_str;
	char *fd_tail;
	int fd, ret;

	struct weston_head *base, *next;

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;
	b->base.destroy = rdp_destroy;
	b->base.create_output = rdp_output_create;
	b->rdp_key = config->rdp_key ? strdup(config->rdp_key) : NULL;
	b->no_clients_resize = config->no_clients_resize;
	b->force_no_compression = config->force_no_compression;
	b->remotefx_codec = config->remotefx_codec;

	b->debug = weston_compositor_add_log_scope(compositor,
						   "rdp-backend",
						   "Debug messages from RDP backend\n",
						   NULL, NULL, NULL);
	b->verbose = weston_compositor_add_log_scope(compositor,
						     "rdp-backend-verbose",
						     "Verbose debug messages from RDP backend\n",
						     NULL, NULL, NULL);

	/* After here, rdp_debug() is ready to be used */

	b->rdp_monitor_refresh_rate = config->refresh_rate * 1000;
	rdp_debug(b, "RDP backend: WESTON_RDP_MONITOR_REFRESH_RATE: %d\n", b->rdp_monitor_refresh_rate);

	compositor->backend = &b->base;

	if (config->server_cert && config->server_key) {
		b->server_cert = strdup(config->server_cert);
		b->server_key = strdup(config->server_key);
		if (!b->server_cert || !b->server_key)
			goto err_free_strings;
	}

	/* if we are listening for client connections on an external listener
	 * fd, we don't need to enforce TLS or RDP security, since FreeRDP
	 * will consider it to be a local connection */
	fd = config->external_listener_fd;
	if (fd < 0) {
		if (!b->rdp_key && (!b->server_cert || !b->server_key)) {
			weston_log("the RDP compositor requires keys and an optional certificate for RDP or TLS security ("
				   "--rdp4-key or --rdp-tls-cert/--rdp-tls-key)\n");
			goto err_free_strings;
		}
		if (b->server_cert && b->server_key) {
			b->tls_enabled = 1;
			rdp_debug(b, "TLS support activated\n");
		}
	}

	if (weston_compositor_set_presentation_clock_software(compositor) < 0)
		goto err_compositor;

	if (pixman_renderer_init(compositor) < 0)
		goto err_compositor;

	if (rdp_head_create(compositor, "rdp") < 0)
		goto err_compositor;

	compositor->capabilities |= WESTON_CAP_ARBITRARY_MODES;

	if (!config->env_socket) {
		b->listener = freerdp_listener_new();
		b->listener->PeerAccepted = rdp_incoming_peer;
		b->listener->param4 = b;
		if (fd >= 0) {
			rdp_debug(b, "Using external fd for incoming connections: %d\n", fd);

			if (!b->listener->OpenFromSocket(b->listener, fd)) {
				weston_log("RDP unable to use external listener fd: %d\n", fd);
				goto err_listener;
			}
		} else {
			if (!b->listener->Open(b->listener, config->bind_address, config->port)) {
				weston_log("RDP unable to bind socket\n");
				goto err_listener;
			}
		}

		if (rdp_implant_listener(b, b->listener) < 0)
			goto err_listener;
	} else {
		/* get the socket from RDP_FD var */
		fd_str = getenv("RDP_FD");
		if (!fd_str) {
			weston_log("RDP_FD env variable not set\n");
			goto err_output;
		}

		fd = strtoul(fd_str, &fd_tail, 10);
		if (errno != 0 || fd_tail == fd_str || *fd_tail != '\0'
		    || rdp_peer_init(freerdp_peer_new(fd), b))
			goto err_output;
	}

	ret = weston_plugin_api_register(compositor, WESTON_RDP_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		weston_log("Failed to register output API.\n");
		goto err_output;
	}

	return b;

err_listener:
	freerdp_listener_free(b->listener);
err_output:
	if (b->output)
		weston_output_release(&b->output->base);
err_compositor:
	wl_list_for_each_safe(base, next, &compositor->head_list, compositor_link)
		rdp_head_destroy(to_rdp_head(base));

	weston_compositor_shutdown(compositor);
err_free_strings:
	if (b->debug)
		weston_log_scope_destroy(b->debug);
	if (b->verbose)
		weston_log_scope_destroy(b->verbose);
	free(b->rdp_key);
	free(b->server_cert);
	free(b->server_key);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_rdp_backend_config *config)
{
	config->bind_address = NULL;
	config->port = 3389;
	config->rdp_key = NULL;
	config->server_cert = NULL;
	config->server_key = NULL;
	config->env_socket = 0;
	config->no_clients_resize = 0;
	config->force_no_compression = 0;
	config->remotefx_codec = true;
	config->external_listener_fd = -1;
	config->refresh_rate = RDP_DEFAULT_FREQ;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct rdp_backend *b;
	struct weston_rdp_backend_config config = {{ 0, }};
	int major, minor, revision;

#if FREERDP_VERSION_MAJOR >= 2
	winpr_InitializeSSL(0);
#endif
	freerdp_get_version(&major, &minor, &revision);
	weston_log("using FreeRDP version %d.%d.%d\n", major, minor, revision);

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_RDP_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_rdp_backend_config)) {
		weston_log("RDP backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = rdp_backend_create(compositor, &config);
	if (b == NULL)
		return -1;
	return 0;
}
