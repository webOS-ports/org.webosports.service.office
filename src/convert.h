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

/*
 * The parts of officed that touch the file system, shared by the daemon and
 * the office-convert helper, and kept free of the bus so the tests can drive
 * them directly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LibreOfficeKit's LOK_DOCTYPE_* as bits, so a format can allow several */
#define OFFICE_TEXT         (1u << 0)
#define OFFICE_SPREADSHEET  (1u << 1)
#define OFFICE_PRESENTATION (1u << 2)
#define OFFICE_DRAWING      (1u << 3)
#define OFFICE_ANY          (OFFICE_TEXT | OFFICE_SPREADSHEET | OFFICE_PRESENTATION | OFFICE_DRAWING)

typedef struct {
	const char *name;      /* as the caller asks for it, and the extension */
	unsigned int doctypes; /* what kind of document can be saved as it */
} OfficeFormat;

/* NULL for anything not in the table */
const OfficeFormat *office_format_find(const char *name);
/* The LOK_DOCTYPE_* value as an OFFICE_* bit; 0 for LOK_DOCTYPE_OTHER */
unsigned int office_doctype_bit(int lok_doctype);
/* "text", "spreadsheet", ... for error messages */
const char *office_doctype_name(int lok_doctype);

typedef struct {
	char *path;          /* resolved, under the root */
	dev_t dev;
	ino_t ino;
	off_t size;
	int64_t mtime_ns;
} OfficeSource;

/*
 * Resolves source and checks that it is a regular file under root (after
 * symbolic links), at most max_size bytes. On failure returns false with
 * *error set to a static message. office_source_clear() frees what it set.
 */
bool office_source_check(const char *root, const char *source, off_t max_size,
                         OfficeSource *out, const char **error);
void office_source_clear(OfficeSource *src);

/*
 * The cache file for a source and format, "<32 hex>.<format>", from the
 * path, identity, size and modification time: a changed file gets a new name,
 * so a cached copy is never stale and never has to be replaced. g_free() it.
 */
char *office_cache_name(const OfficeSource *src, const OfficeFormat *format);

/*
 * The source's own extension, lower case, if it is 1 to 8 letters and digits
 * (LibreOffice detects a type partly by it); "" otherwise. g_free() it.
 */
char *office_source_extension(const char *path);

/*
 * Copies the source into dir as "source[.ext]", reading it through a
 * descriptor opened without following a link and checked to be the file
 * office_source_check() saw: a file swapped in between is refused. Returns
 * the copy's path (g_free) or NULL with *error set.
 */
char *office_stage_source(const OfficeSource *src, off_t max_size, const char *dir,
                          const char **error);

/*
 * Makes target a second name of staged (link(2)): an existing target is never
 * replaced. 0, or an errno value (EEXIST: someone else made it first).
 */
int office_publish(const char *staged, const char *target);

/*
 * Removes a job directory and what is in it, without following links. Only
 * for directories this service made with mkdtemp(); the depth is bounded.
 */
void office_remove_tree(const char *dir);

/*
 * Deletes the oldest regular files in dir (by modification time) until the
 * ones left take at most max_bytes, never the one named keep. Hidden entries
 * (the profile, job directories) are left alone. Returns the bytes left.
 */
uint64_t office_cache_trim(const char *dir, uint64_t max_bytes, const char *keep);

#ifdef __cplusplus
}
#endif
