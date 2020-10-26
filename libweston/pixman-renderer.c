/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Vasily Khoruzhick <anarsoul@gmail.com>
 * Copyright © 2015 Collabora, Ltd.
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
#include <assert.h>

#include "pixman-renderer.h"
#include "color.h"
#include "pixel-formats.h"
#include "shared/helpers.h"
#include "shared/signal.h"
#include "shared/weston-drm-fourcc.h"

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/input.h>

#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"

#ifdef ENABLE_EGL
#include <fcntl.h>
#include <sys/stat.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "shared/platform.h"
#include "shared/weston-egl-ext.h"  /* for PFN* stuff */
#endif

struct pixman_output_state {
	pixman_image_t *shadow_image;
	pixman_image_t *hw_buffer;
	pixman_region32_t *hw_extra_damage;
};

struct pixman_surface_state {
	struct weston_surface *surface;

	pixman_image_t *image;
	struct weston_buffer_reference buffer_ref;
	struct weston_buffer_release_reference buffer_release_ref;

	struct wl_listener buffer_destroy_listener;
	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;
};

struct pixman_renderer {
	struct weston_renderer base;

	int repaint_debug;
	pixman_image_t *debug_color;
	struct weston_binding *debug_binding;

	struct wl_signal destroy_signal;

	struct weston_drm_format_array supported_formats;

#ifdef ENABLE_EGL
	PFNEGLBINDWAYLANDDISPLAYWL bind_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_display;
	PFNEGLQUERYWAYLANDBUFFERWL query_buffer;
	EGLDisplay egl_display;

	int drm_fd;
	struct gbm_device *gbm;

	bool egl_inited;
#endif
};

struct dmabuf_data {
	void *ptr;
	size_t size;
};

#ifdef ENABLE_EGL
/* HACK: For mali_buffer_sharing */
struct egl_buffer_info {
	int dma_fd;
	int width;
	int height;
	unsigned int stride;
};
#endif

static inline struct pixman_output_state *
get_output_state(struct weston_output *output)
{
	return (struct pixman_output_state *)output->renderer_state;
}

static int
pixman_renderer_create_surface(struct weston_surface *surface);

static inline struct pixman_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		pixman_renderer_create_surface(surface);

	return (struct pixman_surface_state *)surface->renderer_state;
}

static inline struct pixman_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct pixman_renderer *)ec->renderer;
}

static int
pixman_renderer_read_pixels(struct weston_output *output,
			    const struct pixel_format_info *format, void *pixels,
			    uint32_t x, uint32_t y,
			    uint32_t width, uint32_t height)
{
	struct pixman_output_state *po = get_output_state(output);
	pixman_image_t *out_buf;

	if (!po->hw_buffer) {
		errno = ENODEV;
		return -1;
	}

	out_buf = pixman_image_create_bits(format->pixman_format,
		width,
		height,
		pixels,
		(PIXMAN_FORMAT_BPP(format->pixman_format) / 8) * width);

	pixman_image_composite32(PIXMAN_OP_SRC,
				 po->hw_buffer, /* src */
				 NULL /* mask */,
				 out_buf, /* dest */
				 x, y, /* src_x, src_y */
				 0, 0, /* mask_x, mask_y */
				 0, 0, /* dest_x, dest_y */
				 pixman_image_get_width (po->hw_buffer), /* width */
				 pixman_image_get_height (po->hw_buffer) /* height */);

	pixman_image_unref(out_buf);

	return 0;
}

#define D2F(v) pixman_double_to_fixed((double)v)

static void
weston_matrix_to_pixman_transform(pixman_transform_t *pt,
				  const struct weston_matrix *wm)
{
	/* Pixman supports only 2D transform matrix, but Weston uses 3D, *
	 * so we're omitting Z coordinate here. */
	pt->matrix[0][0] = pixman_double_to_fixed(wm->d[0]);
	pt->matrix[0][1] = pixman_double_to_fixed(wm->d[4]);
	pt->matrix[0][2] = pixman_double_to_fixed(wm->d[12]);
	pt->matrix[1][0] = pixman_double_to_fixed(wm->d[1]);
	pt->matrix[1][1] = pixman_double_to_fixed(wm->d[5]);
	pt->matrix[1][2] = pixman_double_to_fixed(wm->d[13]);
	pt->matrix[2][0] = pixman_double_to_fixed(wm->d[3]);
	pt->matrix[2][1] = pixman_double_to_fixed(wm->d[7]);
	pt->matrix[2][2] = pixman_double_to_fixed(wm->d[15]);
}

static void
pixman_renderer_compute_transform(pixman_transform_t *transform_out,
				  struct weston_view *ev,
				  struct weston_output *output)
{
	struct weston_matrix matrix;

	weston_matrix_init(&matrix);
	weston_matrix_scale(&matrix, 1.0f / output->down_scale,
			    1.0f / output->down_scale, 1);

	/* Set up the source transformation based on the surface
	   position, the output position/transform/scale and the client
	   specified buffer transform/scale */
	weston_matrix_multiply(&matrix, &output->inverse_matrix);

	if (ev->transform.enabled) {
		weston_matrix_multiply(&matrix, &ev->transform.inverse);
	} else {
		weston_matrix_translate(&matrix,
					-ev->geometry.x, -ev->geometry.y, 0);
	}

	weston_matrix_multiply(&matrix, &ev->surface->surface_to_buffer_matrix);

	weston_matrix_to_pixman_transform(transform_out, &matrix);
}

static bool
view_transformation_is_translation(struct weston_view *view)
{
	if (!view->transform.enabled)
		return true;

	if (view->transform.matrix.type <= WESTON_MATRIX_TRANSFORM_TRANSLATE)
		return true;

	return false;
}

