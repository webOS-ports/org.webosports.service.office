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
 * office-session - keeps one document open in LibreOfficeKit for editing.
 *
 *   office-session <source> <dev> <ino> <session-dir> <profile-dir>
 *
 * Run by officed, one process per open document, for the same reasons as
 * office-convert: a document that crashes or hangs LibreOffice takes only
 * this process along, and its memory goes back when the session ends.
 *
 * The source is copied into the session directory and LibreOffice works on
 * the copy; the user's file is never written. Requests come on stdin and
 * answers go to stdout, one line each (protocol.h). LibreOfficeKit is used
 * from this process's main thread only; its callbacks come on LibreOffice's
 * own main loop thread and are written out as events under the same lock as
 * the answers, so lines never interleave.
 *
 * Tiles are written as raw pixels (pxW * pxH * 4 bytes, in the order
 * getTileMode() says) to a ring of files in the session directory, each
 * written under a temporary name and renamed into place, so a reader never
 * sees half a tile. Saved copies go to the session directory too; officed
 * publishes them into its cache.
 *
 * The process ends when stdin closes (officed is gone or closed the session)
 * or on "quit", without an office shutdown (see office-convert).
 */

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glib.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "convert.h"
#include "exitcodes.h"
#include "protocol.h"

#ifndef LO_PROGRAM_DIR
#define LO_PROGRAM_DIR "/usr/lib/libreoffice/program"
#endif
#ifndef OFFICE_MAX_SOURCE
#define OFFICE_MAX_SOURCE (static_cast<off_t>(100) * 1024 * 1024)
#endif
/* Tile files kept before their names are used again */
#define TILE_RING 64

// protocol.c names the callbacks by number; it must agree with LibreOfficeKit
static_assert(LOK_CALLBACK_INVALIDATE_TILES == 0 && LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR == 1 &&
              LOK_CALLBACK_TEXT_SELECTION == 2 && LOK_CALLBACK_CURSOR_VISIBLE == 5 &&
              LOK_CALLBACK_STATE_CHANGED == 8 && LOK_CALLBACK_DOCUMENT_SIZE_CHANGED == 13 &&
              LOK_CALLBACK_SET_PART == 14 && LOK_CALLBACK_UNO_COMMAND_RESULT == 16 &&
              LOK_CALLBACK_ERROR == 22, "LOK_CALLBACK_* numbers changed: update office_callback_name()");
static_assert(LOK_EXT_TEXTINPUT == 0 && LOK_EXT_TEXTINPUT_END == 2, "LOK_EXT_TEXTINPUT_* numbers changed");

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

struct StrvDeleter
{
	void operator()(char **p) const
	{
		g_strfreev(p);
	}
};
using GStrv = std::unique_ptr<char *, StrvDeleter>;

std::mutex out_lock;

/*
 * LibreOffice and the document, for the life of the process: they are never
 * destroyed (see office-convert), the process ends with _exit().
 */
lok::Office *the_office = nullptr;
lok::Document *the_doc = nullptr;

/* One line to officed, whole; the lock keeps answers and events apart */
void emit(const std::vector<std::string> &fields)
{
	std::vector<const char *> ptrs;

	ptrs.reserve(fields.size());

	for (const std::string &f : fields)
	{
		ptrs.push_back(f.c_str());
	}

	const GStr line(office_proto_join(ptrs.data(), static_cast<int>(ptrs.size())));
	const std::lock_guard<std::mutex> hold(out_lock);

	std::fputs(line.get(), stdout);
	std::fflush(stdout);
}

void answer_ok(const std::string &id, std::vector<std::string> values = {})
{
	values.insert(values.begin(), { "R", id, "ok" });
	emit(values);
}

void answer_err(const std::string &id, const char *code, const char *text)
{
	emit({ "R", id, "err", code, text });
}

/*
 * Whether a text box is being edited, as LibreOffice's cursor says: typing
 * into a slide waits on it (Session::text()).
 */
std::mutex edit_lock;
std::condition_variable edit_changed;
bool editing = false;

void on_callback(int type, const char *payload, void *data)
{
	if (type == LOK_CALLBACK_CURSOR_VISIBLE)
	{
		const std::lock_guard<std::mutex> hold(edit_lock);

		editing = payload && strcmp(payload, "true") == 0;
		edit_changed.notify_all();
	}

	emit({ "E", std::to_string(type), payload ? payload : "" });
}

/* Until LibreOffice edits a text box, for up to 2 s */
void wait_for_editing()
{
	std::unique_lock<std::mutex> hold(edit_lock);

	(void)edit_changed.wait_for(hold, std::chrono::seconds(2), [] { return editing; });
}

