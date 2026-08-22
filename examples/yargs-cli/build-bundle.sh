#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds an AOT bundle of greet.js -- the yargs CLI and its 16-package
# dependency tree -- and leaves it in the output directory for you to keep.
#
# No --include is needed. Everything yargs loads it loads through a literal
# require(), which the scanner follows, so the whole tree is discovered
# statically. The two build-time warnings are expected: yargs computes a few
# specifiers, and the targets are already in the container by another edge,
# so they resolve at run time anyway. That is what the smoke test below
# proves.
#
# This example has no run.sh, so this script is the only place the artifact
# gets exercised; it runs the finished bundle before reporting success.
#
# Usage: ./build-bundle.sh [out-dir] [build-dir]
#   out-dir    where to write the bundle    (default: ./dist)
#   build-dir  the hermes-node build tree   (default: cmake-build-release)

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT_DIR="${1:-$HERE/dist}"
BUILD_DIR="${2:-$ROOT/cmake-build-release}"
if [ -d "$BUILD_DIR" ]; then
  BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
fi

HERMES_NODE="$BUILD_DIR/bin/hermes-node"

if [ ! -f "$HERMES_NODE" ]; then
  echo "ERROR: missing $HERMES_NODE -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node" 1>&2
  exit 1
fi

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

cd "$HERE"

"$HERMES_NODE" --build-bundle="$OUT_DIR/greet.hbb" greet.js

# The bundle is a closed world for modules: require() never reads the disk,
# so this run exercises the container whether or not node_modules is still
# beside it. (bufferutil-addon's script does hide the tree, to show its
# program needs no source tree; here the lit test test/bundle-yargs.js
# already deletes the tree and the entry script and reruns.)
#
# The status is captured before the output is parsed rather than piping
# into grep: with no `set -o pipefail`, an `if cmd | grep -q` sees grep's
# status, so a bundle that printed the right greeting and then exited
# nonzero would report ok.
echo
echo "smoke test:"
status=0
out="$("$HERMES_NODE" --bundle="$OUT_DIR/greet.hbb" -- \
  hello --name World --excited -r 2)" || status=$?
if [ "$status" -ne 0 ]; then
  echo "FAIL: the bundled greet.js exited $status" 1>&2
  echo "$out" 1>&2
  exit 1
fi
# Twice, because -r 2 asked for two.
if [ "$(echo "$out" | grep -c '^Hello, World!!!$')" -eq 2 ]; then
  echo "  ok: greet.hbb hello"
else
  echo "FAIL: greet.hbb did not greet twice" 1>&2
  echo "$out" 1>&2
  exit 1
fi

human() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1048576) printf "%.1f MB", b / 1048576
    else if (b >= 1024) printf "%.1f KB", b / 1024
    else printf "%d B", b
  }'
}

# What was written, and therefore what has to travel together. The producer
# already printed the bundle root and any native sidecars; this is the file
# list that follows from them.
echo
echo "wrote to $OUT_DIR:"
total=0
while IFS= read -r f; do
  bytes="$(wc -c < "$OUT_DIR/$f")"
  total=$((total + bytes))
  printf '  %-56s %s\n' "$f" "$(human "$bytes")"
done < <(cd "$OUT_DIR" && find . -type f | sed 's|^\./||' | sort)
echo "  $(printf '%-56s %s' 'total' "$(human "$total")")"
echo
echo "run it with:"
echo "  $HERMES_NODE --bundle=$OUT_DIR/greet.hbb -- --help"