static void
region_intersect_only_translation(pixman_region32_t *result_global,
				  pixman_region32_t *global,
				  pixman_region32_t *surf,
				  struct weston_view *view)
{
	float view_x, view_y;

	assert(view_transformation_is_translation(view));

	/* Convert from surface to global coordinates */
	pixman_region32_copy(result_global, surf);
	weston_view_to_global_float(view, 0, 0, &view_x, &view_y);
	pixman_region32_translate(result_global, (int)view_x, (int)view_y);

	pixman_region32_intersect(result_global, result_global, global);
}

static void
composite_whole(pixman_op_t op,
		pixman_image_t *src,
		pixman_image_t *mask,
		pixman_image_t *dest,
		const pixman_transform_t *transform,
		pixman_filter_t filter)
{
	int32_t dest_width;
	int32_t dest_height;

	dest_width = pixman_image_get_width(dest);
	dest_height = pixman_image_get_height(dest);

	pixman_image_set_transform(src, transform);
	pixman_image_set_filter(src, filter, NULL, 0);

	/* bilinear filtering needs the equivalent of OpenGL CLAMP_TO_EDGE */
	if (filter == PIXMAN_FILTER_NEAREST)
		pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
	else
		pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);

	pixman_image_composite32(op, src, mask, dest,
				 0, 0, /* src_x, src_y */
				 0, 0, /* mask_x, mask_y */
				 0, 0, /* dest_x, dest_y */
				 dest_width, dest_height);
}

static void
composite_clipped(pixman_image_t *src,
		  pixman_image_t *mask,
		  pixman_image_t *dest,
		  const pixman_transform_t *transform,
		  pixman_filter_t filter,
		  pixman_region32_t *src_clip)
{
	int n_box;
	pixman_box32_t *boxes;
	int32_t dest_width;
	int32_t dest_height;
	int src_stride;
	int bitspp;
	pixman_format_code_t src_format;
	void *src_data;
	int i;

	/*
	 * Hardcoded to use PIXMAN_OP_OVER, because sampling outside of
	 * a Pixman image produces (0,0,0,0) instead of discarding the
	 * fragment.
	 *
	 * Also repeat mode must be PIXMAN_REPEAT_NONE (the default) to
	 * actually sample (0,0,0,0). This may cause issues for clients that
	 * expect OpenGL CLAMP_TO_EDGE sampling behavior on their buffer.
	 * Using temporary 'boximg' it is not possible to apply CLAMP_TO_EDGE
	 * correctly with bilinear filter. Maybe trapezoid rendering could be
	 * the answer instead of source clip?
	 */

	dest_width = pixman_image_get_width(dest);
	dest_height = pixman_image_get_height(dest);
	src_format = pixman_image_get_format(src);
	src_stride = pixman_image_get_stride(src);
	bitspp = PIXMAN_FORMAT_BPP(src_format);
	src_data = pixman_image_get_data(src);

	assert(src_format);

	/* This would be massive overdraw, except when n_box is 1. */
	boxes = pixman_region32_rectangles(src_clip, &n_box);
	for (i = 0; i < n_box; i++) {
		uint8_t *ptr = src_data;
		pixman_image_t *boximg;
		pixman_transform_t adj = *transform;

		ptr += boxes[i].y1 * src_stride;
		ptr += boxes[i].x1 * bitspp / 8;
		boximg = pixman_image_create_bits_no_clear(src_format,
					boxes[i].x2 - boxes[i].x1,
					boxes[i].y2 - boxes[i].y1,
					(uint32_t *)ptr, src_stride);

		pixman_transform_translate(&adj, NULL,
					   pixman_int_to_fixed(-boxes[i].x1),
					   pixman_int_to_fixed(-boxes[i].y1));
		pixman_image_set_transform(boximg, &adj);

		pixman_image_set_filter(boximg, filter, NULL, 0);
		pixman_image_composite32(PIXMAN_OP_OVER, boximg, mask, dest,
					 0, 0, /* src_x, src_y */
					 0, 0, /* mask_x, mask_y */
					 0, 0, /* dest_x, dest_y */
					 dest_width, dest_height);

		pixman_image_unref(boximg);
	}

	if (n_box > 1) {
		static bool warned = false;

		if (!warned)
			weston_log("Pixman-renderer warning: %dx overdraw\n",
				   n_box);
		warned = true;
	}
}

/** Paint an intersected region
 *
 * \param ev The view to be painted.
 * \param output The output being painted.
 * \param repaint_output The region to be painted in output coordinates.
 * \param source_clip The region of the source image to use, in source image
 *                    coordinates. If NULL, use the whole source image.
 * \param pixman_op Compositing operator, either SRC or OVER.
 */
