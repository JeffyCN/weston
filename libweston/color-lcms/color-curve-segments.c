/*
 * Copyright 2023 Collabora, Ltd.
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

#include <lcms2_plugin.h>

#include "color-curve-segments.h"
#include "color-lcms.h"

/**
 * LCMS internally defines MINUS_INF and PLUS_INF arbitrarily to -1e22 and 1e22.
 * This is not specified by the ICC spec. So we pick an arbitrary value as well
 * to be able to do comparisons with such LCMS values.
*/
#define CLOSE_TO_INFINITY 1e10

static float
round_segment_break_value(float value)
{
	if (value < -CLOSE_TO_INFINITY)
		return -INFINITY;
	if (value > CLOSE_TO_INFINITY)
		return INFINITY;
	return value;
}

static void
segment_print(const cmsCurveSegment *seg, struct weston_log_scope *scope)
{
	float g, a, b, c, d, e, f;
	float x0 = round_segment_break_value(seg->x0);
	float x1 = round_segment_break_value(seg->x1);

	weston_log_scope_printf(scope, "%*s(%.2f, %.2f] ", 12, "", x0, x1);

	if (seg->Type == 0) {
		/* Not much to print as this is a sampled curve. We have only
		 * the samples in such case, but that would flood the debug
		 * scope and wouldn't be very useful. */
		weston_log_scope_printf(scope, "sampled curve with %u samples\n",
					seg->nGridPoints);
		return;
	}

	weston_log_scope_printf(scope, "parametric type %d%s", seg->Type,
				(seg->Type > 0) ? "\n" : ", inverse of\n");

	/* These types are the built-in ones supported by LCMS. Some of them are
	 * defined by the ICC spec, but LCMS also accepts creating custom
	 * curves. Probably that's why it does not expose these types in enums.
	 * If we start creating custom curves, we should keep track of them
	 * somehow in order to print them properly here. */
	switch (seg->Type) {
	case 1:
	case -1:
		/* Type 1: power law
		 *
		 * y = x ^ g
		 */
		g = seg->Params[0];
		weston_log_scope_printf(scope, "%*sy = x ^ %.2f\n", 15, "", g);
		break;
	case 2:
	case -2:
		/* Type 2: CIE 122-1966
		 *
		 * y = (a * x + b) ^ g | x >= -b/a
		 * y = 0               | else
		 */
		g = seg->Params[0];
		a = seg->Params[1];
		b = seg->Params[2];
		weston_log_scope_printf(scope, "%*sy = (%.2f * x + %.2f) ^ %.2f, for x >= %.2f\n",
					15, "", a, b, g, -b/a);
		weston_log_scope_printf(scope, "%*sy = 0, for x < %.2f\n",
					15, "", -b/a);
		break;
	case 3:
	case -3:
		/* Type 3: IEC 61966-3
		 *
		 * y = (a * x + b) ^ g + c | x <= -b/a
		 * y = c                   | else
		 */
		g = seg->Params[0];
		a = seg->Params[1];
		b = seg->Params[2];
		c = seg->Params[3];
		weston_log_scope_printf(scope, "%*sy = (%.2f * x + %.2f) ^ %.2f + %.2f, for x <= %.2f\n",
					15, "", a, b, g, c, -b/a);
		weston_log_scope_printf(scope, "%*sy = %.2f, for x > %.2f\n",
					15, "", c, -b/a);
		break;
	case 4:
	case -4:
		/* Type 4: IEC 61966-2.1 (sRGB)
		 *
		 * y = (a * x + b) ^ g | x >= d
		 * y = c * x           | else
		 */
		g = seg->Params[0];
		a = seg->Params[1];
		b = seg->Params[2];
		c = seg->Params[3];
		d = seg->Params[4];
		weston_log_scope_printf(scope, "%*sy = (%.2f * x + %.2f) ^ %.2f, for x >= %.2f\n",
					15, "", a, b, g, d);
		weston_log_scope_printf(scope, "%*sy = %.2f * x, for x < %.2f\n",
					15, "", c, d);
		break;
	case 5:
	case -5:
		/* Type 5:
		 *
		 * y = (a * x + b) ^ g + e | x >= d
		 * y = c * x + f           | else
		 */
		g = seg->Params[0];
		a = seg->Params[1];
		b = seg->Params[2];
		c = seg->Params[3];
		d = seg->Params[4];
		e = seg->Params[5];
		f = seg->Params[6];
		weston_log_scope_printf(scope, "%*sy = (%.2f * x + %.2f) ^ %.2f + %.2f, for x >= %.2f\n",
					15, "", a, b, g, e, d);
		weston_log_scope_printf(scope, "%*sy = %.2f * x + %.2f, for x < %.2f\n",
					15, "", c, f, d);
		break;
	case 6:
	case -6:
		/* Type 6: one of the segmented curves described in ICCSpecRevision_02_11_06_Float.pdf
		 *
		 * y = (a * x + b) ^ g + c
		 */
		g = seg->Params[0];
		a = seg->Params[1];
		b = seg->Params[2];
		c = seg->Params[3];

		if (a == 0) {
			/* Special case in which we have a constant value */
			weston_log_scope_printf(scope, "%*sconstant %.2f\n",
						15, "", pow(b, g) + c);
			break;
		}

		weston_log_scope_printf(scope, "%*sy = (%.2f * x + %.2f) ^ %.2f + %.2f\n",
					15, "", a, b, g, c);
		break;
	case 7:
	case -7:
		/* Type 7: one of the segmented curves described in ICCSpecRevision_02_11_06_Float.pdf
		 *
		 * y = a * log (b * x ^ g + c) + d
		 */
		g = seg->Params[0];
		a = seg->Params[1];
		b = seg->Params[2];
		c = seg->Params[3];
		d = seg->Params[4];
		weston_log_scope_printf(scope, "%*sy = %.2f * log (%.2f * x ^ %.2f + %.2f) + %.2f\n",
					15, "", a, b, g, c, d);
		break;
	case 8:
	case -8:
		/* Type 8: one of the segmented curves described in ICCSpecRevision_02_11_06_Float.pdf
		 *
		 * y = a * b ^ (c * x + d) + e
		 */
		a = seg->Params[0];
		b = seg->Params[1];
		c = seg->Params[2];
		d = seg->Params[3];
		e = seg->Params[4];
		weston_log_scope_printf(scope, "%*sy = %.2f * %.2f ^ (%.2f * x + %.2f) + %.2f\n",
					15, "", a, b, c, d, e);
		break;
	case 108:
	case -108:
		/* Type 108: S-shapped
		 *
		 * y = (1 - (1 - x) ^ 1 / g) ^ 1 / g
		 */
		g = seg->Params[0];
		weston_log_scope_printf(scope, "%*sy = (1 - (1 - x) ^ 1 / %.2f) ^ 1 / %.2f\n",
					15, "", g, g);
		break;
	default:
		weston_log_scope_printf(scope, "%*sunknown curve type\n",
					15, "");
		break;
	}
}

