/*
 * Copyright 2021-2022 Collabora, Ltd.
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
#include <libweston/libweston.h>
#include <assert.h>
#include <string.h>

#include "drm-internal.h"
#include "libdrm-updates.h"

int
drm_output_ensure_hdr_output_metadata_blob(struct drm_output *output)
{
	struct hdr_output_metadata meta;
	uint32_t blob_id = 0;
	int ret;

	if (output->hdr_output_metadata_blob_id)
		return 0;

	/*
	 * Set up the data for Dynamic Range and Mastering InfoFrame,
	 * CTA-861-G, a.k.a the static HDR metadata.
	 */

	memset(&meta, 0, sizeof meta);

	meta.metadata_type = 0; /* Static Metadata Type 1 */

	/* Duplicated field in UABI struct */
	meta.hdmi_metadata_type1.metadata_type = meta.metadata_type;

	switch (output->base.eotf_mode) {
	case WESTON_EOTF_MODE_NONE:
		assert(0 && "bad eotf_mode: none");
		return -1;
	case WESTON_EOTF_MODE_SDR:
		/*
		 * Do not send any static HDR metadata. Video sinks should
		 * respond by switching to traditional SDR mode. If they
		 * do not, the kernel should fix that up.
		 */
		assert(output->hdr_output_metadata_blob_id == 0);
		return 0;
	case WESTON_EOTF_MODE_TRADITIONAL_HDR:
		meta.hdmi_metadata_type1.eotf = 1; /* from CTA-861-G */
		break;
	case WESTON_EOTF_MODE_ST2084:
		meta.hdmi_metadata_type1.eotf = 2; /* from CTA-861-G */
		break;
	case WESTON_EOTF_MODE_HLG:
		meta.hdmi_metadata_type1.eotf = 3; /* from CTA-861-G */
		break;
	}

	if (meta.hdmi_metadata_type1.eotf == 0) {
		assert(0 && "bad eotf_mode");
		return -1;
	}

	/* The other fields are intentionally left as zeroes. */

	ret = drmModeCreatePropertyBlob(output->backend->drm.fd,
					&meta, sizeof meta, &blob_id);
	if (ret != 0) {
		weston_log("Error: failed to create KMS blob for HDR metadata on output '%s': %s\n",
			   output->base.name, strerror(-ret));
		return -1;
	}

	output->hdr_output_metadata_blob_id = blob_id;

	return 0;
}
