/*
 * Copyright © 2012 Intel Corporation
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
#ifndef _WESTON_VERTEX_CLIPPING_H
#define _WESTON_VERTEX_CLIPPING_H

#include <stdbool.h>
#include <pixman.h>

struct clip_vertex {
	float x, y;
};

struct polygon8 {
	struct clip_vertex pos[8];
	int n;
};

struct gl_quad {
	struct polygon8 vertices;
	struct { float x1, y1, x2, y2; } bbox; /* Valid if !axis_aligned. */
	bool axis_aligned;
};

struct clip_context {
	struct clip_vertex prev;

	struct {
		float x1, y1;
		float x2, y2;
	} clip;

	struct clip_vertex *vertices;
};

float
float_difference(float a, float b);

int
clip_simple(struct clip_context *ctx,
	    struct polygon8 *surf,
	    struct clip_vertex *restrict vertices);

int
clip_transformed(struct clip_context *ctx,
		 const struct polygon8 *surf,
		 struct clip_vertex *restrict vertices);

/*
 * Compute the boundary vertices of the intersection of an arbitrary
 * quadrilateral 'quad' and the axis-aligned rectangle 'surf_rect'. The vertices
 * are written to 'vertices', and the return value is the number of vertices.
 * Vertices are produced in clockwise winding order. Guarantees to produce
 * either zero vertices, or 3-8 vertices with non-zero polygon area.
 */
int
clip_quad(struct gl_quad *quad,
	  pixman_box32_t *surf_rect,
	  struct clip_vertex *vertices);

#endif
