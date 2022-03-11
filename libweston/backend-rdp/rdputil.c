/*
 * Copyright © 2020 Microsoft
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "rdp.h"

static int cached_tm_mday = -1;

void rdp_debug_print(struct weston_log_scope *log_scope, bool cont, char *fmt, ...)
{
	char timestr[128];
	int len_va;
	char *str;

	if (!log_scope || !weston_log_scope_is_enabled(log_scope))
		return;

	va_list ap;
	va_start(ap, fmt);

	if (cont) {
		weston_log_scope_vprintf(log_scope, fmt, ap);
		goto end;
	}

	weston_log_timestamp(timestr, sizeof(timestr), &cached_tm_mday);
	len_va = vasprintf(&str, fmt, ap);
	if (len_va >= 0) {
		weston_log_scope_printf(log_scope, "%s %s",
					timestr, str);
		free(str);
	} else {
		const char *oom = "Out of memory";

		weston_log_scope_printf(log_scope, "%s %s",
					timestr, oom);
	}
end:
	va_end(ap);
}
