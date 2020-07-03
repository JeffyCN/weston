/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018, 2024 Collabora, Ltd.
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
#include <libdisplay-info/cta.h>
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>

#include "drm-internal.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/xalloc.h"

struct drm_head_info {
	char *make; /* The monitor make (PNP ID or company name). */
	char *model; /* The monitor model (product name). */
	char *serial_number;

	/* The monitor supported EOTF modes, combination of
 	 * enum weston_eotf_mode bits.
	 */
	uint32_t eotf_mask;

	/* The monitor supported colorimetry modes, combination of
	 * enum weston_colorimetry_mode bits.
	 */
	uint32_t colorimetry_mask;

	/* The monitor supported color foramts, combination of
	 * enum_weston_color_format bits.  */
	uint32_t color_format_mask;
};

static void
drm_head_info_fini(struct drm_head_info *dhi)
{
	free(dhi->make);
	free(dhi->model);
	free(dhi->serial_number);
	*dhi = (struct drm_head_info){};
}

static const char *const aspect_ratio_as_string[] = {
	[WESTON_MODE_PIC_AR_NONE] = "",
	[WESTON_MODE_PIC_AR_4_3] = " 4:3",
	[WESTON_MODE_PIC_AR_16_9] = " 16:9",
	[WESTON_MODE_PIC_AR_64_27] = " 64:27",
	[WESTON_MODE_PIC_AR_256_135] = " 256:135",
};

/*
 * Get the aspect-ratio from drmModeModeInfo mode flags.
 *
 * @param drm_mode_flags- flags from drmModeModeInfo structure.
 * @returns aspect-ratio as encoded in enum 'weston_mode_aspect_ratio'.
 */
static enum weston_mode_aspect_ratio
drm_to_weston_mode_aspect_ratio(uint32_t drm_mode_flags)
{
	switch (drm_mode_flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_4_3:
		return WESTON_MODE_PIC_AR_4_3;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		return WESTON_MODE_PIC_AR_16_9;
	case DRM_MODE_FLAG_PIC_AR_64_27:
		return WESTON_MODE_PIC_AR_64_27;
	case DRM_MODE_FLAG_PIC_AR_256_135:
		return WESTON_MODE_PIC_AR_256_135;
	case DRM_MODE_FLAG_PIC_AR_NONE:
	default:
		return WESTON_MODE_PIC_AR_NONE;
	}
}

static const char *
aspect_ratio_to_string(enum weston_mode_aspect_ratio ratio)
{
	if (ratio < 0 || ratio >= ARRAY_LENGTH(aspect_ratio_as_string) ||
	    !aspect_ratio_as_string[ratio])
		return " (unknown aspect ratio)";

	return aspect_ratio_as_string[ratio];
}

static int
drm_subpixel_to_wayland(int drm_value)
{
	switch (drm_value) {
	default:
	case DRM_MODE_SUBPIXEL_UNKNOWN:
		return WL_OUTPUT_SUBPIXEL_UNKNOWN;
	case DRM_MODE_SUBPIXEL_NONE:
		return WL_OUTPUT_SUBPIXEL_NONE;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
	case DRM_MODE_SUBPIXEL_VERTICAL_RGB:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
	case DRM_MODE_SUBPIXEL_VERTICAL_BGR:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
	}
}

int
drm_mode_ensure_blob(struct drm_device *device, struct drm_mode *mode)
{
	struct drm_backend *backend = device->backend;
	int ret;

	if (mode->blob_id)
		return 0;

	ret = drmModeCreatePropertyBlob(device->kms_device->fd,
					&mode->mode_info,
					sizeof(mode->mode_info),
					&mode->blob_id);
	if (ret != 0)
		weston_log("failed to create mode property blob: %s\n",
			   strerror(errno));

	drm_debug(backend, "\t\t\t[atomic] created new mode blob %lu for %s\n",
		  (unsigned long) mode->blob_id, mode->mode_info.name);

	return ret;
}

static bool
check_non_desktop(struct drm_connector *connector, drmModeObjectPropertiesPtr props)
{
	struct drm_property_info *non_desktop_info =
		&connector->props[WDRM_CONNECTOR_NON_DESKTOP];

	return drm_property_get_value(non_desktop_info, props, 0);
}

