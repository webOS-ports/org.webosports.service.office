#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# static-analysis.sh
#
# Static-analysis gate for officed, office-convert and office-session. Fails on any finding
# from:
#
#   gcc -fanalyzer    use after free, double free, leaks (memory and file
#                     descriptors), NULL dereference, uninitialised reads;
#                     -O0 and -O2, C files (its C++ support is partial)
#   clang analyzer    the same from a second engine, with the alpha checkers
#                     for unix/security/core; C and C++
#   clang-tidy        clang-analyzer-*, bugprone-*, cert-*, concurrency-*,
#                     misc-* and the like; C and C++
#   cppcheck          all checks, exhaustive; C and C++
#   flawfinder        race-prone calls (TOCTOU: access/stat before use), unsafe
#                     string and buffer functions; level 1 and up
#   sparse            type and endianness confusion; C only, it cannot parse C++
#   coccinelle        tests/cocci/*.cocci: descriptors opened without
#                     O_CLOEXEC, an fd leaked into office-convert
#
# OFFICE_CFLAGS: where the headers of glib, luna-service2 and pbnjson are,
# e.g. OFFICE_CFLAGS="-I$SYSROOT/usr/include/glib-2.0
#                     -I$SYSROOT/usr/lib/glib-2.0/include --sysroot=$SYSROOT".
# By default the host's glib and the stand-in bus headers of the harness,
# which have the same declarations. LOK_INCLUDE: the directory that holds
# LibreOfficeKit/. A tool that is not installed is reported and skipped, not
# passed.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../src" && pwd)
LOK_INCLUDE=${LOK_INCLUDE:?set LOK_INCLUDE to the directory holding LibreOfficeKit/}
CFLAGS="${OFFICE_CFLAGS:-$(pkg-config --cflags glib-2.0) -I$HERE/harness/include}"
C_FILES="$SRC/officed.c $SRC/convert.c $SRC/protocol.c"
CXX_FILES="$SRC/office-convert.cxx $SRC/office-session.cxx"
CXXFLAGS="$CFLAGS -I$LOK_INCLUDE -I$SRC -std=c++17"
# Findings are taken from our files only, whatever is included
FILE_RE='(officed[.]c|convert[.][ch]|protocol[.][ch]|office-convert[.]cxx|office-session[.]cxx|exitcodes[.]h)'
fail=0
skipped=""

run() {
	name=$1; shift
	if ! command -v "$1" >/dev/null 2>&1; then
		skipped="$skipped $name"
		echo "== $name: not installed, skipped"
		return
	fi
	echo "== $name"
	out=$("$@" 2>&1)
	status=$?
	findings=$(printf '%s\n' "$out" | grep -E "${FILTER}" || true)
	# Findings always fail; a non-zero exit only does for tools whose exit
	# status means "could not analyse" rather than "found something"
	if [ -n "$findings" ] || { [ $status -ne 0 ] && [ "${IGNORE_STATUS:-0}" = 0 ]; }; then
		printf '%s\n' "${findings:-$out}"
		fail=1
	else
		echo "clean"
	fi
}

for opt in -O0 -O2; do
	for f in $C_FILES; do
		FILTER='warning:|error:' IGNORE_STATUS=0 \
		run "gcc -fanalyzer $opt $(basename "$f")" gcc -c -o /dev/null $opt -fanalyzer -Wanalyzer-too-complex \
			--param=analyzer-bb-explosion-factor=50 --param=analyzer-max-enodes-per-program-point=64 \
			-Wall -Wextra -Wno-unused-parameter $CFLAGS "$f"
	done
done

# alpha.deadcode is left out: it takes service_handle for always-NULL because it
# does not see LSRegister() write it through &service_handle.
CHECKERS="core,unix,deadcode,security,nullability,optin.portability,optin.core"
ALPHA="alpha.unix,alpha.security,alpha.core.PointerArithm,alpha.core.CastSize,alpha.core.Conversion,alpha.unix.cstring"
# optin.core.EnumCastOutOfRange is off: GLib's flag enums are bit sets, and it
# takes every combination of them (G_IO_IN | G_IO_HUP, say) for an invalid value.
NOENUM="-Xanalyzer -analyzer-disable-checker=optin.core.EnumCastOutOfRange"
for f in $C_FILES; do
	FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
	run "clang analyzer $(basename "$f")" clang --analyze -Xanalyzer -analyzer-output=text \
		-Xanalyzer -analyzer-checker=$CHECKERS -Xanalyzer -analyzer-checker=$ALPHA $NOENUM \
		-o /dev/null $CFLAGS "$f"
