/*
 * Copyright 2021 Advanced Micro Devices, Inc.
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

#include <math.h>

#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"
#include "color_util.h"
#include <string.h>
#include <lcms2.h>
#include <linux/limits.h>

struct lcms_pipeline {
	/**
	 * Color space name
	 */
	const char *color_space;
	/**
	 * Chromaticities for output profile
	 */
	cmsCIExyYTRIPLE prim_output;
	/**
	 * tone curve enum
	 */
	enum transfer_fn pre_fn;
	/**
	 * Transform matrix from sRGB to target chromaticities in prim_output
	 */
	struct lcmsMAT3 mat;
	/**
	 * tone curve enum
	 */
	enum transfer_fn post_fn;
	/**
	 * 2/255 or 3/255 maximum possible error, where 255 is 8 bit max value
	 */
	int tolerance;
};

static const int WINDOW_WIDTH  = 256;
static const int WINDOW_HEIGHT = 24;

static cmsCIExyY wp_d65 = { 0.31271, 0.32902, 1.0 };

struct setup_args {
	struct fixture_metadata meta;
	struct lcms_pipeline pipeline;
};

/*
 * Using currently destination gamut bigger than source.
 * Using https://www.colour-science.org/ we can extract conversion matrix:
 * import colour
 * colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['sRGB'], colour.RGB_COLOURSPACES['Adobe RGB (1998)'], None)
 * colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['sRGB'], colour.RGB_COLOURSPACES['ITU-R BT.2020'], None)
 */
const struct setup_args arr_setup[] = {
	{
		.meta.name = "sRGB->sRGB unity",
		.pipeline = {
			.color_space = "sRGB",
			.prim_output = {
				.Red =   { 0.640, 0.330, 1.0 },
				.Green = { 0.300, 0.600, 1.0 },
				.Blue =  { 0.150, 0.060, 1.0 }
			},
			.pre_fn = TRANSFER_FN_SRGB_EOTF,
			.mat = LCMSMAT3(1.0, 0.0, 0.0,
					0.0, 1.0, 0.0,
					0.0, 0.0, 1.0),
			.post_fn = TRANSFER_FN_SRGB_EOTF_INVERSE,
			.tolerance = 0
		}
	},
	{
		.meta.name = "sRGB->adobeRGB",
		.pipeline = {
			.color_space = "adobeRGB",
			.prim_output = {
				.Red =   { 0.640, 0.330, 1.0 },
				.Green = { 0.210, 0.710, 1.0 },
				.Blue =  { 0.150, 0.060, 1.0 }
			},
			.pre_fn = TRANSFER_FN_SRGB_EOTF,
			.mat = LCMSMAT3(0.715119, 0.284881, 0.0,
					0.0,      1.0,      0.0,
					0.0,      0.041169, 0.958831),
			.post_fn = TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE,
			.tolerance = 1
			/*
			 * Tolerance depends more on the 1D LUT used for the
			 * inv EOTF than the tested 3D LUT size:
			 * 9x9x9, 17x17x17, 33x33x33, 127x127x127
			 */
		}
	},
	{
		.meta.name = "sRGB->bt2020",
		.pipeline = {
			.color_space = "bt2020",
			.prim_output = {
				.Red =   { 0.708, 0.292, 1.0 },
				.Green = { 0.170, 0.797, 1.0 },
				.Blue =  { 0.131, 0.046, 1.0 }
			},
			.pre_fn = TRANSFER_FN_SRGB_EOTF,
			.mat = LCMSMAT3(0.627402, 0.329292, 0.043306,
					0.069095, 0.919544, 0.011360,
					0.016394, 0.088028, 0.895578),
			/* this is equivalent to BT.1886 with zero black level */
			.post_fn = TRANSFER_FN_POWER2_4_EOTF_INVERSE,
			.tolerance = 5
			/*
			 * TODO: when we add power-law in the curve enumeration
			 * in GL-renderer, then we should fix the tolerance
			 * as the error should reduce a lot.
			 */
		}
	}
};

struct image_header {
	int width;
	int height;
	int stride;
	int depth;
	pixman_format_code_t pix_format;
	uint32_t *data;
};

static void
get_image_prop(struct buffer *buf, struct image_header *header)
{
	header->width  = pixman_image_get_width(buf->image);
	header->height = pixman_image_get_height(buf->image);
	header->stride = pixman_image_get_stride(buf->image);
	header->depth = pixman_image_get_depth(buf->image);
	header->pix_format = pixman_image_get_format (buf->image);
	header->data = pixman_image_get_data(buf->image);
}

