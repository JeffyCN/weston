/*
 * Copyright © 2014 Pekka Paalanen <pq@iki.fi>
 * Copyright © 2014, 2019 Collabora, Ltd.
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

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "timeline.h"
#include <libweston/libweston.h>

WL_EXPORT int weston_timeline_enabled_;

struct timeline_emit_context {
	FILE *cur;
	FILE *out;
	unsigned series;
};

static unsigned
timeline_new_id(void)
{
	static unsigned idc;

	if (++idc == 0)
		++idc;

	return idc;
}

static int
check_series(struct timeline_emit_context *ctx,
	     struct weston_timeline_object *to)
{
	if (to->series == 0 || to->series != ctx->series) {
		to->series = ctx->series;
		to->id = timeline_new_id();
		return 1;
	}

	if (to->force_refresh) {
		to->force_refresh = 0;
		return 1;
	}

	return 0;
}

static void
fprint_quoted_string(FILE *fp, const char *str)
{
	if (!str) {
		fprintf(fp, "null");
		return;
	}

	fprintf(fp, "\"%s\"", str);
}

static int
emit_weston_output(struct timeline_emit_context *ctx, void *obj)
{
	struct weston_output *o = obj;

	if (check_series(ctx, &o->timeline)) {
		fprintf(ctx->out, "{ \"id\":%u, "
			"\"type\":\"weston_output\", \"name\":",
			o->timeline.id);
		fprint_quoted_string(ctx->out, o->name);
		fprintf(ctx->out, " }\n");
	}

	fprintf(ctx->cur, "\"wo\":%u", o->timeline.id);

	return 1;
}

static void
check_weston_surface_description(struct timeline_emit_context *ctx,
				 struct weston_surface *s)
{
	struct weston_surface *mains;
	char d[512];
	char mainstr[32];

	if (!check_series(ctx, &s->timeline))
		return;

	mains = weston_surface_get_main_surface(s);
	if (mains != s) {
		check_weston_surface_description(ctx, mains);
		if (snprintf(mainstr, sizeof(mainstr),
			     ", \"main_surface\":%u", mains->timeline.id) < 0)
			mainstr[0] = '\0';
	} else {
		mainstr[0] = '\0';
	}

	if (!s->get_label || s->get_label(s, d, sizeof(d)) < 0)
		d[0] = '\0';

	fprintf(ctx->out, "{ \"id\":%u, "
		"\"type\":\"weston_surface\", \"desc\":", s->timeline.id);
	fprint_quoted_string(ctx->out, d[0] ? d : NULL);
	fprintf(ctx->out, "%s }\n", mainstr);
}

static int
emit_weston_surface(struct timeline_emit_context *ctx, void *obj)
{
	struct weston_surface *s = obj;

	check_weston_surface_description(ctx, s);
	fprintf(ctx->cur, "\"ws\":%u", s->timeline.id);

	return 1;
}

static int
emit_vblank_timestamp(struct timeline_emit_context *ctx, void *obj)
{
	struct timespec *ts = obj;

	fprintf(ctx->cur, "\"vblank\":[%" PRId64 ", %ld]",
		(int64_t)ts->tv_sec, ts->tv_nsec);

	return 1;
}

static int
emit_gpu_timestamp(struct timeline_emit_context *ctx, void *obj)
{
	struct timespec *ts = obj;

	fprintf(ctx->cur, "\"gpu\":[%" PRId64 ", %ld]",
		(int64_t)ts->tv_sec, ts->tv_nsec);

	return 1;
}

typedef int (*type_func)(struct timeline_emit_context *ctx, void *obj);

static const type_func type_dispatch[] = {
	[TLT_OUTPUT] = emit_weston_output,
	[TLT_SURFACE] = emit_weston_surface,
	[TLT_VBLANK] = emit_vblank_timestamp,
	[TLT_GPU] = emit_gpu_timestamp,
};

WL_EXPORT void
weston_timeline_point(const char *name, ...)
{
	va_list argp;
	struct timespec ts;
	enum timeline_type otype;
	void *obj;
	char buf[512];
	struct timeline_emit_context ctx;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	ctx.cur = fmemopen(buf, sizeof(buf), "w");

	if (!ctx.cur) {
		weston_log("Timeline error in fmemopen, closing.\n");
		return;
	}

	fprintf(ctx.cur, "{ \"T\":[%" PRId64 ", %ld], \"N\":\"%s\"",
		(int64_t)ts.tv_sec, ts.tv_nsec, name);

	va_start(argp, name);
	while (1) {
		otype = va_arg(argp, enum timeline_type);
		if (otype == TLT_END)
			break;

		obj = va_arg(argp, void *);
		if (type_dispatch[otype]) {
			fprintf(ctx.cur, ", ");
			type_dispatch[otype](&ctx, obj);
		}
	}
	va_end(argp);

	fprintf(ctx.cur, " }\n");
	fflush(ctx.cur);
	if (ferror(ctx.cur)) {
		weston_log("Timeline error in constructing entry, closing.\n");
	} else {
		fprintf(ctx.out, "%s", buf);
	}

	fclose(ctx.cur);
}