bool is_editing()
{
	const std::lock_guard<std::mutex> hold(edit_lock);

	return editing;
}

GStr to_url(const char *path)
{
	GStr url(g_filename_to_uri(path, nullptr, nullptr));
	return url;
}

bool parse_u64(const char *text, guint64 *out)
{
	char *end = nullptr; // NOLINT(misc-const-correctness) - g_ascii_strtoull() writes it

	errno = 0;
	*out = g_ascii_strtoull(text, &end, 10);
	return errno == 0 && end != text && *end == '\0' && text[0] != '-';
}

/* Writes data to dir/name through a temporary file, so a reader sees all or nothing */
bool write_whole(const char *dir, const char *name, const unsigned char *data, size_t len)
{
	const GStr path(g_build_filename(dir, name, nullptr));
	const GStr tmp(g_strdup_printf("%s.tmp", path.get()));
	/* Flawfinder: ignore - our own name in the session directory, which only officed makes; no link followed */
	const int fd = open(tmp.get(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
	size_t done = 0;

	if (fd < 0)
	{
		return false;
	}

	while (done < len)
	{
		const ssize_t n = write(fd, data + done, len - done);

		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			close(fd);
			(void)unlink(tmp.get());
			return false;
		}

		done += static_cast<size_t>(n);
	}

	if (close(fd) != 0 || rename(tmp.get(), path.get()) != 0)
	{
		(void)unlink(tmp.get());
		return false;
	}

	return true;
}

class Session
{
public:
	Session(lok::Office *office, lok::Document *doc, const char *dir) : office_(office), doc_(doc), dir_(dir)
	{
	}

	/* One request; false when the session is to end */
	bool handle(char **f, int n)
	{
		const std::string id = f[0];
		const char *cmd = n > 1 ? f[1] : "";
		long v[7] = {};

		if (strcmp(cmd, "quit") == 0)
		{
			answer_ok(id);
			return false;
		}

		if (strcmp(cmd, "paint") == 0 && n == 9 && ints(f + 2, 7, v) && v[3] > 0 && v[4] > 0 &&
		        v[5] > 0 && v[5] <= OFFICE_TILE_MAX_PX && v[6] > 0 && v[6] <= OFFICE_TILE_MAX_PX)
		{
			paint(id, v);
		}
		else if (strcmp(cmd, "key") == 0 && n == 5 && ints(f + 2, 3, v) &&
		         (v[0] == LOK_KEYEVENT_KEYINPUT || v[0] == LOK_KEYEVENT_KEYUP))
		{
			key(static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2]));
			answer_ok(id);
		}
		else if (strcmp(cmd, "mouse") == 0 && n == 8 && ints(f + 2, 6, v) &&
		         v[0] >= LOK_MOUSEEVENT_MOUSEBUTTONDOWN && v[0] <= LOK_MOUSEEVENT_MOUSEMOVE)
		{
			doc_->postMouseEvent(static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2]),
			                     static_cast<int>(v[3]), static_cast<int>(v[4]), static_cast<int>(v[5]));
			answer_ok(id);
		}
		else if (strcmp(cmd, "uno") == 0 && n == 4 && office_uno_allowed(f[2]))
		{
			doc_->postUnoCommand(f[2], f[3][0] ? f[3] : nullptr, false);
			answer_ok(id);
		}
		else if (strcmp(cmd, "text") == 0 && n == 4 && ints(f + 2, 1, v) &&
		         (v[0] == LOK_EXT_TEXTINPUT || v[0] == LOK_EXT_TEXTINPUT_END) && g_utf8_validate(f[3], -1, nullptr))
		{
			text(static_cast<int>(v[0]), f[3]);
			answer_ok(id);
		}
		else if (strcmp(cmd, "part") == 0 && n == 3 && ints(f + 2, 1, v) && v[0] >= 0 && v[0] < doc_->getParts())
		{
			doc_->setPart(static_cast<int>(v[0]));
			info(id);
		}
		else if (strcmp(cmd, "info") == 0 && n == 2)
		{
			info(id);
		}
		else if (strcmp(cmd, "save") == 0 && n == 3)
		{
			save(id, f[2]);
		}
		else
		{
			answer_err(id, "badRequest", "not a request this session takes");
		}

		return true;
	}

	void info(const std::string &id)
	{
		long width = 0, height = 0;

		doc_->getDocumentSize(&width, &height);
		answer_ok(id, { std::to_string(doc_->getParts()), std::to_string(doc_->getPart()),
		                std::to_string(width), std::to_string(height) });
	}

