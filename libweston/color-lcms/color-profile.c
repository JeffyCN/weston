/*
 * Copyright 2019 Sebastian Wick
 * Copyright 2021 Collabora, Ltd.
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
#include <stdio.h>
#include <string.h>
#include <libweston/libweston.h>

#include "color.h"
#include "color-lcms.h"
#include "shared/helpers.h"
#include "shared/string-helpers.h"

/* FIXME: sync with spec! */
static bool
validate_icc_profile(cmsHPROFILE profile, char **errmsg)
{
	cmsColorSpaceSignature cs = cmsGetColorSpace(profile);
	uint32_t nr_channels = cmsChannelsOf(cs);
	uint8_t version = cmsGetEncodedICCversion(profile) >> 24;

	if (version != 2 && version != 4) {
		str_printf(errmsg,
			   "ICC profile major version %d is unsupported, should be 2 or 4.",
			   version);
		return false;
	}

	if (nr_channels != 3) {
		str_printf(errmsg,
			   "ICC profile must contain 3 channels for the color space, not %u.",
			   nr_channels);
		return false;
	}

	if (cmsGetDeviceClass(profile) != cmsSigDisplayClass) {
		str_printf(errmsg, "ICC profile is required to be of Display device class, but it is not.");
		return false;
	}

	return true;
}

static struct cmlcms_color_profile *
cmlcms_find_color_profile_by_md5(const struct weston_color_manager_lcms *cm,
				 const struct cmlcms_md5_sum *md5sum)
{
	struct cmlcms_color_profile *cprof;

	wl_list_for_each(cprof, &cm->color_profile_list, link) {
		if (memcmp(cprof->md5sum.bytes,
			   md5sum->bytes, sizeof(md5sum->bytes)) == 0)
			return cprof;
	}

	return NULL;
}

static struct cmlcms_color_profile *
cmlcms_color_profile_create(struct weston_color_manager_lcms *cm,
			    cmsHPROFILE profile,
			    char *desc,
			    char **errmsg)
{
	struct cmlcms_color_profile *cprof;

	cprof = zalloc(sizeof *cprof);
	if (!cprof)
		return NULL;

	weston_color_profile_init(&cprof->base, &cm->base);
	cprof->base.description = desc;
	cprof->profile = profile;
	cmsGetHeaderProfileID(profile, cprof->md5sum.bytes);
	wl_list_insert(&cm->color_profile_list, &cprof->link);

	return cprof;
}

static void
cmlcms_color_profile_destroy(struct cmlcms_color_profile *cprof)
{
	wl_list_remove(&cprof->link);
	cmsCloseProfile(cprof->profile);
	free(cprof->base.description);
	free(cprof);
}

static char *
make_icc_file_description(cmsHPROFILE profile,
			  const struct cmlcms_md5_sum *md5sum,
			  const char *name_part)
{
	char md5sum_str[sizeof(md5sum->bytes) * 2 + 1];
	char *desc;
	size_t i;

	for (i = 0; i < sizeof(md5sum->bytes); i++) {
		snprintf(md5sum_str + 2 * i, sizeof(md5sum_str) - 2 * i,
			 "%02x", md5sum->bytes[i]);
	}

	str_printf(&desc, "ICCv%f %s %s", cmsGetProfileVersion(profile),
		   name_part, md5sum_str);

	return desc;
}

bool
cmlcms_get_color_profile_from_icc(struct weston_color_manager *cm_base,
				  const void *icc_data,
				  size_t icc_len,
				  const char *name_part,
				  struct weston_color_profile **cprof_out,
				  char **errmsg)
{
	struct weston_color_manager_lcms *cm = get_cmlcms(cm_base);
	cmsHPROFILE profile;
	struct cmlcms_md5_sum md5sum;
	struct cmlcms_color_profile *cprof;
	char *desc = NULL;

	if (!icc_data || icc_len < 1) {
		str_printf(errmsg, "No ICC data.");
		return false;
	}
	if (icc_len >= UINT32_MAX) {
		str_printf(errmsg, "Too much ICC data.");
		return false;
	}

	profile = cmsOpenProfileFromMemTHR(cm->lcms_ctx, icc_data, icc_len);
	if (!profile) {
		str_printf(errmsg, "ICC data not understood.");
		return false;
	}

	if (!validate_icc_profile(profile, errmsg))
		goto err_close;

	if (!cmsMD5computeID(profile)) {
		str_printf(errmsg, "Failed to compute MD5 for ICC profile.");
		goto err_close;
	}

	cmsGetHeaderProfileID(profile, md5sum.bytes);
	cprof = cmlcms_find_color_profile_by_md5(cm, &md5sum);
	if (cprof) {
		*cprof_out = weston_color_profile_ref(&cprof->base);
		cmsCloseProfile(profile);
		return true;
	}

	desc = make_icc_file_description(profile, &md5sum, name_part);
	if (!desc)
		goto err_close;

	cprof = cmlcms_color_profile_create(cm, profile, desc, errmsg);
	if (!cprof)
		goto err_close;

	*cprof_out = &cprof->base;
	return true;

err_close:
	free(desc);
	cmsCloseProfile(profile);
	return false;
}

void
cmlcms_destroy_color_profile(struct weston_color_profile *cprof_base)
{
	struct cmlcms_color_profile *cprof = get_cprof(cprof_base);

	cmlcms_color_profile_destroy(cprof);
}
