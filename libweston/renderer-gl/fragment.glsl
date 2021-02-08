/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019,2021 Collabora, Ltd.
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

/* GLSL version 1.00 ES, defined in gl-shaders.c */

/*
 * Enumeration of shader variants, must match enum gl_shader_texture_variant.
 */
#define SHADER_VARIANT_RGBX     1
#define SHADER_VARIANT_RGBA     2
#define SHADER_VARIANT_Y_U_V    3
#define SHADER_VARIANT_Y_UV     4
#define SHADER_VARIANT_Y_XUXV   5
#define SHADER_VARIANT_XYUV     6
#define SHADER_VARIANT_SOLID    7
#define SHADER_VARIANT_EXTERNAL 8

#if DEF_VARIANT == SHADER_VARIANT_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

precision mediump float;

/*
 * These undeclared identifiers will be #defined by a runtime generated code
 * snippet.
 */
const int c_variant = DEF_VARIANT;
const bool c_green_tint = DEF_GREEN_TINT;

vec4
yuva2rgba(vec4 yuva)
{
	vec4 color_out;
	float Y, su, sv;

	Y = 1.16438356 * (yuva.x - 0.0625);
	su = yuva.y - 0.5;
	sv = yuva.z - 0.5;

	color_out.r = Y                   + 1.59602678 * sv;
	color_out.g = Y - 0.39176229 * su - 0.81296764 * sv;
	color_out.b = Y + 2.01723214 * su;

	color_out.rgb *= yuva.w;
	color_out.a = yuva.w;

	return color_out;
}

#if DEF_VARIANT == SHADER_VARIANT_EXTERNAL
uniform samplerExternalOES tex;
#else
uniform sampler2D tex;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex1;
uniform sampler2D tex2;
uniform float alpha;
uniform vec4 unicolor;

void
main()
{
	vec4 yuva;

	if (c_variant == SHADER_VARIANT_RGBA ||
	    c_variant == SHADER_VARIANT_EXTERNAL) {
		gl_FragColor = alpha * texture2D(tex, v_texcoord);

	} else if (c_variant == SHADER_VARIANT_RGBX) {
		gl_FragColor.rgb = alpha * texture2D(tex, v_texcoord).rgb;
		gl_FragColor.a = alpha;

	} else if (c_variant == SHADER_VARIANT_Y_U_V) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.y = texture2D(tex1, v_texcoord).x;
		yuva.z = texture2D(tex2, v_texcoord).x;
		yuva.w = alpha;
		gl_FragColor = yuva2rgba(yuva);

	} else if (c_variant == SHADER_VARIANT_Y_UV) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.y = texture2D(tex1, v_texcoord).r;
		yuva.z = texture2D(tex1, v_texcoord).g;
		yuva.w = alpha;
		gl_FragColor = yuva2rgba(yuva);

	} else if (c_variant == SHADER_VARIANT_Y_XUXV) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.y = texture2D(tex1, v_texcoord).g;
		yuva.z = texture2D(tex1, v_texcoord).a;
		yuva.w = alpha;
		gl_FragColor = yuva2rgba(yuva);

	} else if (c_variant == SHADER_VARIANT_XYUV) {
		yuva.x = texture2D(tex, v_texcoord).b;
		yuva.y = texture2D(tex, v_texcoord).g;
		yuva.z = texture2D(tex, v_texcoord).r;
		yuva.w = alpha;
		gl_FragColor = yuva2rgba(yuva);

	} else if (c_variant == SHADER_VARIANT_SOLID) {
		gl_FragColor = alpha * unicolor;

	} else {
		/* Never reached, bad variant value. */
		gl_FragColor = vec4(1.0, 0.3, 1.0, 1.0);
	}

	if (c_green_tint)
		gl_FragColor = vec4(0.0, 0.3, 0.0, 0.2) + gl_FragColor * 0.8;
}
