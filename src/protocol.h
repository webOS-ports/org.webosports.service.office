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
 * What officed and an office-session process say to each other over the
 * session's pipes: one line per message, fields separated by tabs, every
 * field escaped as g_strescape() does (so a tab or a newline in a field, or
 * in a LibreOffice callback's payload, cannot end it early).
 *
 * officed -> office-session (stdin):
 *   <id> paint <part> <x> <y> <w> <h> <pxW> <pxH>   twips; a tile file back
 *   <id> key <type> <charCode> <keyCode>
 *   <id> mouse <type> <x> <y> <count> <buttons> <modifiers>
 *   <id> uno <command> <arguments>                  only office_uno_allowed()
 *   <id> text <type> <text>                         LOK_EXT_TEXTINPUT (0, preedit) or _END (2, commit)
 *   <id> part <n>
 *   <id> info
 *   <id> save <format>                              a file back
 *   <id> quit
 *
 * office-session -> officed (stdout):
 *   R <id> ok [values...]
 *   R <id> err <errorCode> <errorText>
 *   E <LOK_CALLBACK_* number> <payload>
 *
 * The first line is the answer to the load, with id 0:
 *   R 0 ok <LOK_DOCTYPE_*> <parts> <part> <width> <height> <rgba|bgra>
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest line either side accepts; longer ones are a protocol error */
#define OFFICE_PROTO_MAX_LINE ((size_t)64 * 1024)
/* The largest tile side, in pixels */
#define OFFICE_TILE_MAX_PX 1024

/* fields joined, escaped, with a newline at the end: g_free() it */
char *office_proto_join(const char *const *fields, int n);

/*
 * line (without its newline) split into fields and unescaped: a NULL
 * terminated vector for g_strfreev(), or NULL for an empty or too long line.
 * *n gets the number of fields.
 */
char **office_proto_split(const char *line, int *n);

/* A whole decimal int within [min, max], and nothing else */
bool office_proto_int(const char *text, long min, long max, long *out);

/*
 * The UNO commands a session may run: formatting and editing, nothing that
 * opens, saves, prints or reaches outside the document (".uno:SaveAs" with a
 * URL would write anywhere LibreOffice can).
 */
bool office_uno_allowed(const char *command);

/* A name for a LOK_CALLBACK_* number ("invalidate", "cursor"...), or "other" */
const char *office_callback_name(int type);

#ifdef __cplusplus
}
#endif
