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

#include "weston-test-client-helper.h"
#include "libweston/color-lcms/color-lcms.h"
#include "libweston/color-lcms/color-curve-segments.h"

#define N_CHANNELS 3
#define PRECISION 1e-4

struct pipeline_context {
        cmsContext context_id;
        cmsPipeline *pipeline;
};

struct curve {
	cmsInt32Number type;
	cmsFloat64Number params[10];
};

/* Parametric power-law curve with nothing special. */
const struct curve power_law_curve_A = {
	.type = 1,
	.params = {2.0},
};

/* Inverse power-law curve with another gamma value. */
const struct curve power_law_curve_B = {
	.type = -1,
	.params = {8.0},
};

/* Parametric type 4 comes from IEC 61966-2.1 (sRGB). */
const struct curve srgb_curve = {
	.type = 4,
	.params = {2.4, 1.0/1.055, 0.055/1.055, 1.0/12.92, 0.04045},
};

/* Identity matrix. */
const cmsFloat64Number identity_matrix[N_CHANNELS * N_CHANNELS] = {
	1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
};

/* Matrix with no special characteristics. */
const cmsFloat64Number regular_matrix[N_CHANNELS * N_CHANNELS] = {
	1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
};

/* Inverse matrices (matrix_A * matrix_A_inverse == identity). */
const cmsFloat64Number matrix_A[N_CHANNELS * N_CHANNELS] = {
	0.218024, 0.192559, 0.071524,
	0.111244, 0.358458, 0.030306,
	0.006960, 0.048534, 0.356962,
};
const cmsFloat64Number matrix_A_inverse[N_CHANNELS * N_CHANNELS] = {
	 6.268277, -3.234369, -0.981373,
	-1.957467,  3.832202,  0.066866,
	 0.143926, -0.457981,  2.811465,
};

static struct pipeline_context
pipeline_context_new(void)
{
	struct pipeline_context ret;

	ret.context_id = cmsCreateContext(NULL, NULL);
	assert(ret.context_id);

	ret.pipeline = cmsPipelineAlloc(ret.context_id, N_CHANNELS, N_CHANNELS);
	assert(ret.pipeline);

	return ret;
}

static void
pipeline_context_release(struct pipeline_context *pc)
{
	cmsPipelineFree(pc->pipeline);
	cmsDeleteContext(pc->context_id);
}

static void
add_curve(struct pipeline_context *pc, cmsInt32Number type,
	  const cmsFloat64Number params[])
{
	cmsToneCurve *curveset[N_CHANNELS];
	cmsToneCurve *curve;
	cmsStage *stage;
	unsigned int i;

	curve = cmsBuildParametricToneCurve(pc->context_id, type, params);
	assert(curve);

	for (i = 0; i < N_CHANNELS; i++)
		curveset[i] = curve;

	stage = cmsStageAllocToneCurves(pc->context_id, ARRAY_LENGTH(curveset), curveset);
	assert(stage);

	assert(cmsPipelineInsertStage(pc->pipeline, cmsAT_END, stage));

	cmsFreeToneCurve(curve);
}

static void
add_identity_curve(struct pipeline_context *pc)
{
	cmsStage *stage;

	stage = cmsStageAllocToneCurves(pc->context_id, N_CHANNELS, NULL);
	assert(stage);

	assert(cmsPipelineInsertStage(pc->pipeline, cmsAT_END, stage));
}

static void
add_matrix(struct pipeline_context *pc,
	   const cmsFloat64Number matrix[] /* row-major matrix */)
{
	cmsStage *stage;

	stage = cmsStageAllocMatrix(pc->context_id, N_CHANNELS, N_CHANNELS, matrix, NULL);
	assert(stage);

	assert(cmsPipelineInsertStage(pc->pipeline, cmsAT_END, stage));
}

static bool
are_matrices_equal(const cmsFloat64Number matrix_A[N_CHANNELS * N_CHANNELS],
		   const cmsFloat64Number matrix_B[N_CHANNELS * N_CHANNELS])
{
	unsigned int i;

	for (i = 0; i < N_CHANNELS * N_CHANNELS; i++)
		if (fabs(matrix_A[i] - matrix_B[i]) > PRECISION)
			return false;

	return true;
}