static uint32_t
get_panel_orientation(struct drm_connector *connector, drmModeObjectPropertiesPtr props)
{
	struct drm_property_info *orientation =
		&connector->props[WDRM_CONNECTOR_PANEL_ORIENTATION];
	uint64_t kms_val =
		drm_property_get_value(orientation, props,
				       WDRM_PANEL_ORIENTATION_NORMAL);

	switch (kms_val) {
	case WDRM_PANEL_ORIENTATION_NORMAL:
		return WL_OUTPUT_TRANSFORM_NORMAL;
	case WDRM_PANEL_ORIENTATION_UPSIDE_DOWN:
		return WL_OUTPUT_TRANSFORM_180;
	case WDRM_PANEL_ORIENTATION_LEFT_SIDE_UP:
		return WL_OUTPUT_TRANSFORM_90;
	case WDRM_PANEL_ORIENTATION_RIGHT_SIDE_UP:
		return WL_OUTPUT_TRANSFORM_270;
	default:
		assert(!"unknown property value in get_panel_orientation");
	}
}

static int
parse_modeline(const char *s, drmModeModeInfo *mode)
{
	char hsync[16];
	char vsync[16];
	float fclock;

	memset(mode, 0, sizeof *mode);

	mode->type = DRM_MODE_TYPE_USERDEF;
	mode->hskew = 0;
	mode->vscan = 0;
	mode->vrefresh = 0;
	mode->flags = 0;

	if (sscanf(s, "%f %hd %hd %hd %hd %hd %hd %hd %hd %15s %15s",
		   &fclock,
		   &mode->hdisplay,
		   &mode->hsync_start,
		   &mode->hsync_end,
		   &mode->htotal,
		   &mode->vdisplay,
		   &mode->vsync_start,
		   &mode->vsync_end,
		   &mode->vtotal, hsync, vsync) != 11)
		return -1;

	mode->clock = fclock * 1000;
	if (strcasecmp(hsync, "+hsync") == 0)
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	else if (strcasecmp(hsync, "-hsync") == 0)
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	else
		return -1;

	if (strcasecmp(vsync, "+vsync") == 0)
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	else if (strcasecmp(vsync, "-vsync") == 0)
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	else
		return -1;

	snprintf(mode->name, sizeof mode->name, "%dx%d@%.3f",
		 mode->hdisplay, mode->vdisplay, fclock);

	return 0;
}

static uint32_t
get_eotf_mask(const struct di_info *info)
{
	const struct di_hdr_static_metadata *hdr_static;
	uint32_t mask = 0;

	hdr_static = di_info_get_hdr_static_metadata(info);
	if (!hdr_static->type1)
		return WESTON_EOTF_MODE_SDR;

	if (hdr_static->traditional_sdr)
		mask |= WESTON_EOTF_MODE_SDR;

	if (hdr_static->traditional_hdr)
		mask |= WESTON_EOTF_MODE_TRADITIONAL_HDR;

	if (hdr_static->pq)
		mask |= WESTON_EOTF_MODE_ST2084;

	if (hdr_static->hlg)
		mask |= WESTON_EOTF_MODE_HLG;

	return mask;
}

static uint32_t
get_colorimetry_mask(const struct di_info *info)
{
	const struct di_supported_signal_colorimetry *ssc;
	uint32_t mask = WESTON_COLORIMETRY_MODE_DEFAULT;

	ssc = di_info_get_supported_signal_colorimetry(info);
	if (!ssc)
		return mask;

	if (ssc->bt2020_cycc)
		mask |= WESTON_COLORIMETRY_MODE_BT2020_CYCC;

	if (ssc->bt2020_rgb)
		mask |= WESTON_COLORIMETRY_MODE_BT2020_RGB;

	if (ssc->bt2020_ycc)
		mask |= WESTON_COLORIMETRY_MODE_BT2020_YCC;

	if (ssc->st2113_rgb) {
		mask |= WESTON_COLORIMETRY_MODE_P3D65;
		mask |= WESTON_COLORIMETRY_MODE_P3DCI;
	}

	if (ssc->ictcp)
		mask |= WESTON_COLORIMETRY_MODE_ICTCP;

	return mask;
}

static bool
has_yuv420_cap_map(const struct di_edid_cta *cta)
{
       const struct di_cta_data_block *const *data_blocks;
       enum di_cta_data_block_tag db_tag;
       int i;

       data_blocks = di_edid_cta_get_data_blocks(cta);
       for (i = 0; data_blocks[i] != NULL; i++) {
               db_tag = di_cta_data_block_get_tag(data_blocks[i]);
               if (db_tag == DI_CTA_DATA_BLOCK_YCBCR420_CAP_MAP)
                       return true;
       }

       return false;
}