static void
gen_ramp_rgb(const struct image_header *header, int bitwidth, int width_bar)
{
	static const int hue[][3] = {
		{ 1, 1, 1 },	/* White	*/
		{ 1, 1, 0 },	/* Yellow 	*/
		{ 0, 1, 1 },	/* Cyan 	*/
		{ 0, 1, 0 },	/* Green 	*/
		{ 1, 0, 1 },	/* Magenta 	*/
		{ 1, 0, 0 },	/* Red 		*/
		{ 0, 0, 1 },	/* Blue 	*/
	};
	const int num_hues = ARRAY_LENGTH(hue);

	float val_max;
	int x, y;
	int hue_index;
	float value;
	unsigned char r, g, b;
	uint32_t *pixel;

	float n_steps = width_bar - 1;

	val_max = (1 << bitwidth) - 1;

	for (y = 0; y < header->height; y++) {
		hue_index = (y * num_hues) / (header->height - 1);
		hue_index = MIN(hue_index, num_hues - 1);

		for (x = 0; x < header->width; x++) {
			struct color_float rgb = { .rgb = { 0, 0, 0 } };

			value = (float)x / (float)(header->width - 1);

			if (width_bar > 1)
				value = floor(value * n_steps) / n_steps;

			if (hue[hue_index][0])
				rgb.r = value;
			if (hue[hue_index][1])
				rgb.g = value;
			if (hue[hue_index][2])
				rgb.b = value;

			sRGB_delinearize(&rgb);

			r = round(rgb.r * val_max);
			g = round(rgb.g * val_max);
			b = round(rgb.b * val_max);

			pixel = header->data + (y * header->stride / 4) + x;
			*pixel = (255U << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static cmsHPROFILE
build_lcms_profile_output(const struct lcms_pipeline *pipeline)
{
	cmsToneCurve *arr_curves[3];
	cmsHPROFILE hRGB;
	int type_inverse_tone_curve;
	double inverse_tone_curve_param[5];

	assert(find_tone_curve_type(pipeline->post_fn, &type_inverse_tone_curve,
				    inverse_tone_curve_param));

	/*
	 * We are creating output profile and therefore we can use the following:
	 * calling semantics:
	 * cmsBuildParametricToneCurve(type_inverse_tone_curve, inverse_tone_curve_param)
	 * The function find_tone_curve_type sets the type of curve positive if it
	 * is tone curve and negative if it is inverse. When we create an ICC
	 * profile we should use a tone curve, the inversion is done by LCMS
	 * when the profile is used for output.
	 */

	arr_curves[0] = arr_curves[1] = arr_curves[2] =
		cmsBuildParametricToneCurve(NULL,
					    (-1) * type_inverse_tone_curve,
					    inverse_tone_curve_param);

	assert(arr_curves[0]);
	hRGB = cmsCreateRGBProfileTHR(NULL, &wp_d65,
				      &pipeline->prim_output, arr_curves);
	assert(hRGB);

	cmsFreeToneCurve(arr_curves[0]);
	return hRGB;
}

static char *
build_output_icc_profile(const struct lcms_pipeline *pipe)
{
	char *profile_name = NULL;
	cmsHPROFILE profile = NULL;
	char *wd;
	int ret;
	bool saved;

	wd = realpath(".", NULL);
	assert(wd);
	ret = asprintf(&profile_name, "%s/matrix-shaper-test-%s.icm", wd,
		       pipe->color_space);
	assert(ret > 0);

	profile = build_lcms_profile_output(pipe);
	assert(profile);

	saved = cmsSaveProfileToFile(profile, profile_name);
	assert(saved);

	cmsCloseProfile(profile);

	return profile_name;
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;
	char *file_name;

	compositor_setup_defaults(&setup);
	setup.renderer = RENDERER_GL;
	setup.backend = WESTON_BACKEND_HEADLESS;
	setup.width = WINDOW_WIDTH;
	setup.height = WINDOW_HEIGHT;
	setup.shell = SHELL_TEST_DESKTOP;

	file_name = build_output_icc_profile(&arg->pipeline);
	if (!file_name)
		return RESULT_HARD_ERROR;

	weston_ini_setup(&setup,
		cfgln("[core]"),
		cfgln("color-management=true"),
		cfgln("[output]"),
		cfgln("name=headless"),
		cfgln("icc_profile=%s", file_name));

	free(file_name);

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, arr_setup, meta);

static bool
compare_float(float ref, float dst, int x, const char *chan,
	      float *max_diff, float max_allow_diff)
{
#if 0
	/*
	 * This file can be loaded in Octave for visualization.
	 *
	 * S = load('compare_float_dump.txt');
	 *
	 * rvec = S(S(:,1)==114, 2:3);
	 * gvec = S(S(:,1)==103, 2:3);
	 * bvec = S(S(:,1)==98, 2:3);
	 *
	 * figure
	 * subplot(3, 1, 1);
	 * plot(rvec(:,1), rvec(:,2) .* 255, 'r');
	 * subplot(3, 1, 2);
	 * plot(gvec(:,1), gvec(:,2) .* 255, 'g');
	 * subplot(3, 1, 3);
	 * plot(bvec(:,1), bvec(:,2) .* 255, 'b');
	 */
	static FILE *fp = NULL;

	if (!fp)
		fp = fopen("compare_float_dump.txt", "w");
	fprintf(fp, "%d %d %f\n", chan[0], x, dst - ref);
	fflush(fp);
#endif

	float diff  = fabsf(ref - dst);

	if (diff > *max_diff)
		*max_diff = diff;

	if (diff <= max_allow_diff)
		return true;

	testlog("x=%d %s: ref %f != dst %f, delta %f\n",
			x, chan, ref, dst, dst - ref);

	return false;
}

static bool
process_pipeline_comparison(const struct image_header *src,
			    const struct image_header *shot,
			    const struct setup_args * arg)
{
	const float max_pixel_value = 255.0;
	struct color_float max_diff_pipeline = { .rgb = { 0.0f, 0.0f, 0.0f } };
	float max_allow_diff = arg->pipeline.tolerance / max_pixel_value;
	float max_err = 0;
	float f_max_err = 0;
	bool ok = true;
	uint32_t *row_ptr, *row_ptr_shot;
	int y, x;
	struct color_float pix_src;
	struct color_float pix_src_pipeline;
	struct color_float pix_shot;

	for (y = 0; y < src->height; y++) {
		row_ptr = (uint32_t*)((uint8_t*)src->data + (src->stride * y));
		row_ptr_shot  = (uint32_t*)((uint8_t*)shot->data + (shot->stride * y));

		for (x = 0; x < src->width; x++) {
			pix_src = a8r8g8b8_to_float(row_ptr[x]);
			pix_shot = a8r8g8b8_to_float(row_ptr_shot[x]);
			/* do pipeline processing */
			process_pixel_using_pipeline(arg->pipeline.pre_fn,
						     &arg->pipeline.mat,
						     arg->pipeline.post_fn,
						     &pix_src, &pix_src_pipeline);
			/* check if pipeline matches to shader variant */
			ok &= compare_float(pix_src_pipeline.r, pix_shot.r, x,"r",
					    &max_diff_pipeline.r, max_allow_diff);
			ok &= compare_float(pix_src_pipeline.g, pix_shot.g, x, "g",
					    &max_diff_pipeline.g, max_allow_diff);
			ok &= compare_float(pix_src_pipeline.b, pix_shot.b, x, "b",
					    &max_diff_pipeline.b, max_allow_diff);
		}
	}
	max_err = max_diff_pipeline.r;
	if (max_err < max_diff_pipeline.g)
		max_err = max_diff_pipeline.g;
	if (max_err < max_diff_pipeline.b)
		max_err = max_diff_pipeline.b;

	f_max_err = max_pixel_value * max_err;

	testlog("%s %s %s tol_req %d, tol_cal %f, max diff: r=%f, g=%f, b=%f\n",
		__func__, ok == true? "SUCCESS":"FAILURE",
		arg->meta.name, arg->pipeline.tolerance, f_max_err,
		max_diff_pipeline.r, max_diff_pipeline.g, max_diff_pipeline.b);

	return ok;
}

static bool
check_process_pattern_ex(struct buffer *src, struct buffer *shot,
		const struct setup_args * arg)
{
	struct image_header header_src;
	struct image_header header_shot;
	bool ok;

	get_image_prop(src, &header_src);
	get_image_prop(shot, &header_shot);

	/* no point to compare different images */
	assert(header_src.width == header_shot.width);
	assert(header_src.height == header_shot.height);

	ok = process_pipeline_comparison(&header_src, &header_shot, arg);

	return ok;
}

/*
 * Test that matrix-shaper profile does CM correctly, it is used color ramp pattern
 */
TEST(shaper_matrix)
{
	const int width = WINDOW_WIDTH;
	const int height = WINDOW_HEIGHT;
	const int bitwidth = 8;
	const int width_bar = 32;

	struct client *client;
	struct buffer *buf;
	struct buffer *shot;
	struct wl_surface *surface;
	struct image_header image;
	bool match;
	int seq_no = get_test_fixture_index();

	client = create_client_and_test_surface(0, 0, width, height);
	assert(client);
	surface = client->surface->wl_surface;

	buf = create_shm_buffer_a8r8g8b8(client, width, height);
	get_image_prop(buf, &image);
	gen_ramp_rgb(&image, bitwidth, width_bar);

	wl_surface_attach(surface, buf->proxy, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	shot = capture_screenshot_of_output(client);
	assert(shot);

	match = verify_image(shot, "shaper_matrix", seq_no, NULL, seq_no);
	assert(check_process_pattern_ex(buf, shot, &arr_setup[seq_no]));
	assert(match);
	buffer_destroy(shot);
	buffer_destroy(buf);
	client_destroy(client);
}
