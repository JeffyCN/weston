/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019 Collabora, Ltd.
 * Copyright 2016 NVIDIA Corporation
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

/* enum gl_shader_texcoord_input */
#define SHADER_TEXCOORD_INPUT_ATTRIB  0
#define SHADER_TEXCOORD_INPUT_SURFACE 1

/* Always use high-precision for vertex calculations */
precision highp float;

#ifdef GL_FRAGMENT_PRECISION_HIGH
#define FRAG_PRECISION highp
#else
#define FRAG_PRECISION mediump
#endif

uniform mat4 proj;
uniform mat4 surface_to_buffer;

attribute vec2 position;
attribute vec2 texcoord;

/* Match the varying precision to the fragment shader */
varying FRAG_PRECISION vec2 v_texcoord;

void main()
{
	gl_Position = proj * vec4(position, 0.0, 1.0);

#if DEF_TEXCOORD_INPUT == SHADER_TEXCOORD_INPUT_ATTRIB
	v_texcoord = texcoord;
#elif DEF_TEXCOORD_INPUT == SHADER_TEXCOORD_INPUT_SURFACE
	v_texcoord = vec2(surface_to_buffer * vec4(position, 0.0, 1.0));
#endif
}
