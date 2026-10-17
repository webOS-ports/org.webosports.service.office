// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
//
// Every descriptor officed opens must be O_CLOEXEC: office-convert is
// spawned from the same process, and an inherited descriptor (a cache
// directory, a source document) would stay open in LibreOffice for the
// whole conversion.

@o@
expression path, flags;
position p;
@@
open@p(path, flags, ...)

@script:python@
p << o.p;
flags << o.flags;
@@
if "O_CLOEXEC" not in flags:
    print("%s:%s: FINDING open() without O_CLOEXEC" % (p[0].file, p[0].line))

@oa@
expression dir, path, flags;
position p;
@@
openat@p(dir, path, flags, ...)

@script:python@
p << oa.p;
flags << oa.flags;
@@
if "O_CLOEXEC" not in flags:
    print("%s:%s: FINDING openat() without O_CLOEXEC" % (p[0].file, p[0].line))

@f@
expression path, m;
position p;
@@
fopen@p(path, m)

@script:python@
p << f.p;
m << f.m;
@@
if "e" not in m.strip('"').replace("\\", ""):
    print("%s:%s: FINDING fopen() without \"e\"" % (p[0].file, p[0].line))