static uint32_t
get_color_format_mask(const struct di_info *info)
{
       const struct di_edid *edid = di_info_get_edid(info);
       const struct di_edid_color_encoding_formats *fmts =
               di_edid_get_color_encoding_formats(edid);
       const struct di_edid_ext *const *exts = di_edid_get_extensions(edid);
       uint32_t mask = WESTON_COLOR_FORMAT_AUTO;
       int i;

       if (fmts) {
               if (fmts->rgb444)
                       mask |= WESTON_COLOR_FORMAT_RGB;

               if (fmts->ycrcb444)
                       mask |= WESTON_COLOR_FORMAT_YUV444;

               if (fmts->ycrcb422)
                       mask |= WESTON_COLOR_FORMAT_YUV422;
       }

       for (i = 0; exts[i] ; i++) {
               const struct di_edid_cta *cta;
               const struct di_edid_cta_flags *cta_flags;

               switch (di_edid_ext_get_tag(exts[i])) {
               case DI_EDID_EXT_CEA:
                       cta = di_edid_ext_get_cta(exts[i]);
                       cta_flags = di_edid_cta_get_flags(cta);

                       mask |= WESTON_COLOR_FORMAT_RGB;

                       if (cta_flags->ycc444)
                               mask |= WESTON_COLOR_FORMAT_YUV444;

                       if (cta_flags->ycc422)
                               mask |= WESTON_COLOR_FORMAT_YUV422;

                       /* FIXME: YUV420 detection is really complicated,
                        * do it properly at some point...
                        */
                       if (has_yuv420_cap_map(cta))
                               mask |= WESTON_COLOR_FORMAT_YUV420;

                       break;
               default:
                       break;
               }
       }

       return mask;
}

static struct di_info *
drm_head_info_from_edid(struct drm_head_info *dhi,
			const uint8_t *data,
			size_t length)
{
	struct di_info *di_ctx;
	const char *msg;

	di_ctx = di_info_parse_edid(data, length);
	if (!di_ctx) {
		memset(dhi, 0, sizeof(*dhi));
		dhi->eotf_mask = WESTON_EOTF_MODE_SDR;
		dhi->colorimetry_mask = WESTON_COLORIMETRY_MODE_DEFAULT;
		return NULL;
	}

	msg = di_info_get_failure_msg(di_ctx);
	if (msg)
		weston_log("DRM: EDID for the following head fails conformity:\n%s\n", msg);

	dhi->make = di_info_get_make(di_ctx);
	dhi->model = di_info_get_model(di_ctx);
	dhi->serial_number = di_info_get_serial(di_ctx);
	dhi->eotf_mask = get_eotf_mask(di_ctx);
	dhi->colorimetry_mask = get_colorimetry_mask(di_ctx);
	dhi->color_format_mask = get_color_format_mask(di_ctx);

	return di_ctx;
}

void
drm_free_display_info(struct di_info **display_info)
{
	if (!*display_info)
		return;

	di_info_destroy(*display_info);
	*display_info = NULL;
}

static void
drm_head_set_display_data(struct drm_head *head, const void *data, size_t len)
{
	free(head->display_data);

	if (!data || len == 0) {
		head->display_data = NULL;
		head->display_data_len = 0;
		return;
	}

	head->display_data = xmalloc(len);
	head->display_data_len = len;
	memcpy(head->display_data, data, len);
}

static bool
drm_head_maybe_update_display_data(struct drm_head *head,
				   drmModeObjectPropertiesPtr props)
{
	struct drm_device *device = head->connector.device;
	drmModePropertyBlobPtr edid_blob = NULL;
	uint32_t blob_id;
	bool changed = false;

	blob_id =
		drm_property_get_value(
			&head->connector.props[WDRM_CONNECTOR_EDID],
			props, 0);
	if (blob_id)
		edid_blob = drmModeGetPropertyBlob(device->kms_device->fd, blob_id);

	if (edid_blob && edid_blob->length > 0) {
		if (!head->display_data ||
		    head->display_data_len != edid_blob->length ||
		    memcmp(head->display_data, edid_blob->data, edid_blob->length)) {
			drm_head_set_display_data(head, edid_blob->data, edid_blob->length);
			changed = true;
		}
	} else {
		if (head->display_data) {
			drm_head_set_display_data(head, NULL, 0);
			changed = true;
		}
	}

	drmModeFreePropertyBlob(edid_blob);

	return changed;
}

