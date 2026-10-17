#!/usr/bin/env bash
# Layout bench on the host: bash build/bench/bench.sh [layout.c]
# With no argument it measures src/layout.c; with a path it measures that file
# (to compare an older version: git show REV:src/layout.c > /some/path/x.c).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$(dirname "$HERE")")
SRC=$(realpath "${1:-$ROOT/src/layout.c}")
OUT=$ROOT/build/out
mkdir -p "$OUT"
# GRAPHOLLY_PLUGIN_PACKED_H: skips plugin.h, which wants the real windows.h.
gcc -O2 -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
  -I"$HERE/stub" -I"$ROOT/src" -DGRAPHOLLY_PLUGIN_PACKED_H \
  ${BENCHFLAGS:-} -DLAYOUT_C="\"$SRC\"" -o "$OUT/bench" "$HERE/bench.c"
"$OUT/bench"
