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
 * officed - org.webosports.service.office
 *
 * Converts documents with LibreOfficeKit, so that the Documents app can open
 * the formats it does not read itself (.doc, .ppt, .rtf, ...) as OpenDocument,
 * and keeps documents open for the app to edit through the same engine.
 *
 *   convert {"source": "/media/internal/...", "format": "odt"}
 *     -> {"returnValue": true, "target": "<cache>/<hash>.odt", "cached": false}
 *
 *   open {"source"} -> {session, kind, parts, part, width, height (twips), tileMode}
 *   events {"session", "subscribe": true} -> {event, type, payload} as LibreOffice
 *       calls back (tiles to paint again, the cursor, the state of Bold...),
 *       and {event: "closed", reason} at the end
 *   paintTile {"session", part, x, y, width, height (twips), pixelWidth, pixelHeight}
 *       -> {file}: pixelWidth * pixelHeight * 4 bytes, in tileMode's order
 *   postKey {"session", type: "input"|"up", charCode, keyCode}
 *   postMouse {"session", type: "down"|"up"|"move", x, y (twips), count, buttons, modifiers}
 *   uno {"session", command, arguments}   formatting and editing commands only
 *   extTextInput {"session", type: "preedit"|"commit", text}
 *   setPart {"session", part}, info {"session"} -> {parts, part, width, height}
 *   save {"session", format} -> {target}: a copy in the cache
 *   close {"session"}
 *
 * See "Editing sessions" below.
 *
 * The result goes into a cache directory, never next to the original, under a
 * name made from the source's path, identity, size and modification time:
 * a changed source gets a new name, so a file in the cache is never replaced
 * (results are published with link(2), which cannot overwrite) and a second
 * request for the same document is answered from the cache.
 *
 * Each conversion runs in its own office-convert process, one at a time: a
 * document that crashes or hangs LibreOffice takes only that process along
 * (it is killed after OFFICE_TIMEOUT_S), and LibreOffice's memory goes back
 * once the job is done. A dynamic service: the hub starts it on the first
 * call, and it exits once it has been idle for OFFICE_IDLE_S.
 */

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "convert.h"
#include "exitcodes.h"
#include "protocol.h"

#define OFFICE_SERVICE "org.webosports.service.office"

/* Overridable so the tests can use a directory and a helper of their own */
#ifndef OFFICE_ROOT
#define OFFICE_ROOT "/media/internal"
#endif
#ifndef OFFICE_CACHE_DIR
#define OFFICE_CACHE_DIR "/media/internal/.cache/" OFFICE_SERVICE
#endif
#ifndef OFFICE_HELPER
#define OFFICE_HELPER "/usr/libexec/office-convert"
#endif
#ifndef OFFICE_TIMEOUT_S
#define OFFICE_TIMEOUT_S 180
#endif
#ifndef OFFICE_IDLE_S
#define OFFICE_IDLE_S 60
#endif
#ifndef OFFICE_MAX_SOURCE
#define OFFICE_MAX_SOURCE ((off_t)100 * 1024 * 1024)
#endif
#ifndef OFFICE_SESSION_HELPER
#define OFFICE_SESSION_HELPER "/usr/libexec/office-session"
#endif
/* Documents open for editing at once: each is a LibreOffice of a few hundred MB */
#ifndef OFFICE_MAX_SESSIONS
#define OFFICE_MAX_SESSIONS 2
#endif
/* A session nobody asked anything of for this long is closed */
#ifndef OFFICE_SESSION_IDLE_S
#define OFFICE_SESSION_IDLE_S 600
#endif
/* An answer to a session's request is due within this */
#ifndef OFFICE_SESSION_REPLY_S
#define OFFICE_SESSION_REPLY_S 30
#endif

/* The cache is trimmed to this after every conversion, oldest results first */
#define OFFICE_CACHE_MAX ((uint64_t)256 * 1024 * 1024)
/* Requests waiting behind the one being converted; more get "busy" */
#define MAX_QUEUED 8

typedef struct
{
	GPtrArray *messages;        /* everyone who asked for this result */
	OfficeSource src;
	const OfficeFormat *format;
	char *name;                 /* in the cache */
	char *target;
	char *job_dir;
	GPid pid;
	guint timeout_id;
	bool timed_out;
} Job;

static LSHandle *service_handle = NULL;
static GMainLoop *main_loop = NULL;
static GQueue pending = G_QUEUE_INIT;
static Job *running = NULL;
static guint idle_id = 0;
static bool stopping = false;
static int exit_status = 0;

static void reply_json(LSMessage *message, jvalue_ref reply)
{
	LSError lserror;

	LSErrorInit(&lserror);

	if (!LSMessageReply(service_handle, message, jvalue_tostring_simple(reply), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}
}

static void reply_error(LSMessage *message, const char *code, const char *text)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
	jobject_put(reply, J_CSTR_TO_JVAL("errorCode"), jstring_create(code));
	jobject_put(reply, J_CSTR_TO_JVAL("errorText"), jstring_create(text));
	reply_json(message, reply);
	j_release(&reply);
}

static void reply_target(LSMessage *message, const Job *job, bool cached)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("target"), jstring_create(job->target));
	jobject_put(reply, J_CSTR_TO_JVAL("format"), jstring_create(job->format->name));
	jobject_put(reply, J_CSTR_TO_JVAL("cached"), jboolean_create(cached));
	reply_json(message, reply);
	j_release(&reply);
}

static void message_unref(gpointer message)
{
	LSMessageUnref(message);
}

static void job_free(Job *job)
{
	if (job->job_dir)
	{
		office_remove_tree(job->job_dir);
	}

	g_ptr_array_unref(job->messages);
	office_source_clear(&job->src);
	g_free(job->name);
	g_free(job->target);
	g_free(job->job_dir);
	g_free(job);
}

static void job_fail(Job *job, const char *code, const char *text)
{
	for (guint i = 0; i < job->messages->len; i++)
	{
		reply_error(g_ptr_array_index(job->messages, i), code, text);
	}

	job_free(job);
}

static void job_succeed(Job *job, bool cached)
{
	for (guint i = 0; i < job->messages->len; i++)
	{
		reply_target(g_ptr_array_index(job->messages, i), job, cached);
	}

	job_free(job);
}

