#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Fail unless every public header carries exactly one stability label, and
# that label is `free` (AD-4, runtime AD-14): nothing in this library is
# frozen before the contract story, so a `stable` header would promise what
# no one decided. The label is a Doxygen tag in a comment line:
#
#     * @stability free
#
# The real headers must pass; a clean copy must pass (the control); a header
# with no label, with two, and with `stable` must each be refused.
#
# Usage: check-labels.sh <root>

set -eu

ROOT="${1:?usage: check-labels.sh <root>}"

# scan <include-dir>: print each problem, return 1 if there was one.
scan() {
  base=$1
  headers=$(find "$base" -type f -name '*.h' | sort)
  if [ -z "$headers" ]; then
    printf 'check-labels: no headers under %s; this gate is measuring nothing\n' "$base" >&2
    return 1
  fi
  bad=0
  for h in $headers; do
    labels=$(grep -E '^[[:space:]]*(\*|//)[[:space:]]*@stability[[:space:]]' "$h" \
      | sed 's/.*@stability[[:space:]]*//; s/[[:space:]]*$//' || true)
    n=$(printf '%s' "$labels" | grep -c . || true)
    if [ "$n" -eq 0 ]; then
      printf 'check-labels: %s has no @stability label\n' "$h" >&2
      bad=1
    elif [ "$n" -ne 1 ]; then
      printf 'check-labels: %s has more than one @stability label\n' "$h" >&2
      bad=1
    elif [ "$labels" != free ]; then
      printf 'check-labels: %s is labelled "%s"; every header here is free\n' "$h" "$labels" >&2
      bad=1
    fi
  done
  return $bad
}

BASE="$ROOT/include/ghoti.io/defiant-storage-sql"
[ -d "$BASE" ] || { printf 'check-labels: %s is missing; this gate is measuring nothing\n' "$BASE" >&2; exit 1; }
scan "$BASE" || exit 1
printf 'check-labels: %s public headers, each labelled free once\n' \
  "$(find "$BASE" -name '*.h' | wc -l | tr -d ' ')"

plant=$(mktemp -d)
trap 'rm -rf "$plant"' EXIT
cp "$BASE"/*.h "$plant/"
if ! scan "$plant" 2>/dev/null; then
  printf 'check-labels: the clean control copy was refused; the scan proves nothing\n' >&2
  exit 1
fi
printf 'check-labels: control: a clean copy passes\n'

refused() {
  what=$1
  if scan "$plant" 2>/dev/null; then
    printf 'check-labels: the planted %s passed\n' "$what" >&2
    exit 1
  fi
  printf 'check-labels: planted %s was refused\n' "$what"
  cp "$BASE"/*.h "$plant/"
}

sed -i '/@stability free/d' "$plant/core.h"
refused "header with no label (core.h)"
sed -i 's/@stability free/@stability free\n * @stability free/' "$plant/allocator.h"
refused "header with two labels (allocator.h)"
sed -i 's/@stability free/@stability stable/' "$plant/libver.h"
refused "header labelled stable (libver.h)"
