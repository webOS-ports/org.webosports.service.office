// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

/* office-convert's exit status, which officed turns into an errorCode */
#pragma once

#define OFFICE_EXIT_OK     0
#define OFFICE_EXIT_USAGE  2  /* bad arguments: officed's bug */
#define OFFICE_EXIT_SOURCE 3  /* the source could not be copied */
#define OFFICE_EXIT_INIT   4  /* LibreOfficeKit did not start */
#define OFFICE_EXIT_LOAD   5  /* LibreOffice could not open the document */
#define OFFICE_EXIT_KIND   6  /* the format does not fit the document */
#define OFFICE_EXIT_SAVE   7  /* LibreOffice could not save it */