private:
	/*
	 * Text from the app's keyboard. A composition in progress (a keypad's T9, a
	 * predicting keyboard) is LibreOffice's: LOK_EXT_TEXTINPUT shows it, and _END
	 * keeps what the last LOK_EXT_TEXTINPUT said (its own text is ignored).
	 * Text committed with no composition in progress is typed as keys, one
	 * character at a time: Impress takes no composition into a text box that
	 * is only selected, while a typed key starts editing it, as in every module.
	 */
	/* A typed character that starts editing a slide's text box waits for it, as in text() */
	void key(int type, int char_code, int key_code)
	{
		const int kind = doc_->getDocumentType();
		const bool starts = type == LOK_KEYEVENT_KEYINPUT && char_code > 0 &&
		                    (kind == LOK_DOCTYPE_PRESENTATION || kind == LOK_DOCTYPE_DRAWING) && !is_editing();

		doc_->postKeyEvent(type, char_code, key_code);

		if (starts)
		{
			wait_for_editing();
		}
	}

	void text(int type, const char *utf8)
	{
		if (type == LOK_EXT_TEXTINPUT)
		{
			doc_->postWindowExtTextInputEvent(0, LOK_EXT_TEXTINPUT, utf8);
			composing_ = true;
			return;
		}

		if (composing_)
		{
			doc_->postWindowExtTextInputEvent(0, LOK_EXT_TEXTINPUT, utf8);
			doc_->postWindowExtTextInputEvent(0, LOK_EXT_TEXTINPUT_END, "");
			composing_ = false;
			return;
		}

		/*
		 * The first key into a slide's text box that is only selected starts
		 * editing it, and while LibreOffice does that, a key posted after it
		 * can be handled before the keys posted before it (seen on the
		 * TouchPad: "a1bcdef" for "abcdef" then "1"). So that key goes alone,
		 * and the rest, and the next request, wait until editing has begun.
		 */
		const int kind = doc_->getDocumentType();
		bool starts = (kind == LOK_DOCTYPE_PRESENTATION || kind == LOK_DOCTYPE_DRAWING) && !is_editing();

		for (const char *p = utf8; *p; p = g_utf8_next_char(p))
		{
			const int c = static_cast<int>(g_utf8_get_char(p));
			// A newline or a tab is its key: Enter (1280) and Tab (1282)
			const int key = c == '\n' ? 1280 : c == '\t' ? 1282 : 0;

			doc_->postKeyEvent(LOK_KEYEVENT_KEYINPUT, key ? 0 : c, key);
			doc_->postKeyEvent(LOK_KEYEVENT_KEYUP, key ? 0 : c, key);

			if (starts)
			{
				wait_for_editing();
				starts = false;
			}
		}
	}

	static bool ints(char **text, int n, long *out)
	{
		for (int i = 0; i < n; i++)
		{
			if (!office_proto_int(text[i], -1000000000L, 1000000000L, &out[i]))
			{
				return false;
			}
		}

		return true;
	}

	void paint(const std::string &id, const long *v)
	{
		const size_t px_w = static_cast<size_t>(v[5]), px_h = static_cast<size_t>(v[6]);
		std::vector<unsigned char> buffer(px_w * px_h * 4);
		const GStr name(g_strdup_printf("tile-%u.raw", tile_seq_++ % TILE_RING));

		if (v[0] < 0 || v[0] >= doc_->getParts())
		{
			answer_err(id, "badRequest", "no such part");
			return;
		}

		doc_->paintPartTile(buffer.data(), static_cast<int>(v[0]), 0, static_cast<int>(px_w), static_cast<int>(px_h),
		                    static_cast<int>(v[1]), static_cast<int>(v[2]), static_cast<int>(v[3]), static_cast<int>(v[4]));

		if (!write_whole(dir_, name.get(), buffer.data(), buffer.size()))
		{
			answer_err(id, "internalError", "the tile could not be written");
			return;
		}

		const GStr path(g_build_filename(dir_, name.get(), nullptr));
		answer_ok(id, { path.get() });
	}

	void save(const std::string &id, const char *format_name)
	{
		const OfficeFormat *format = office_format_find(format_name);

		if (!format)
		{
			answer_err(id, "badFormat", "not a format a document can be saved in");
			return;
		}

		if (!(office_doctype_bit(doc_->getDocumentType()) & format->doctypes))
		{
			answer_err(id, "unsupportedConversion", "the document cannot be saved in this format");
			return;
		}

		const GStr path(g_strdup_printf("%s/save-%u.%s", dir_, save_seq_++, format->name));
		const GStr url = to_url(path.get());
		struct stat st;

		if (!url || !doc_->saveAs(url.get(), format->name, nullptr) ||
		        stat(path.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
		{
			char *error = office_->getError();

			std::fprintf(stderr, "office-session: cannot save: %s\n", error ? error : "(no reason given)");

			if (error)
			{
				office_->freeError(error);
			}

			answer_err(id, "saveFailed", "LibreOffice could not save the document");
			return;
		}

		answer_ok(id, { path.get() });
	}

	lok::Office *office_;
	lok::Document *doc_;
	const char *dir_;
	unsigned tile_seq_ = 0;
	unsigned save_seq_ = 0;
	bool composing_ = false;
};

int run(int argc, char **argv)
{
	OfficeSource src = {};
	const char *problem = nullptr;
	guint64 dev, ino;

	if (argc != 6 || !parse_u64(argv[2], &dev) || !parse_u64(argv[3], &ino) ||
	        argv[1][0] != '/' || argv[4][0] != '/' || argv[5][0] != '/')
	{
		std::fprintf(stderr, "usage: office-session <source> <dev> <ino> <session-dir> <profile-dir>\n");
		return OFFICE_EXIT_USAGE;
	}

	src.path = argv[1];
	src.dev = static_cast<dev_t>(dev);
	src.ino = static_cast<ino_t>(ino);
	const char *dir = argv[4];
	// Tiles and saved copies are read by the Documents app
	umask(022); /* Flawfinder: ignore - widens nothing: tiles are meant to be world-readable */

	const GStr copy(office_stage_source(&src, OFFICE_MAX_SOURCE, dir, &problem));

	if (!copy)
	{
		answer_err("0", "badSource", problem);
		return OFFICE_EXIT_SOURCE;
	}

	const GStr in_url = to_url(copy.get());
	const GStr profile_url = to_url(argv[5]);

	if (!in_url || !profile_url)
	{
		answer_err("0", "internalError", "bad paths");
		return OFFICE_EXIT_USAGE;
	}

	lok::Office *office = the_office = lok::lok_cpp_init(LO_PROGRAM_DIR, profile_url.get());

	if (!office)
	{
		answer_err("0", "internalError", "LibreOffice did not start");
		return OFFICE_EXIT_INIT;
	}

	/*
	 * LibreOffice only says which tiles to paint again for tiles it painted,
	 * which it keeps by part; without the part in the invalidation it matches
	 * none of them, and nothing is ever repainted after an edit.
	 */
	office->setOptionalFeatures(LOK_FEATURE_PART_IN_INVALIDATION_CALLBACK);

	lok::Document *doc = the_doc = office->documentLoad(in_url.get());

	if (!doc)
	{
		char *error = office->getError();

		std::fprintf(stderr, "office-session: cannot load %s: %s\n", src.path, error ? error : "(no reason given)");

		if (error)
		{
			office->freeError(error);
		}

		answer_err("0", "loadFailed", "LibreOffice could not open the document");
		return OFFICE_EXIT_LOAD;
	}

	// The Documents app draws the tiles; LibreOffice keeps the view state
	doc->initializeForRendering(nullptr);
	doc->registerCallback(on_callback, nullptr);

	Session session(office, doc, dir);
	long width = 0, height = 0;

	doc->getDocumentSize(&width, &height);
	answer_ok("0", { std::to_string(doc->getDocumentType()), std::to_string(doc->getParts()),
	                 std::to_string(doc->getPart()), std::to_string(width), std::to_string(height),
	                 doc->getTileMode() == LOK_TILEMODE_BGRA ? "bgra" : "rgba" });

	char *line = nullptr;
	size_t cap = 0;
	ssize_t len;
	bool more = true;

	while (more && (len = getline(&line, &cap, stdin)) > 0)
	{
		int n = 0;

		if (line[len - 1] == '\n')
		{
			line[len - 1] = '\0';
		}

		const GStrv fields(office_proto_split(line, &n));

		if (!fields || n < 2)
		{
			// Without an id there is nobody to answer: it is officed's mistake
			std::fprintf(stderr, "office-session: a request that is not one\n");
			continue;
		}

		more = session.handle(fields.get(), n);
	}

	free(line);
	return OFFICE_EXIT_OK;
}

} // namespace

int main(int argc, char **argv)
{
	const int status = run(argc, argv);

	// See office-convert: no office shutdown, and no static destructors
	std::fflush(nullptr);
	_exit(status);
}
