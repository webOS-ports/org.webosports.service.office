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
 * and later edit through the same engine.
 *
 *   convert {"source": "/media/internal/...", "format": "odt"}
 *     -> {"returnValue": true, "target": "<cache>/<hash>.odt", "cached": false}
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "convert.h"
#include "exitcodes.h"

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

static void idle_arm(void)
{
	if (idle_id == 0 && running == NULL && g_queue_is_empty(&pending) && !stopping)
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

/* Job directories a killed officed left behind */
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
		if (g_str_has_prefix(name, ".job-"))
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

static LSMethod methods[] =
{
	{ "convert", cb_convert, LUNA_METHOD_FLAGS_NONE },
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

	if (service_handle && !LSUnregister(service_handle, &lserror))
	{
		LSErrorFree(&lserror);
	}

	g_main_context_release(NULL);
	g_main_loop_unref(main_loop);
	return exit_status;
}
