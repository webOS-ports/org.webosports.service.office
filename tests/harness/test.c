/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drives officed's real officed.c, unchanged, against the stand-in bus and
 * JSON library, with the document storage and cache in a work directory and
 * the real office-convert built against the fake LibreOfficeKit (fakelok.cxx),
 * whose behaviour the "mode" file next to it sets.
 *
 * Usage: officed-test <mode>   (run.sh builds it with the paths compiled in)
 *
 * Every call must be answered exactly once; any failed check, sanitizer
 * report or leftover (message, JSON value, file descriptor, job directory)
 * exits non-zero.
 */

#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>

#include "shim.h"

#define main officed_main
#include OFFICED_C
#undef main

static const char *mode;
static int failures = 0;
static int calls = 0, answered = 0;
static GPtrArray *replies;   /* "request => reply", in the order they came */

#define CHECK(cond, ...) \
	do \
	{ \
		if (!(cond)) \
		{ \
			g_printerr("FAIL %s:%d: ", __FILE__, __LINE__); \
			g_printerr(__VA_ARGS__); \
			g_printerr("\n"); \
			failures++; \
		} \
	} while (0)

static char *doc_path(const char *name)
{
	return g_build_filename(OFFICE_ROOT, name, NULL);
}

static void put_doc(const char *name, const char *text)
{
	char *path = doc_path(name);
	g_assert(g_file_set_contents(path, text, -1, NULL));
	g_free(path);
}

static void set_mode(const char *fake_mode)
{
	g_assert(g_file_set_contents(FAKELOK_DIR "/mode", fake_mode, -1, NULL));
}

static int loads(void)
{
	char *text = NULL;
	int n = 0;

	if (g_file_get_contents(FAKELOK_DIR "/loads", &text, NULL, NULL))
	{
		for (char *p = text; *p; p++)
		{
			n += *p == '\n';
		}
	}

	g_free(text);
	return n;
}

static void on_reply(const char *request, const char *reply, gpointer user_data)
{
	answered++;
	g_ptr_array_add(replies, g_strdup_printf("%s => %s", request, reply));
}

static void call(const char *payload)
{
	calls++;
	CHECK(shim_call("convert", payload, on_reply, NULL), "convert not handled: %s", payload);
}

static void convert(const char *name, const char *format)
{
	char *path = doc_path(name);
	char *payload = g_strdup_printf("{\"source\":\"%s\",\"format\":\"%s\"}", path, format);

	call(payload);
	g_free(payload);
	g_free(path);
}

/* The reply to the n-th answered call, or "" */
static const char *reply_at(guint n)
{
	return n < replies->len ? g_ptr_array_index(replies, n) : "";
}

static bool reply_has(guint n, const char *text)
{
	return strstr(reply_at(n), text) != NULL;
}

/* The "target" of a reply, g_free it */
static char *target_of(guint n)
{
	const char *r = strstr(reply_at(n), "\"target\":\"");
	const char *end;

	if (!r)
	{
		return g_strdup("");
	}

	r += strlen("\"target\":\"");
	end = strchr(r, '"');
	return end ? g_strndup(r, (gsize)(end - r)) : g_strdup("");
}

/* ------------------------------------------------------------- the modes */

static void after_first(const char *request, const char *reply, gpointer user_data)
{
	on_reply(request, reply, user_data);
	/* Answered from the cache, at once */
	convert("Letter.doc", "odt");
	CHECK(answered == 2, "the cached answer was not immediate");
}

static void start_convert(void)
{
	put_doc("Letter.doc", "hello");
	calls++;
	{
		char *path = doc_path("Letter.doc");
		char *payload = g_strdup_printf("{\"source\":\"%s\",\"format\":\"odt\"}", path);
		CHECK(shim_call("convert", payload, after_first, NULL), "convert not handled");
		g_free(payload);
		g_free(path);
	}
}