static void
prune_eotf_modes_by_kms_support(struct drm_head *head, uint32_t *eotf_mask)
{
	const struct drm_property_info *info;

	/* Without the KMS property, cannot do anything but SDR. */

	info = &head->connector.props[WDRM_CONNECTOR_HDR_OUTPUT_METADATA];
	if (!head->connector.device->atomic_modeset || info->prop_id == 0)
		*eotf_mask = WESTON_EOTF_MODE_SDR;
}

static uint32_t
drm_head_get_kms_colorimetry_modes(const struct drm_head *head)
{
	const struct drm_property_info *info;

	/* Cannot bother implementing without atomic */
	if (!head->connector.device->atomic_modeset)
		return WESTON_COLORIMETRY_MODE_DEFAULT;

	info = &head->connector.props[WDRM_CONNECTOR_COLORSPACE];
	if (info->prop_id == 0)
		return WESTON_COLORIMETRY_MODE_DEFAULT;

	uint32_t colorimetry_modes = WESTON_COLORIMETRY_MODE_NONE;
	unsigned i; /* actually enum wdrm_colorspace */

	for (i = 0; i < WDRM_COLORSPACE__COUNT; i++) {
		if (info->enum_values[i].valid) {
			const struct weston_colorimetry_mode_info *cm;

			cm = weston_colorimetry_mode_info_get_by_wdrm(i);
			if (cm)
				colorimetry_modes |= cm->mode;
		}
	}

	return colorimetry_modes;
}

static bool
drm_head_get_margin_caps(const struct drm_head *head,
			 uint32_t *hborder_max,
			 uint32_t *vborder_max)
{
	const struct drm_property_info *info;
	uint32_t border = 0;

	info = &head->connector.props[WDRM_CONNECTOR_LEFT_MARGIN];
	if (info->prop_id == 0 || info->num_range_values != 2)
		goto no_margins;
	border = info->range_values[1];

	info = &head->connector.props[WDRM_CONNECTOR_RIGHT_MARGIN];
	if (info->prop_id == 0 || info->num_range_values != 2)
		goto no_margins;
	*hborder_max = MIN(border, info->range_values[1]);

	info = &head->connector.props[WDRM_CONNECTOR_TOP_MARGIN];
	if (info->prop_id == 0 || info->num_range_values != 2)
		goto no_margins;
	border = info->range_values[1];

	info = &head->connector.props[WDRM_CONNECTOR_BOTTOM_MARGIN];
	if (info->prop_id == 0 || info->num_range_values != 2)
		goto no_margins;
	*vborder_max = MIN(border, info->range_values[1]);

	return true;

no_margins:
	*hborder_max = 0;
	*vborder_max = 0;
	return false;
}

static void
drm_head_get_underscan_caps(const struct drm_head *head,
			    uint32_t *hborder_max_out,
			    uint32_t *vborder_max_out)
{
	const struct drm_property_info *info;
	uint32_t hborder_max;

	*hborder_max_out = 0;
	*vborder_max_out = 0;
	info = &head->connector.props[WDRM_CONNECTOR_UNDERSCAN];
	if (info->prop_id == 0)
		return;

	info = &head->connector.props[WDRM_CONNECTOR_UNDERSCAN_HBORDER];
	if (info->prop_id == 0 || info->num_range_values != 2)
		return;
	hborder_max = info->range_values[1];

	info = &head->connector.props[WDRM_CONNECTOR_UNDERSCAN_VBORDER];
	if (info->prop_id == 0 || info->num_range_values != 2)
		return;
	*vborder_max_out = info->range_values[1];

	*hborder_max_out = hborder_max;
}

static enum weston_color_format
color_format_from_wdrm_color_format(enum wdrm_color_format format)
{
	switch (format) {
	case WDRM_COLOR_FORMAT_AUTO:
		return WESTON_COLOR_FORMAT_AUTO;
	case WDRM_COLOR_FORMAT_RGB:
		return WESTON_COLOR_FORMAT_RGB;
	case WDRM_COLOR_FORMAT_YUV444:
		return WESTON_COLOR_FORMAT_YUV444;
	case WDRM_COLOR_FORMAT_YUV422:
		return WESTON_COLOR_FORMAT_YUV422;
	case WDRM_COLOR_FORMAT_YUV420:
		return WESTON_COLOR_FORMAT_YUV420;
	default:
		return WESTON_COLOR_FORMAT_AUTO;
	}
}