static gboolean on_idle_exit(gpointer user_data)
{
	idle_id = 0;
	g_message("idle for %d s, exiting", OFFICE_IDLE_S);
	g_main_loop_quit(main_loop);
	return G_SOURCE_REMOVE;
}

static guint session_count(void);

static void idle_arm(void)
{
	if (idle_id == 0 && running == NULL && g_queue_is_empty(&pending) && session_count() == 0 && !stopping)
	{
		idle_id = g_timeout_add_seconds(OFFICE_IDLE_S, on_idle_exit, NULL);
	}
}

static void idle_disarm(void)
{
	if (idle_id != 0)
	{
		g_source_remove(idle_id);
		idle_id = 0;
	}
}

/* Is there already a usable result? Touched so that trimming keeps it longer */
static bool cache_has(const char *target)
{
	struct stat st;

	if (lstat(target, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
	{
		return false;
	}

	(void)utimensat(AT_FDCWD, target, NULL, AT_SYMLINK_NOFOLLOW);
	return true;
}

static bool cache_prepare(void)
{
	/* 0755: the Documents app reads the results; the profile is ours alone */
	if (g_mkdir_with_parents(OFFICE_CACHE_DIR, 0755) != 0 ||
	        g_mkdir_with_parents(OFFICE_CACHE_DIR "/.profile", 0700) != 0)
	{
		g_warning("cannot create %s: %s", OFFICE_CACHE_DIR, g_strerror(errno));
		return false;
	}

	return true;
}

/* Job and session directories a killed officed left behind */
static void cache_sweep(void)
{
	GDir *dir = g_dir_open(OFFICE_CACHE_DIR, 0, NULL);
	const char *name;

	if (!dir)
	{
		return;
	}

	while ((name = g_dir_read_name(dir)) != NULL)
	{
		if (g_str_has_prefix(name, ".job-") || g_str_has_prefix(name, ".sess-"))
		{
			char *path = g_build_filename(OFFICE_CACHE_DIR, name, NULL);
			office_remove_tree(path);
			g_free(path);
		}
	}

	g_dir_close(dir);
}

/* In the child, between fork and exec: only async-signal-safe calls */
static void child_setup(gpointer user_data)
{
	struct rlimit no_core = { 0, 0 };

	/* Gone with officed, whatever kills it */
	(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
	/* A crashing LibreOffice would otherwise dump hundreds of megabytes */
	(void)setrlimit(RLIMIT_CORE, &no_core);
}

static void start_next(void);

static const char *exit_code_name(int code)
{
	switch (code)
	{
		case OFFICE_EXIT_SOURCE:
			return "badSource";
		case OFFICE_EXIT_LOAD:
			return "loadFailed";
		case OFFICE_EXIT_KIND:
			return "unsupportedConversion";
		case OFFICE_EXIT_SAVE:
			return "saveFailed";
		default:
			return "internalError";
	}
}

static const char *exit_code_text(int code, const OfficeFormat *format)
{
	switch (code)
	{
		case OFFICE_EXIT_SOURCE:
			return "the document could not be read";
		case OFFICE_EXIT_INIT:
			return "LibreOffice did not start";
		case OFFICE_EXIT_LOAD:
			return "LibreOffice could not open the document";
		case OFFICE_EXIT_KIND:
			return format->doctypes == OFFICE_TEXT ? "only a text document can be saved in this format" :
			       format->doctypes == OFFICE_SPREADSHEET ? "only a spreadsheet can be saved in this format" :
			       format->doctypes == OFFICE_PRESENTATION ? "only a presentation can be saved in this format" :
			       "the document cannot be saved in this format";
		case OFFICE_EXIT_SAVE:
			return "LibreOffice could not save the document";
		default:
			return "the conversion failed";
	}
}

static void on_child_exit(GPid pid, gint wait_status, gpointer user_data)
{
	Job *job = running;
	char *result;
	int err;

	g_spawn_close_pid(pid);
	running = NULL;

	if (job->timeout_id != 0)
	{
		g_source_remove(job->timeout_id);
		job->timeout_id = 0;
	}

	if (stopping)
	{
		job_fail(job, "internalError", "the service is shutting down");
	}
	else if (job->timed_out)
	{
		g_warning("%s: no result after %d s, killed", job->src.path, OFFICE_TIMEOUT_S);
		job_fail(job, "timeout", "the conversion took too long");
	}
	else if (WIFSIGNALED(wait_status))
	{
		g_warning("%s: office-convert died of signal %d", job->src.path, WTERMSIG(wait_status));
		job_fail(job, "crashed", "LibreOffice crashed on the document");
	}
	else if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != OFFICE_EXIT_OK)
	{
		int code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
		job_fail(job, exit_code_name(code), exit_code_text(code, job->format));
	}
	else
	{
		result = g_strdup_printf("%s/result.%s", job->job_dir, job->format->name);
		err = office_publish(result, job->target);
		g_free(result);

		if (err == 0 || (err == EEXIST && cache_has(job->target)))
		{
			char *keep = g_strdup(job->name);
			job_succeed(job, err == EEXIST);
			office_cache_trim(OFFICE_CACHE_DIR, OFFICE_CACHE_MAX, keep);
			g_free(keep);
		}
		else
		{
			g_warning("cannot publish %s: %s", job->target, g_strerror(err));
			job_fail(job, "internalError", "the result could not be stored");
		}
	}

	start_next();
	idle_arm();
}

static gboolean on_timeout(gpointer user_data)
{
	Job *job = user_data;

	job->timeout_id = 0;
	job->timed_out = true;
	(void)kill(job->pid, SIGKILL);
	return G_SOURCE_REMOVE;
}

static void start_next(void)
{
	while (running == NULL && !stopping && !g_queue_is_empty(&pending))
	{
		Job *job = g_queue_pop_head(&pending);
		char *profile = g_build_filename(OFFICE_CACHE_DIR, ".profile", NULL);
		char *job_dir = g_build_filename(OFFICE_CACHE_DIR, ".job-XXXXXX", NULL);
		char dev[24], ino[24];
		char *home = g_strdup_printf("HOME=%s", profile);
		/*
		 * Only what LibreOffice needs: the headless build has the svp backend
		 * alone, and HOME is where fontconfig and LibreOffice keep their caches.
		 */
		char *envp[] = { "PATH=/usr/bin:/bin", home, "LANG=C.UTF-8", "SAL_USE_VCLPLUGIN=svp", NULL };
		GError *error = NULL;
		bool started = false;

		/* A job answered from the cache in the meantime needs no process */
		if (cache_has(job->target))
		{
			job_succeed(job, true);
			g_free(profile);
			g_free(job_dir);
			g_free(home);
			continue;
		}

		if (cache_prepare() && g_mkdtemp_full(job_dir, 0700) != NULL)
		{
			char *argv[] = { OFFICE_HELPER, job->src.path, dev, ino, job_dir,
			                 (char *)job->format->name, profile, NULL
			               };

			job->job_dir = job_dir;
			job_dir = NULL;
			g_snprintf(dev, sizeof(dev), "%ju", (uintmax_t)job->src.dev);
			g_snprintf(ino, sizeof(ino), "%ju", (uintmax_t)job->src.ino);

			if (g_spawn_async(NULL, argv, envp, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES,
			                  child_setup, NULL, &job->pid, &error))
			{
				running = job;
				g_child_watch_add(job->pid, on_child_exit, NULL);
				job->timeout_id = g_timeout_add_seconds(OFFICE_TIMEOUT_S, on_timeout, job);
				started = true;
			}
			else
			{
				g_warning("cannot start %s: %s", OFFICE_HELPER, error->message);
				g_error_free(error);
			}
		}
		else
		{
			g_warning("cannot create a job directory in %s: %s", OFFICE_CACHE_DIR, g_strerror(errno));
		}

		g_free(profile);
		g_free(job_dir);
		g_free(home);

		if (!started)
		{
			job_fail(job, "internalError", "the conversion could not be started");
		}
	}
}

static Job *find_job(const char *name)
{
	if (running && strcmp(running->name, name) == 0)
	{
		return running;
	}

	for (GList *l = pending.head; l; l = l->next)
	{
		Job *job = l->data;

		if (strcmp(job->name, name) == 0)
		{
			return job;
		}
	}

	return NULL;
}

static bool get_string(jvalue_ref parsed, const char *key, raw_buffer *out)
{
	jvalue_ref value;

	return jobject_get_exists(parsed, j_cstr_to_buffer(key), &value) && jis_string(value) &&
	       (*out = jstring_get_fast(value), out->m_len > 0);
}

static bool cb_convert(LSHandle *sh, LSMessage *message, void *ctx)
{
	JSchemaInfo schema;
	jvalue_ref parsed;
	raw_buffer source_buf, format_buf;
	char *source = NULL, *format_name = NULL;
	const OfficeFormat *format;
	const char *problem = NULL;
	Job *job, *same;

	idle_disarm();

	if (stopping)
	{
		reply_error(message, "internalError", "the service is shutting down");
		return true;
	}

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)), DOMOPT_NOOPT, &schema);

	if (!jis_object(parsed) || !get_string(parsed, "source", &source_buf) ||
	        !get_string(parsed, "format", &format_buf))
	{
		j_release(&parsed);
		reply_error(message, "badRequest", "need \"source\" (a path) and \"format\"");
		idle_arm();
		return true;
	}

	/* Copied out: the buffers are borrowed from parsed, and not NUL-terminated */
	source = g_strndup(source_buf.m_str, source_buf.m_len);
	format_name = g_strndup(format_buf.m_str, format_buf.m_len);
	j_release(&parsed);

	format = office_format_find(format_name);
	job = g_new0(Job, 1);
	job->messages = g_ptr_array_new_with_free_func(message_unref);
	job->format = format;

	/* Flawfinder: ignore - g_strndup() terminated both */
	if (strlen(source) != source_buf.m_len || strlen(format_name) != format_buf.m_len)
	{
		reply_error(message, "badRequest", "the source and format cannot contain NUL");
		job_free(job);
	}
	else if (!format)
	{
		reply_error(message, "badFormat", "format must be one of odt, docx, ods, xlsx, odp, pptx, pdf");
		job_free(job);
	}
	else if (!office_source_check(OFFICE_ROOT, source, OFFICE_MAX_SOURCE, &job->src, &problem))
	{
		reply_error(message, "badSource", problem);
		job_free(job);
	}
	else
	{
		job->name = office_cache_name(&job->src, format);
		job->target = g_build_filename(OFFICE_CACHE_DIR, job->name, NULL);
		LSMessageRef(message);
		g_ptr_array_add(job->messages, message);

		if (cache_has(job->target))
		{
			job_succeed(job, true);
		}
		else if ((same = find_job(job->name)) != NULL)
		{
			/* Asked for twice: one conversion, both answered */
			g_ptr_array_add(same->messages, g_ptr_array_steal_index(job->messages, 0));
			job_free(job);
		}
		else if (g_queue_get_length(&pending) >= MAX_QUEUED)
		{
			job_fail(job, "busy", "too many conversions are waiting");
		}
		else
		{
			g_queue_push_tail(&pending, job);
			start_next();
		}
	}

	g_free(source);
	g_free(format_name);
	idle_arm();
	return true;
}