static void
repaint_region(struct weston_view *ev, struct weston_output *output,
	       pixman_region32_t *repaint_output,
	       pixman_region32_t *source_clip,
	       pixman_op_t pixman_op)
{
	struct pixman_renderer *pr =
		(struct pixman_renderer *) output->compositor->renderer;
	struct pixman_surface_state *ps = get_surface_state(ev->surface);
	struct pixman_output_state *po = get_output_state(output);
	struct weston_buffer_viewport *vp = &ev->surface->buffer_viewport;
	pixman_image_t *target_image;
	pixman_transform_t transform;
	pixman_filter_t filter;
	pixman_image_t *mask_image;
	pixman_color_t mask = { 0, };

	if (po->shadow_image)
		target_image = po->shadow_image;
	else
		target_image = po->hw_buffer;

 	/* Clip rendering to the damaged output region */
	pixman_image_set_clip_region32(target_image, repaint_output);

	pixman_renderer_compute_transform(&transform, ev, output);

	if (ev->transform.enabled || output->current_scale != vp->buffer.scale)
		filter = PIXMAN_FILTER_BILINEAR;
	else
		filter = PIXMAN_FILTER_NEAREST;

	if (output->down_scale != 1.0f) {
		struct weston_matrix matrix;
		pixman_region32_t clip;

		weston_matrix_init(&matrix);
		weston_matrix_scale(&matrix, output->down_scale,
				    output->down_scale, 1);

		pixman_region32_init(&clip);
		weston_matrix_transform_region(&clip, &matrix, repaint_output);

		pixman_image_set_clip_region32(target_image, &clip);
	}

	if (ps->buffer_ref.buffer &&
	    ps->buffer_ref.buffer->type == WESTON_BUFFER_SHM)
		wl_shm_buffer_begin_access(ps->buffer_ref.buffer->shm_buffer);

	if (ev->alpha < 1.0) {
		mask.alpha = 0xffff * ev->alpha;
		mask_image = pixman_image_create_solid_fill(&mask);
	} else {
		mask_image = NULL;
	}

	if (source_clip)
		composite_clipped(ps->image, mask_image, target_image,
				  &transform, filter, source_clip);
	else
		composite_whole(pixman_op, ps->image, mask_image,
				target_image, &transform, filter);

	if (mask_image)
		pixman_image_unref(mask_image);

	if (ps->buffer_ref.buffer &&
	    ps->buffer_ref.buffer->type == WESTON_BUFFER_SHM)
		wl_shm_buffer_end_access(ps->buffer_ref.buffer->shm_buffer);

	if (pr->repaint_debug)
		pixman_image_composite32(PIXMAN_OP_OVER,
					 pr->debug_color, /* src */
					 NULL /* mask */,
					 target_image, /* dest */
					 0, 0, /* src_x, src_y */
					 0, 0, /* mask_x, mask_y */
					 0, 0, /* dest_x, dest_y */
					 pixman_image_get_width (target_image), /* width */
					 pixman_image_get_height (target_image) /* height */);

	pixman_image_set_clip_region32(target_image, NULL);
}

static void
draw_view_translated(struct weston_view *view, struct weston_output *output,
		     pixman_region32_t *repaint_global)
{
	struct weston_surface *surface = view->surface;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	/* region to be painted in output coordinates: */
	pixman_region32_t repaint_output;

	pixman_region32_init(&repaint_output);

	/* Blended region is whole surface minus opaque region,
	 * unless surface alpha forces us to blend all.
	 */
	pixman_region32_init_rect(&surface_blend, 0, 0,
				  surface->width, surface->height);

	if (!(view->alpha < 1.0)) {
		pixman_region32_subtract(&surface_blend, &surface_blend,
					 &surface->opaque);

		if (pixman_region32_not_empty(&surface->opaque)) {
			region_intersect_only_translation(&repaint_output,
							  repaint_global,
							  &surface->opaque,
							  view);
			weston_output_region_from_global(output,
							 &repaint_output);

			repaint_region(view, output, &repaint_output, NULL,
				       PIXMAN_OP_SRC);
		}
	}

	if (pixman_region32_not_empty(&surface_blend)) {
		region_intersect_only_translation(&repaint_output,
						  repaint_global,
						  &surface_blend, view);
		weston_output_region_from_global(output, &repaint_output);

		repaint_region(view, output, &repaint_output, NULL,
			       PIXMAN_OP_OVER);
	}

	pixman_region32_fini(&surface_blend);
	pixman_region32_fini(&repaint_output);
}

static void
draw_view_source_clipped(struct weston_view *view,
			 struct weston_output *output,
			 pixman_region32_t *repaint_global)
{
	struct weston_surface *surface = view->surface;
	pixman_region32_t surf_region;
	pixman_region32_t buffer_region;
	pixman_region32_t repaint_output;

	/* Do not bother separating the opaque region from non-opaque.
	 * Source clipping requires PIXMAN_OP_OVER in all cases, so painting
	 * opaque separately has no benefit.
	 */

	pixman_region32_init_rect(&surf_region, 0, 0,
				  surface->width, surface->height);
	if (view->geometry.scissor_enabled)
		pixman_region32_intersect(&surf_region, &surf_region,
					  &view->geometry.scissor);

	pixman_region32_init(&buffer_region);
	weston_surface_to_buffer_region(surface, &surf_region, &buffer_region);

	pixman_region32_init(&repaint_output);
	pixman_region32_copy(&repaint_output, repaint_global);
	weston_output_region_from_global(output, &repaint_output);

	repaint_region(view, output, &repaint_output, &buffer_region,
		       PIXMAN_OP_OVER);

	pixman_region32_fini(&repaint_output);
	pixman_region32_fini(&buffer_region);
	pixman_region32_fini(&surf_region);
}

static void
draw_paint_node(struct weston_paint_node *pnode,
		pixman_region32_t *damage /* in global coordinates */)
{
	struct pixman_surface_state *ps = get_surface_state(pnode->surface);
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;

	if (!pnode->surf_xform_valid)
		return;

	assert(pnode->surf_xform.transform == NULL);

	/* No buffer attached */
	if (!ps->image)
		return;

	/* if we still have a reference, but the underlying buffer is no longer
	 * available signal that we should unref image_t as well. This happens
	 * when using close animations, with the reference surviving the
	 * animation while the underlying buffer went away as the client was
	 * terminated. This is a particular use-case and should probably be
	 * refactored to provide some analogue with the GL-renderer (as in, to
	 * still maintain the buffer and let the compositor dispose of it). */
	if (ps->buffer_ref.buffer && !ps->buffer_ref.buffer->shm_buffer) {
		pixman_image_unref(ps->image);
		ps->image = NULL;
		return;
	}

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &pnode->view->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &pnode->view->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	if (view_transformation_is_translation(pnode->view)) {
		/* The simple case: The surface regions opaque, non-opaque,
		 * etc. are convertible to global coordinate space.
		 * There is no need to use a source clip region.
		 * It is possible to paint opaque region as PIXMAN_OP_SRC.
		 * Also the boundingbox is accurate rather than an
		 * approximation.
		 */
		draw_view_translated(pnode->view, pnode->output, &repaint);
	} else {
		/* The complex case: the view transformation does not allow
		 * converting opaque etc. regions into global coordinate space.
		 * Therefore we need source clipping to avoid sampling from
		 * unwanted source image areas, unless the source image is
		 * to be used whole. Source clipping does not work with
		 * PIXMAN_OP_SRC.
		 */
		draw_view_source_clipped(pnode->view, pnode->output, &repaint);
	}

out:
	pixman_region32_fini(&repaint);
}
static void
repaint_surfaces(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_paint_node *pnode;

	wl_list_for_each_reverse(pnode, &output->paint_node_z_order_list,
				 z_order_link) {
		if (pnode->view->plane == &compositor->primary_plane)
			draw_paint_node(pnode, damage);
	}
}

