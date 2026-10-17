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
 * office-convert - converts one document with LibreOfficeKit, then exits.
 *
 *   office-convert <source> <dev> <ino> <job-dir> <format> <profile-dir>
 *
 * Run by officed, one process per document: LibreOffice is large, not
 * thread-safe and parses whatever it is given, so a document that crashes or
 * hangs it takes only this process along (officed kills it after a while),
 * and the hundreds of megabytes it maps go back when the job is done.
 *
 * The source is copied into the job directory first and LibreOffice opens the
 * copy, so it never sees the user's file (or leaves its lock file next to it),
 * and a file swapped in after officed checked it is refused. The result is
 * <job-dir>/result.<format>; officed publishes it into the cache.
 *
 * C++ because LibreOfficeKit's unstable API - getDocumentType() here, the
 * tile rendering and input of an editing session later - only builds as C++
 * (LibreOfficeKitEnums.h uses static_cast under LOK_USE_UNSTABLE_API).
 *
 * The exit status says what went wrong; the details go to stderr, the journal.
 */

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <glib.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "convert.h"
#include "exitcodes.h"

#ifndef LO_PROGRAM_DIR
#define LO_PROGRAM_DIR "/usr/lib/libreoffice/program"
#endif
#ifndef OFFICE_MAX_SOURCE
#define OFFICE_MAX_SOURCE (static_cast<off_t>(100) * 1024 * 1024)
#endif

namespace
{

struct GFreeDeleter
{
	void operator()(char *p) const
	{
		g_free(p);
	}
};
using GStr = std::unique_ptr<char, GFreeDeleter>;

bool parse_u64(const char *text, guint64 *out)
{
	char *end = nullptr; // NOLINT(misc-const-correctness) - g_ascii_strtoull() writes it

	errno = 0;
	*out = g_ascii_strtoull(text, &end, 10);
	return errno == 0 && end != text && *end == '\0' && text[0] != '-';
}

GStr to_url(const char *path)
{
	GError *error = nullptr;
	GStr url(g_filename_to_uri(path, nullptr, &error));

	if (!url)
	{
		std::fprintf(stderr, "office-convert: %s\n", error->message);
		g_error_free(error);
	}

	return url;
}

void print_error(lok::Office *office, const char *what, const char *path)
{
	char *error = office->getError();

	std::fprintf(stderr, "office-convert: %s %s: %s\n", what, path, error ? error : "(no reason given)");

	if (error)
	{
		office->freeError(error);
	}
}

int convert(int argc, char **argv)
{
	OfficeSource src = {};
	const char *problem = nullptr;
	guint64 dev, ino;

	if (argc != 7 || !parse_u64(argv[2], &dev) || !parse_u64(argv[3], &ino) ||
	        argv[1][0] != '/' || argv[4][0] != '/' || argv[6][0] != '/')
	{
		std::fprintf(stderr, "usage: office-convert <source> <dev> <ino> <job-dir> <format> <profile-dir>\n");
		return OFFICE_EXIT_USAGE;
	}

	const OfficeFormat *format = office_format_find(argv[5]);

	if (!format)
	{
		std::fprintf(stderr, "office-convert: unknown format \"%s\"\n", argv[5]);
		return OFFICE_EXIT_USAGE;
	}

	src.path = argv[1];
	src.dev = static_cast<dev_t>(dev);
	src.ino = static_cast<ino_t>(ino);
	const char *job_dir = argv[4];
	// What LibreOffice writes is the cache's, which the Documents app reads
	umask(022); /* Flawfinder: ignore - widens nothing: results are meant to be world-readable */

	const GStr copy(office_stage_source(&src, OFFICE_MAX_SOURCE, job_dir, &problem));

	if (!copy)
	{
		std::fprintf(stderr, "office-convert: %s: %s\n", src.path, problem);
		return OFFICE_EXIT_SOURCE;
	}

	const GStr result(g_strdup_printf("%s/result.%s", job_dir, format->name));
	const GStr in_url = to_url(copy.get());
	const GStr out_url = to_url(result.get());
	const GStr profile_url = to_url(argv[6]);

	if (!in_url || !out_url || !profile_url)
	{
		return OFFICE_EXIT_USAGE;
	}

	/*
	 * Never deleted: ~Office() is a full office shutdown, seconds of work for a
	 * process that is about to end, and the result is closed by then.
	 */
	lok::Office *office = lok::lok_cpp_init(LO_PROGRAM_DIR, profile_url.get());

	if (!office)
	{
		std::fprintf(stderr, "office-convert: LibreOfficeKit did not start from %s\n", LO_PROGRAM_DIR);
		return OFFICE_EXIT_INIT;
	}

	std::unique_ptr<lok::Document> doc(office->documentLoad(in_url.get()));

	if (!doc)
	{
		print_error(office, "cannot load", src.path);
		return OFFICE_EXIT_LOAD;
	}

	const int doctype = doc->getDocumentType();

	if (!(office_doctype_bit(doctype) & format->doctypes))
	{
		std::fprintf(stderr, "office-convert: %s is a %s and cannot be saved as %s\n",
		             src.path, office_doctype_name(doctype), format->name);
		return OFFICE_EXIT_KIND;
	}

	if (!doc->saveAs(out_url.get(), format->name, nullptr))
	{
		print_error(office, "cannot save", src.path);
		return OFFICE_EXIT_SAVE;
	}

	struct stat st;

	if (stat(result.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
	{
		std::fprintf(stderr, "office-convert: LibreOffice reported %s as saved, but it is not there\n", result.get());
		return OFFICE_EXIT_SAVE;
	}

	return OFFICE_EXIT_OK;
}

} // namespace

int main(int argc, char **argv)
{
	const int status = convert(argc, argv);

	// See ~Office() above: no office shutdown, and no static destructors
	std::fflush(nullptr);
	_exit(status);
}
