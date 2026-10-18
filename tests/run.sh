#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# run.sh [workdir]
#
# Builds and runs, on the host:
#   unit            src/convert.c on its own: path checks, the cache name,
#                   staging (a file or link swapped in is refused), publishing
#                   that never replaces, the tree removal and the trimming
#   harness         the real src/officed.c against stand-ins for luna-service2
#                   and pbnjson, spawning the real office-convert built against
#                   a fake LibreOfficeKit (tests/fakelok.cxx): conversions, the
#                   cache, coalescing, every error path, a full queue, a
#                   crashing and a hanging LibreOffice, SIGTERM, a lost hub
# each under
#   ASan + UBSan (gcc and clang), TSan (clang), valgrind memcheck.
#
# Needs glib's development files and the LibreOfficeKit headers: LOK_INCLUDE
# is the directory that holds LibreOfficeKit/ (the recipe's libreoffice-lok-dev,
# or include/ in a LibreOffice source tree).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../src" && pwd)
WORK=${1:-${TMPDIR:-/tmp}/officed-tests}
LOK_INCLUDE=${LOK_INCLUDE:?set LOK_INCLUDE to the directory holding LibreOfficeKit/}
GLIB_CFLAGS=$(pkg-config --cflags glib-2.0)
GLIB_LIBS=$(pkg-config --libs glib-2.0)
WARN="-Wall -Wextra -Wno-unused-parameter -Werror"
MODES=${MODES:-"convert coalesce errors kind changed busy sigterm disconnect sweep
       fail-loadfail fail-savefail fail-savelie fail-crash fail-hang fail-initfail
       session session-typing session-clipboard session-errors session-crash session-hang session-idle session-abandon session-loadfail session-sigterm"}
# BUILDS: a subset of "asan-gcc asan-clang tsan plain", for quick runs
BUILDS=${BUILDS:-"asan-gcc asan-clang tsan plain"}
fail=0

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1
export G_SLICE=always-malloc G_DEBUG=gc-friendly
# The symbolizer otherwise reaches for debuginfod and can crash on its cache
export DEBUGINFOD_URLS=

# build <name> <cc> <cxx> [flags...]
build() {
	name=$1; cc=$2; cxx=$3; shift 3
	w="$WORK/$name"
	echo "== build $name"
	rm -rf "$w" && mkdir -p "$w/lok" "$w/root" "$w/unit" || return 1
	# shellcheck disable=SC2086
	$cxx -g -O1 -fno-omit-frame-pointer $WARN "$@" -fPIC -shared -I"$LOK_INCLUDE" $GLIB_CFLAGS \
		"$HERE/fakelok.cxx" -o "$w/lok/libmergedlo.so" $GLIB_LIBS &&
	$cc -g -O1 -fno-omit-frame-pointer $WARN "$@" $GLIB_CFLAGS -c "$SRC/convert.c" -o "$w/convert.o" &&
	$cc -g -O1 -fno-omit-frame-pointer $WARN "$@" $GLIB_CFLAGS -c "$SRC/protocol.c" -o "$w/protocol.o" &&
	$cxx -g -O1 -fno-omit-frame-pointer $WARN "$@" -I"$LOK_INCLUDE" -I"$SRC" $GLIB_CFLAGS \
		-DLO_PROGRAM_DIR="\"$w/lok\"" -DOFFICE_MAX_SOURCE=32 \
		"$SRC/office-convert.cxx" "$w/convert.o" -o "$w/office-convert" $GLIB_LIBS -ldl &&
	$cxx -g -O1 -fno-omit-frame-pointer $WARN "$@" -I"$LOK_INCLUDE" -I"$SRC" $GLIB_CFLAGS \
		-DLO_PROGRAM_DIR="\"$w/lok\"" -DOFFICE_MAX_SOURCE=32 \
		"$SRC/office-session.cxx" "$w/convert.o" "$w/protocol.o" -o "$w/office-session" $GLIB_LIBS -ldl -lpthread &&
	$cc -g -O1 -fno-omit-frame-pointer $WARN "$@" $GLIB_CFLAGS -I"$SRC" \
		"$HERE/unit.c" "$w/convert.o" "$w/protocol.o" -o "$w/unit-test" $GLIB_LIBS &&
	$cc -g -O1 -fno-omit-frame-pointer $WARN "$@" -I"$HERE/harness/include" -I"$HERE/harness" -I"$SRC" $GLIB_CFLAGS \
		-DOFFICED_C="\"$SRC/officed.c\"" -DWORK_DIR="\"$w\"" -DFAKELOK_DIR="\"$w/lok\"" \
		-DOFFICE_ROOT="\"$w/root\"" -DOFFICE_CACHE_DIR="\"$w/root/.cache/org.webosports.service.office\"" \
		-DOFFICE_HELPER="\"$w/office-convert\"" -DOFFICE_TIMEOUT_S=1 -DOFFICE_IDLE_S=1 -DOFFICE_MAX_SOURCE=32 \
		-DOFFICE_SESSION_HELPER="\"$w/office-session\"" -DOFFICE_SESSION_IDLE_S=2 -DOFFICE_SESSION_REPLY_S=1 \
		"$HERE/harness/test.c" "$HERE/harness/shim.c" "$w/convert.o" "$w/protocol.o" -o "$w/officed-test" $GLIB_LIBS ||
	{ echo "FAIL build $name"; fail=1; return 1; }
}

# run <name> [wrapper...]
run() {
	name=$1; shift
	w="$WORK/$name"
	rm -rf "$w/unit" && mkdir -p "$w/unit"
	if "$@" "$w/unit-test" "$w/unit" >"$w/unit.log" 2>&1; then
		echo "PASS $name unit"
	else
		echo "FAIL $name unit"; tail -40 "$w/unit.log"; fail=1
	fi
	for m in $MODES; do
		rm -rf "$w/root" "$w/lok/unos" && mkdir -p "$w/root"
		if "$@" "$w/officed-test" "$m" >"$w/$m.log" 2>&1; then
			echo "PASS $name $m"
		else
			echo "FAIL $name $m"; tail -40 "$w/$m.log"; fail=1
		fi
	done
}

wanted() { case " $BUILDS " in *" $1 "*) return 0 ;; esac; return 1; }

wanted asan-gcc && build asan-gcc gcc g++ -fsanitize=address,undefined -fno-sanitize-recover=all && run asan-gcc
wanted asan-clang && build asan-clang clang clang++ -fsanitize=address,undefined,integer,nullability -fno-sanitize-recover=all \
	-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base && run asan-clang
wanted tsan && build tsan clang clang++ -fsanitize=thread && run tsan
wanted plain && build plain gcc g++ && {
	if command -v valgrind >/dev/null 2>&1; then
		run plain valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=definite,indirect \
			--errors-for-leak-kinds=definite,indirect --quiet
	else
		echo "valgrind not installed; skipped"
	fi
}

exit $fail