static void
copy_to_hw_buffer(struct weston_output *output, pixman_region32_t *region)
{
	struct pixman_output_state *po = get_output_state(output);
	pixman_region32_t output_region;

	pixman_region32_init(&output_region);
	pixman_region32_copy(&output_region, region);

	weston_output_region_from_global(output, &output_region);

	if (output->down_scale != 1.0f) {
		struct weston_matrix matrix;
		weston_matrix_init(&matrix);
		weston_matrix_scale(&matrix, output->down_scale,
				    output->down_scale, 1);
		weston_matrix_transform_region(&output_region,
					       &matrix, &output_region);
	}

	pixman_image_set_clip_region32 (po->hw_buffer, &output_region);
	pixman_region32_fini(&output_region);

	pixman_image_composite32(PIXMAN_OP_SRC,
				 po->shadow_image, /* src */
				 NULL /* mask */,
				 po->hw_buffer, /* dest */
				 0, 0, /* src_x, src_y */
				 0, 0, /* mask_x, mask_y */
				 0, 0, /* dest_x, dest_y */
				 pixman_image_get_width (po->hw_buffer), /* width */
				 pixman_image_get_height (po->hw_buffer) /* height */);

	pixman_image_set_clip_region32 (po->hw_buffer, NULL);
}

static void
pixman_renderer_repaint_output(struct weston_output *output,
			       pixman_region32_t *output_damage)
{
	struct pixman_output_state *po = get_output_state(output);
	pixman_region32_t hw_damage;

	assert(output->from_blend_to_output_by_backend ||
	       output->color_outcome->from_blend_to_output == NULL);

	if (!po->hw_buffer) {
		po->hw_extra_damage = NULL;
 		return;
	}

	pixman_region32_init(&hw_damage);
	if (po->hw_extra_damage) {
		pixman_region32_union(&hw_damage,
				      po->hw_extra_damage, output_damage);
		po->hw_extra_damage = NULL;
	} else {
		pixman_region32_copy(&hw_damage, output_damage);
	}

	if (po->shadow_image) {
		repaint_surfaces(output, output_damage);
		copy_to_hw_buffer(output, &hw_damage);
	} else {
		repaint_surfaces(output, &hw_damage);
	}
	pixman_region32_fini(&hw_damage);

	wl_signal_emit(&output->frame_signal, output_damage);

	/* Actual flip should be done by caller */
}

static void
pixman_renderer_flush_damage(struct weston_surface *surface,
			     struct weston_buffer *buffer)
{
	/* No-op for pixman renderer */
}

static void
buffer_state_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct pixman_surface_state *ps;

	ps = container_of(listener, struct pixman_surface_state,
			  buffer_destroy_listener);

	if (ps->image) {
		pixman_image_unref(ps->image);
		ps->image = NULL;
	}

	ps->buffer_destroy_listener.notify = NULL;
}

static void
pixman_renderer_surface_set_color(struct weston_surface *es,
		 float red, float green, float blue, float alpha)
{
	struct pixman_surface_state *ps = get_surface_state(es);
	pixman_color_t color;

	color.red = red * 0xffff;
	color.green = green * 0xffff;
	color.blue = blue * 0xffff;
	color.alpha = alpha * 0xffff;

	if (ps->image) {
		pixman_image_unref(ps->image);
		ps->image = NULL;
	}

	ps->image = pixman_image_create_solid_fill(&color);
}

static void
pixman_renderer_destroy_dmabuf(struct linux_dmabuf_buffer *dmabuf)
{
	struct dmabuf_data *data = dmabuf->user_data;
	linux_dmabuf_buffer_set_user_data(dmabuf, NULL, NULL);

	if (data) {
		if (data->ptr)
			munmap(data->ptr, data->size);

		free(data);
	}
}

static bool
pixman_renderer_prepare_dmabuf(struct linux_dmabuf_buffer *dmabuf)
{
	struct dmabuf_attributes *attributes = &dmabuf->attributes;
	struct dmabuf_data *data;
	size_t total_size, vstride0;
	void *ptr;
	int i;

	data = linux_dmabuf_buffer_get_user_data(dmabuf);
	if (data)
		return true;

	total_size = lseek(attributes->fd[0], 0, SEEK_END);

	for (i = 0; i < attributes->n_planes; i++) {
		if (DRM_MOD_VALID(attributes->modifier[i]))
			return false;
	}

	/* reject all flags we do not recognize or handle */
	if (attributes->flags & ~ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT)
		return false;

	if (attributes->n_planes < 0)
		return false;

	if (attributes->n_planes == 1)
		goto out;

	vstride0 = (attributes->offset[1] - attributes->offset[0]) /
		attributes->stride[0];

	for (i = 1; i < attributes->n_planes; i++) {
		size_t size = attributes->offset[i] - attributes->offset[i - 1];
		size_t vstride = size / attributes->stride[i - 1];

		/* not contig */
		if (size <= 0 || vstride <= 0 ||
		    attributes->offset[i - 1] + size > total_size)
			return false;

		/* stride unmatched */
		if ((vstride != vstride0 && vstride != vstride0 / 2) ||
		    (attributes->stride[i] != attributes->stride[0] &&
		     attributes->stride[i] != attributes->stride[0] / 2))
			return false;
	}

out:
	/* Handle contig dma buffer */

	ptr = mmap(NULL, total_size, PROT_READ,
		   MAP_SHARED, attributes->fd[0], 0);
	if (!ptr)
		return false;

	data = zalloc(sizeof *data);
	if (!data) {
		munmap(ptr, total_size);
		return false;
	}

	data->size = total_size;
	data->ptr = ptr;
	linux_dmabuf_buffer_set_user_data(dmabuf, data,
					  pixman_renderer_destroy_dmabuf);
	return true;
}

