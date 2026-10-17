/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for src/convert.c, in a work directory of their own:
 *
 *   unit <work-dir>
 *
 * Any failed check exits non-zero.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/convert.h"

static int failures = 0;

#define CHECK(cond, ...) \
	do \
	{ \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
			failures++; \
		} \
	} while (0)

static char *work, *root;

static char *at(const char *name)
{
	return g_build_filename(work, name, NULL);
}

static void put(const char *path, const char *text)
{
	g_assert(g_file_set_contents(path, text, -1, NULL));
}

static void test_formats(void)
{
	static const char *const names[] = { "odt", "docx", "ods", "xlsx", "odp", "pptx", "pdf" };

	for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
	{
		const OfficeFormat *f = office_format_find(names[i]);
		CHECK(f && strcmp(f->name, names[i]) == 0, "format %s not found", names[i]);
	}
	CHECK(office_format_find("ODT") == NULL, "formats are lower case");
	CHECK(office_format_find("doc") == NULL, "doc is read, not written");
	CHECK(office_format_find("") == NULL && office_format_find(NULL) == NULL, "empty format");
	CHECK(office_format_find("odt ") == NULL, "trailing space");

	CHECK(office_format_find("odt")->doctypes == OFFICE_TEXT, "odt is for text");
	CHECK(office_format_find("xlsx")->doctypes == OFFICE_SPREADSHEET, "xlsx is for sheets");
	CHECK(office_format_find("pptx")->doctypes == OFFICE_PRESENTATION, "pptx is for slides");
	CHECK(office_format_find("pdf")->doctypes == OFFICE_ANY, "pdf is for anything");

	CHECK(office_doctype_bit(0) == OFFICE_TEXT && office_doctype_bit(1) == OFFICE_SPREADSHEET &&
	      office_doctype_bit(2) == OFFICE_PRESENTATION && office_doctype_bit(3) == OFFICE_DRAWING,
	      "LOK_DOCTYPE_* to bits");
	CHECK(office_doctype_bit(4) == 0 && office_doctype_bit(-1) == 0 && office_doctype_bit(99) == 0,
	      "other kinds have no bit");
	CHECK(strcmp(office_doctype_name(1), "spreadsheet") == 0, "kind name");
	CHECK(strcmp(office_doctype_name(-3), "document of another kind") == 0, "unknown kind name");
}

static bool check(const char *source, OfficeSource *src, const char **error, off_t max)
{
	*error = NULL;
	return office_source_check(root, source, max, src, error);
}

static void expect_refused(const char *source, const char *why, off_t max)
{
	OfficeSource src;
	const char *error;
	bool ok = check(source, &src, &error, max);

	CHECK(!ok, "%s accepted, want \"%s\"", source ? source : "(null)", why);
	CHECK(!ok && error && strcmp(error, why) == 0, "%s: \"%s\", want \"%s\"",
	      source ? source : "(null)", error ? error : "(none)", why);
	CHECK(src.path == NULL, "nothing to free after a refusal");
	office_source_clear(&src);
}