/* ---------------------------------------------------------- Editing sessions */

/*
 * A session keeps one document open in its own office-session process
 * (office-session.cxx) for the Documents app to edit: it asks for tiles,
 * sends taps, keys and commands, and gets LibreOffice's callbacks (which
 * tiles to paint again, where the cursor is, whether Bold is on) as events.
 * Requests go to the process as lines on its stdin and are answered from the
 * lines on its stdout (protocol.h); each request keeps its message until then.
 */

typedef struct Session Session;

typedef struct
{
	LSMessage *message;
	char *cmd;
} Request;

struct Session
{
	char *id;
	GPid pid;
	int in_fd;              /* the process's stdin; non-blocking */
	GIOChannel *out;        /* its stdout */
	guint out_watch;
	char *dir;              /* the copy, tiles and saved copies; removed at the end */
	char *sub_key;          /* "events/<id>" */
	LSMessage *open_message;   /* until the document is loaded */
	GHashTable *requests;   /* request id (text) -> Request */
	guint next_request;
	guint watchdog_id;      /* an answer is due */
	guint idle_id;          /* closed after OFFICE_SESSION_IDLE_S without requests */
	unsigned save_seq;
	int doctype;
	char *tile_mode;
	bool timed_out;
	bool closing;
	const char *close_reason;
};

