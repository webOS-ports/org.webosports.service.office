// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "convert.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The formats a caller can ask for. LibreOfficeKit's saveAs() takes the
 * extension and picks the export filter for the document's kind itself
 * (writer_pdf_Export for a text document, calc_pdf_Export for a workbook...),
 * so the table only has to say which kinds each one makes sense for.
 */
static const OfficeFormat formats[] = {
        {"odt", OFFICE_TEXT},
        {"docx", OFFICE_TEXT},
        {"ods", OFFICE_SPREADSHEET},
        {"xlsx", OFFICE_SPREADSHEET},
        {"odp", OFFICE_PRESENTATION},
        {"pptx", OFFICE_PRESENTATION},
        {"pdf", OFFICE_ANY},
};

const OfficeFormat *office_format_find(const char *name)
{
	if (!name)
	{
		return NULL;
	}
	for (size_t i = 0; i < G_N_ELEMENTS(formats); i++)
	{
		if (strcmp(formats[i].name, name) == 0)
		{
			return &formats[i];
		}
	}
	return NULL;
}

/* LibreOfficeKitEnums.h: TEXT, SPREADSHEET, PRESENTATION, DRAWING, OTHER */
unsigned int office_doctype_bit(int lok_doctype)
{
	return lok_doctype >= 0 && lok_doctype <= 3 ? 1u << lok_doctype : 0;
}

const char *office_doctype_name(int lok_doctype)
{
	static const char *const names[] = {"text document", "spreadsheet", "presentation", "drawing"};
	return lok_doctype >= 0 && lok_doctype <= 3 ? names[lok_doctype] : "document of another kind";
}

static bool under_root(const char *root, const char *path)
{
	size_t len = strlen(root); /* Flawfinder: ignore - NUL-terminated by every caller */
	/* "/" as the root would make every path match; nobody configures that */
	return len > 1 && strncmp(path, root, len) == 0 && path[len] == '/' && path[len + 1] != '\0';
}

bool office_source_check(const char *root, const char *source, off_t max_size,
        OfficeSource *out, const char **error)
{
	char *real_root = NULL, *real = NULL;
	struct stat st;
	bool ok = false;

	*out = (OfficeSource){ 0 };
	if (!source || source[0] != '/')
	{
		*error = "source must be an absolute path";
		return false;
	}
	if (strlen(source) >= PATH_MAX) /* Flawfinder: ignore - a C string from the bus */
	{
		*error = "source path is too long";
		return false;
	}
	real_root = realpath(root, NULL); /* Flawfinder: ignore - allocates its own buffer */
	if (!real_root)
	{
		*error = "the document storage is not available";
		return false;
	}
	real = realpath(source, NULL); /* Flawfinder: ignore - allocates its own buffer */
	if (!real)
	{
		*error = errno == ENOENT || errno == ENOTDIR ? "source does not exist" : "source cannot be read";
		goto out;
	}
	if (!under_root(real_root, real))
	{
		*error = "source is outside the document storage";
		goto out;
	}
	if (stat(real, &st) != 0)
	{
		*error = "source cannot be read";
		goto out;
	}
	if (!S_ISREG(st.st_mode))
	{
		*error = "source is not a file";
		goto out;
	}
	if (st.st_size > max_size)
	{
		*error = "source is too large";
		goto out;
	}
	out->path = g_strdup(real);
	out->dev = st.st_dev;
	out->ino = st.st_ino;
	out->size = st.st_size;
	out->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
	ok = true;
out:
	free(real);
	free(real_root);
	return ok;
}

void office_source_clear(OfficeSource *src)
{
	g_free(src->path);
	src->path = NULL;
}

char *office_cache_name(const OfficeSource *src, const OfficeFormat *format)
{
	/* The NUL after the path keeps "a" + "1..." apart from "a1" + "..." */
	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
	char ident[96];
	int n = g_snprintf(ident, sizeof(ident), "%ju:%ju:%jd:%jd:%s",
	        (uintmax_t)src->dev, (uintmax_t)src->ino, (intmax_t)src->size,
	        (intmax_t)src->mtime_ns, format->name);
	char *name;

	g_checksum_update(sum, (const guchar *)src->path, (gssize)strlen(src->path) + 1); /* Flawfinder: ignore - from realpath() */
	g_checksum_update(sum, (const guchar *)ident, n);
	/* 128 bits of the digest are plenty for a cache of one device's files */
	name = g_strdup_printf("%.32s.%s", g_checksum_get_string(sum), format->name);
	g_checksum_free(sum);
	return name;
}

char *office_source_extension(const char *path)
{
	const char *base = strrchr(path, '/');
	const char *dot;
	size_t len;

	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	if (!dot || dot == base)
	{
		return g_strdup("");
	}
	len = strlen(dot + 1); /* Flawfinder: ignore - inside a C string */
	if (len < 1 || len > 8)
	{
		return g_strdup("");
	}
	for (size_t i = 1; i <= len; i++)
	{
		if (!g_ascii_isalnum(dot[i]))
		{
			return g_strdup("");
		}
	}
	return g_ascii_strdown(dot + 1, (gssize)len);
}

static bool write_all(int fd, const char *buf, size_t len)
{
	while (len > 0)
	{
		ssize_t n = write(fd, buf, len);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return false;
		}
		buf += n;
		len -= (size_t)n;
	}
	return true;
}