static void
pixman_renderer_attach_dmabuf(struct weston_surface *es,
			      struct weston_buffer *buffer,
			      struct linux_dmabuf_buffer *dmabuf)
{
	struct pixman_surface_state *ps = get_surface_state(es);
	struct dmabuf_attributes *attributes = &dmabuf->attributes;
	struct dmabuf_data *data;
	pixman_format_code_t pixman_format;
	size_t vstride;

	if (!pixman_renderer_prepare_dmabuf(dmabuf))
		goto err;

	data = linux_dmabuf_buffer_get_user_data(dmabuf);
	if (!data || !data->ptr)
		goto err;

	buffer->width = attributes->width;
	buffer->height = attributes->height;

	if (attributes->n_planes == 1)
		vstride = attributes->height;
	else
		vstride = (attributes->offset[1] - attributes->offset[0]) /
			attributes->stride[0];

	switch (attributes->format) {
	case DRM_FORMAT_RGBA8888:
		pixman_format = PIXMAN_r8g8b8a8;
		break;
	case DRM_FORMAT_RGBX8888:
		pixman_format = PIXMAN_r8g8b8x8;
		break;
	case DRM_FORMAT_BGRA8888:
		pixman_format = PIXMAN_b8g8r8a8;
		break;
	case DRM_FORMAT_BGRX8888:
		pixman_format = PIXMAN_b8g8r8x8;
		break;
	case DRM_FORMAT_ABGR8888:
		pixman_format = PIXMAN_a8b8g8r8;
		break;
	case DRM_FORMAT_XBGR8888:
		pixman_format = PIXMAN_x8b8g8r8;
		break;
	case DRM_FORMAT_BGR888:
		pixman_format = PIXMAN_b8g8r8;
		break;
	case DRM_FORMAT_ARGB8888:
		pixman_format = PIXMAN_a8r8g8b8;
		break;
	case DRM_FORMAT_XRGB8888:
		pixman_format = PIXMAN_x8r8g8b8;
		break;
	case DRM_FORMAT_RGB888:
		pixman_format = PIXMAN_r8g8b8;
		break;
	case DRM_FORMAT_YUYV:
		pixman_format = PIXMAN_yuy2;
		break;
	case DRM_FORMAT_YVU420:
		pixman_format = PIXMAN_yv12;
		break;
#ifdef HAVE_PIXMAN_I420
	case DRM_FORMAT_YUV420:
		pixman_format = PIXMAN_i420;
		break;
#endif
#ifdef HAVE_PIXMAN_NV12
	case DRM_FORMAT_NV12:
		pixman_format = PIXMAN_nv12;
		break;
#endif
#ifdef HAVE_PIXMAN_NV16
	case DRM_FORMAT_NV16:
		pixman_format = PIXMAN_nv16;
		break;
#endif
	default:
		weston_log("Unsupported dmabuf format\n");
		goto err;
	break;
	}

	ps->image = pixman_image_create_bits(pixman_format,
					     buffer->width, vstride,
					     data->ptr + attributes->offset[0],
					     attributes->stride[0]);

	ps->buffer_destroy_listener.notify =
		buffer_state_handle_buffer_destroy;
	wl_signal_add(&buffer->destroy_signal,
		      &ps->buffer_destroy_listener);
	return;
err:
	weston_buffer_reference(&ps->buffer_ref, NULL,
				BUFFER_WILL_NOT_BE_ACCESSED);
	weston_buffer_release_reference(&ps->buffer_release_ref, NULL);
}

static void
pixman_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct pixman_surface_state *ps = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	const struct pixel_format_info *pixel_info;

	weston_buffer_reference(&ps->buffer_ref, buffer,
				buffer ? BUFFER_MAY_BE_ACCESSED :
					 BUFFER_WILL_NOT_BE_ACCESSED);
	weston_buffer_release_reference(&ps->buffer_release_ref,
					es->buffer_release_ref.buffer_release);

	if (ps->buffer_destroy_listener.notify) {
		wl_list_remove(&ps->buffer_destroy_listener.link);
		ps->buffer_destroy_listener.notify = NULL;
	}

	if (ps->image) {
		pixman_image_unref(ps->image);
		ps->image = NULL;
	}

	if (!buffer)
		return;

	if (buffer->type == WESTON_BUFFER_SOLID) {
		pixman_renderer_surface_set_color(es,
						  buffer->solid.r,
						  buffer->solid.g,
						  buffer->solid.b,
						  buffer->solid.a);
		weston_buffer_reference(&ps->buffer_ref, NULL,
					BUFFER_WILL_NOT_BE_ACCESSED);
		weston_buffer_release_reference(&ps->buffer_release_ref, NULL);
		return;
	}

	if (buffer->type == WESTON_BUFFER_DMABUF) {
		struct linux_dmabuf_buffer *dmabuf =
			linux_dmabuf_buffer_get(buffer->resource);
		pixman_renderer_attach_dmabuf(es, buffer, dmabuf);
		return;
	}

