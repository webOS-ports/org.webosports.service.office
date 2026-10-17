org.webosports.service.office
=============================

Summary
-------
LuneOS service that converts office documents with LibreOfficeKit.

Description
-----------
`officed` puts LibreOffice's import and export filters on the bus, so that
the Documents app (org.webosports.app.documents) can open what it does not
read itself - .doc, .ppt, .rtf, WordPerfect, old StarOffice and so on - as
OpenDocument, and make .pdf, .docx, .xlsx and .pptx copies.

It is a dynamic service: the hub starts it on the first call, and it exits
after a minute without work. Each conversion runs in its own `office-convert`
process, one at a time:

- a document that crashes or hangs LibreOffice takes only that process along
  (it is killed after 3 minutes), and the hundreds of megabytes LibreOffice
  maps go back as soon as the job is done;
- the source is copied into a private job directory and LibreOffice opens the
  copy, so it never touches the user's file or leaves a lock file next to it.
  The copy is read through a descriptor opened without following links and
  checked to be the file that was validated, so a file or link swapped in
  between is refused.

Results go into a cache, never next to the original:
`/media/internal/.cache/org.webosports.service.office/<hash>.<format>`, the
hash made from the source's path, device, inode, size and modification time.
A changed source gets a new name, so a file in the cache is never replaced -
results are published with link(2), which cannot overwrite - and asking again
for the same document is answered from the cache. The cache is kept under
256 MB, oldest results first. The Library of the Documents app does not walk
hidden folders, so the cache does not show up there.

Needs LibreOffice built headless (`--disable-gui`) with LibreOfficeKit, in
`/usr/lib/libreoffice` (the `libreoffice` recipe in meta-luneos).

API
---

`luna://org.webosports.service.office/convert`

    luna-send -n 1 luna://org.webosports.service.office/convert \
        '{"source": "/media/internal/Letter.doc", "format": "odt"}'

    {"returnValue": true, "target": "/media/internal/.cache/org.webosports.service.office/5f0c...e1.odt",
     "format": "odt", "cached": false}

`format` is one of `odt`, `docx` (text documents), `ods`, `xlsx`
(spreadsheets), `odp`, `pptx` (presentations) or `pdf` (any). The source must
be a file under `/media/internal`, after links are resolved, of at most
100 MB.

The reply comes once the conversion is done. Up to 8 requests wait behind
the one being converted; the same request twice is converted once and both
are answered.

Errors: `{"returnValue": false, "errorCode": ..., "errorText": ...}`

| errorCode | |
|---|---|
| `badRequest` | no `source` or `format` string |
| `badFormat` | a format not in the list |
| `badSource` | not absolute, outside `/media/internal`, missing, not a file, too large, or could not be read |
| `unsupportedConversion` | the format does not fit the document (a spreadsheet as `odt`) |
| `loadFailed` | LibreOffice could not open it |
| `saveFailed` | LibreOffice could not save it |
| `crashed` | LibreOffice crashed on it |
| `timeout` | no result after 3 minutes |
| `busy` | 8 requests are already waiting |
| `internalError` | LibreOffice did not start, the cache could not be written, or the service is stopping |

The method is in the `office.operation` group; an app asks for it in
appinfo.json's `requiredPermissions`.

Next: editing
-------------

The same engine can edit, as Collabora Office does on Android and iOS: the
document stays loaded in a long-lived LibreOfficeKit process, which renders
tiles and takes input, while the web app draws the tiles and its own
toolbars. Planned as a second process type next to `office-convert`, one per
open document, with the session methods on `officed`:

| Method | Does | LibreOfficeKit |
|---|---|---|
| `open {source}` | loads the document; `{session, kind, parts, width, height}` (twips) | `documentLoad`, `initializeForRendering`, `getDocumentSize`, `getParts` |
| `paintTile {session, part, x, y, w, h, scale}` | renders a 256 px tile into shared memory or a PNG in the session directory; `{file}` | `setPart`, `paintTile` (RGBA with `--enable-cairo-rgba`) |
| `postKey {session, type, char, key}` | typing, from the virtual keyboard | `postKeyEvent` |
| `postMouse {session, type, x, y, count, buttons}` | taps, selection handles | `postMouseEvent` |
| `uno {session, command, args}` | `.uno:Bold`, `.uno:Undo`, `.uno:InsertTable`... | `postUnoCommand` |
| `events {session}` (subscribe) | invalidated rectangles, cursor and selection, state of the toolbar's commands | `registerCallback`: `LOK_CALLBACK_INVALIDATE_TILES`, `..._CURSOR`, `..._STATE_CHANGED`, `..._TEXT_SELECTION` |
| `save {session, format}` | into the cache, as `convert`; the app then offers "Save a Copy" | `saveAs` |
| `close {session}` | ends the process | `destroy` |

Each call goes to the session's process over a pipe; LibreOfficeKit is used
from that process's one thread only. Tiles are cached by the app and
re-requested only for invalidated rectangles.

Checks
------

- `tests/run.sh`: `tests/unit.c` against `src/convert.c` (the path checks,
  the cache names, staging with a swapped file and a swapped link, publishing
  that never replaces, tree removal that does not follow links, trimming),
  and the real `src/officed.c` against stand-ins for luna-service2 and
  pbnjson, spawning the real `office-convert` built against a fake
  LibreOfficeKit (`tests/fakelok.cxx`): conversion and the cache, coalescing,
  every error, a changed source, a full queue, LibreOffice failing to load,
  to save, lying about saving, crashing, hanging and not starting, SIGTERM
  with work queued, a lost hub, job directories left by a killed officed.
  Each under ASan+UBSan (gcc and clang), TSan and valgrind memcheck, checking
  that every call is answered once, from the main context, and that no
  message, JSON value, descriptor or job directory is left behind.
- `tests/static-analysis.sh`: gcc -fanalyzer (-O0 and -O2), the clang static
  analyzer with its alpha unix/security checkers, clang-tidy, cppcheck,
  flawfinder, sparse (C only) and a Coccinelle rule for descriptors opened
  without O_CLOEXEC. Any finding fails.

Both need `LOK_INCLUDE`, the directory holding `LibreOfficeKit/`.

Copyright and License Information
=================================
Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

SPDX-License-Identifier: Apache-2.0