static GHashTable *sessions = NULL;   /* id -> Session */

static guint session_count(void)
{
	return sessions ? g_hash_table_size(sessions) : 0;
}

static const char *doctype_kind(int doctype)
{
	static const char *const kinds[] = { "text", "spreadsheet", "presentation", "drawing" };
	return doctype >= 0 && doctype <= 3 ? kinds[doctype] : "other";
}

static void request_free(gpointer data)
{
	Request *r = data;

	if (r->message)
	{
		LSMessageUnref(r->message);
	}

	g_free(r->cmd);
	g_free(r);
}

static void session_post(Session *s, jvalue_ref event)
{
	LSError lserror;

	LSErrorInit(&lserror);
	jobject_put(event, J_CSTR_TO_JVAL("session"), jstring_create(s->id));

	if (!LSSubscriptionReply(service_handle, s->sub_key, jvalue_tostring_simple(event), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}
}

static void session_kill(Session *s, const char *reason)
{
	if (!s->closing)
	{
		s->closing = true;
		s->close_reason = reason;
		(void)kill(s->pid, SIGKILL);
	}
}

static gboolean on_session_watchdog(gpointer user_data)
{
	Session *s = user_data;

	s->watchdog_id = 0;
	s->timed_out = true;
	g_warning("session %s: no answer after %d s, killed", s->id, s->open_message ? OFFICE_TIMEOUT_S : OFFICE_SESSION_REPLY_S);
	session_kill(s, "timeout");
	return G_SOURCE_REMOVE;
}

static void session_watchdog_update(Session *s)
{
	bool due = s->open_message != NULL || g_hash_table_size(s->requests) > 0;

	if (s->watchdog_id != 0)
	{
		g_source_remove(s->watchdog_id);
		s->watchdog_id = 0;
	}

	if (due && !s->closing)
	{
		s->watchdog_id = g_timeout_add_seconds(s->open_message ? OFFICE_TIMEOUT_S : OFFICE_SESSION_REPLY_S,
		                                       on_session_watchdog, s);
	}
}

static gboolean on_session_idle(gpointer user_data)
{
	Session *s = user_data;

	s->idle_id = 0;
	g_message("session %s: idle for %d s, closed", s->id, OFFICE_SESSION_IDLE_S);
	session_kill(s, "idle");
	return G_SOURCE_REMOVE;
}

static void session_touch(Session *s)
{
	if (s->idle_id != 0)
	{
		g_source_remove(s->idle_id);
	}

	s->idle_id = s->closing ? 0 : g_timeout_add_seconds(OFFICE_SESSION_IDLE_S, on_session_idle, s);
}

/* Writes one request line; false (and the message answered) when it cannot go */
static bool session_send(Session *s, LSMessage *message, const char *cmd, const char *const *args, int nargs)
{
	const char *fields[12];
	char id[16];
	char *line;
	size_t len, done = 0;
	Request *r;

	g_assert(nargs + 2 <= (int)G_N_ELEMENTS(fields));

	if (s->closing || s->open_message)
	{
		reply_error(message, "badSession", s->closing ? "the session is closing" : "the document is still loading");
		return false;
	}

	g_snprintf(id, sizeof(id), "%u", ++s->next_request);
	fields[0] = id;
	fields[1] = cmd;

	for (int i = 0; i < nargs; i++)
	{
		fields[i + 2] = args[i];
	}

	line = office_proto_join(fields, nargs + 2);
	len = strlen(line); /* Flawfinder: ignore - office_proto_join() terminates it */

	while (done < len)
	{
		ssize_t n = write(s->in_fd, line + done, len - done);

		if (n < 0 && errno == EINTR)
		{
			continue;
		}

		if (n <= 0)
		{
			/* A full pipe: the process is not reading; it will be killed if it never answers */
			int err = n < 0 ? errno : EPIPE;

			g_free(line);

			/* Half a line in the pipe: nothing after it could be read right */
			if (done > 0)
			{
				session_kill(s, "crashed");
			}

			reply_error(message, err == EAGAIN ? "busy" : "badSession",
			            err == EAGAIN ? "the session is not keeping up" : "the session has ended");
			return false;
		}

		done += (size_t)n;
	}

	g_free(line);
	r = g_new0(Request, 1);
	r->message = message;
	r->cmd = g_strdup(cmd);
	LSMessageRef(message);
	g_hash_table_insert(s->requests, g_strdup(id), r);
	session_watchdog_update(s);
	session_touch(s);
	return true;
}

static jvalue_ref ok_reply(void)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	return reply;
}

static void put_int(jvalue_ref obj, const char *key, const char *text)
{
	long value = 0;

	(void)office_proto_int(text, INT32_MIN, INT32_MAX, &value);
	jobject_put(obj, jstring_create(key), jnumber_create_i32((int32_t)value));
}

/* A saved copy into the cache, under a name nothing else has: never replacing */
static char *session_publish(Session *s, const char *saved)
{
	const char *ext = strrchr(saved, '.');
	char *name = g_strdup_printf("%s-%u%s", s->id, ++s->save_seq, ext ? ext : "");
	char *target = g_build_filename(OFFICE_CACHE_DIR, name, NULL);
	int err = office_publish(saved, target);

	if (err != 0)
	{
		g_warning("cannot publish %s: %s", target, g_strerror(err));
		g_free(target);
		target = NULL;
	}
	else
	{
		office_cache_trim(OFFICE_CACHE_DIR, OFFICE_CACHE_MAX, name);
	}

	g_free(name);
	return target;
}