static uint32_t
drm_head_get_kms_color_formats(const struct drm_head *head)
{
	const struct drm_property_info *info;
	uint32_t color_formats = WESTON_COLOR_FORMAT_AUTO;
	unsigned i;

	/* Cannot bother implementing without atomic */
	if (!head->connector.device->atomic_modeset)
		return color_formats;

	info = &head->connector.props[WDRM_CONNECTOR_COLOR_FORMAT];
	if (info->prop_id == 0)
		return color_formats;

	for (i = 0; i < WDRM_COLOR_FORMAT__COUNT; i++)
		if (info->enum_values[i].valid)
			color_formats |= color_format_from_wdrm_color_format(i);

	return color_formats;
}

static uint32_t
drm_refresh_rate_mHz(const drmModeModeInfo *info)
{
	uint64_t refresh;

	/* Calculate higher precision (mHz) refresh rate */
	refresh = (info->clock * 1000000LL / info->htotal +
		   info->vtotal / 2) / info->vtotal;

	if (info->flags & DRM_MODE_FLAG_INTERLACE)
		refresh *= 2;
	if (info->flags & DRM_MODE_FLAG_DBLSCAN)
		refresh /= 2;
	if (info->vscan > 1)
	    refresh /= info->vscan;

	return refresh;
}

/**
 * Add a mode to output's mode list
 *
 * Copy the supplied DRM mode into a Weston mode structure, and add it to the
 * output's mode list.
 *
 * @param output DRM output to add mode to
 * @param info DRM mode structure to add
 * @returns Newly-allocated Weston/DRM mode structure
 */
static struct drm_mode *
drm_output_add_mode(struct drm_output *output, const drmModeModeInfo *info)
{
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct drm_mode *mode;

	mode = malloc(sizeof *mode);
	if (mode == NULL)
		return NULL;

	mode->base.flags = 0;
	mode->base.width = info->hdisplay;
	mode->base.height = info->vdisplay;

	/* Override mode dimensions with virtual size if set */
	if (b->virtual_width && b->virtual_height) {
		mode->base.width = b->virtual_width;
		mode->base.height = b->virtual_height;
	} else if (output->base.fixed_size) {
		mode->base.width = output->base.width;
		mode->base.height = output->base.height;
	}

	mode->base.refresh = drm_refresh_rate_mHz(info);
	mode->mode_info = *info;
	mode->blob_id = 0;

	if (info->type & DRM_MODE_TYPE_PREFERRED)
		mode->base.flags |= WL_OUTPUT_MODE_PREFERRED;

	mode->base.aspect_ratio = drm_to_weston_mode_aspect_ratio(info->flags);

	wl_list_insert(output->base.mode_list.prev, &mode->base.link);

	return mode;
}

/**
 * Destroys a mode, and removes it from the list.
 */
static void
drm_output_destroy_mode(struct drm_device *device, struct drm_mode *mode)
{
	if (mode->blob_id)
		drmModeDestroyPropertyBlob(device->kms_device->fd, mode->blob_id);
	wl_list_remove(&mode->base.link);
	free(mode);
}

/** Destroy a list of drm_modes
 *
 * @param device The device for releasing mode property blobs.
 * @param mode_list The list linked by drm_mode::base.link.
 */
void
drm_mode_list_destroy(struct drm_device *device, struct wl_list *mode_list)
{
	struct drm_mode *mode, *next;

	wl_list_for_each_safe(mode, next, mode_list, base.link)
		drm_output_destroy_mode(device, mode);
}

void
drm_output_print_modes(struct drm_output *output)
{
	struct weston_mode *m;
	struct drm_mode *dm;
	const char *aspect_ratio;
	bool virtual_size = false;

	wl_list_for_each(m, &output->base.mode_list, link) {
		dm = to_drm_mode(m);

		aspect_ratio = aspect_ratio_to_string(m->aspect_ratio);
		weston_log_continue(STAMP_SPACE "%s@%.1f%s%s%s, %.1f MHz\n",
				    dm->mode_info.name, m->refresh / 1000.0,
				    aspect_ratio,
				    m->flags & WL_OUTPUT_MODE_PREFERRED ?
				    ", preferred" : "",
				    m->flags & WL_OUTPUT_MODE_CURRENT ?
				    ", current" : "",
				    dm->mode_info.clock / 1000.0);

		if(m->flags & WL_OUTPUT_MODE_CURRENT &&
		   (dm->mode_info.hdisplay != m->width ||
		    dm->mode_info.vdisplay != m->height))
			virtual_size = true;
	}

	if (virtual_size)
		weston_log("Output %s: using virtual size %dx%d\n",
			   output->base.name,
			   output->base.current_mode->width,
			   output->base.current_mode->height);
}