#ifdef ENABLE_EGL
	if (buffer->type == WESTON_BUFFER_RENDERER_OPAQUE) {
		struct egl_buffer_info *info;
		struct linux_dmabuf_buffer dmabuf = { 0 };
		struct dmabuf_attributes *attributes = &dmabuf.attributes;

		info = wl_resource_get_user_data(buffer->resource);

		attributes->format = buffer->pixel_format->format;
		attributes->width = buffer->width;
		attributes->height = buffer->height;

		attributes->n_planes = 1;
		attributes->fd[0] = info->dma_fd;
		attributes->stride[0] = info->stride;

		pixman_renderer_attach_dmabuf(es, buffer, &dmabuf);
		return;
	}
#endif

	if (buffer->type != WESTON_BUFFER_SHM) {
		weston_log("unhandled buffer type!\n");
		weston_buffer_reference(&ps->buffer_ref, NULL,
					BUFFER_WILL_NOT_BE_ACCESSED);
		weston_buffer_release_reference(&ps->buffer_release_ref,
						NULL);
		return;
	}

	shm_buffer = buffer->shm_buffer;

	pixel_info = pixel_format_get_info_shm(wl_shm_buffer_get_format(shm_buffer));
	if (!pixel_info || !pixman_format_supported_source(pixel_info->pixman_format)) {
		weston_log("Unsupported SHM buffer format 0x%x\n",
			wl_shm_buffer_get_format(shm_buffer));
		weston_buffer_reference(&ps->buffer_ref, NULL,
					BUFFER_WILL_NOT_BE_ACCESSED);
		weston_buffer_release_reference(&ps->buffer_release_ref, NULL);
		weston_buffer_send_server_error(buffer,
			"disconnecting due to unhandled buffer type");
		return;
	}

	ps->image = pixman_image_create_bits(pixel_info->pixman_format,
		buffer->width, buffer->height,
		wl_shm_buffer_get_data(shm_buffer),
		wl_shm_buffer_get_stride(shm_buffer));

	ps->buffer_destroy_listener.notify =
		buffer_state_handle_buffer_destroy;
	wl_signal_add(&buffer->destroy_signal,
		      &ps->buffer_destroy_listener);
}

#ifdef ENABLE_EGL
static bool
pixman_renderer_fill_buffer_info(struct weston_compositor *ec,
				 struct weston_buffer *buffer)
{
	struct pixman_renderer *pr = get_renderer(ec);
	struct egl_buffer_info *info;
	struct stat s;
	EGLint format;
	uint32_t fourcc;
	EGLint y_inverted;
	bool ret = true;

	if (!pr->egl_inited)
		return false;

	info = wl_resource_get_user_data(buffer->resource);
	if (!info)
		return false;

	buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;
	ret &= pr->query_buffer(pr->egl_display, buffer->legacy_buffer,
			        EGL_WIDTH, &buffer->width);
	ret &= pr->query_buffer(pr->egl_display, buffer->legacy_buffer,
				EGL_HEIGHT, &buffer->height);
	ret &= pr->query_buffer(pr->egl_display, buffer->legacy_buffer,
				EGL_TEXTURE_FORMAT, &format);
	if (!ret) {
		weston_log("eglQueryWaylandBufferWL failed\n");
		return false;
	}

	if (fstat(info->dma_fd, &s) < 0 ||
	    info->width != buffer->width || info->height != buffer->height)
		return false;

	switch (format) {
	case EGL_TEXTURE_RGB:
		fourcc = DRM_FORMAT_XRGB8888;
		break;
	case EGL_TEXTURE_RGBA:
		fourcc = DRM_FORMAT_ARGB8888;
		break;
	default:
		return false;
	}

	buffer->pixel_format = pixel_format_get_info(fourcc);
	assert(buffer->pixel_format);
	buffer->format_modifier = DRM_FORMAT_MOD_INVALID;

	/* Assume scanout co-ordinate space i.e. (0,0) is top-left
	 * if the query fails */
	ret = pr->query_buffer(pr->egl_display, buffer->legacy_buffer,
			       EGL_WAYLAND_Y_INVERTED_WL, &y_inverted);
	if (!ret || y_inverted)
		buffer->buffer_origin = ORIGIN_TOP_LEFT;
	else
		buffer->buffer_origin = ORIGIN_BOTTOM_LEFT;

	return true;
}
#endif

static void
pixman_renderer_surface_state_destroy(struct pixman_surface_state *ps)
{
	wl_list_remove(&ps->surface_destroy_listener.link);
	wl_list_remove(&ps->renderer_destroy_listener.link);
	if (ps->buffer_destroy_listener.notify) {
		wl_list_remove(&ps->buffer_destroy_listener.link);
		ps->buffer_destroy_listener.notify = NULL;
	}

	ps->surface->renderer_state = NULL;

	if (ps->image) {
		pixman_image_unref(ps->image);
		ps->image = NULL;
	}
	weston_buffer_reference(&ps->buffer_ref, NULL,
				BUFFER_WILL_NOT_BE_ACCESSED);
	weston_buffer_release_reference(&ps->buffer_release_ref, NULL);
	free(ps);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct pixman_surface_state *ps;

	ps = container_of(listener, struct pixman_surface_state,
			  surface_destroy_listener);

	pixman_renderer_surface_state_destroy(ps);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct pixman_surface_state *ps;

	ps = container_of(listener, struct pixman_surface_state,
			  renderer_destroy_listener);

	pixman_renderer_surface_state_destroy(ps);
}

