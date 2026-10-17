/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stand-in for LibreOffice's libmergedlo.so: lok_init_2() finds it in the
 * directory office-convert is built to look in and calls its
 * libreofficekit_hook_2(). It "converts" by writing
 * "converted <format> from <kind>\n" and then the source's bytes, and takes
 * its orders from the file "mode" next to it, since officed hands the helper
 * an environment of its own:
 *
 *   ok        convert; the kind comes from the source's extension
 *   initfail  the hook returns NULL
 *   loadfail  documentLoad() returns NULL with an error
 *   savefail  saveAs() returns false
 *   savelie   saveAs() returns true and writes nothing
 *   crash       documentLoad() aborts
 *   hang        documentLoad() never returns
 *   crash-paint paintPartTile() aborts (an editing session dies)
 *   hang-paint  paintPartTile() never returns
 *
 * Every load is appended to "loads" (the URL), for the tests to count.
 *
 * For editing sessions a document also has parts (3 for a presentation),
 * paints tiles whose every pixel is (part * 40, x / 100, y / 100, 255) in
 * twips, takes typed characters and committed text into the text it saves
 * after the source's bytes ("|typed:<text>"), logs every UNO command to
 * "unos", toggles Bold, and calls back as LibreOffice does: from another
 * thread, for the tiles to paint again, the cursor and the state of Bold.
 */

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>

#include <glib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <unistd.h>

static char *here = nullptr;
static char *mode = nullptr;

static const char *mode_text()
{
	return mode ? mode : "ok";
}

typedef struct
{
	LibreOfficeKitDocument base;
	char *path;
	int kind;
	int part;
	bool bold;
	GString *typed;
	char *preedit;     /* the composition in progress, as LibreOffice keeps it */
	std::atomic<bool> editing;    /* a presentation: a typed key starts editing the selected text box */
	std::atomic<bool> starting;   /* ... which takes LibreOffice a while: see doc_key() */
	LibreOfficeKitCallback callback;
	void *callback_data;
} FakeDoc;

/* As LibreOffice: from a thread of its own, while the caller goes on */
static void call_back(FakeDoc *doc, int type, const std::string &payload)
{
	if (!doc->callback)
		return;
	std::thread t([doc, type, payload]() { doc->callback(type, payload.c_str(), doc->callback_data); });
	t.join();
}

static void log_line(const char *file, const char *text)
{
	char *path = g_build_filename(here, file, nullptr);
	FILE *f = std::fopen(path, "a");

	if (f)
	{
		fprintf(f, "%s\n", text);
		fclose(f);
	}
	g_free(path);
}

static char *error_text = nullptr;

static void doc_destroy(LibreOfficeKitDocument *pThis)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	g_free(doc->path);
	if (doc->typed)
		g_string_free(doc->typed, TRUE);
	g_free(doc->preedit);
	g_free(doc);
}

static int doc_type(LibreOfficeKitDocument *pThis)
{
	return reinterpret_cast<FakeDoc *>(pThis)->kind;
}

static int doc_save_as(LibreOfficeKitDocument *pThis, const char *url, const char *format, const char *options)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);
	static const char *const kinds[] = { "text", "spreadsheet", "presentation", "drawing", "other" };
	char *out = g_filename_from_uri(url, nullptr, nullptr);
	char *data = nullptr, *body;
	gsize len = 0;
	int ok = 0;

	if (strcmp(mode, "savefail") == 0)
	{
		g_free(error_text);
		error_text = g_strdup("fake: refused to save");
	}
	else if (strcmp(mode, "savelie") == 0)
	{
		ok = 1;
	}
	else if (out && g_file_get_contents(doc->path, &data, &len, nullptr))
	{
		body = g_strdup_printf("converted %s from %s\n", format, kinds[doc->kind]);
		GString *all = g_string_new(body);
		g_string_append_len(all, data, static_cast<gssize>(len));
		if (doc->typed && doc->typed->len > 0)
			g_string_append_printf(all, "|typed:%s", doc->typed->str);
		ok = g_file_set_contents(out, all->str, static_cast<gssize>(all->len), nullptr);
		g_string_free(all, TRUE);
		g_free(body);
	}

	g_free(data);
	g_free(out);
	return ok;
}

static int doc_parts(LibreOfficeKitDocument *pThis)
{
	return reinterpret_cast<FakeDoc *>(pThis)->kind == LOK_DOCTYPE_PRESENTATION ? 3 : 1;
}

static int doc_get_part(LibreOfficeKitDocument *pThis)
{
	return reinterpret_cast<FakeDoc *>(pThis)->part;
}

static void doc_set_part(LibreOfficeKitDocument *pThis, int part)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	doc->part = part;
	call_back(doc, LOK_CALLBACK_SET_PART, std::to_string(part));
}

static void doc_size(LibreOfficeKitDocument *pThis, long *width, long *height)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	*width = doc->kind == LOK_DOCTYPE_PRESENTATION ? 28000 : 12240;
	*height = doc->kind == LOK_DOCTYPE_PRESENTATION ? 15750 : 15840;
}

