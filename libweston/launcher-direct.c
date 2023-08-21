/*
 * Copyright © 2012 Benjamin Franzke
 * Copyright © 2013 Intel Corporation
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

#include <libweston/libweston.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <linux/major.h>

#include "launcher-impl.h"

#define DRM_MAJOR 226

/* major()/minor() */
#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#include <sys/sysmacros.h>
#endif

#ifdef BUILD_DRM_COMPOSITOR

#include <xf86drm.h>

static inline int
is_drm_master(int drm_fd)
{
	drm_magic_t magic;

	return drmGetMagic(drm_fd, &magic) == 0 &&
		drmAuthMagic(drm_fd, magic) == 0;
}

#else

static inline int
drmDropMaster(int drm_fd)
{
	return 0;
}

static inline int
drmSetMaster(int drm_fd)
{
	return 0;
}

static inline int
is_drm_master(int drm_fd)
{
	return 0;
}

#endif

struct launcher_direct {
	struct weston_launcher base;
	struct weston_compositor *compositor;
	int drm_fd;
};

static int
launcher_direct_open(struct weston_launcher *launcher_base, const char *path, int flags)
{
	struct launcher_direct *launcher = wl_container_of(launcher_base, launcher, base);
	struct stat s;
	int fd;

	fd = open(path, flags | O_CLOEXEC);
	if (fd == -1) {
		weston_log("couldn't open: %s! error=%s\n", path, strerror(errno));
		return -1;
	}

	if (geteuid() != 0) {
		weston_log("WARNING! Succeeded opening %s as non-root user."
			   " This implies your device can be spied on.\n",
			   path);
	}

	if (fstat(fd, &s) == -1) {
		weston_log("couldn't fstat: %s! error=%s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	if (major(s.st_rdev) == DRM_MAJOR) {
		launcher->drm_fd = fd;
		if (!is_drm_master(fd)) {
			weston_log("drm fd not master\n");
			close(fd);
			return -1;
		}
	}

	return fd;
}

static void
launcher_direct_close(struct weston_launcher *launcher_base, int fd)
{
	close(fd);
}

static int
launcher_direct_activate_vt(struct weston_launcher *launcher_base, int vt)
{
	return 0;
}

static int
launcher_direct_connect(struct weston_launcher **out, struct weston_compositor *compositor,
			const char *seat_id, bool sync_drm)
{
	struct launcher_direct *launcher;

	launcher = zalloc(sizeof(*launcher));
	if (launcher == NULL) {
		weston_log("failed to alloc for launcher\n");
		return -ENOMEM;
	}

	launcher->base.iface = &launcher_direct_iface;
	launcher->compositor = compositor;

	* (struct launcher_direct **) out = launcher;
	return 0;
}

static void
launcher_direct_destroy(struct weston_launcher *launcher_base)
{
	struct launcher_direct *launcher = wl_container_of(launcher_base, launcher, base);
	free(launcher);
}

static int
launcher_direct_get_vt(struct weston_launcher *base)
{
	return -ENOSYS;
}

const struct launcher_interface launcher_direct_iface = {
	.name = "direct",
	.connect = launcher_direct_connect,
	.destroy = launcher_direct_destroy,
	.open = launcher_direct_open,
	.close = launcher_direct_close,
	.activate_vt = launcher_direct_activate_vt,
	.get_vt = launcher_direct_get_vt,
};