/* Pipeline with a regular matrix. Nothing should change. */
TEST(keep_regular_matrix)
{
	struct pipeline_context pc = pipeline_context_new();
	const _cmsStageMatrixData *data;
	cmsStage *elem;

	add_matrix(&pc, regular_matrix);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	elem = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(elem);
	data = cmsStageData(elem);
	assert(are_matrices_equal(regular_matrix, data->Double));

	assert(!cmsStageNext(elem));

	pipeline_context_release(&pc);
}

/* Pipeline with a identity matrix, which should be removed. */
TEST(drop_identity_matrix)
{
	struct pipeline_context pc = pipeline_context_new();

	add_matrix(&pc, identity_matrix);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

/* Pipeline with two inverse matrices. When merged they become identity, which
 * should be removed. */
TEST(drop_inverse_matrices)
{
	struct pipeline_context pc = pipeline_context_new();

	add_matrix(&pc, matrix_A);
	add_matrix(&pc, matrix_A_inverse);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

/* Pipeline with an identity and two inverse matrices. Pipeline must be empty
 * afterwards. */
TEST(drop_identity_and_inverse_matrices)
{
	struct pipeline_context pc = pipeline_context_new();

	add_matrix(&pc, identity_matrix);
	add_matrix(&pc, matrix_A);
	add_matrix(&pc, matrix_A_inverse);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

/* Pipeline has a regular matrix followed by two inverses, which should be
 * removed. Pipeline must contain only the regular matrix afterwards. */
TEST(only_drop_inverse_matrices)
{
	struct pipeline_context pc = pipeline_context_new();
	const _cmsStageMatrixData *data;
	cmsStage *elem;

	add_matrix(&pc, regular_matrix);
	add_matrix(&pc, matrix_A);
	add_matrix(&pc, matrix_A_inverse);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	elem = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(elem);
	data = cmsStageData(elem);
	assert(are_matrices_equal(regular_matrix, data->Double));

	assert(!cmsStageNext(elem));

	pipeline_context_release(&pc);
}

/* Same as above, but the regular matrix is the last element. */
TEST(only_drop_inverse_matrices_another_order)
{
	struct pipeline_context pc = pipeline_context_new();
	const _cmsStageMatrixData *data;
	cmsStage *elem;

	add_matrix(&pc, matrix_A);
	add_matrix(&pc, matrix_A_inverse);
	add_matrix(&pc, regular_matrix);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	elem = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(elem);
	data = cmsStageData(elem);
	assert(are_matrices_equal(regular_matrix, data->Double));

	assert(!cmsStageNext(elem));

	pipeline_context_release(&pc);
}

/* Pipeline has an identity curve, which should be removed. */
TEST(drop_identity_curve)
{
	struct pipeline_context pc = pipeline_context_new();

	add_identity_curve(&pc);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

/* Pipeline has two parametric curves that are inverse. So they should be
 * removed from the pipeline. */
TEST(drop_inverse_curves)
{
	struct pipeline_context pc = pipeline_context_new();

	add_curve(&pc, srgb_curve.type, srgb_curve.params);
	add_curve(&pc, -srgb_curve.type, srgb_curve.params);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

/* Pipeline has two parametric inverse curves followed by an identity. Pipeline
 * should be empty afterwards. */
TEST(drop_identity_and_inverse_curves)
{
	struct pipeline_context pc = pipeline_context_new();

	add_identity_curve(&pc);
	add_curve(&pc, srgb_curve.type, srgb_curve.params);
	add_curve(&pc, -srgb_curve.type, srgb_curve.params);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

/* Same as above, but the identity is the last element. */
TEST(drop_identity_and_inverse_curves_another_order)
{
	struct pipeline_context pc = pipeline_context_new();

	add_curve(&pc, srgb_curve.type, srgb_curve.params);
	add_curve(&pc, -srgb_curve.type, srgb_curve.params);
	add_identity_curve(&pc);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	assert(cmsPipelineStageCount(pc.pipeline) == 0);

	pipeline_context_release(&pc);
}

#if HAVE_CMS_GET_TONE_CURVE_SEGMENT

static bool
are_curveset_curves_equal_to_curve(struct pipeline_context *pc,
				   const _cmsStageToneCurvesData *curveset_data,
				   const struct curve *curve)
{
	unsigned int i;
	bool ret = true;
	cmsToneCurve *cms_curve =
		cmsBuildParametricToneCurve(pc->context_id, curve->type,
					    curve->params);

	assert(curveset_data->nCurves == N_CHANNELS);

	for (i = 0; i < N_CHANNELS; i++) {
		if (!are_curves_equal(curveset_data->TheCurves[i], cms_curve)) {
			ret = false;
			break;
		}
	}

	cmsFreeToneCurve(cms_curve);

	return ret;
}

/* Pipeline with a regular curve. Nothing should change. */
TEST(keep_regular_curve)
{
	struct pipeline_context pc = pipeline_context_new();
	cmsStage *stage;

	add_curve(&pc, power_law_curve_A.type, power_law_curve_A.params);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	stage = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(stage);
	assert(are_curveset_curves_equal_to_curve(&pc, cmsStageData(stage),
						  &power_law_curve_A));

	assert(!cmsStageNext(stage));

	pipeline_context_release(&pc);
}

/* Inverse curves followed by a parametric curve. Inverse curves are dropped (as
 * they would become identity if merged), and parametric curve should not be
 * changed. */
TEST(do_not_merge_identity_with_parametric)
{
	struct pipeline_context pc = pipeline_context_new();
	cmsStage *stage;

	add_curve(&pc, srgb_curve.type, srgb_curve.params);
	add_curve(&pc, -srgb_curve.type, srgb_curve.params);
	add_curve(&pc, srgb_curve.type, srgb_curve.params);

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	stage = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(stage);
	assert(are_curveset_curves_equal_to_curve(&pc, cmsStageData(stage),
						  &srgb_curve));

	assert(!cmsStageNext(stage));

	pipeline_context_release(&pc);
}

/* Merge power-law function with itself. */
TEST(merge_power_law_curves_with_itself)
{
	struct pipeline_context pc = pipeline_context_new();
	cmsStage *stage;
	struct curve result_curve;

	add_curve(&pc, power_law_curve_A.type, power_law_curve_A.params);
	add_curve(&pc, power_law_curve_A.type, power_law_curve_A.params);

	memset(&result_curve, 0, sizeof(result_curve));
	result_curve.type = power_law_curve_A.type;
	result_curve.params[0] = power_law_curve_A.params[0] * power_law_curve_A.params[0];

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	stage = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(stage);
	assert(are_curveset_curves_equal_to_curve(&pc, cmsStageData(stage),
						  &result_curve));

	assert(!cmsStageNext(stage));

	pipeline_context_release(&pc);
}

/* Merge power-law functions into a single parametric one of the same type. */
TEST(merge_power_law_curves_with_another)
{
	struct pipeline_context pc = pipeline_context_new();
	cmsStage *stage;
	struct curve result_curve;

	add_curve(&pc, power_law_curve_A.type, power_law_curve_A.params);
	add_curve(&pc, power_law_curve_B.type, power_law_curve_B.params);

	memset(&result_curve, 0, sizeof(result_curve));
	result_curve.type = power_law_curve_A.type;
	result_curve.params[0] = power_law_curve_A.params[0] / power_law_curve_B.params[0];

	lcms_optimize_pipeline(&pc.pipeline, pc.context_id);

	stage = cmsPipelineGetPtrToFirstStage(pc.pipeline);
	assert(stage);
	assert(are_curveset_curves_equal_to_curve(&pc, cmsStageData(stage),
						  &result_curve));

	assert(!cmsStageNext(stage));

	pipeline_context_release(&pc);
}

#endif /* HAVE_CMS_GET_TONE_CURVE_SEGMENT */