static int
pixman_renderer_create_surface(struct weston_surface *surface)
{
	struct pixman_surface_state *ps;
	struct pixman_renderer *pr = get_renderer(surface->compositor);

	ps = zalloc(sizeof *ps);
	if (ps == NULL)
		return -1;

	surface->renderer_state = ps;

	ps->surface = surface;

	ps->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &ps->surface_destroy_listener);

	ps->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&pr->destroy_signal,
		      &ps->renderer_destroy_listener);

	if (surface->buffer_ref.buffer)
		pixman_renderer_attach(surface, surface->buffer_ref.buffer);

	return 0;
}

static void
pixman_renderer_destroy(struct weston_compositor *ec)
{
	struct pixman_renderer *pr = get_renderer(ec);

#ifdef ENABLE_EGL
	if (pr->egl_inited) {
		if (pr->unbind_display)
			pr->unbind_display(pr->egl_display, ec->wl_display);

		eglTerminate(pr->egl_display);
		eglReleaseThread();

		if (pr->gbm)
			gbm_device_destroy(pr->gbm);

		close(pr->drm_fd);
	}
#endif

	wl_signal_emit(&pr->destroy_signal, pr);
	weston_binding_destroy(pr->debug_binding);

	weston_drm_format_array_fini(&pr->supported_formats);

	free(pr);

	ec->renderer = NULL;
}

static int
pixman_renderer_surface_copy_content(struct weston_surface *surface,
				     void *target, size_t size,
				     int src_x, int src_y,
				     int width, int height)
{
	const pixman_format_code_t format = PIXMAN_a8b8g8r8;
	const size_t bytespp = 4; /* PIXMAN_a8b8g8r8 */
	struct pixman_surface_state *ps = get_surface_state(surface);
	pixman_image_t *out_buf;

	if (!ps->image)
		return -1;

	out_buf = pixman_image_create_bits(format, width, height,
					   target, width * bytespp);

	pixman_image_set_transform(ps->image, NULL);
	pixman_image_composite32(PIXMAN_OP_SRC,
				 ps->image,    /* src */
				 NULL,         /* mask */
				 out_buf,      /* dest */
				 src_x, src_y, /* src_x, src_y */
				 0, 0,         /* mask_x, mask_y */
				 0, 0,         /* dest_x, dest_y */
				 width, height);

	pixman_image_unref(out_buf);

	return 0;
}

static bool
pixman_renderer_resize_output(struct weston_output *output,
			      const struct weston_size *fb_size,
			      const struct weston_geometry *area)
{
	check_compositing_area(fb_size, area);

	/*
	 * Pixman-renderer does not implement output decorations blitting,
	 * wayland-backend does it on its own.
	 */
	assert(area->x == 0);
	assert(area->y == 0);
	assert(fb_size->width == area->width);
	assert(fb_size->height == area->height);

	return true;
}

static void
debug_binding(struct weston_keyboard *keyboard, const struct timespec *time,
	      uint32_t key, void *data)
{
	struct weston_compositor *ec = data;
	struct pixman_renderer *pr = (struct pixman_renderer *) ec->renderer;

	pr->repaint_debug ^= 1;

	if (pr->repaint_debug) {
		pixman_color_t red = {
			0x3fff, 0x0000, 0x0000, 0x3fff
		};

		pr->debug_color = pixman_image_create_solid_fill(&red);
	} else {
		pixman_image_unref(pr->debug_color);
		weston_compositor_damage_all(ec);
	}
}

static bool
pixman_renderer_import_dmabuf(struct weston_compositor *ec,
			      struct linux_dmabuf_buffer *dmabuf)
{
	return pixman_renderer_prepare_dmabuf(dmabuf);
}

static const struct weston_drm_format_array *
pixman_renderer_get_supported_formats(struct weston_compositor *ec)
{
	struct pixman_renderer *pr = get_renderer(ec);

	return &pr->supported_formats;
}

static int
populate_supported_formats(struct weston_compositor *ec,
			   struct weston_drm_format_array *supported_formats)
{
	struct weston_drm_format *fmt;
	int i, ret = 0;

	/* TODO: support more formats */
	static const int formats[] = {
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_RGBA8888,
		DRM_FORMAT_RGBX8888,
		DRM_FORMAT_ABGR8888,
		DRM_FORMAT_XBGR8888,
		DRM_FORMAT_BGRA8888,
		DRM_FORMAT_BGRX8888,
		DRM_FORMAT_YUYV,
		DRM_FORMAT_YVU420,
		DRM_FORMAT_YUV420,
		DRM_FORMAT_NV12,
		DRM_FORMAT_NV16,
	};

	int num_formats = ARRAY_LENGTH(formats);

	for (i = 0; i < num_formats; i++) {
		fmt = weston_drm_format_array_add_format(supported_formats,
							 formats[i]);
		if (!fmt) {
			ret = -1;
			goto out;
		}

		/* Always add DRM_FORMAT_MOD_INVALID, as EGL implementations
		 * support implicit modifiers. */
		ret = weston_drm_format_add_modifier(fmt, DRM_FORMAT_MOD_INVALID);
		if (ret < 0)
			goto out;
	}

out:
	return ret;
}

#ifdef ENABLE_EGL
static bool
pixman_renderer_init_egl(struct pixman_renderer *pr,
			 struct weston_compositor *ec)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	const char *extensions;

	get_platform_display =
		(void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
	pr->query_buffer =
		(void *) eglGetProcAddress("eglQueryWaylandBufferWL");
	pr->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	pr->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");

	if (!get_platform_display || !pr->query_buffer ||
	    !pr->bind_display || !pr->unbind_display) {
		weston_log("Failed to get egl proc\n");
		return false;
	}

	pr->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (pr->drm_fd < 0) {
		weston_log("Failed to open drm dev\n");
		return false;
	}

	pr->gbm = gbm_create_device(pr->drm_fd);
	if (!pr->gbm) {
		weston_log("Failed to create gbm device\n");
		goto err_close_fd;
	}

	pr->egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR,
					       (void*) pr->gbm, NULL);
	if (pr->egl_display == EGL_NO_DISPLAY) {
		weston_log("Failed to create egl display\n");
		goto err_destroy_gbm;
	}

	if (!eglInitialize(pr->egl_display, NULL, NULL)) {
		weston_log("Failed to initialize egl\n");
		goto err_terminate_display;
	}

	extensions =
		(const char *) eglQueryString(pr->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		goto err_terminate_display;
	}

	if (!weston_check_egl_extension(extensions,
					"EGL_WL_bind_wayland_display")) {
		weston_log("Wayland extension not supported.\n");
		goto err_terminate_display;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		weston_log("Failed to bind api\n");
		goto err_terminate_display;
	}

	if (!pr->bind_display(pr->egl_display, ec->wl_display))
		goto err_terminate_display;

	pr->egl_inited = true;
	return true;