static int doc_tile_mode(LibreOfficeKitDocument *pThis)
{
	return LOK_TILEMODE_RGBA;
}

static void doc_init_rendering(LibreOfficeKitDocument *pThis, const char *arguments)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	if (!doc->typed)
		doc->typed = g_string_new(nullptr);
}

static void doc_register_callback(LibreOfficeKitDocument *pThis, LibreOfficeKitCallback cb, void *data)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	doc->callback = cb;
	doc->callback_data = data;
}

static void doc_paint_part(LibreOfficeKitDocument *pThis, unsigned char *buffer, const int part, const int mode,
                           const int canvas_w, const int canvas_h, const int x, const int y, const int w, const int h)
{
	if (strcmp(mode_text(), "crash-paint") == 0)
		abort();
	if (strcmp(mode_text(), "hang-paint") == 0)
		for (;;)
			pause();

	for (int row = 0; row < canvas_h; row++)
	{
		for (int col = 0; col < canvas_w; col++)
		{
			unsigned char *p = buffer + (static_cast<size_t>(row) * static_cast<size_t>(canvas_w) + static_cast<size_t>(col)) * 4;
			p[0] = static_cast<unsigned char>(part * 40);
			p[1] = static_cast<unsigned char>((x / 100) & 0xff);
			p[2] = static_cast<unsigned char>((y / 100) & 0xff);
			p[3] = 255;
		}
	}
}

/*
 * As LibreOffice: it only invalidates tiles it painted, which it keeps by
 * part, and without this feature an invalidation has no part, so it matches
 * none of them and is never sent.
 */
static unsigned long long features = 0;

static void kit_set_optional_features(LibreOfficeKit *pThis, unsigned long long wanted)
{
	features = wanted;
}

static void typed_append(FakeDoc *doc, const char *text)
{
	g_string_append(doc->typed, text);
	if (features & LOK_FEATURE_PART_IN_INVALIDATION_CALLBACK)
		call_back(doc, LOK_CALLBACK_INVALIDATE_TILES, "0, 0, 1000, 1000, " + std::to_string(doc->part) + ", 0");
	call_back(doc, LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR, "100, 200, 0, 300");
}

static void doc_key(LibreOfficeKitDocument *pThis, int type, int char_code, int key_code)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);
	char utf8[8] = {};

	/*
	 * As Impress: the first key into a slide's selected text box starts
	 * editing it, and says so (the cursor shown) a while later, from its own
	 * thread. A key that comes before that may be handled out of its order:
	 * the fake marks it with '#'.
	 */
	if (type == LOK_KEYEVENT_KEYINPUT && doc->starting)
		typed_append(doc, "#");
	if (type == LOK_KEYEVENT_KEYINPUT && doc->kind == LOK_DOCTYPE_PRESENTATION && !doc->editing && !doc->starting)
	{
		doc->starting = true;
		std::thread([doc]() {
			g_usleep(100 * 1000);
			doc->editing = true;
			doc->starting = false;
			if (doc->callback)
				doc->callback(LOK_CALLBACK_CURSOR_VISIBLE, "true", doc->callback_data);
		}).detach();
	}
	else if (type == LOK_KEYEVENT_KEYINPUT)
		doc->editing = true;
	/* A key typed during a composition ends it, its text kept */
	if (type == LOK_KEYEVENT_KEYINPUT && doc->preedit)
	{
		typed_append(doc, doc->preedit);
		g_free(doc->preedit);
		doc->preedit = nullptr;
	}
	if (type == LOK_KEYEVENT_KEYINPUT && key_code == 1282)
		typed_append(doc, "\t");
	else if (type == LOK_KEYEVENT_KEYINPUT && char_code > 0)
	{
		g_unichar_to_utf8(static_cast<gunichar>(char_code), utf8);
		typed_append(doc, utf8);
	}
}

static void doc_mouse(LibreOfficeKitDocument *pThis, int type, int x, int y, int count, int buttons, int modifiers)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	if (type == LOK_MOUSEEVENT_MOUSEBUTTONUP)
		call_back(doc, LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR,
		          std::to_string(x) + ", " + std::to_string(y) + ", 0, 300");
}

static void doc_uno(LibreOfficeKitDocument *pThis, const char *command, const char *arguments, bool notify)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);
	char *line = g_strdup_printf("%s %s", command, arguments ? arguments : "-");

	log_line("unos", line);
	g_free(line);
	if (strcmp(command, ".uno:Bold") == 0)
	{
		doc->bold = !doc->bold;
		call_back(doc, LOK_CALLBACK_STATE_CHANGED, doc->bold ? ".uno:Bold=true" : ".uno:Bold=false");
	}
}

/*
 * As LibreOffice: TEXTINPUT sets the composition, _END commits it and ignores
 * its own text. As Impress: a text box that is only selected (no key typed
 * yet) takes no composition.
 */