/**
 * Find the closest-matching mode for a given target
 *
 * Given a target mode, find the most suitable mode amongst the output's
 * current mode list to use, preferring the current mode if possible, to
 * avoid an expensive mode switch.
 *
 * @param output DRM output
 * @param target_mode Mode to attempt to match
 * @returns Pointer to a mode from the output's mode list
 */
struct drm_mode *
drm_output_choose_mode(struct drm_output *output,
		       struct weston_mode *target_mode)
{
	struct drm_mode *tmp_mode = NULL, *mode_fall_back = NULL, *mode, *tmode;
	enum weston_mode_aspect_ratio src_aspect = WESTON_MODE_PIC_AR_NONE;
	enum weston_mode_aspect_ratio target_aspect = WESTON_MODE_PIC_AR_NONE;
	struct drm_device *device;

	mode = to_drm_mode(output->base.current_mode);
	tmode = to_drm_mode(target_mode);

	device = output->device;

	target_aspect = target_mode->aspect_ratio;
	src_aspect = output->base.current_mode->aspect_ratio;
	if (!strcmp(mode->mode_info.name, tmode->mode_info.name) &&
	    (output->base.current_mode->refresh == target_mode->refresh ||
	     target_mode->refresh == 0)) {
		if (!device->aspect_ratio_supported || src_aspect == target_aspect)
			return to_drm_mode(output->base.current_mode);
	}

	wl_list_for_each(mode, &output->base.mode_list, base.link) {

		src_aspect = mode->base.aspect_ratio;
		if (!strcmp(mode->mode_info.name, tmode->mode_info.name)) {
			if (mode->base.refresh == target_mode->refresh ||
			    target_mode->refresh == 0) {
				if (!device->aspect_ratio_supported ||
				    src_aspect == target_aspect)
					return mode;
				else if (!mode_fall_back)
					mode_fall_back = mode;
			} else if (!tmp_mode) {
				tmp_mode = mode;
			}
		}
	}

	if (mode_fall_back)
		return mode_fall_back;

	return tmp_mode;
}

void
update_head_from_connector(struct drm_head *head)
{
	struct drm_connector *connector = &head->connector;
	struct drm_backend *b = head->connector.device->backend;
	drmModeObjectProperties *props = connector->props_drm;
	drmModeConnector *conn = connector->conn;
	int vrr_capable;
	uint32_t vrr_mode_mask = 0;
	uint32_t hborder_max, vborder_max;
	uint32_t conn_id = head->connector.connector_id;
	bool ret;

	ret = weston_head_set_non_desktop(&head->base,
				    check_non_desktop(connector, props));
	if (ret)
		drm_debug(b, "\t[CONN:%d] non-desktop property changed\n", conn_id);

	ret = weston_head_set_subpixel(&head->base,
				 drm_subpixel_to_wayland(conn->subpixel));
	if (ret)
		drm_debug(b, "\t[CONN:%d] subpixel property changed\n", conn_id);

	ret = weston_head_set_physical_size(&head->base, conn->mmWidth, conn->mmHeight);
	if (ret)
		drm_debug(b, "\t[CONN:%d] physical size changed\n", conn_id);

	ret = weston_head_set_transform(&head->base,
				  get_panel_orientation(connector, props));
	if (ret)
		drm_debug(b, "\t[CONN:%d] transform property changed\n", conn_id);

	/* Unknown connection status is assumed disconnected. */
	ret = weston_head_set_connection_status(&head->base,
				conn->connection == DRM_MODE_CONNECTED);
	if (ret)
		drm_debug(b, "\t[CONN:%d] connection status changed\n", conn_id);

	/* If EDID did not change, skip everything about it */
	if (!drm_head_maybe_update_display_data(head, props)) {
		if (!ret)
			drm_debug(b, "\t[CONN:%d] Hot-plug event received "
				  "but no connector changes detected\n", conn_id);
		return;
	}

	struct drm_head_info dhi;

	drm_free_display_info(&head->base.display_info);
	head->base.display_info = drm_head_info_from_edid(&dhi, head->display_data,
							  head->display_data_len);
	weston_head_set_device_changed(&head->base);

	weston_head_set_monitor_strings(&head->base, dhi.make,
					dhi.model,
					dhi.serial_number);

	prune_eotf_modes_by_kms_support(head, &dhi.eotf_mask);

	weston_head_set_supported_eotf_mask(&head->base, dhi.eotf_mask);

	dhi.colorimetry_mask &= drm_head_get_kms_colorimetry_modes(head);
	weston_head_set_supported_colorimetry_mask(&head->base, dhi.colorimetry_mask);

	vrr_capable = drm_property_get_value(&head->connector.props[WDRM_CONNECTOR_VRR_CAPABLE],
					      head->connector.props_drm, 0);
	if (vrr_capable)
		vrr_mode_mask = WESTON_VRR_MODE_GAME;
	weston_head_set_supported_vrr_modes_mask(&head->base, vrr_mode_mask);

	if (!drm_head_get_margin_caps(head, &hborder_max, &vborder_max))
		drm_head_get_underscan_caps(head, &hborder_max, &vborder_max);
	weston_head_set_supported_underscan(&head->base, hborder_max, vborder_max);

	dhi.color_format_mask &= drm_head_get_kms_color_formats(head);
	weston_head_set_supported_color_format_mask(&head->base, dhi.color_format_mask);

	drm_head_info_fini(&dhi);
}