err_terminate_display:
	eglTerminate(pr->egl_display);
err_destroy_gbm:
	gbm_device_destroy(pr->gbm);
	pr->gbm = NULL;
err_close_fd:
	close(pr->drm_fd);
	pr->drm_fd = -1;
	return false;
}
#endif

WL_EXPORT int
pixman_renderer_init(struct weston_compositor *ec)
{
	struct pixman_renderer *renderer;
	const struct pixel_format_info *pixel_info, *info_argb8888, *info_xrgb8888;
	unsigned int i, num_formats;
	int ret;

	renderer = zalloc(sizeof *renderer);
	if (renderer == NULL)
		return -1;

	renderer->repaint_debug = 0;
	renderer->debug_color = NULL;
	renderer->base.read_pixels = pixman_renderer_read_pixels;
	renderer->base.repaint_output = pixman_renderer_repaint_output;
	renderer->base.resize_output = pixman_renderer_resize_output;
	renderer->base.flush_damage = pixman_renderer_flush_damage;
	renderer->base.attach = pixman_renderer_attach;
	renderer->base.destroy = pixman_renderer_destroy;
	renderer->base.surface_copy_content =
		pixman_renderer_surface_copy_content;
#ifdef ENABLE_EGL
	renderer->base.fill_buffer_info = pixman_renderer_fill_buffer_info;
#endif
	ec->renderer = &renderer->base;
	ec->capabilities |= WESTON_CAP_ROTATION_ANY;
	ec->capabilities |= WESTON_CAP_VIEW_CLIP_MASK;

	weston_drm_format_array_init(&renderer->supported_formats);

	ret = populate_supported_formats(ec, &renderer->supported_formats);
	if (ret < 0) {
		weston_drm_format_array_fini(&renderer->supported_formats);
		free(renderer);
		return -1;
	}

	renderer->debug_binding =
		weston_compositor_add_debug_binding(ec, KEY_R,
						    debug_binding, ec);

	info_argb8888 = pixel_format_get_info_shm(WL_SHM_FORMAT_ARGB8888);
	info_xrgb8888 = pixel_format_get_info_shm(WL_SHM_FORMAT_XRGB8888);

	num_formats = pixel_format_get_info_count();
	for (i = 0; i < num_formats; i++) {
		pixel_info = pixel_format_get_info_by_index(i);
		if (!pixman_format_supported_source(pixel_info->pixman_format))
			continue;

		/* skip formats which libwayland registers by default */
		if (pixel_info == info_argb8888 || pixel_info == info_xrgb8888)
			continue;

		wl_display_add_shm_format(ec->wl_display, pixel_info->format);
	}

	wl_signal_init(&renderer->destroy_signal);

	renderer->base.import_dmabuf = pixman_renderer_import_dmabuf;

	renderer->base.get_supported_formats =
		pixman_renderer_get_supported_formats;

#ifdef ENABLE_EGL
	if (!getenv("WESTON_PIXMAN_WITHOUT_EGL"))
		pixman_renderer_init_egl(renderer, ec);
#endif

	return 0;
}

WL_EXPORT void
pixman_renderer_output_set_buffer(struct weston_output *output,
				  pixman_image_t *buffer)
{
	struct pixman_output_state *po = get_output_state(output);
	pixman_format_code_t pixman_format;

	if (po->hw_buffer)
		pixman_image_unref(po->hw_buffer);
	po->hw_buffer = buffer;

	if (po->hw_buffer) {
		pixman_format = pixman_image_get_format(po->hw_buffer);
		output->compositor->read_format =
			pixel_format_get_info_by_pixman(pixman_format);
		pixman_image_ref(po->hw_buffer);
	}
}

WL_EXPORT void
pixman_renderer_output_set_hw_extra_damage(struct weston_output *output,
					   pixman_region32_t *extra_damage)
{
	struct pixman_output_state *po = get_output_state(output);

	po->hw_extra_damage = extra_damage;
}

WL_EXPORT int
pixman_renderer_output_create(struct weston_output *output,
			      const struct pixman_renderer_output_options *options)
{
	struct pixman_output_state *po;
	int w, h;

	po = zalloc(sizeof *po);
	if (po == NULL)
		return -1;

	if (options->use_shadow) {
		/* set shadow image transformation */
		w = output->current_mode->width;
		h = output->current_mode->height;

		po->shadow_image =
			pixman_image_create_bits_no_clear(PIXMAN_x8r8g8b8,
							  w, h, NULL, 0);

		if (!po->shadow_image) {
			free(po);
			return -1;
		}
	}

	output->renderer_state = po;

	return 0;
}

WL_EXPORT void
pixman_renderer_output_destroy(struct weston_output *output)
{
	struct pixman_output_state *po = get_output_state(output);

	if (po->shadow_image)
		pixman_image_unref(po->shadow_image);

	if (po->hw_buffer)
		pixman_image_unref(po->hw_buffer);

	po->shadow_image = NULL;
	po->hw_buffer = NULL;

	free(po);
}