static void
curve_print(const cmsToneCurve *curve, struct weston_log_scope *scope)
{
	const cmsCurveSegment *seg;
	unsigned int i;

	weston_log_scope_printf(scope, "%*sSegments\n", 9, "");

	for (i = 0; ; i++) {
		seg = cmsGetToneCurveSegment(i, curve);
		if (!seg)
			break;
		segment_print(seg, scope);
	}

	if (i == 0)
		weston_log_scope_printf(scope, "%*sNo segments\n", 12, "");
}

static bool
are_segment_breaks_equal(float a, float b)
{
	const float PRECISION = 1e-5;

	if (a < -CLOSE_TO_INFINITY && b < -CLOSE_TO_INFINITY)
		return true;
	if (a > CLOSE_TO_INFINITY && b > CLOSE_TO_INFINITY)
		return true;
	if (fabs(b - a) < PRECISION)
		return true;

	return false;
}

static bool
are_segments_equal(const cmsCurveSegment *seg_A, const cmsCurveSegment *seg_B)
{
	/* These come from the built-in supported types from LCMS. See
	 * segment_print() for more information. */
	uint32_t types[] =          {1, 2, 3, 4, 5, 6, 7, 8, 108};
	uint32_t types_n_params[] = {1, 3, 4, 5, 7, 4, 5, 5, 1};
	unsigned int i;
	const float PRECISION = 1e-5;
	int32_t n_params = -1;

	if (seg_A->Type != seg_B->Type)
		return false;

	if (!are_segment_breaks_equal(seg_A->x0, seg_B->x0))
		return false;
	if (!are_segment_breaks_equal(seg_A->x1, seg_B->x1))
		return false;

	/* Sampled curve, so we must compare the set of samples of each segment. */
	if (seg_A->Type == 0) {
		if (seg_A->nGridPoints != seg_B->nGridPoints)
			return false;
		for (i = 0; i < seg_A->nGridPoints; i++) {
			if (fabs(seg_A->SampledPoints[i] - seg_B->SampledPoints[i]) > PRECISION)
				return false;
		}
		/* The samples are all the same, so the segments are equal. */
		return true;
	}

	/* Parametric curve. Determine the number of params that we should
	 * compare. */
	for (i = 0; i < ARRAY_LENGTH(types); i++) {
		if (types[i] == abs(seg_A->Type)) {
			n_params = types_n_params[i];
			break;
		}
	}

	/* The curve is unknown to us, so it's reasonable to assume the segments
	 * are not equal. */
	if (n_params < 0)
		return false;

	/* Compare the parameters from the segments. */
	for (i = 0; i < (uint32_t)n_params; i++) {
		if (fabs(seg_A->Params[i] - seg_B->Params[i]) > PRECISION)
			return false;
	}

	/* The parameters are all the same, so the segments are equal. */
	return true;
}