/* One line from the process */
static void session_line(Session *s, char **f, int n)
{
	if (n >= 3 && strcmp(f[0], "E") == 0)
	{
		long type = -1;
		jvalue_ref event = jobject_create();

		(void)office_proto_int(f[1], 0, 1000, &type);
		jobject_put(event, J_CSTR_TO_JVAL("event"), jstring_create(office_callback_name((int)type)));
		jobject_put(event, J_CSTR_TO_JVAL("type"), jnumber_create_i32((int32_t)type));
		jobject_put(event, J_CSTR_TO_JVAL("payload"), jstring_create(f[2]));
		session_post(s, event);
		j_release(&event);
		return;
	}

	if (n < 3 || strcmp(f[0], "R") != 0)
	{
		g_warning("session %s: a line that is not an answer or an event", s->id);
		return;
	}

	if (strcmp(f[1], "0") == 0)
	{
		LSMessage *message = s->open_message;
		long doctype = -1;

		if (!message)
		{
			return;
		}

		s->open_message = NULL;

		if (strcmp(f[2], "ok") == 0 && n == 9 && office_proto_int(f[3], -1, 10, &doctype))
		{
			jvalue_ref reply = ok_reply();

			s->doctype = (int)doctype;
			s->tile_mode = g_strdup(f[8]);
			jobject_put(reply, J_CSTR_TO_JVAL("session"), jstring_create(s->id));
			jobject_put(reply, J_CSTR_TO_JVAL("kind"), jstring_create(doctype_kind(s->doctype)));
			put_int(reply, "parts", f[4]);
			put_int(reply, "part", f[5]);
			put_int(reply, "width", f[6]);
			put_int(reply, "height", f[7]);
			jobject_put(reply, J_CSTR_TO_JVAL("tileMode"), jstring_create(s->tile_mode));
			reply_json(message, reply);
			j_release(&reply);
			session_touch(s);
		}
		else
		{
			reply_error(message, n >= 5 ? f[3] : "loadFailed", n >= 5 ? f[4] : "LibreOffice could not open the document");
			session_kill(s, "loadFailed");
		}

		LSMessageUnref(message);
		session_watchdog_update(s);
		return;
	}

	Request *r = g_hash_table_lookup(s->requests, f[1]);

	if (!r)
	{
		g_warning("session %s: an answer to no request", s->id);
		return;
	}

	if (strcmp(f[2], "ok") != 0)
	{
		reply_error(r->message, n >= 5 ? f[3] : "internalError", n >= 5 ? f[4] : "the request failed");
	}
	else
	{
		jvalue_ref reply = ok_reply();

		if (strcmp(r->cmd, "paint") == 0 && n == 4)
		{
			jobject_put(reply, J_CSTR_TO_JVAL("file"), jstring_create(f[3]));
			jobject_put(reply, J_CSTR_TO_JVAL("tileMode"), jstring_create(s->tile_mode ? s->tile_mode : "rgba"));
			reply_json(r->message, reply);
		}
		else if ((strcmp(r->cmd, "info") == 0 || strcmp(r->cmd, "part") == 0) && n == 7)
		{
			put_int(reply, "parts", f[3]);
			put_int(reply, "part", f[4]);
			put_int(reply, "width", f[5]);
			put_int(reply, "height", f[6]);
			reply_json(r->message, reply);
		}
		else if (strcmp(r->cmd, "save") == 0 && n == 4)
		{
			char *target = session_publish(s, f[3]);

			if (target)
			{
				jobject_put(reply, J_CSTR_TO_JVAL("target"), jstring_create(target));
				reply_json(r->message, reply);
			}
			else
			{
				reply_error(r->message, "internalError", "the saved copy could not be stored");
			}

			g_free(target);
		}
		else
		{
			reply_json(r->message, reply);
		}

		j_release(&reply);
	}

	g_hash_table_remove(s->requests, f[1]);
	session_watchdog_update(s);
}

/* Reads every whole line there is; false at the end of the stream */
static bool session_read(Session *s)
{
	for (;;)
	{
		char *line = NULL;
		gsize len = 0, term = 0;
		GIOStatus status = g_io_channel_read_line(s->out, &line, &len, &term, NULL);

		if (status == G_IO_STATUS_NORMAL && line)
		{
			char **fields;
			int n = 0;

			line[term] = '\0';
			fields = office_proto_split(line, &n);

			if (fields)
			{
				session_line(s, fields, n);
			}

			g_strfreev(fields);
			g_free(line);
			continue;
		}

		g_free(line);
		return status == G_IO_STATUS_AGAIN;
	}
}

static gboolean on_session_out(GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
	Session *s = user_data;

	if (session_read(s))
	{
		return G_SOURCE_CONTINUE;
	}

	/* The end of its output: the child watch finishes the session */
	s->out_watch = 0;
	return G_SOURCE_REMOVE;
}

static void session_free(Session *s)
{
	if (s->watchdog_id)
	{
		g_source_remove(s->watchdog_id);
	}

	if (s->idle_id)
	{
		g_source_remove(s->idle_id);
	}

	if (s->out_watch)
	{
		g_source_remove(s->out_watch);
	}

	if (s->out)
	{
		g_io_channel_unref(s->out);
	}

	if (s->in_fd >= 0)
	{
		close(s->in_fd);
	}

	if (s->dir)
	{
		office_remove_tree(s->dir);
	}

	g_hash_table_unref(s->requests);
	g_free(s->id);
	g_free(s->dir);
	g_free(s->sub_key);
	g_free(s->tile_mode);
	g_free(s);
}

static void on_session_exit(GPid pid, gint wait_status, gpointer user_data)
{
	Session *s = user_data;
	const char *code, *text;
	GHashTableIter iter;
	gpointer value;
	jvalue_ref event;

	g_spawn_close_pid(pid);

	/* What it said before it went is still in the pipe */
	if (s->out_watch)
	{
		g_source_remove(s->out_watch);
		s->out_watch = 0;
		(void)session_read(s);
	}

	if (stopping)
	{
		code = "internalError";
		text = "the service is shutting down";
	}
	else if (s->timed_out)
	{
		code = "timeout";
		text = "LibreOffice did not answer";
	}
	else if (s->closing)
	{
		code = "badSession";
		text = "the session was closed";
	}
	else
	{
		code = "crashed";
		text = "LibreOffice crashed on the document";

		if (WIFSIGNALED(wait_status))
		{
			g_warning("session %s: office-session died of signal %d", s->id, WTERMSIG(wait_status));
		}
	}

	if (s->open_message)
	{
		reply_error(s->open_message, code, text);
		LSMessageUnref(s->open_message);
		s->open_message = NULL;
	}

	g_hash_table_iter_init(&iter, s->requests);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		reply_error(((Request *)value)->message, code, text);
	}

	g_hash_table_remove_all(s->requests);

	event = jobject_create();
	jobject_put(event, J_CSTR_TO_JVAL("event"), jstring_create("closed"));
	jobject_put(event, J_CSTR_TO_JVAL("reason"),
	            jstring_create(stopping ? "shutdown" : s->timed_out ? "timeout" :
	                           s->closing && s->close_reason ? s->close_reason : "crashed"));
	session_post(s, event);
	j_release(&event);

	g_hash_table_remove(sessions, s->id);
	idle_arm();
}