static void check_convert(void)
{
	char *t0 = target_of(0), *t1 = target_of(1), *data = NULL;
	char *dir;

	CHECK(reply_has(0, "\"returnValue\":true") && reply_has(0, "\"cached\":false") &&
	      reply_has(0, "\"format\":\"odt\""), "first reply: %s", reply_at(0));
	CHECK(reply_has(1, "\"returnValue\":true") && reply_has(1, "\"cached\":true"), "second reply: %s", reply_at(1));
	CHECK(t0[0] && strcmp(t0, t1) == 0, "targets differ: %s / %s", t0, t1);
	CHECK(g_str_has_prefix(t0, OFFICE_CACHE_DIR "/") && g_str_has_suffix(t0, ".odt"), "target %s", t0);
	CHECK(g_file_get_contents(t0, &data, NULL, NULL) && strcmp(data, "converted odt from text\nhello") == 0,
	      "result: %s", data ? data : "(missing)");
	CHECK(loads() == 1, "%d loads, want 1", loads());

	/* Nothing new next to the original */
	dir = g_strdup(OFFICE_ROOT);
	{
		GDir *d = g_dir_open(dir, 0, NULL);
		const char *name;
		int n = 0;
		while (d && (name = g_dir_read_name(d)) != NULL)
		{
			n += strcmp(name, ".cache") != 0;
		}
		if (d)
		{
			g_dir_close(d);
		}
		CHECK(n == 1, "%d entries next to the original, want 1", n);
	}
	g_free(dir);
	g_free(data);
	g_free(t0);
	g_free(t1);
}

static void start_coalesce(void)
{
	put_doc("Sheet.xls", "1,2,3");
	convert("Sheet.xls", "ods");
	convert("Sheet.xls", "ods");
	CHECK(answered == 0, "answered before the conversion ran");
}

static void check_coalesce(void)
{
	char *t0 = target_of(0), *t1 = target_of(1);

	CHECK(reply_has(0, "\"cached\":false") && reply_has(1, "\"cached\":false"), "%s / %s", reply_at(0), reply_at(1));
	CHECK(t0[0] && strcmp(t0, t1) == 0, "targets differ");
	CHECK(loads() == 1, "%d loads, want 1", loads());
	g_free(t0);
	g_free(t1);
}

static void start_errors(void)
{
	char *outside = g_build_filename(WORK_DIR, "outside.doc", NULL);
	char *payload;
	static const char *const plain[] = {
		"not json",
		"[]",
		"{}",
		"{\"source\":\"/x.doc\"}",
		"{\"format\":\"odt\"}",
		"{\"source\":1,\"format\":\"odt\"}",
		"{\"source\":\"/x.doc\",\"format\":true}",
		"{\"source\":\"\",\"format\":\"odt\"}",
	};

	put_doc("big.doc", "0123456789012345678901234567890123456789");
	g_assert(g_file_set_contents(outside, "secret", -1, NULL));
	g_mkdir(OFFICE_ROOT "/folder.doc", 0755);

	for (size_t i = 0; i < G_N_ELEMENTS(plain); i++)
	{
		call(plain[i]);
	}

	convert("Letter.doc", "doc");                /* 8: badFormat */
	call("{\"source\":\"Letter.doc\",\"format\":\"odt\"}");   /* 9: relative */
	payload = g_strdup_printf("{\"source\":\"%s\",\"format\":\"odt\"}", outside);
	call(payload);                                /* 10: outside */
	g_free(payload);
	payload = g_strdup_printf("{\"source\":\"%s/../outside.doc\",\"format\":\"odt\"}", OFFICE_ROOT);
	call(payload);                                /* 11: escapes with .. */
	g_free(payload);
	convert("missing.doc", "odt");               /* 12 */
	convert("folder.doc", "odt");                /* 13 */
	convert("big.doc", "odt");                   /* 14: over OFFICE_MAX_SOURCE */
	CHECK(answered == calls, "%d of %d answered at once", answered, calls);
	g_free(outside);
}

