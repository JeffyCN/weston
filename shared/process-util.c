/*
 * Copyright 2022 Collabora, Ltd.
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-util.h>

#include "helpers.h"
#include "os-compatibility.h"
#include "process-util.h"
#include "string-helpers.h"

extern char **environ; /* defined by libc */

void
fdstr_update_str1(struct fdstr *s)
{
	snprintf(s->str1, sizeof(s->str1), "%d", s->fds[1]);
}

void
fdstr_set_fd1(struct fdstr *s, int fd)
{
	s->fds[0] = -1;
	s->fds[1] = fd;
	fdstr_update_str1(s);
}

bool
fdstr_clear_cloexec_fd1(struct fdstr *s)
{
	return os_fd_clear_cloexec(s->fds[1]) >= 0;
}

void
fdstr_close_all(struct fdstr *s)
{
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(s->fds); i++) {
		close(s->fds[i]);
		s->fds[i] = -1;
	}
}

void
custom_env_init_from_environ(struct custom_env *env)
{
	char **it;
	char **ep;

	wl_array_init(&env->p);
	env->finalized = false;

	for (it = environ; *it; it++) {
		ep = wl_array_add(&env->p, sizeof *ep);
		assert(ep);
		*ep = strdup(*it);
		assert(*ep);
	}
}

void
custom_env_fini(struct custom_env *env)
{
	char **ep;

	wl_array_for_each(ep, &env->p)
		free(*ep);

	wl_array_release(&env->p);
}

static char **
custom_env_find_element(struct custom_env *env, const char *name)
{
	char **ep;
	size_t name_len = strlen(name);

	wl_array_for_each(ep, &env->p) {
		char *entry = *ep;

		if (strncmp(entry, name, name_len) == 0 &&
		    entry[name_len] == '=') {
			return ep;
		}
	}

	return NULL;
}

void
custom_env_set(struct custom_env *env, const char *name, const char *value)
{
	char **ep;

	assert(strchr(name, '=') == NULL);
	assert(!env->finalized);

	ep = custom_env_find_element(env, name);
	if (ep)
		free(*ep);
	else
		ep = wl_array_add(&env->p, sizeof *ep);
	assert(ep);

	str_printf(ep, "%s=%s", name, value);
	assert(*ep);
}

char *const *
custom_env_get_envp(struct custom_env *env)
{
	char **ep;

	assert(!env->finalized);

	/* add terminating NULL */
	ep = wl_array_add(&env->p, sizeof *ep);
	assert(ep);
	*ep = NULL;

	env->finalized = true;

	return env->p.data;
}