static bool
are_curves_equal(cmsToneCurve *curve_A, cmsToneCurve *curve_B)
{
	unsigned int i;
	const cmsCurveSegment *seg_A, *seg_B;

	/* Curves point to the same address, so they are the same. */
	if (curve_A == curve_B)
		return true;

	for (i = 0; ; i++) {
		seg_A = cmsGetToneCurveSegment(i, curve_A);
		seg_B = cmsGetToneCurveSegment(i, curve_B);

		/* The number of segments in A and B are different, so the
		 * curves are not equal. */
		if ((seg_A == NULL) != (seg_B == NULL))
			return false;

		/* No more segments to compare. */
		if (!seg_A && !seg_B)
			break;

		if (!are_segments_equal(seg_A, seg_B))
			return false;
	}

	/* We exhausted the pair of segments from the curves and they all do
	 * match, so the curves are equal. */
	return true;
}

void
curve_set_print(cmsStage *stage, struct weston_log_scope *scope)
{
	const _cmsStageToneCurvesData *data;
	uint32_t already_printed = 0;
	unsigned int i, j;

	assert(cmsStageType(stage) == cmsSigCurveSetElemType);
	data = cmsStageData(stage);

	if (data->nCurves == 0) {
		weston_log_scope_printf(scope, "%*sNo curves in the set\n", 6, "");
		return;
	}

	/* We can have multiple curves that are the same. So we print their
	 * indices and print the curve content only once. */
	for (i = 0; i < data->nCurves; i++) {
		if (((already_printed >> i) & 1) == 1)
			continue;

		weston_log_scope_printf(scope, "%*sCurve(s) %u", 6, "", i);
		already_printed |= (1 << i);

		for (j = i + 1; j < data->nCurves; j++) {
			/* We are only printing indices of the curves that are
			 * the same as curve i. */
			if (!are_curves_equal(data->TheCurves[i], data->TheCurves[j]))
				continue;

			weston_log_scope_printf(scope, ", %u", j);
			already_printed |= (1 << j);
		}

		weston_log_scope_printf(scope, "\n");
		curve_print(data->TheCurves[i], scope);
	}
}