static void check_errors(void)
{
	static const char *const want[] = {
		"badRequest", "badRequest", "badRequest", "badRequest", "badRequest", "badRequest", "badRequest",
		"badRequest", "badFormat", "badSource", "badSource", "badSource", "badSource", "badSource", "badSource",
	};

	CHECK(replies->len == G_N_ELEMENTS(want), "%u replies", replies->len);
	for (guint i = 0; i < replies->len && i < G_N_ELEMENTS(want); i++)
	{
		char *code = g_strdup_printf("\"errorCode\":\"%s\"", want[i]);
		CHECK(reply_has(i, "\"returnValue\":false") && reply_has(i, code), "reply %u: %s, want %s", i, reply_at(i), want[i]);
		g_free(code);
	}
	CHECK(reply_has(10, "outside the document storage") && reply_has(11, "outside the document storage"),
	      "outside: %s", reply_at(10));
	CHECK(reply_has(14, "too large"), "big: %s", reply_at(14));
	CHECK(loads() == 0, "LibreOffice was asked anyway");
}

static void start_kind(void)
{
	put_doc("Letter.doc", "text");
	put_doc("Sheet.xls", "sheet");
	put_doc("Slides.ppt", "slides");
	convert("Letter.doc", "xlsx");
	convert("Sheet.xls", "odt");
	convert("Slides.ppt", "pdf");
	convert("Slides.ppt", "pptx");
}

static void check_kind(void)
{
	CHECK(replies->len == 4, "%u replies", replies->len);
	CHECK(reply_has(0, "\"errorCode\":\"unsupportedConversion\"") && reply_has(0, "only a spreadsheet"), "%s", reply_at(0));
	CHECK(reply_has(1, "\"errorCode\":\"unsupportedConversion\"") && reply_has(1, "only a text document"), "%s", reply_at(1));
	CHECK(reply_has(2, "\"returnValue\":true") && reply_has(2, ".pdf"), "%s", reply_at(2));
	CHECK(reply_has(3, "\"returnValue\":true") && reply_has(3, ".pptx"), "%s", reply_at(3));
}

static const char *fail_mode, *fail_code;

static void start_fail(void)
{
	set_mode(fail_mode);
	put_doc("Letter.doc", "text");
	convert("Letter.doc", "odt");
}

static void check_fail(void)
{
	char *code = g_strdup_printf("\"errorCode\":\"%s\"", fail_code);
	GDir *d = g_dir_open(OFFICE_CACHE_DIR, 0, NULL);
	const char *name;

	CHECK(replies->len == 1 && reply_has(0, "\"returnValue\":false") && reply_has(0, code),
	      "%s: %s, want %s", fail_mode, reply_at(0), fail_code);
	/* No result for a failed conversion, and so no "cached" success later */
	while (d && (name = g_dir_read_name(d)) != NULL)
	{
		CHECK(name[0] == '.', "%s: %s left in the cache", fail_mode, name);
	}
	if (d)
	{
		g_dir_close(d);
	}
	g_free(code);
}

static void start_busy(void)
{
	set_mode("hang");
	for (int i = 0; i < MAX_QUEUED + 2; i++)
	{
		char *name = g_strdup_printf("doc%d.doc", i);
		put_doc(name, "x");
		convert(name, "odt");
		g_free(name);
	}
	/* One running, MAX_QUEUED waiting, the last one turned away */
	CHECK(answered == 1 && reply_has(0, "\"errorCode\":\"busy\""), "busy: %s", reply_at(0));

	/* A result already in the cache is still handed out, queue or not */
	{
		OfficeSource src;
		const char *error = NULL;
		char *path = doc_path("cached.doc"), *name, *target;

		put_doc("cached.doc", "x");
		g_assert(office_source_check(OFFICE_ROOT, path, OFFICE_MAX_SOURCE, &src, &error));
		name = office_cache_name(&src, office_format_find("odt"));
		target = g_build_filename(OFFICE_CACHE_DIR, name, NULL);
		g_assert(g_file_set_contents(target, "made earlier", -1, NULL));
		convert("cached.doc", "odt");
		CHECK(answered == 2 && reply_has(1, "\"cached\":true"), "cached while busy: %s", reply_at(1));
		office_source_clear(&src);
		g_free(target);
		g_free(name);
		g_free(path);
	}
}

