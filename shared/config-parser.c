/*
 * Copyright © 2011 Intel Corporation
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

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <wayland-util.h>
#include <libweston/zalloc.h>
#include <libweston/config-parser.h>
#include <libweston/libweston.h>
#include "helpers.h"
#include "string-helpers.h"

/**
 * \defgroup weston-config Weston configuration
 *
 * Helper functions to read out ini configuration file.
 */

struct weston_config_entry {
	char *key;
	char *value;
	struct wl_list link;
};

struct weston_config_section {
	char *name;
	struct wl_list entry_list;
	struct wl_list link;
};

struct weston_config {
	struct wl_list section_list;
	char path[PATH_MAX];
};

static int
open_config_file(struct weston_config *c, const char *name)
{
	const char *config_dir  = getenv("XDG_CONFIG_HOME");
	const char *home_dir	= getenv("HOME");
	const char *config_dirs = getenv("XDG_CONFIG_DIRS");
	const char *p, *next;
	int fd;

	if (!c) {
		if (name[0] != '/')
			return -1;

		return open(name, O_RDONLY | O_CLOEXEC);
	}

	if (name[0] == '/') {
		snprintf(c->path, sizeof c->path, "%s", name);
		return open(name, O_RDONLY | O_CLOEXEC);
	}

	/* Precedence is given to config files in the home directory,
	 * then to directories listed in XDG_CONFIG_DIRS. */

	/* $XDG_CONFIG_HOME */
	if (config_dir) {
		snprintf(c->path, sizeof c->path, "%s/%s", config_dir, name);
		fd = open(c->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}

	/* $HOME/.config */
	if (home_dir) {
		snprintf(c->path, sizeof c->path,
			 "%s/.config/%s", home_dir, name);
		fd = open(c->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}

	/* For each $XDG_CONFIG_DIRS: weston/<config_file> */
	if (!config_dirs)
		config_dirs = "/etc/xdg";  /* See XDG base dir spec. */

	for (p = config_dirs; *p != '\0'; p = next) {
		next = strchrnul(p, ':');
		snprintf(c->path, sizeof c->path,
			 "%.*s/weston/%s", (int)(next - p), p, name);
		fd = open(c->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			return fd;

		if (*next == ':')
			next++;
	}

	return -1;
}

static struct weston_config_entry *
config_section_get_entry(struct weston_config_section *section,
			 const char *key)
{
	struct weston_config_entry *e;

	if (section == NULL)
		return NULL;
	wl_list_for_each(e, &section->entry_list, link)
		if (strcmp(e->key, key) == 0)
			return e;

	return NULL;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT struct weston_config_section *
weston_config_get_section(struct weston_config *config, const char *section,
			  const char *key, const char *value)
{
	struct weston_config_section *s;
	struct weston_config_entry *e;

	if (config == NULL)
		return NULL;
	wl_list_for_each(s, &config->section_list, link) {
		if (strcmp(s->name, section) != 0)
			continue;
		if (key == NULL)
			return s;
		e = config_section_get_entry(s, key);
		if (e && strcmp(e->value, value) == 0)
			return s;
	}

	return NULL;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_section_get_int(struct weston_config_section *section,
			      const char *key,
			      int32_t *value, int32_t default_value)
{
	struct weston_config_entry *entry;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	if (!safe_strtoint(entry->value, value)) {
		*value = default_value;
		return -1;
	}

	return 0;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_section_get_uint(struct weston_config_section *section,
			       const char *key,
			       uint32_t *value, uint32_t default_value)
{
	long int ret;
	struct weston_config_entry *entry;
	char *end;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	errno = 0;
	ret = strtol(entry->value, &end, 0);
	if (errno != 0 || end == entry->value || *end != '\0') {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	/* check range */
	if (ret < 0 || ret > INT_MAX) {
		*value = default_value;
		errno = ERANGE;
		return -1;
	}

	*value = ret;

	return 0;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_section_get_color(struct weston_config_section *section,
				const char *key,
				uint32_t *color, uint32_t default_color)
{
	struct weston_config_entry *entry;
	int len;
	char *end;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*color = default_color;
		errno = ENOENT;
		return -1;
	}

	len = strlen(entry->value);
	if (len == 1 && entry->value[0] == '0') {
		*color = 0;
		return 0;
	} else if (len != 8 && len != 10) {
		*color = default_color;
		errno = EINVAL;
		return -1;
	}

	errno = 0;
	*color = strtoul(entry->value, &end, 16);
	if (errno != 0 || end == entry->value || *end != '\0') {
		*color = default_color;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_section_get_double(struct weston_config_section *section,
				 const char *key,
				 double *value, double default_value)
{
	struct weston_config_entry *entry;
	char *end;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	*value = strtod(entry->value, &end);
	if (*end != '\0') {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_section_get_string(struct weston_config_section *section,
				 const char *key,
				 char **value, const char *default_value)
{
	struct weston_config_entry *entry;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		if (default_value)
			*value = strdup(default_value);
		else
			*value = NULL;
		errno = ENOENT;
		return -1;
	}

	*value = strdup(entry->value);

	return 0;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_section_get_bool(struct weston_config_section *section,
			       const char *key,
			       bool *value, bool default_value)
{
	struct weston_config_entry *entry;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	if (strcmp(entry->value, "false") == 0)
		*value = false;
	else if (strcmp(entry->value, "true") == 0)
		*value = true;
	else {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT const char *
weston_config_get_name_from_env(void)
{
	const char *name;

	name = getenv(WESTON_CONFIG_FILE_ENV_VAR);
	if (name)
		return name;

	return "weston.ini";
}

static struct weston_config_section *
config_add_section(struct weston_config *config, const char *name)
{
	struct weston_config_section *section;

	/* squash single sessions */
	if (!strstr(name, "launcher") && !strstr(name, "output")) {
		section = weston_config_get_section(config, name, NULL, NULL);
		if (section)
			return section;
	}

	section = zalloc(sizeof *section);
	if (section == NULL)
		return NULL;

	section->name = strdup(name);
	if (section->name == NULL) {
		free(section);
		return NULL;
	}

	wl_list_init(&section->entry_list);
	wl_list_insert(config->section_list.prev, &section->link);

	return section;
}

static struct weston_config_entry *
section_add_entry(struct weston_config_section *section,
		  const char *key, const char *value, const char *file_name)
{
	struct weston_config_entry *entry;

	/* hack for removing entry */
	if (key[0] == '-') {
		key ++;
		value = NULL;
	}

	/* drop old entry */
	entry = config_section_get_entry(section, key);
	if (entry) {
		if (getenv("WESTON_MAIN_PARSE")) {
			printf("%s: \"%s/%s\" from \"%s\" to \"%s\"\n",
			       file_name ?: "unknown", section->name,
			       entry->key, entry->value ?: "", value ?: "");
		}
		wl_list_remove(&entry->link);
		free(entry->key);
		free(entry->value);
		free(entry);
	}

	if (!value || value[0] == '\0')
		return NULL;

	entry = zalloc(sizeof *entry);
	if (entry == NULL)
		return NULL;

	entry->key = strdup(key);
	if (entry->key == NULL) {
		free(entry);
		return NULL;
	}

	entry->value = strdup(value);
	if (entry->value == NULL) {
		free(entry->key);
		free(entry);
		return NULL;
	}

	wl_list_insert(section->entry_list.prev, &entry->link);

	return entry;
}

static bool
weston_config_parse_internal(struct weston_config *config, FILE *fp,
			     const char *file_name)
{
	struct weston_config_section *section = NULL;
	char line[512], *p;
	int i;

	while (fgets(line, sizeof line, fp)) {
		switch (line[0]) {
		case '#':
		case '\n':
			continue;
		case '[':
			p = strchr(&line[1], ']');
			if (!p || p[1] != '\n') {
				fprintf(stderr, "malformed "
					"section header: %s\n", line);
				return false;
			}
			p[0] = '\0';
			section = config_add_section(config, &line[1]);
			continue;
		default:
			p = strchr(line, '=');
			if (!p || p == line || !section) {
				fprintf(stderr, "malformed "
					"config line: %s\n", line);
				return false;
			}

			p[0] = '\0';
			p++;
			while (isspace(*p))
				p++;
			i = strlen(p);
			while (i > 0 && isspace(p[i - 1])) {
				p[i - 1] = '\0';
				i--;
			}
			section_add_entry(section, line, p, file_name);
			continue;
		}
	}

	return true;
}

WESTON_EXPORT_FOR_TESTS struct weston_config *
weston_config_parse_fp(FILE *file)
{
	struct weston_config *config = zalloc(sizeof(*config));

	if (config == NULL)
		return NULL;

	wl_list_init(&config->section_list);
	if (!weston_config_parse_internal(config, file, NULL)) {
		weston_config_destroy(config);
		return NULL;
	}

	return config;
}

static FILE *
weston_open_config_file(struct weston_config *config, const char *name)
{
	FILE *fp;
	struct stat filestat;
	int fd;

	fd = open_config_file(config, name);
	if (fd == -1)
		return NULL;

	if (fstat(fd, &filestat) < 0 ||
	    !S_ISREG(filestat.st_mode)) {
		close(fd);
		return NULL;
	}

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		close(fd);
		return NULL;
	}

	return fp;
}

static int
accept_config_file(const struct dirent *entry)
{
	const char *suffix = ".ini";
	char *end = strstr(entry->d_name, suffix);
	return end && end[strlen(suffix)] == '\0';
}

/**
 * \ingroup weston-config
 */
WL_EXPORT struct weston_config *
weston_config_parse(const char *name)
{
	FILE *fp;
	struct weston_config *config;
	struct stat st;
	struct dirent **namelist;
	char path[sizeof(config->path) + 2];
	bool ret;
	int n, i;

	config = zalloc(sizeof *config);
	if (config == NULL)
		return NULL;

	wl_list_init(&config->section_list);

	fp = weston_open_config_file(config, name);
	if (fp) {
		ret = weston_config_parse_internal(config, fp, name);

		fclose(fp);

		if (!ret) {
			fprintf(stderr, "failed to parse %s\n", config->path);
			free(config);
			return NULL;
		}
	}

	strcpy(path, config->path);
	strcat(path, ".d");
	if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
		return config;

	n = scandir(path, &namelist, accept_config_file, alphasort);
	if (n < 0)
		return config;

	for (i = 0; i < n; i++) {
		char *file = namelist[i]->d_name;
		char *sep = "/";
		char fpath[strlen(path)+strlen(sep)+strlen(file) + 1];
		strcpy(fpath, path);
		strcat(fpath, sep);
		strcat(fpath, file);
		free(namelist[i]);

		fp = weston_open_config_file(NULL, fpath);
		if (!fp)
			continue;

		ret = weston_config_parse_internal(config, fp, fpath);

		fclose(fp);

		if (!ret) {
			fprintf(stderr, "failed to parse '%s'\n", fpath);
			free(config);
			config = NULL;
			break;
		}
	}

	for (i++; i < n; i++)
		free(namelist[i]);

	free(namelist);
	return config;
}

WL_EXPORT const char *
weston_config_get_full_path(struct weston_config *config)
{
	return config == NULL ? NULL : config->path;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT int
weston_config_next_section(struct weston_config *config,
			   struct weston_config_section **section,
			   const char **name)
{
	if (config == NULL)
		return 0;

	if (*section == NULL)
		*section = container_of(config->section_list.next,
					struct weston_config_section, link);
	else
		*section = container_of((*section)->link.next,
					struct weston_config_section, link);

	if (&(*section)->link == &config->section_list)
		return 0;

	*name = (*section)->name;

	return 1;
}

/**
 * \ingroup weston-config
 */
WL_EXPORT void
weston_config_destroy(struct weston_config *config)
{
	struct weston_config_section *s, *next_s;
	struct weston_config_entry *e, *next_e;

	if (config == NULL)
		return;

	wl_list_for_each_safe(s, next_s, &config->section_list, link) {
		wl_list_for_each_safe(e, next_e, &s->entry_list, link) {
			free(e->key);
			free(e->value);
			free(e);
		}
		free(s->name);
		free(s);
	}

	free(config);
}

/**
 * \ingroup weston-config
 */
WL_EXPORT uint32_t
weston_config_get_binding_modifier(struct weston_config *config,
				   uint32_t default_mod)
{
	struct weston_config_section *shell_section = NULL;
	char *mod_string = NULL;
	uint32_t mod = default_mod;

	if (config)
		shell_section = weston_config_get_section(config, "shell", NULL, NULL);

	if (shell_section)
		weston_config_section_get_string(shell_section,
				"binding-modifier", &mod_string, "super");

	if (!mod_string || !strcmp(mod_string, "none"))
		mod = default_mod;
	else if (!strcmp(mod_string, "super"))
		mod = MODIFIER_SUPER;
	else if (!strcmp(mod_string, "alt"))
		mod = MODIFIER_ALT;
	else if (!strcmp(mod_string, "ctrl"))
		mod = MODIFIER_CTRL;
	else if (!strcmp(mod_string, "shift"))
		mod = MODIFIER_SHIFT;

	free(mod_string);

	return mod;
}
