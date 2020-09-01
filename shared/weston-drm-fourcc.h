/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2015,2019 Collabora, Ltd.
 * Copyright © 2016 NVIDIA Corporation
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

#ifndef WESTON_DRM_FOURCC_H
#define WESTON_DRM_FOURCC_H

#include <drm_fourcc.h>

/* Check if modifier is neither LINEAR nor INVALID (i.e., a "real" modifier) */
#define DRM_MOD_VALID(mod) \
	((mod) != DRM_FORMAT_MOD_LINEAR && (mod) != DRM_FORMAT_MOD_INVALID)

/* The kernel header drm_fourcc.h defines the DRM formats below.  We duplicate
 * some of the definitions here so that building Weston won't require
 * bleeding-edge kernel headers.
 */
#ifndef DRM_FORMAT_XYUV8888
#define DRM_FORMAT_XYUV8888      fourcc_code('X', 'Y', 'U', 'V') /* [31:0] X:Y:Cb:Cr 8:8:8:8 little endian */
#endif

#ifndef DRM_FORMAT_XBGR16161616
#define DRM_FORMAT_XBGR16161616  fourcc_code('X', 'B', '4', '8') /* [63:0] x:B:G:R 16:16:16:16 little endian */
#endif

#ifndef DRM_FORMAT_ABGR16161616
#define DRM_FORMAT_ABGR16161616  fourcc_code('A', 'B', '4', '8') /* [63:0] A:B:G:R 16:16:16:16 little endian */
#endif

/*
 * 2 plane YCbCr
 * index 0 = Y plane, [39:0] Y3:Y2:Y1:Y0 little endian
 * index 1 = Cr:Cb plane, [39:0] Cr1:Cb1:Cr0:Cb0 little endian
 */
#ifndef DRM_FORMAT_NV15
#define DRM_FORMAT_NV15		fourcc_code('N', 'V', '1', '5') /* 2x2 subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_NV20
#define DRM_FORMAT_NV20		fourcc_code('N', 'V', '2', '0') /* 2x1 subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_NV30
#define DRM_FORMAT_NV30		fourcc_code('N', 'V', '3', '0') /* non-subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_P030
#define DRM_FORMAT_P030		fourcc_code('P', '0', '3', '0') /* 2x2 subsampled Cr:Cb plane 10 bits per channel packed */
#endif

/*
 * 3 plane YCbCr LSB aligned
 * In order to use these formats in a similar fashion to MSB aligned ones
 * implementation can multiply the values by 2^6=64. For that reason the padding
 * must only contain zeros.
 * index 0 = Y plane, [15:0] z:Y [6:10] little endian
 * index 1 = Cr plane, [15:0] z:Cr [6:10] little endian
 * index 2 = Cb plane, [15:0] z:Cb [6:10] little endian
 */
#ifndef DRM_FORMAT_S010
#define DRM_FORMAT_S010	fourcc_code('S', '0', '1', '0') /* 2x2 subsampled Cb (1) and Cr (2) planes 10 bits per channel */
#endif
#ifndef DRM_FORMAT_S210
#define DRM_FORMAT_S210	fourcc_code('S', '2', '1', '0') /* 2x1 subsampled Cb (1) and Cr (2) planes 10 bits per channel */
#endif
#ifndef DRM_FORMAT_S410
#define DRM_FORMAT_S410	fourcc_code('S', '4', '1', '0') /* non-subsampled Cb (1) and Cr (2) planes 10 bits per channel */
#endif

/*
 * 3 plane YCbCr LSB aligned
 * In order to use these formats in a similar fashion to MSB aligned ones
 * implementation can multiply the values by 2^4=16. For that reason the padding
 * must only contain zeros.
 * index 0 = Y plane, [15:0] z:Y [4:12] little endian
 * index 1 = Cr plane, [15:0] z:Cr [4:12] little endian
 * index 2 = Cb plane, [15:0] z:Cb [4:12] little endian
 */
#ifndef DRM_FORMAT_S012
#define DRM_FORMAT_S012	fourcc_code('S', '0', '1', '2') /* 2x2 subsampled Cb (1) and Cr (2) planes 12 bits per channel */
#endif
#ifndef DRM_FORMAT_S212
#define DRM_FORMAT_S212	fourcc_code('S', '2', '1', '2') /* 2x1 subsampled Cb (1) and Cr (2) planes 12 bits per channel */
#endif
#ifndef DRM_FORMAT_S412
#define DRM_FORMAT_S412	fourcc_code('S', '4', '1', '2') /* non-subsampled Cb (1) and Cr (2) planes 12 bits per channel */
#endif

/*
 * 3 plane YCbCr
 * index 0 = Y plane, [15:0] Y little endian
 * index 1 = Cr plane, [15:0] Cr little endian
 * index 2 = Cb plane, [15:0] Cb little endian
 */
#ifndef DRM_FORMAT_S016
#define DRM_FORMAT_S016	fourcc_code('S', '0', '1', '6') /* 2x2 subsampled Cb (1) and Cr (2) planes 16 bits per channel */
#endif
#ifndef DRM_FORMAT_S216
#define DRM_FORMAT_S216	fourcc_code('S', '2', '1', '6') /* 2x1 subsampled Cb (1) and Cr (2) planes 16 bits per channel */
#endif
#ifndef DRM_FORMAT_S416
#define DRM_FORMAT_S416	fourcc_code('S', '4', '1', '6') /* non-subsampled Cb (1) and Cr (2) planes 16 bits per channel */
#endif

#endif