/* "<the directory's six random letters><16 hex digits from getrandom()>" */
static char *session_id_for(const char *dir)
{
	guint8 bytes[8] = { 0 };
	GString *id = g_string_new(strrchr(dir, '-') + 1);
	size_t got = 0;

	while (got < sizeof(bytes))
	{
		ssize_t n = getrandom(bytes + got, sizeof(bytes) - got, 0);

		if (n < 0 && errno == EINTR)
		{
			continue;
		}

		if (n <= 0)
		{
			/* No randomness (a very old kernel): the directory's part alone */
			break;
		}

		got += (size_t)n;
	}

	for (size_t i = 0; i < sizeof(bytes); i++)
	{
		g_string_append_printf(id, "%02x", bytes[i]);
	}

	return g_string_free(id, FALSE);
}

static Session *session_of(LSMessage *message, jvalue_ref parsed)
{
	raw_buffer id;
	Session *s = NULL;

	if (jis_object(parsed) && get_string(parsed, "session", &id))
	{
		char *key = g_strndup(id.m_str, id.m_len);

		s = sessions ? g_hash_table_lookup(sessions, key) : NULL;
		g_free(key);
	}

	if (!s)
	{
		reply_error(message, "badSession", "no such session");
	}

	return s;
}

static jvalue_ref parse_payload(LSMessage *message)
{
	JSchemaInfo schema;

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	return jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)), DOMOPT_NOOPT, &schema);
}

/* A whole number in [min, max] under key, as text for the session's line */
static bool get_int_text(jvalue_ref parsed, const char *key, long min, long max, char *out, size_t size)
{
	jvalue_ref value;
	int32_t n;

	if (!jobject_get_exists(parsed, j_cstr_to_buffer(key), &value) || !jis_number(value) ||
	        jnumber_get_i32(value, &n) != CONV_OK || n < min || n > max)
	{
		return false;
	}

	g_snprintf(out, size, "%d", (int)n);
	return true;
}

/* A string under key, copied out and checked to hold no NUL; g_free() it */
static char *get_text(jvalue_ref parsed, const char *key)
{
	raw_buffer buf;
	char *text;

	if (!get_string(parsed, key, &buf))
	{
		return NULL;
	}

	text = g_strndup(buf.m_str, buf.m_len);

	/* Flawfinder: ignore - g_strndup() terminated it */
	if (strlen(text) != buf.m_len)
	{
		g_free(text);
		return NULL;
	}

	return text;
}

#define TWIPS_MAX 100000000L

static bool cb_open(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed;
	char *source;
	OfficeSource src = { 0 };
	const char *problem = NULL;
	Session *s;
	char *profile, *home, dev[24], ino[24];
	char *envp[5];
	int in_fd = -1, out_fd = -1;
	GError *error = NULL;

	idle_disarm();
	parsed = parse_payload(message);
	source = jis_object(parsed) ? get_text(parsed, "source") : NULL;
	j_release(&parsed);

	if (stopping)
	{
		reply_error(message, "internalError", "the service is shutting down");
	}
	else if (!source)
	{
		reply_error(message, "badRequest", "need \"source\" (a path)");
	}
	else if (!office_source_check(OFFICE_ROOT, source, OFFICE_MAX_SOURCE, &src, &problem))
	{
		reply_error(message, "badSource", problem);
	}
	else if (session_count() >= OFFICE_MAX_SESSIONS)
	{
		reply_error(message, "busy", "too many documents are open for editing");
	}
	else if (!cache_prepare())
	{
		reply_error(message, "internalError", "the cache could not be made");
	}
	else
	{
		s = g_new0(Session, 1);
		s->in_fd = -1;
		s->requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, request_free);
		s->dir = g_build_filename(OFFICE_CACHE_DIR, ".sess-XXXXXX", NULL);

		/* 0755: the Documents app reads the tiles */
		if (g_mkdtemp_full(s->dir, 0755) == NULL)
		{
			g_warning("cannot create a session directory in %s: %s", OFFICE_CACHE_DIR, g_strerror(errno));
			g_free(s->dir);
			s->dir = NULL;
			session_free(s);
			reply_error(message, "internalError", "the session could not be started");
		}
		else
		{
			char *argv[] = { OFFICE_SESSION_HELPER, src.path, dev, ino, s->dir, NULL, NULL };

			/* The directory's random part, and more from the kernel: other callers cannot guess it */
			s->id = session_id_for(s->dir);
			s->sub_key = g_strdup_printf("events/%s", s->id);
			profile = g_build_filename(OFFICE_CACHE_DIR, ".profile", NULL);
			home = g_strdup_printf("HOME=%s", profile);
			envp[0] = "PATH=/usr/bin:/bin";
			envp[1] = home;
			envp[2] = "LANG=C.UTF-8";
			envp[3] = "SAL_USE_VCLPLUGIN=svp";
			envp[4] = NULL;
			argv[5] = profile;
			g_snprintf(dev, sizeof(dev), "%ju", (uintmax_t)src.dev);
			g_snprintf(ino, sizeof(ino), "%ju", (uintmax_t)src.ino);

			if (g_spawn_async_with_pipes(NULL, argv, envp, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES,
			                             child_setup, NULL, &s->pid, &in_fd, &out_fd, NULL, &error))
			{
				s->in_fd = in_fd;
				(void)g_unix_set_fd_nonblocking(in_fd, TRUE, NULL);
				s->out = g_io_channel_unix_new(out_fd);
				g_io_channel_set_close_on_unref(s->out, TRUE);
				(void)g_io_channel_set_encoding(s->out, NULL, NULL);
				(void)g_io_channel_set_flags(s->out, G_IO_FLAG_NONBLOCK, NULL);
				s->out_watch = g_io_add_watch(s->out, G_IO_IN | G_IO_HUP | G_IO_ERR, on_session_out, s);
				s->open_message = message;
				LSMessageRef(message);

				if (!sessions)
				{
					sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)session_free);
				}

				g_hash_table_insert(sessions, s->id, s);
				g_child_watch_add(s->pid, on_session_exit, s);
				session_watchdog_update(s);
			}
			else
			{
				g_warning("cannot start %s: %s", OFFICE_SESSION_HELPER, error->message);
				g_error_free(error);
				session_free(s);
				reply_error(message, "internalError", "the session could not be started");
			}

			g_free(home);
			g_free(profile);
		}
	}

	office_source_clear(&src);
	g_free(source);
	idle_arm();
	return true;
}

