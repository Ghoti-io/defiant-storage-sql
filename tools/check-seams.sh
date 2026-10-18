#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Fail if defiant-storage-sql reaches past one of its seams:
#
#  1. A private header (src/*.h) is included only by this library's own
#     sources: never by a public header, a test, a fuzzer or an example.
#  2. No engine: no header, library or pkg-config name of SQLite, libpq or
#     MySQL anywhere in the library or its build. The engines are the
#     backends' (S3, S4); this library speaks to them only through the
#     executor (executor.h), so it cannot grow a dependency on one by
#     accident.
#  3. No reach into another library's private code: no angle-bracket
#     include of an *_internal.h, no path through src/, no "../".
#
# The real tree must pass, a clean temporary tree must pass (the control),
# and each planted violation must be refused, by name.
#
# Usage: check-seams.sh <root>

set -eu

ROOT="${1:?usage: check-seams.sh <root>}"

include_lines() {
  for d in "$@"; do
    [ -d "$d" ] || continue
    grep -R -n -E '^[[:space:]]*#[[:space:]]*include' "$d" \
      --include='*.c' --include='*.h' --include='*.cpp' --include='*.inc' 2>/dev/null || true
  done
}

scan() {
  tree=$1
  bad=0
  # Rule 1: src/ headers, from outside src/.
  for hdr in $(find "$tree/src" -name '*.h' 2>/dev/null | sed 's|.*/||' | sort -u); do
    hits=$(include_lines "$tree/include" "$tree/tests" "$tree/examples" |
      grep -E "[\"</]${hdr}[\">]" || true)
    if [ -n "$hits" ]; then
      printf '%s\n' "$hits" | sed "s|^|check-seams: outside src/, includes private $hdr: |" >&2
      bad=1
    fi
  done
  # Rule 2: no engine, in the code or in the build.
  engine=$( (include_lines "$tree/src" "$tree/include";
      grep -n -E 'sqlite|libpq|pq-fe|mysql' "$tree/Makefile" "$tree"/pkgconfig/*.pc 2>/dev/null || true) |
    grep -i -E 'sqlite3?\.h|libpq|pq-fe\.h|mysql\.h|-lsqlite|-lpq|vendor-sqlite|ghoti\.io-vendor|mysqlclient' || true)
  if [ -n "$engine" ]; then
    printf '%s\n' "$engine" | sed 's/^/check-seams: names an engine: /' >&2
    bad=1
  fi
  # Rule 3.
  cross=$(include_lines "$tree/src" "$tree/include" "$tree/tests" "$tree/examples" |
    grep -E '#[[:space:]]*include[[:space:]]*<[^>]*internal\.h>|#[[:space:]]*include[[:space:]]*["<][^">]*/src/|#[[:space:]]*include[[:space:]]*"\.\./' || true)
  if [ -n "$cross" ]; then
    printf '%s\n' "$cross" | sed 's/^/check-seams: reaches into private code: /' >&2
    bad=1
  fi
  return $bad
}

if [ ! -f "$ROOT/src/core.c" ] || [ ! -d "$ROOT/include/ghoti.io/defiant-storage-sql" ] ||
   [ ! -f "$ROOT/Makefile" ]; then
  printf 'check-seams: the files this gate reads are missing; it is measuring nothing\n' >&2
  exit 1
fi
scan "$ROOT" || exit 1
printf 'check-seams: defiant-storage-sql keeps its seams\n'

plant=$(mktemp -d)
trap 'rm -rf "$plant"' EXIT
reset() {
  rm -rf "$plant"/*
  mkdir -p "$plant/src" "$plant/include/ghoti.io/defiant-storage-sql" "$plant/tests" "$plant/pkgconfig"
  printf '%s\n' '/* private */' > "$plant/src/sql_internal.h"
  printf '%s\n' '#include "sql_internal.h"' > "$plant/src/backend.c"
  printf '%s\n' '#include <ghoti.io/defiant-storage-sql/core.h>' > "$plant/tests/test_x.cpp"
  printf '%s\n' '#include <ghoti.io/defiant-storage/model_backend.h>' > "$plant/include/ghoti.io/defiant-storage-sql/backend.h"
  printf '%s\n' 'STOR_PC ?= ghoti.io-defiant-storage$(BRANCH)' > "$plant/Makefile"
  printf '%s\n' 'Requires: (REQUIRES)' > "$plant/pkgconfig/x.pc"
}
reset
if ! scan "$plant" 2>/dev/null; then
  printf 'check-seams: the clean control tree was refused; the scan proves nothing\n' >&2
  exit 1
fi
printf 'check-seams: control: a clean tree passes\n'

refused() {
  if scan "$plant" 2>/dev/null; then
    printf 'check-seams: the planted %s passed\n' "$1" >&2
    exit 1
  fi
  printf 'check-seams: planted %s was refused\n' "$1"
  reset
}

printf '%s\n' '#include "sql_internal.h"' >> "$plant/tests/test_x.cpp"
refused "test include of a private header"
printf '%s\n' '#include <sqlite3.h>' >> "$plant/src/backend.c"
refused "SQLite header in the library"
printf '%s\n' '#include <libpq-fe.h>' >> "$plant/include/ghoti.io/defiant-storage-sql/backend.h"
refused "libpq header in a public header"
printf '%s\n' 'SQLITE_PC := ghoti.io-vendor-sqlite$(BRANCH)' >> "$plant/Makefile"
refused "engine dependency in the Makefile"
printf '%s\n' '#include <ghoti.io/defiant-storage/model_internal.h>' >> "$plant/src/backend.c"
refused "include of another library's internal header"
printf '%s\n' '#include "../../defiant-storage/src/internal.h"' >> "$plant/src/backend.c"
refused "include through another library's src/"
