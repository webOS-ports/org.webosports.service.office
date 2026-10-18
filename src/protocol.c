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

#include "protocol.h"

#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

char *office_proto_join(const char *const *fields, int n)
{
	GString *line = g_string_new(NULL);

	for (int i = 0; i < n; i++)
	{
		char *escaped = g_strescape(fields[i] ? fields[i] : "", NULL);

		if (i > 0)
		{
			g_string_append_c(line, '\t');
		}

		g_string_append(line, escaped);
		g_free(escaped);
	}

	g_string_append_c(line, '\n');
	return g_string_free(line, FALSE);
}

char **office_proto_split(const char *line, int *n)
{
	char **fields;

	*n = 0;

	/* Flawfinder: ignore - the caller's line is NUL-terminated */
	if (!line || !*line || strlen(line) > OFFICE_PROTO_MAX_LINE)
	{
		return NULL;
	}

	fields = g_strsplit(line, "\t", -1);

	for (char **f = fields; *f; f++)
	{
		char *plain = g_strcompress(*f);

		g_free(*f);
		*f = plain;
		(*n)++;
	}

	return fields;
}

bool office_proto_int(const char *text, long min, long max, long *out)
{
	char *end = NULL;
	long value;

	if (!text || !*text)
	{
		return false;
	}

	errno = 0;
	value = strtol(text, &end, 10);

	if (errno != 0 || *end != '\0' || value < min || value > max)
	{
		return false;
	}

	*out = value;
	return true;
}

bool office_uno_allowed(const char *command)
{
	static const char *const allowed[] = {
		/* character */
		".uno:Bold", ".uno:Italic", ".uno:Underline", ".uno:Strikeout",
		".uno:SuperScript", ".uno:SubScript", ".uno:Grow", ".uno:Shrink",
		".uno:FontHeight", ".uno:Color", ".uno:CharBackColor", ".uno:ResetAttributes",
		/* paragraph */
		".uno:LeftPara", ".uno:CenterPara", ".uno:RightPara", ".uno:JustifyPara",
		".uno:DefaultBullet", ".uno:DefaultNumbering", ".uno:IncrementIndent", ".uno:DecrementIndent",
		/* editing */
		".uno:Undo", ".uno:Redo", ".uno:SelectAll", ".uno:Cut", ".uno:Copy", ".uno:Paste",
		".uno:Delete", ".uno:InsertPara",
		/* slides */
		".uno:InsertPage", ".uno:DeletePage", ".uno:DuplicatePage",
		/* text boxes and tables */
		".uno:Text", ".uno:InsertRowsAfter", ".uno:InsertColumnsAfter",
		".uno:DeleteRows", ".uno:DeleteColumns",
	};

	if (!command)
	{
		return false;
	}

	for (size_t i = 0; i < G_N_ELEMENTS(allowed); i++)
	{
		if (strcmp(command, allowed[i]) == 0)
		{
			return true;
		}
	}

	return false;
}

const char *office_callback_name(int type)
{
	/* LibreOfficeKitEnums.h's numbers; office-session checks them at build time */
	switch (type)
	{
		case 0:
			return "invalidate";
		case 1:
			return "cursor";
		case 2:
			return "selection";
		case 3:
			return "selectionStart";
		case 4:
			return "selectionEnd";
		case 5:
			return "cursorVisible";
		case 6:
			return "graphicSelection";
		case 7:
			return "hyperlink";
		case 8:
			return "state";
		case 13:
			return "documentSize";
		case 14:
			return "part";
		case 16:
			return "unoResult";
		case 22:
			return "error";
		default:
			return "other";
	}
}