/* The requests that only pass numbers on: {session, ...} -> one line */
static bool cb_paint(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char v[7][16];
	const char *args[7];

	if (s && get_int_text(parsed, "part", 0, 10000, v[0], sizeof(v[0])) &&
	        get_int_text(parsed, "x", -TWIPS_MAX, TWIPS_MAX, v[1], sizeof(v[1])) &&
	        get_int_text(parsed, "y", -TWIPS_MAX, TWIPS_MAX, v[2], sizeof(v[2])) &&
	        get_int_text(parsed, "width", 1, TWIPS_MAX, v[3], sizeof(v[3])) &&
	        get_int_text(parsed, "height", 1, TWIPS_MAX, v[4], sizeof(v[4])) &&
	        get_int_text(parsed, "pixelWidth", 1, OFFICE_TILE_MAX_PX, v[5], sizeof(v[5])) &&
	        get_int_text(parsed, "pixelHeight", 1, OFFICE_TILE_MAX_PX, v[6], sizeof(v[6])))
	{
		for (int i = 0; i < 7; i++)
		{
			args[i] = v[i];
		}

		(void)session_send(s, message, "paint", args, 7);
	}
	else if (s)
	{
		reply_error(message, "badRequest", "need part, x, y, width, height (twips), pixelWidth and pixelHeight (1-1024)");
	}

	j_release(&parsed);
	return true;
}

static bool cb_post_key(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char *type = s ? get_text(parsed, "type") : NULL;
	char code[16] = "0", key[16] = "0";
	const char *args[3];
	jvalue_ref present;
	/* Either may be left out (0); a given one must be valid; one of them is needed */
	bool has_code = s && jobject_get_exists(parsed, j_cstr_to_buffer("charCode"), &present);
	bool has_key = s && jobject_get_exists(parsed, j_cstr_to_buffer("keyCode"), &present);

	if (s && type && (strcmp(type, "input") == 0 || strcmp(type, "up") == 0) && (has_code || has_key) &&
	        (!has_code || get_int_text(parsed, "charCode", 0, 0x10FFFF, code, sizeof(code))) &&
	        (!has_key || get_int_text(parsed, "keyCode", 0, 0xFFFF, key, sizeof(key))))
	{
		args[0] = strcmp(type, "input") == 0 ? "0" : "1";
		args[1] = code;
		args[2] = key;
		(void)session_send(s, message, "key", args, 3);
	}
	else if (s)
	{
		reply_error(message, "badRequest", "need type (input or up) and charCode and/or keyCode");
	}

	g_free(type);
	j_release(&parsed);
	return true;
}

static bool cb_post_mouse(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char *type = s ? get_text(parsed, "type") : NULL;
	char v[5][16] = { "0", "0", "1", "1", "0" };
	const char *args[6];
	static const char *const keys[] = { "x", "y", "count", "buttons", "modifiers" };
	static const long lo[] = { -TWIPS_MAX, -TWIPS_MAX, 1, 0, 0 };
	static const long hi[] = { TWIPS_MAX, TWIPS_MAX, 3, 7, 0xFFFF };
	bool ok = s && type && (strcmp(type, "down") == 0 || strcmp(type, "up") == 0 || strcmp(type, "move") == 0);

	for (int i = 0; ok && i < 5; i++)
	{
		jvalue_ref present;

		/* x and y are needed; the others have their defaults */
		ok = get_int_text(parsed, keys[i], lo[i], hi[i], v[i], sizeof(v[i])) ||
		     (i >= 2 && !jobject_get_exists(parsed, j_cstr_to_buffer(keys[i]), &present));
	}

	if (ok)
	{
		args[0] = strcmp(type, "down") == 0 ? "0" : strcmp(type, "up") == 0 ? "1" : "2";

		for (int i = 0; i < 5; i++)
		{
			args[i + 1] = v[i];
		}

		(void)session_send(s, message, "mouse", args, 6);
	}
	else if (s)
	{
		reply_error(message, "badRequest", "need type (down, up or move), x and y (twips)");
	}

	g_free(type);
	j_release(&parsed);
	return true;
}

static bool cb_uno(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char *command = s ? get_text(parsed, "command") : NULL;
	char *arguments = s ? get_text(parsed, "arguments") : NULL;
	const char *args[2];

	if (s && command && office_uno_allowed(command) && (!arguments || strlen(arguments) <= 4096)) /* Flawfinder: ignore */
	{
		args[0] = command;
		args[1] = arguments ? arguments : "";
		(void)session_send(s, message, "uno", args, 2);
	}
	else if (s)
	{
		reply_error(message, "badRequest", "not a command a session may run");
	}

	g_free(command);
	g_free(arguments);
	j_release(&parsed);
	return true;
}

static bool cb_ext_text_input(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char *type = s ? get_text(parsed, "type") : NULL;
	char *text = s ? get_text(parsed, "text") : NULL;
	const char *args[2];

	if (s && type && text && (strcmp(type, "preedit") == 0 || strcmp(type, "commit") == 0) &&
	        g_utf8_validate(text, -1, NULL) && strlen(text) <= 4096) /* Flawfinder: ignore */
	{
		args[0] = strcmp(type, "preedit") == 0 ? "0" : "2";
		args[1] = text;
		(void)session_send(s, message, "text", args, 2);
	}
	else if (s)
	{
		reply_error(message, "badRequest", "need type (preedit or commit) and text");
	}

	g_free(type);
	g_free(text);
	j_release(&parsed);
	return true;
}

static bool cb_set_part(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char part[16];
	const char *args[1] = { part };

	if (s && get_int_text(parsed, "part", 0, 10000, part, sizeof(part)))
	{
		(void)session_send(s, message, "part", args, 1);
	}
	else if (s)
	{
		reply_error(message, "badRequest", "need part");
	}

	j_release(&parsed);
	return true;
}