/**
 * Choose suitable mode for an output
 *
 * Find the most suitable mode to use for initial setup (or reconfiguration on
 * hotplug etc) for a DRM output.
 *
 * @param device the DRM device
 * @param output DRM output to choose mode for
 * @param mode Strategy and preference to use when choosing mode
 * @param modeline Manually-entered mode (may be NULL)
 * @param current_mode Mode currently being displayed on this output
 * @returns A mode from the output's mode list, or NULL if none available
 */
struct drm_mode *
drm_output_choose_initial_mode(struct drm_device *device,
			       struct drm_output *output,
			       enum weston_drm_backend_output_mode mode,
			       const char *modeline,
			       const drmModeModeInfo *current_mode)
{
	struct drm_mode *preferred = NULL;
	struct drm_mode *current = NULL;
	struct drm_mode *configured = NULL;
	struct drm_mode *config_fall_back = NULL;
	struct drm_mode *best = NULL;
	struct drm_mode *drm_mode;
	drmModeModeInfo drm_modeline;
	char name[16] = "\0";
	int32_t width = 0;
	int32_t height = 0;
	uint32_t refresh = 0;
	uint32_t aspect_width = 0;
	uint32_t aspect_height = 0;
	enum weston_mode_aspect_ratio aspect_ratio = WESTON_MODE_PIC_AR_NONE;
	int n;

	if (mode == WESTON_DRM_BACKEND_OUTPUT_PREFERRED && modeline) {
		sscanf(modeline, "%12[^@pP]", name);

		n = sscanf(modeline, "%dx%d%*[^0-9]%d %u:%u", &width, &height,
			   &refresh, &aspect_width, &aspect_height);
		if (device->aspect_ratio_supported && n == 5) {
			if (aspect_width == 4 && aspect_height == 3)
				aspect_ratio = WESTON_MODE_PIC_AR_4_3;
			else if (aspect_width == 16 && aspect_height == 9)
				aspect_ratio = WESTON_MODE_PIC_AR_16_9;
			else if (aspect_width == 64 && aspect_height == 27)
				aspect_ratio = WESTON_MODE_PIC_AR_64_27;
			else if (aspect_width == 256 && aspect_height == 135)
				aspect_ratio = WESTON_MODE_PIC_AR_256_135;
			else
				weston_log("Invalid modeline \"%s\" for output %s\n",
					   modeline, output->base.name);
		}
		if (n != 2 && n != 3 && n != 5) {
			width = -1;

			if (parse_modeline(modeline, &drm_modeline) == 0) {
				configured = drm_output_add_mode(output, &drm_modeline);
				if (!configured)
					return NULL;
			} else {
				weston_log("Invalid modeline \"%s\" for output %s\n",
					   modeline, output->base.name);
			}
		}
	}

	wl_list_for_each_reverse(drm_mode, &output->base.mode_list, base.link) {
		if (!strcmp(name, drm_mode->mode_info.name) &&
		    (refresh == 0 || refresh == drm_mode->mode_info.vrefresh)) {
			if (!device->aspect_ratio_supported ||
			    aspect_ratio == drm_mode->base.aspect_ratio)
				configured = drm_mode;
			else
				config_fall_back = drm_mode;
		}

		if (memcmp(current_mode, &drm_mode->mode_info,
			   sizeof *current_mode) == 0)
			current = drm_mode;

		if (drm_mode->base.flags & WL_OUTPUT_MODE_PREFERRED)
			preferred = drm_mode;

		best = drm_mode;
	}

	if (current == NULL && current_mode->clock != 0) {
		current = drm_output_add_mode(output, current_mode);
		if (!current)
			return NULL;
	}

	if (mode == WESTON_DRM_BACKEND_OUTPUT_CURRENT)
		configured = current;

	if (configured)
		return configured;

	if (config_fall_back)
		return config_fall_back;

	if (preferred)
		return preferred;

	if (current)
		return current;

	if (best)
		return best;

	weston_log("no available modes for %s\n", output->base.name);
	return NULL;
}