static void check_busy(void)
{
	int timeouts = 0;

	for (guint i = 2; i < replies->len; i++)
	{
		timeouts += reply_has(i, "\"errorCode\":\"timeout\"");
	}
	CHECK(replies->len == MAX_QUEUED + 3 && timeouts == MAX_QUEUED + 1, "%u replies, %d timeouts", replies->len, timeouts);
	CHECK(loads() == MAX_QUEUED + 1, "%d loads", loads());
}

static void after_changed_first(const char *request, const char *reply, gpointer user_data)
{
	on_reply(request, reply, user_data);
	/* The document changes: a new result, the old one kept */
	put_doc("Letter.doc", "hello, changed");
	convert("Letter.doc", "odt");
}

static void start_changed(void)
{
	char *path = doc_path("Letter.doc");
	char *payload = g_strdup_printf("{\"source\":\"%s\",\"format\":\"odt\"}", path);

	put_doc("Letter.doc", "hello");
	calls++;
	CHECK(shim_call("convert", payload, after_changed_first, NULL), "not handled");
	g_free(payload);
	g_free(path);
}

static void check_changed(void)
{
	char *t0 = target_of(0), *t1 = target_of(1), *data = NULL;

	CHECK(reply_has(0, "\"cached\":false") && reply_has(1, "\"cached\":false"), "%s / %s", reply_at(0), reply_at(1));
	CHECK(t0[0] && t1[0] && strcmp(t0, t1) != 0, "same target for a changed document");
	CHECK(g_file_get_contents(t0, &data, NULL, NULL) && strcmp(data, "converted odt from text\nhello") == 0,
	      "old result replaced: %s", data ? data : "(gone)");
	CHECK(loads() == 2, "%d loads", loads());
	g_free(data);
	g_free(t0);
	g_free(t1);
}

static gboolean send_sigterm(gpointer user_data)
{
	kill(getpid(), SIGTERM);
	return G_SOURCE_REMOVE;
}

static void start_sigterm(void)
{
	set_mode("hang");
	put_doc("a.doc", "a");
	put_doc("b.doc", "b");
	put_doc("c.doc", "c");
	convert("a.doc", "odt");
	convert("b.doc", "odt");
	convert("c.doc", "pdf");
	g_timeout_add(300, send_sigterm, NULL);
}

static void check_sigterm(void)
{
	CHECK(replies->len == 3, "%u replies", replies->len);
	for (guint i = 0; i < replies->len; i++)
	{
		CHECK(reply_has(i, "shutting down"), "%s", reply_at(i));
	}
	/* Only the first was ever started */
	CHECK(loads() == 1, "%d loads", loads());
}

static void start_disconnect(void)
{
	shim_disconnect();
}

/* ------------------------------------------------------------- the checks */

static char *open_files(void)
{
	GString *out = g_string_new(NULL);
	GDir *d = g_dir_open("/proc/self/fd", 0, NULL);
	const char *name;

	while (d && (name = g_dir_read_name(d)) != NULL)
	{
		char *link = g_build_filename("/proc/self/fd", name, NULL);
		char *target = g_file_read_link(link, NULL);
		/* The directory being read, and glib's wakeup fd made on first use */
		if (target && !g_str_has_prefix(target, "/proc/") && !g_str_has_prefix(target, "anon_inode:"))
		{
			g_string_append_printf(out, "%s -> %s\n", name, target);
		}
		g_free(target);
		g_free(link);
	}
	if (d)
	{
		g_dir_close(d);
	}
	return g_string_free(out, FALSE);
}