static bool cb_info(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);

	if (s)
	{
		(void)session_send(s, message, "info", NULL, 0);
	}

	j_release(&parsed);
	return true;
}

static bool cb_save(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	char *format = s ? get_text(parsed, "format") : NULL;
	const char *args[1] = { format };

	if (s && format && office_format_find(format))
	{
		(void)session_send(s, message, "save", args, 1);
	}
	else if (s)
	{
		reply_error(message, "badFormat", "format must be one of odt, docx, ods, xlsx, odp, pptx, pdf");
	}

	g_free(format);
	j_release(&parsed);
	return true;
}

static bool cb_events(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);
	LSError lserror;

	LSErrorInit(&lserror);

	if (s && !LSMessageIsSubscription(message))
	{
		reply_error(message, "badRequest", "subscribe to the events");
	}
	else if (s && !LSSubscriptionAdd(sh, s->sub_key, message, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
		reply_error(message, "internalError", "cannot subscribe");
	}
	else if (s)
	{
		jvalue_ref reply = ok_reply();

		jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(true));
		reply_json(message, reply);
		j_release(&reply);
	}

	j_release(&parsed);
	return true;
}

static bool cb_close(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	Session *s = session_of(message, parsed);

	if (s)
	{
		jvalue_ref reply = ok_reply();

		session_kill(s, "closed");
		reply_json(message, reply);
		j_release(&reply);
	}

	j_release(&parsed);
	return true;
}

/*
 * Whoever had the session's events went away (the app closed, or its page was
 * reloaded) or stopped listening: when that was the last one, nobody can use
 * the session any more, so it is closed now rather than at the idle timeout.
 * The bus calls this before it drops the subscription, so it still counts.
 */
static bool on_subscription_cancel(LSHandle *sh, LSMessage *message, void *ctx)
{
	jvalue_ref parsed = parse_payload(message);
	raw_buffer id;

	if (jis_object(parsed) && get_string(parsed, "session", &id) && sessions)
	{
		char *key = g_strndup(id.m_str, id.m_len);
		Session *s = g_hash_table_lookup(sessions, key);

		if (s && LSSubscriptionGetHandleSubscribersCount(sh, s->sub_key) <= 1)
		{
			session_kill(s, "abandoned");
		}

		g_free(key);
	}

	j_release(&parsed);
	return true;
}

/* At the end: every process killed, every message answered (on_session_exit) */
static void sessions_stop(void)
{
	GHashTableIter iter;
	gpointer value;

	if (!sessions)
	{
		return;
	}

	g_hash_table_iter_init(&iter, sessions);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		(void)kill(((Session *)value)->pid, SIGKILL);
	}

	while (g_hash_table_size(sessions) > 0)
	{
		g_main_context_iteration(NULL, TRUE);
	}

	g_hash_table_unref(sessions);
	sessions = NULL;
}

static LSMethod methods[] =
{
	{ "convert", cb_convert, LUNA_METHOD_FLAGS_NONE },
	{ "open", cb_open, LUNA_METHOD_FLAGS_NONE },
	{ "events", cb_events, LUNA_METHOD_FLAGS_NONE },
	{ "paintTile", cb_paint, LUNA_METHOD_FLAGS_NONE },
	{ "postKey", cb_post_key, LUNA_METHOD_FLAGS_NONE },
	{ "postMouse", cb_post_mouse, LUNA_METHOD_FLAGS_NONE },
	{ "uno", cb_uno, LUNA_METHOD_FLAGS_NONE },
	{ "extTextInput", cb_ext_text_input, LUNA_METHOD_FLAGS_NONE },
	{ "setPart", cb_set_part, LUNA_METHOD_FLAGS_NONE },
	{ "info", cb_info, LUNA_METHOD_FLAGS_NONE },
	{ "save", cb_save, LUNA_METHOD_FLAGS_NONE },
	{ "close", cb_close, LUNA_METHOD_FLAGS_NONE },
	{ NULL, NULL, LUNA_METHOD_FLAGS_NONE },
};

/* The registration died with the hub; the hub starts a fresh officed on the next call */
static void hub_disconnected(LSHandle *sh, void *ctx)
{
	g_warning("lost the luna-service2 hub; exiting");
	exit_status = 1;
	g_main_loop_quit(main_loop);
}

static gboolean on_terminate(gpointer user_data)
{
	g_main_loop_quit(main_loop);
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	LSError lserror;
	Job *job;

	LSErrorInit(&lserror);
	main_loop = g_main_loop_new(NULL, FALSE);
	cache_sweep();

	g_unix_signal_add(SIGTERM, on_terminate, NULL);
	g_unix_signal_add(SIGINT, on_terminate, NULL);

	if (!LSRegister(OFFICE_SERVICE, &service_handle, &lserror) ||
	        !LSRegisterCategory(service_handle, "/", methods, NULL, NULL, &lserror) ||
	        !LSGmainAttach(service_handle, main_loop, &lserror) ||
	        !LSSubscriptionSetCancelFunction(service_handle, on_subscription_cancel, NULL, &lserror) ||
	        !LSSetDisconnectHandler(service_handle, hub_disconnected, NULL, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
		exit_status = 1;
	}
	else
	{
		idle_arm();
		g_main_loop_run(main_loop);
	}

	/*
	 * Everyone still waiting is told; the conversion under way is killed.
	 * The loop has stopped, so the context is taken back first: replies
	 * belong to the thread that owns it.
	 */
	g_main_context_acquire(NULL);
	stopping = true;
	idle_disarm();

	while ((job = g_queue_pop_head(&pending)) != NULL)
	{
		job_fail(job, "internalError", "the service is shutting down");
	}

	if (running)
	{
		(void)kill(running->pid, SIGKILL);

		/* on_child_exit answers it and clears running */
		while (running)
		{
			g_main_context_iteration(NULL, TRUE);
		}
	}

	sessions_stop();

	if (service_handle && !LSUnregister(service_handle, &lserror))
	{
		LSErrorFree(&lserror);
	}

	g_main_context_release(NULL);
	g_main_loop_unref(main_loop);
	return exit_status;
}
