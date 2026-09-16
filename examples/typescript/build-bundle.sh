#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT bundle, and -- when a link kit is available -- the
# standalone executable, into ./dist.
#
# Three things about this graph are worth knowing before reading the script.
#
# 1. The producer warns three times and every warning is benign: an
#    uninstalled optional `source-map-support`, the `inspector` builtin
#    hermes-node does not have, and one computed require() at
#    _tsc.js:5023:28. All three sit inside a try in _tsc.js and none is on
#    the path a plain `tsc` invocation takes. So this script does not chase
#    them with --include, and run.sh does not assert a silent build the way
#    tetris and ditz2 do.
#
# 2. tsc reads its standard library -- 100 lib*.d.ts files -- from beside
#    the file it is executing. Those are data, not JavaScript, so the
#    producer does not package them and they have to travel beside the
#    artifact, at the path the bundled module still resolves:
#    <artifact dir>/node_modules/typescript/lib. Same reason gtop's terminfo
#    travels beside its artifact. Placing them once in OUT_DIR serves both
#    artifacts, since a container re-roots identities at its own directory
#    and an executable at its own -- and here those are the same directory.
#
# 3. Arguments for a bundled program go after `--`. Without it hermes-node
#    parses them itself, and the failure is silent for anything it happens
#    to understand: `--bundle=tsc.hbb --version` prints hermes-node's
#    version, not tsc's. The executable needs no `--`, because there every
#    argument belongs to the program already.
#
# Usage: ./build-bundle.sh [out-dir] [build-dir]

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT_DIR="${1:-$HERE/dist}"
BUILD_DIR="${2:-$ROOT/cmake-build-release}"
if [ -d "$BUILD_DIR" ]; then
  BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
fi

HERMES_NODE="$BUILD_DIR/bin/hermes-node"
KIT_DIR="$BUILD_DIR/kit"

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

"$HERMES_NODE" --build-bundle="$OUT_DIR/tsc.hbb" tsc.js

# The standard library, at the path the bundled _tsc.js still resolves.
TS_LIB_DST="$OUT_DIR/node_modules/typescript/lib"
mkdir -p "$TS_LIB_DST"
cp node_modules/typescript/lib/lib*.d.ts "$TS_LIB_DST/"

# --build-exe takes its options before the container, not after: an argument
# starting with '-' that appears after it is refused by name.
if [ -f "$KIT_DIR/kit.manifest" ]; then
  "$HERMES_NODE" --build-exe="$OUT_DIR/tsc" --kit="$KIT_DIR" "$OUT_DIR/tsc.hbb"
else
  echo "note: no link kit at $KIT_DIR, so no executable was built."
  echo "      cmake --build $BUILD_DIR --target hermes-node-kit"
fi

echo
echo "wrote to $OUT_DIR:"
( cd "$OUT_DIR" && find . -type f -not -path './node_modules/*' | sed 's|^\./||' |
  sort | while read -r f; do
    printf "  %-56s %8.1f KB\n" "$f" "$(echo "scale=1; $(wc -c < "$f") / 1024" | bc)"
  done )
printf "  %-56s %8.1f KB\n" \
  "node_modules/typescript/lib/*.d.ts ($(ls "$TS_LIB_DST" | wc -l | tr -d ' ') files)" \
  "$(echo "scale=1; $(cat "$TS_LIB_DST"/* | wc -c) / 1024" | bc)"
echo
echo "note: the artifact is more than one file. tsc's standard library is"
echo "      data, not JavaScript, so it ships beside the bundle rather"
echo "      than inside it."
echo
echo "run it with:"
echo "  $HERMES_NODE --bundle=$OUT_DIR/tsc.hbb -- --version   # the -- is required"
if [ -f "$OUT_DIR/tsc" ]; then
  echo "  $OUT_DIR/tsc --version        # no hermes-node, no node_modules"
fi