char *office_stage_source(const OfficeSource *src, off_t max_size, const char *dir,
        const char **error)
{
	char buf[65536];
	char *ext = office_source_extension(src->path);
	char *copy = g_strdup_printf("%s/source%s%s", dir, ext[0] ? "." : "", ext);
	int in = -1, out = -1;
	off_t total = 0;
	struct stat st;
	bool ok = false;

	g_free(ext);
	/*
	 * The path was resolved, so it has no links left: a link now means it was
	 * swapped after the check, and O_NOFOLLOW refuses it. A file renamed into
	 * its place is caught by the device and inode.
	 */
	in = open(src->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK); /* Flawfinder: ignore - checked by identity below */
	if (in < 0)
	{
		*error = "source cannot be read";
		goto out;
	}
	if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != src->dev || st.st_ino != src->ino)
	{
		*error = "source changed while it was being opened";
		goto out;
	}
	out = open(copy, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600); /* Flawfinder: ignore - O_EXCL in our own 0700 directory */
	if (out < 0)
	{
		*error = "cannot write the working copy";
		goto out;
	}
	for (;;)
	{
		ssize_t n = read(in, buf, sizeof(buf)); /* Flawfinder: ignore - bounded by sizeof(buf) */
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			*error = "source cannot be read";
			goto out;
		}
		if (n == 0)
		{
			break;
		}
		/* It may have grown since it was checked */
		total += n;
		if (total > max_size)
		{
			*error = "source is too large";
			goto out;
		}
		if (!write_all(out, buf, (size_t)n))
		{
			*error = "cannot write the working copy";
			goto out;
		}
	}
	if (close(out) != 0)
	{
		out = -1;
		*error = "cannot write the working copy";
		goto out;
	}
	out = -1;
	ok = true;
out:
	if (in >= 0)
	{
		close(in);
	}
	if (out >= 0)
	{
		close(out);
	}
	if (!ok)
	{
		unlink(copy);
		g_free(copy);
		copy = NULL;
	}
	return copy;
}

int office_publish(const char *staged, const char *target)
{
	return link(staged, target) == 0 ? 0 : errno;
}

/*
 * Recursive, but bounded by depth: the directories are ours and shallow.
 * readdir() is safe here: each DIR stream is used by the one thread that
 * opened it.
 */
static void remove_at(int parent, const char *name, int depth) // NOLINT(misc-no-recursion)
{
	int fd;
	DIR *d;
	struct dirent *e;

	if (unlinkat(parent, name, 0) == 0 || errno != EISDIR)
	{
		return;
	}
	if (depth <= 0)
	{
		return;
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
	{
		return;
	}
	d = fdopendir(fd);
	if (!d)
	{
		close(fd);
		return;
	}
	/* fd stays valid while d is open: fdopendir() took it over */
	while ((e = readdir(d)) != NULL) // NOLINT(concurrency-mt-unsafe)
	{
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
		{
			continue;
		}
		remove_at(fd, e->d_name, depth - 1); // NOLINT(misc-no-recursion)
	}
	closedir(d);
	unlinkat(parent, name, AT_REMOVEDIR);
}

void office_remove_tree(const char *dir)
{
	char *parent = g_path_get_dirname(dir);
	char *base = g_path_get_basename(dir);
	int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC); /* Flawfinder: ignore - only a base for unlinkat/openat */

	if (fd >= 0)
	{
		/* LibreOffice leaves at most a lock file; 4 levels is ample */
		remove_at(fd, base, 4);
		close(fd);
	}
	g_free(parent);
	g_free(base);
}

typedef struct
{
	char *name;
	int64_t mtime_ns;
	uint64_t size;
} CacheEntry;

static void entry_free(gpointer data)
{
	CacheEntry *entry = data;

	g_free(entry->name);
	g_free(entry);
}

/* g_ptr_array_sort() hands over pointers to the elements */
static gint entry_older(gconstpointer a, gconstpointer b)
{
	const CacheEntry *x = *(CacheEntry *const *)a, *y = *(CacheEntry *const *)b;
	return x->mtime_ns < y->mtime_ns ? -1 : x->mtime_ns > y->mtime_ns ? 1
	                                                                  : strcmp(x->name, y->name);
}

uint64_t office_cache_trim(const char *dir, uint64_t max_bytes, const char *keep)
{
	GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
	uint64_t total = 0;
	struct dirent *e;
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); /* Flawfinder: ignore - only a base for fstatat/unlinkat */
	DIR *d = fd >= 0 ? fdopendir(fd) : NULL;

	if (!d)
	{
		if (fd >= 0)
		{
			close(fd);
		}
		g_ptr_array_unref(entries);
		return 0;
	}
	while ((e = readdir(d)) != NULL) // NOLINT(concurrency-mt-unsafe) - see remove_at()
	{
		struct stat st;
		CacheEntry *entry;

		if (e->d_name[0] == '.')
		{
			continue;
		}
		if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
		{
			continue;
		}
		entry = g_new(CacheEntry, 1);
		entry->name = g_strdup(e->d_name);
		entry->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
		entry->size = (uint64_t)st.st_size;
		total += entry->size;
		g_ptr_array_add(entries, entry);
	}
	g_ptr_array_sort(entries, entry_older);
	for (guint i = 0; i < entries->len && total > max_bytes; i++)
	{
		CacheEntry *entry = g_ptr_array_index(entries, i);
		if (keep && strcmp(entry->name, keep) == 0)
		{
			continue;
		}
		if (unlinkat(fd, entry->name, 0) == 0)
		{
			total -= entry->size;
		}
	}
	g_ptr_array_unref(entries);
	closedir(d);
	return total;
}