static void test_source_check(void)
{
	OfficeSource src;
	const char *error;
	char *doc = g_build_filename(root, "Letter.doc", NULL);
	char *sub = g_build_filename(root, "sub", NULL);
	char *inner_link = g_build_filename(root, "sub", "link.doc", NULL);
	char *outer_link = g_build_filename(root, "escape.doc", NULL);
	char *outside = at("outside.doc");
	char *sibling_dir = g_strconcat(root, "foo", NULL);
	char *sibling = g_build_filename(sibling_dir, "a.doc", NULL);
	char *dotdot = g_strconcat(root, "/../outside.doc", NULL);
	char *trailing = g_strconcat(root, "/", NULL);

	put(doc, "0123456789");
	g_mkdir(sub, 0755);
	put(outside, "secret");
	g_mkdir(sibling_dir, 0755);
	put(sibling, "x");
	g_assert(symlink(doc, inner_link) == 0);
	g_assert(symlink(outside, outer_link) == 0);

	CHECK(check(doc, &src, &error, 100), "plain document refused: %s", error);
	CHECK(src.path && strcmp(src.path, doc) == 0, "resolved %s, want %s", src.path, doc);
	CHECK(src.size == 10, "size %jd", (intmax_t)src.size);
	office_source_clear(&src);

	/* A link inside the storage to a file inside it is followed */
	CHECK(check(inner_link, &src, &error, 100), "link inside refused: %s", error);
	CHECK(src.path && strcmp(src.path, doc) == 0, "link resolved to %s", src.path);
	office_source_clear(&src);

	expect_refused(NULL, "source must be an absolute path", 100);
	expect_refused("Letter.doc", "source must be an absolute path", 100);
	expect_refused("", "source must be an absolute path", 100);
	expect_refused(outer_link, "source is outside the document storage", 100);
	expect_refused(outside, "source is outside the document storage", 100);
	expect_refused(dotdot, "source is outside the document storage", 100);
	/* /x/root and /x/rootfoo share a prefix but not a directory */
	expect_refused(sibling, "source is outside the document storage", 100);
	expect_refused(root, "source is outside the document storage", 100);
	expect_refused(trailing, "source is outside the document storage", 100);
	expect_refused(sub, "source is not a file", 100);
	expect_refused("/proc/self/status", "source is outside the document storage", 100);
	{
		char *missing = g_build_filename(root, "missing.doc", NULL);
		expect_refused(missing, "source does not exist", 100);
		g_free(missing);
	}
	expect_refused(doc, "source is too large", 9);
	CHECK(check(doc, &src, &error, 10), "exactly the limit refused");
	office_source_clear(&src);
	{
		char *long_path = g_strnfill(5000, 'a');
		long_path[0] = '/';
		expect_refused(long_path, "source path is too long", 100);
		g_free(long_path);
	}
	{
		OfficeSource s2;
		const char *e2 = NULL;
		CHECK(!office_source_check("/nonexistent-root-for-test", doc, 100, &s2, &e2) &&
		      e2 && strcmp(e2, "the document storage is not available") == 0, "missing root");
	}

	g_free(doc);
	g_free(sub);
	g_free(inner_link);
	g_free(outer_link);
	g_free(outside);
	g_free(sibling_dir);
	g_free(sibling);
	g_free(dotdot);
	g_free(trailing);
}

