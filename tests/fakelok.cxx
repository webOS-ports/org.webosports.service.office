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
 *   crash     documentLoad() aborts
 *   hang      documentLoad() never returns
 *
 * Every load is appended to "loads" (the URL), for the tests to count.
 */

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>

#include <glib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static char *here = nullptr;
static char *mode = nullptr;

typedef struct
{
	LibreOfficeKitDocument base;
	char *path;
	int kind;
} FakeDoc;

static char *error_text = nullptr;

static void doc_destroy(LibreOfficeKitDocument *pThis)
{
	FakeDoc *doc = reinterpret_cast<FakeDoc *>(pThis);

	g_free(doc->path);
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
		ok = g_file_set_contents(out, all->str, static_cast<gssize>(all->len), nullptr);
		g_string_free(all, TRUE);
		g_free(body);
	}

	g_free(data);
	g_free(out);
	return ok;
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
	doc_class.nSize = sizeof(doc_class);
	doc_class.destroy = doc_destroy;
	doc_class.saveAs = doc_save_as;
	doc_class.getDocumentType = doc_type;
	kit.pClass = &kit_class;
	return &kit;
}