static void doc_ext_text(LibreOfficeKitDocument *pThis, unsigned window, int type, const char *text)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	if (doc->kind == LOK_DOCTYPE_PRESENTATION && !doc->editing)
		return;

	if (type == LOK_EXT_TEXTINPUT)
	{
		g_free(doc->preedit);
		doc->preedit = g_strdup(text ? text : "");
	}
	else if (type == LOK_EXT_TEXTINPUT_END)
	{
		if (doc->preedit)
			typed_append(doc, doc->preedit);
		g_free(doc->preedit);
		doc->preedit = nullptr;
	}
}

static LibreOfficeKitDocumentClass doc_class;

static int kind_of(const char *path)
{
	static const char *const text[] = { ".doc", ".rtf", ".odt", ".docx", ".txt", ".wpd" };
	static const char *const sheet[] = { ".xls", ".ods", ".xlsx", ".csv" };
	static const char *const slides[] = { ".ppt", ".odp", ".pptx" };

	for (size_t i = 0; i < G_N_ELEMENTS(text); i++)
		if (g_str_has_suffix(path, text[i]))
			return LOK_DOCTYPE_TEXT;
	for (size_t i = 0; i < G_N_ELEMENTS(sheet); i++)
		if (g_str_has_suffix(path, sheet[i]))
			return LOK_DOCTYPE_SPREADSHEET;
	for (size_t i = 0; i < G_N_ELEMENTS(slides); i++)
		if (g_str_has_suffix(path, slides[i]))
			return LOK_DOCTYPE_PRESENTATION;
	return LOK_DOCTYPE_OTHER;
}

static LibreOfficeKitDocument *kit_load(LibreOfficeKit *pThis, const char *url)
{
	char *log = g_build_filename(here, "loads", nullptr);
	FILE *f = std::fopen(log, "a");
	FakeDoc *doc;

	if (f)
	{
		fprintf(f, "%s\n", url);
		fclose(f);
	}
	g_free(log);

	if (strcmp(mode, "crash") == 0)
		abort();
	if (strcmp(mode, "hang") == 0)
		for (;;)
			pause();
	if (strcmp(mode, "loadfail") == 0)
	{
		g_free(error_text);
		error_text = g_strdup("fake: not a document");
		return nullptr;
	}

	doc = g_new0(FakeDoc, 1);
	doc->path = g_filename_from_uri(url, nullptr, nullptr);
	if (!doc->path)
	{
		g_free(doc);
		return nullptr;
	}
	doc->kind = kind_of(doc->path);
	doc->base.pClass = &doc_class;
	return &doc->base;
}

static char *kit_error(LibreOfficeKit *pThis)
{
	return error_text ? strdup(error_text) : nullptr;
}

static void kit_free_error(char *text)
{
	free(text);
}

static LibreOfficeKitDocument *kit_load_with_options(LibreOfficeKit *pThis, const char *url, const char *options)
{
	return kit_load(pThis, url);
}

static void kit_destroy(LibreOfficeKit *pThis)
{
}

static LibreOfficeKitClass kit_class;
static LibreOfficeKit kit;

extern "C" __attribute__((visibility("default")))
LibreOfficeKit *libreofficekit_hook_2(const char *install_path, const char *user_profile_url);

extern "C" LibreOfficeKit *libreofficekit_hook_2(const char *install_path, const char *user_profile_url)
{
	char *mode_file;

	here = g_strdup(install_path);
	mode_file = g_build_filename(here, "mode", nullptr);
	if (!g_file_get_contents(mode_file, &mode, nullptr, nullptr))
		mode = g_strdup("ok");
	g_strstrip(mode);
	g_free(mode_file);

	/* office-convert must hand over a URL, as LibreOffice wants */
	if (!user_profile_url || strncmp(user_profile_url, "file:///", 8) != 0 || strcmp(mode, "initfail") == 0)
		return nullptr;

	kit_class.nSize = sizeof(kit_class);
	kit_class.destroy = kit_destroy;
	kit_class.documentLoad = kit_load;
	kit_class.documentLoadWithOptions = kit_load_with_options;
	kit_class.getError = kit_error;
	kit_class.freeError = kit_free_error;
	kit_class.setOptionalFeatures = kit_set_optional_features;
	doc_class.nSize = sizeof(doc_class);
	doc_class.destroy = doc_destroy;
	doc_class.saveAs = doc_save_as;
	doc_class.getDocumentType = doc_type;
	doc_class.getParts = doc_parts;
	doc_class.getPart = doc_get_part;
	doc_class.setPart = doc_set_part;
	doc_class.getDocumentSize = doc_size;
	doc_class.getTileMode = doc_tile_mode;
	doc_class.initializeForRendering = doc_init_rendering;
	doc_class.registerCallback = doc_register_callback;
	doc_class.paintPartTile = doc_paint_part;
	doc_class.postKeyEvent = doc_key;
	doc_class.postMouseEvent = doc_mouse;
	doc_class.postUnoCommand = doc_uno;
	doc_class.postWindowExtTextInputEvent = doc_ext_text;
	kit.pClass = &kit_class;
	return &kit;
}
