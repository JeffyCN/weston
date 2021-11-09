/*
 * Copyright 2020 Collabora, Ltd.
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
#include "color_util.h"
#include <assert.h>

static float
sRGB_EOTF(float e)
{
	assert(e >= 0.0f);
	assert(e <= 1.0f);

	if (e <= 0.04045)
		return e / 12.92;
	else
		return pow((e + 0.055) / 1.055, 2.4);
}

static float
sRGB_EOTF_inv(float o)
{
	assert(o >= 0.0f);
	assert(o <= 1.0f);

	if (o <= 0.04045 / 12.92)
		return o * 12.92;
	else
		return pow(o, 1.0 / 2.4) * 1.055 - 0.055;
}


void
sRGB_linearize(struct color_float *cf)
{
	cf->r = sRGB_EOTF(cf->r);
	cf->g = sRGB_EOTF(cf->g);
	cf->b = sRGB_EOTF(cf->b);
}

void
sRGB_delinearize(struct color_float *cf)
{
	cf->r = sRGB_EOTF_inv(cf->r);
	cf->g = sRGB_EOTF_inv(cf->g);
	cf->b = sRGB_EOTF_inv(cf->b);
}

struct color_float
a8r8g8b8_to_float(uint32_t v)
{
	struct color_float cf;

	cf.a = ((v >> 24) & 0xff) / 255.f;
	cf.r = ((v >> 16) & 0xff) / 255.f;
	cf.g = ((v >>  8) & 0xff) / 255.f;
	cf.b = ((v >>  0) & 0xff) / 255.f;

	return cf;
}