done
for f in $CXX_FILES; do
	FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
	run "clang analyzer $(basename "$f")" clang++ --analyze -Xanalyzer -analyzer-output=text \
		-Xanalyzer -analyzer-checker=$CHECKERS,cplusplus -Xanalyzer -analyzer-checker=$ALPHA $NOENUM \
		-o /dev/null $CXXFLAGS "$f"
done

# misc-unused-parameters: the callback signatures are glib's and LS2's to set.
# cert-err33-c: the casts to (void) mark the results ignored on purpose.
TIDY='clang-analyzer-*,bugprone-*,cert-*,concurrency-*,misc-*,performance-*,portability-*,readability-misleading-indentation,readability-suspicious-call-argument,-bugprone-easily-swappable-parameters,-misc-include-cleaner,-misc-unused-parameters,-cert-err33-c,-bugprone-assignment-in-if-condition,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp,-clang-analyzer-optin.core.EnumCastOutOfRange'
FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run "clang-tidy C" clang-tidy --quiet $C_FILES --checks="$TIDY" -- -Wno-unused-parameter $CFLAGS
FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run "clang-tidy C++" clang-tidy --quiet $CXX_FILES --checks="$TIDY" -- -Wno-unused-parameter $CXXFLAGS

# Library internals are not ours to lint; their headers are only there so the
# types resolve
CPPCHECK="cppcheck --enable=all --check-level=exhaustive --inconclusive --library=gnu --library=posix
	--force --inline-suppr --quiet --suppress=missingIncludeSystem --suppress=missingInclude
	--suppress=unmatchedSuppression --suppress=checkersReport --suppress=unusedFunction
	--suppress=staticFunction --template={file}:{line}:\ {severity}:\ {message}\ [{id}]"
FILTER=': (error|warning|style|performance|portability):' IGNORE_STATUS=1 \
run "cppcheck C" $CPPCHECK --std=c11 -D__linux__ -D__GNUC__ $C_FILES
FILTER=': (error|warning|style|performance|portability):' IGNORE_STATUS=1 \
run "cppcheck C++" $CPPCHECK --std=c++17 --language=c++ -I"$SRC" -D__linux__ -D__GNUC__ $CXX_FILES

FILTER='^.*:[0-9]+: +\[[1-5]\]' IGNORE_STATUS=1 \
run flawfinder flawfinder --minlevel=1 --falsepositive --columns --dataonly --quiet "$SRC"

# sparse has no --sysroot: hand it the sysroot's include directory directly
SPARSE_FLAGS=$(printf '%s\n' $CFLAGS | sed -n 's|^--sysroot=\(.*\)|-isystem \1/usr/include|p')
for f in $C_FILES; do
	FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
	run "sparse $(basename "$f")" sparse -Wsparse-all -Wno-declaration-after-statement -Wno-non-pointer-null \
		-Wno-decl -Wno-shadow $SPARSE_FLAGS $(printf '%s\n' $CFLAGS | grep -v '^--sysroot') "$f"
done

for cocci in "$HERE"/cocci/*.cocci; do
	for f in $C_FILES; do
		FILTER='FINDING' IGNORE_STATUS=0 \
		run "coccinelle $(basename "$cocci") $(basename "$f")" spatch --very-quiet --sp-file "$cocci" "$f"
	done
	for f in $CXX_FILES; do
		FILTER='FINDING' IGNORE_STATUS=0 \
		run "coccinelle $(basename "$cocci") $(basename "$f")" spatch --very-quiet --c++ --sp-file "$cocci" "$f"
	done
done

[ -n "$skipped" ] && echo "skipped:$skipped"
exit $fail