static void test_cache_name(void)
{
	const OfficeFormat *odt = office_format_find("odt"), *pdf = office_format_find("pdf");
	OfficeSource a = { .path = "/m/a.doc", .dev = 1, .ino = 2, .size = 3, .mtime_ns = 4 };
	OfficeSource b = a;
	char *base = office_cache_name(&a, odt), *other;

	CHECK(strlen(base) == 32 + 4 && g_str_has_suffix(base, ".odt"), "name %s", base);
	for (int i = 0; i < 32; i++)
	{
		CHECK(g_ascii_isxdigit(base[i]) && !g_ascii_isupper(base[i]), "name %s", base);
	}

	other = office_cache_name(&b, odt);
	CHECK(strcmp(base, other) == 0, "same source, same name");
	g_free(other);

#define DIFFERS(field, value) \
	do \
	{ \
		b = a; \
		b.field = value; \
		other = office_cache_name(&b, odt); \
		CHECK(strcmp(base, other) != 0, "a different " #field " keeps the name"); \
		g_free(other); \
	} while (0)
	DIFFERS(path, "/m/b.doc");
	DIFFERS(dev, 9);
	DIFFERS(ino, 9);
	DIFFERS(size, 9);
	DIFFERS(mtime_ns, 9);
#undef DIFFERS

	other = office_cache_name(&a, pdf);
	CHECK(g_str_has_suffix(other, ".pdf") && strncmp(base, other, 32) != 0, "format changes the hash");
	g_free(other);
	g_free(base);
}

static void test_extension(void)
{
	static const struct
	{
		const char *path, *ext;
	} cases[] = {
		{ "/m/Letter.DOC", "doc" },
		{ "/m/a.b.Rtf", "rtf" },
		{ "/m/noext", "" },
		{ "/m/.hidden", "" },
		{ "/m/trailing.", "" },
		{ "/m/a.ninechars", "" },
		{ "/m/a.eightchr", "eightchr" },
		{ "/m/a.b-c", "" },
		{ "/m/a.d\xc3\xa9", "" },
		{ "/m/dir.d/file", "" },
		{ "plain.ppt", "ppt" },
	};

	for (size_t i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		char *ext = office_source_extension(cases[i].path);
		CHECK(strcmp(ext, cases[i].ext) == 0, "%s: \"%s\", want \"%s\"", cases[i].path, ext, cases[i].ext);
		g_free(ext);
	}
}

static char *job_dir(const char *name)
{
	char *dir = at(name);
	g_assert(g_mkdir(dir, 0700) == 0);
	return dir;
}

static void test_stage(void)
{
	OfficeSource src;
	const char *error = NULL;
	char *doc = g_build_filename(root, "Stage Me.DOC", NULL);
	char *other = g_build_filename(root, "other.doc", NULL);
	char *dir = job_dir("job1"), *copy, *data = NULL;
	gsize len;

	put(doc, "the document");
	g_assert(check(doc, &src, &error, 1000));

	copy = office_stage_source(&src, 1000, dir, &error);
	CHECK(copy != NULL, "staging failed: %s", error);
	if (copy)
	{
		char *want = g_build_filename(dir, "source.doc", NULL);
		struct stat st;
		CHECK(strcmp(copy, want) == 0, "copy %s, want %s", copy, want);
		CHECK(g_file_get_contents(copy, &data, &len, NULL) && len == 12 && memcmp(data, "the document", 12) == 0,
		      "copy content");
		CHECK(stat(copy, &st) == 0 && (st.st_mode & 0777) == 0600, "copy mode %o", st.st_mode & 0777);
		g_free(data);
		g_free(want);

		/* A second staging into the same directory does not overwrite the first */
		error = NULL;
		CHECK(office_stage_source(&src, 1000, dir, &error) == NULL &&
		      error && strcmp(error, "cannot write the working copy") == 0, "second copy");
	}
	g_free(copy);

	/* Swapped for another file after the check: refused, nothing left behind */
	{
		char *dir2 = job_dir("job2"), *left;
		put(other, "another");
		g_assert(g_rename(other, doc) == 0);
		error = NULL;
		CHECK(office_stage_source(&src, 1000, dir2, &error) == NULL &&
		      error && strcmp(error, "source changed while it was being opened") == 0,
		      "swapped file: %s", error ? error : "(accepted)");
		left = g_build_filename(dir2, "source.doc", NULL);
		CHECK(!g_file_test(left, G_FILE_TEST_EXISTS), "no copy after a refusal");
		g_free(left);
		g_free(dir2);
	}

	/* Swapped for a link to the very same file: refused by O_NOFOLLOW */
	{
		char *dir3 = job_dir("job3"), *real = g_build_filename(root, "real.doc", NULL);
		OfficeSource s3;
		put(real, "real");
		g_assert(check(real, &s3, &error, 1000));
		g_assert(g_rename(real, other) == 0);
		g_assert(symlink(other, real) == 0);
		error = NULL;
		CHECK(office_stage_source(&s3, 1000, dir3, &error) == NULL &&
		      error && strcmp(error, "source cannot be read") == 0, "link swapped in: %s", error ? error : "(accepted)");
		office_source_clear(&s3);
		g_free(real);
		g_free(dir3);
	}

	/* Grown past the limit after the check */
	{
		char *dir4 = job_dir("job4"), *big = g_build_filename(root, "grows.doc", NULL), *left;
		OfficeSource s4;
		put(big, "small");
		g_assert(check(big, &s4, &error, 10));
		put(big, "small, then grown past ten bytes");
		/* g_file_set_contents replaces the file: keep the identity it had */
		{
			struct stat st;
			g_assert(stat(big, &st) == 0);
			s4.ino = st.st_ino;
		}
		error = NULL;
		CHECK(office_stage_source(&s4, 10, dir4, &error) == NULL &&
		      error && strcmp(error, "source is too large") == 0, "grown file: %s", error ? error : "(accepted)");
		left = g_build_filename(dir4, "source.doc", NULL);
		CHECK(!g_file_test(left, G_FILE_TEST_EXISTS), "no partial copy left");
		g_free(left);
		office_source_clear(&s4);
		g_free(big);
		g_free(dir4);
	}

	office_source_clear(&src);
	g_free(doc);
	g_free(other);
	g_free(dir);
}

static void test_publish(void)
{
	char *staged = at("staged.odt"), *target = at("target.odt"), *data = NULL;

	put(staged, "first");
	CHECK(office_publish(staged, target) == 0, "publish");
	put(staged, "second");
	CHECK(office_publish(staged, target) == EEXIST, "second publish must not replace");
	CHECK(g_file_get_contents(target, &data, NULL, NULL) && strcmp(data, "first") == 0,
	      "target replaced: %s", data);
	g_free(data);
	g_free(staged);
	g_free(target);
}

static void test_remove_tree(void)
{
	char *dir = at("tree"), *deep = g_build_filename(dir, "a", "b", NULL);
	char *keep_dir = at("keep"), *keep = g_build_filename(keep_dir, "precious", NULL);
	char *link = g_build_filename(dir, "a", "link", NULL);
	char *flink = g_build_filename(dir, "flink", NULL);

	g_assert(g_mkdir_with_parents(deep, 0700) == 0);
	g_assert(g_mkdir(keep_dir, 0700) == 0);
	put(keep, "do not delete");
	{
		char *f = g_build_filename(deep, "file", NULL);
		put(f, "x");
		g_free(f);
	}
	g_assert(symlink(keep_dir, link) == 0);
	g_assert(symlink(keep, flink) == 0);

	office_remove_tree(dir);
	CHECK(!g_file_test(dir, G_FILE_TEST_EXISTS), "tree left behind");
	CHECK(g_file_test(keep, G_FILE_TEST_IS_REGULAR), "followed a link out of the tree");

	g_free(dir);
	g_free(deep);
	g_free(keep_dir);
	g_free(keep);
	g_free(link);
	g_free(flink);
}

static void put_aged(const char *dir, const char *name, size_t size, time_t age)
{
	char *path = g_build_filename(dir, name, NULL);
	char *data = g_strnfill(size, 'x');
	struct timespec times[2];

	put(path, data);
	times[0].tv_sec = times[1].tv_sec = time(NULL) - age;
	times[0].tv_nsec = times[1].tv_nsec = 0;
	g_assert(utimensat(AT_FDCWD, path, times, 0) == 0);
	g_free(data);
	g_free(path);
}

static bool exists_in(const char *dir, const char *name)
{
	char *path = g_build_filename(dir, name, NULL);
	bool there = g_file_test(path, G_FILE_TEST_EXISTS);
	g_free(path);
	return there;
}

static void test_trim(void)
{
	char *dir = at("cache"), *hidden;
	uint64_t left;

	g_assert(g_mkdir(dir, 0755) == 0);
	put_aged(dir, "oldest.odt", 100, 400);
	put_aged(dir, "old.odt", 100, 300);
	put_aged(dir, "new.odt", 100, 200);
	put_aged(dir, "newest.odt", 100, 100);
	put_aged(dir, ".profile-file", 1000, 999);
	hidden = g_build_filename(dir, ".job-x", NULL);
	g_assert(g_mkdir(hidden, 0700) == 0);

	left = office_cache_trim(dir, 1000, NULL);
	CHECK(left == 400, "under the limit: %ju left", (uintmax_t)left);
	CHECK(exists_in(dir, "oldest.odt"), "trimmed under the limit");

	/* "oldest" is the one just made: kept, the next oldest go */
	left = office_cache_trim(dir, 200, "oldest.odt");
	CHECK(left == 200, "%ju left, want 200", (uintmax_t)left);
	CHECK(exists_in(dir, "oldest.odt") && !exists_in(dir, "old.odt") && !exists_in(dir, "new.odt") &&
	      exists_in(dir, "newest.odt"), "wrong files trimmed");
	CHECK(exists_in(dir, ".profile-file") && exists_in(dir, ".job-x"), "hidden entries trimmed");

	left = office_cache_trim(dir, 0, NULL);
	CHECK(left == 0 && !exists_in(dir, "oldest.odt") && !exists_in(dir, "newest.odt"), "trim to nothing");
	CHECK(office_cache_trim("/nonexistent-cache-for-test", 0, NULL) == 0, "missing cache");

	g_free(hidden);
	g_free(dir);
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: unit <empty work dir>\n");
		return 2;
	}
	work = realpath(argv[1], NULL);
	g_assert(work);
	root = at("root");
	g_assert(g_mkdir(root, 0755) == 0);

	test_formats();
	test_source_check();
	test_cache_name();
	test_extension();
	test_stage();
	test_publish();
	test_remove_tree();
	test_trim();

	g_free(root);
	free(work);
	if (failures)
	{
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("unit: all checks passed\n");
	return 0;
}