static uint32_t
u32distance(uint32_t a, uint32_t b)
{
	if (a < b)
		return b - a;
	else
		return a - b;
}

/** Choose equivalent mode
 *
 * If the two modes are not equivalent, return NULL.
 * Otherwise return the mode that is more likely to work in place of both.
 *
 * None of the fuzzy matching criteria in this function have any justification.
 *
 * typedef struct _drmModeModeInfo {
 *         uint32_t clock;
 *         uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
 *         uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
 *
 *         uint32_t vrefresh;
 *
 *         uint32_t flags;
 *         uint32_t type;
 *         char name[DRM_DISPLAY_MODE_LEN];
 * } drmModeModeInfo, *drmModeModeInfoPtr;
 */
static const drmModeModeInfo *
drm_mode_pick_equivalent(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
	uint32_t refresh_a, refresh_b;

	if (a->hdisplay != b->hdisplay || a->vdisplay != b->vdisplay)
		return NULL;

	if (a->flags != b->flags)
		return NULL;

	/* kHz */
	if (u32distance(a->clock, b->clock) > 500)
		return NULL;

	refresh_a = drm_refresh_rate_mHz(a);
	refresh_b = drm_refresh_rate_mHz(b);
	if (u32distance(refresh_a, refresh_b) > 50)
		return NULL;

	if ((a->type ^ b->type) & DRM_MODE_TYPE_PREFERRED) {
		if (a->type & DRM_MODE_TYPE_PREFERRED)
			return a;
		else
			return b;
	}

	return a;
}

/* If the given mode info is not already in the list, add it.
 * If it is in the list, either keep the existing or replace it,
 * depending on which one is "better".
 */
static int
drm_output_try_add_mode(struct drm_output *output, const drmModeModeInfo *info)
{
	struct weston_mode *base;
	struct drm_mode *mode = NULL;
	struct drm_device *device = output->device;
	const drmModeModeInfo *chosen = NULL;

	assert(info);

	wl_list_for_each(base, &output->base.mode_list, link) {
		mode = to_drm_mode(base);
		chosen = drm_mode_pick_equivalent(&mode->mode_info, info);
		if (chosen)
			break;
	}

	if (chosen == info) {
		assert(mode);
		drm_output_destroy_mode(device, mode);
		chosen = NULL;
	}

	if (!chosen) {
		mode = drm_output_add_mode(output, info);
		if (!mode)
			return -1;
	}
	/* else { the equivalent mode is already in the list } */

	return 0;
}

/** Rewrite the output's mode list
 *
 * @param output The output.
 * @return 0 on success, -1 on failure.
 *
 * Destroy all existing modes in the list, and reconstruct a new list from
 * scratch, based on the currently attached heads.
 *
 * On failure the output's mode list may contain some modes.
 */
static int
drm_output_update_modelist_from_heads(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct weston_head *head_base;
	struct drm_head *head;
	drmModeConnector *conn;
	int i;
	int ret;

	assert(!output->base.enabled);

	drm_mode_list_destroy(device, &output->base.mode_list);

	wl_list_for_each(head_base, &output->base.head_list, output_link) {
		head = to_drm_head(head_base);
		conn = head->connector.conn;
		for (i = 0; i < conn->count_modes; i++) {
			ret = drm_output_try_add_mode(output, &conn->modes[i]);
			if (ret < 0)
				return -1;
		}
	}

	return 0;
}

int
drm_output_set_mode(struct weston_output *base,
		    enum weston_drm_backend_output_mode mode,
		    const char *modeline)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_device *device = output->device;
	struct drm_head *head = to_drm_head(weston_output_get_first_head(base));

	struct drm_mode *current;

	if (output->is_virtual)
		return -1;

	if (drm_output_update_modelist_from_heads(output) < 0)
		return -1;

	current = drm_output_choose_initial_mode(device, output, mode, modeline,
						 &head->inherited_mode);
	if (!current)
		return -1;

	output->base.current_mode = &current->base;
	output->base.current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	/* Set native_ fields, so weston_output_mode_switch_to_native() works */
	weston_output_copy_native_mode(&output->base, output->base.current_mode);
	output->base.native_scale = output->base.current_scale;

	return 0;
}