static void check_no_job_dirs(void)
{
	GDir *d = g_dir_open(OFFICE_CACHE_DIR, 0, NULL);
	const char *name;

	while (d && (name = g_dir_read_name(d)) != NULL)
	{
		CHECK(!g_str_has_prefix(name, ".job-"), "job directory %s left behind", name);
	}
	if (d)
	{
		g_dir_close(d);
	}
}

int main(int argc, char **argv)
{
	void (*check)(void) = NULL;
	int status, want_status = 0;
	char *before, *after;

	mode = argc > 1 ? argv[1] : "convert";
	replies = g_ptr_array_new_with_free_func(g_free);
	set_mode("ok");
	g_unlink(FAKELOK_DIR "/loads");

	if (strcmp(mode, "convert") == 0)
	{
		shim_set_start(start_convert);
		check = check_convert;
	}
	else if (strcmp(mode, "coalesce") == 0)
	{
		shim_set_start(start_coalesce);
		check = check_coalesce;
	}
	else if (strcmp(mode, "errors") == 0)
	{
		shim_set_start(start_errors);
		check = check_errors;
	}
	else if (strcmp(mode, "kind") == 0)
	{
		shim_set_start(start_kind);
		check = check_kind;
	}
	else if (g_str_has_prefix(mode, "fail-"))
	{
		static const char *const table[][2] = {
			{ "loadfail", "loadFailed" }, { "savefail", "saveFailed" }, { "savelie", "saveFailed" },
			{ "crash", "crashed" }, { "hang", "timeout" }, { "initfail", "internalError" },
		};
		fail_mode = mode + 5;
		for (size_t i = 0; i < G_N_ELEMENTS(table); i++)
		{
			if (strcmp(table[i][0], fail_mode) == 0)
			{
				fail_code = table[i][1];
			}
		}
		g_assert(fail_code);
		shim_set_start(start_fail);
		check = check_fail;
	}
	else if (strcmp(mode, "busy") == 0)
	{
		shim_set_start(start_busy);
		check = check_busy;
	}
	else if (strcmp(mode, "changed") == 0)
	{
		shim_set_start(start_changed);
		check = check_changed;
	}
	else if (strcmp(mode, "sigterm") == 0)
	{
		shim_set_start(start_sigterm);
		check = check_sigterm;
	}
	else if (strcmp(mode, "disconnect") == 0)
	{
		shim_set_start(start_disconnect);
		want_status = 1;
	}
	else if (strcmp(mode, "sweep") == 0)
	{
		/* A job directory a killed officed left: gone once it starts */
		g_assert(g_mkdir_with_parents(OFFICE_CACHE_DIR "/.job-stale/sub", 0700) == 0);
		g_assert(g_file_set_contents(OFFICE_CACHE_DIR "/.job-stale/sub/f", "x", -1, NULL));
	}
	else
	{
		g_printerr("unknown mode %s\n", mode);
		return 2;
	}

	before = open_files();
	status = officed_main(0, NULL);
	after = open_files();

	CHECK(status == want_status, "%s: exit status %d, want %d", mode, status, want_status);
	CHECK(strcmp(before, after) == 0, "%s: file descriptors left open:\n%s\nwas:\n%s", mode, after, before);
	CHECK(answered == calls, "%s: %d of %d calls answered", mode, answered, calls);
	CHECK(shim_bad_replies() == 0, "%s: bad replies", mode);
	if (check)
	{
		check();
	}
	check_no_job_dirs();

	g_ptr_array_free(replies, TRUE);
	CHECK(shim_live_messages() == 0, "%s: %d messages leaked", mode, shim_live_messages());
	CHECK(shim_live_values() == 0, "%s: %d JSON values leaked", mode, shim_live_values());
	g_free(before);
	g_free(after);

	if (failures)
	{
		g_printerr("%s: %d check(s) failed\n", mode, failures);
		return 1;
	}

	g_print("%s: ok\n", mode);
	return 0;
}
