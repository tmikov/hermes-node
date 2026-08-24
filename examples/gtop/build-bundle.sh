#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT bundle, and -- when a link kit is available -- the
# standalone executable, into ./dist.
#
# Unlike examples/tetris, this graph needs help in two ways, and both are
# the interesting part of the example.
#
# 1. blessed loads its widgets by computed name. lib/widget.js does
#    require('./widgets/' + name) in a loop, which no static scanner can
#    follow, so every widget has to be named with --include. The producer
#    warns about the computed call; the run-time loader would otherwise
#    throw MODULE_NOT_FOUND for the first widget the layout touches, naming
#    the exact --include to add. That message is how this list was built.
#
# 2. blessed ships its own terminfo database as data files under usr/, and
#    reads them relative to __dirname. The bundler packages JavaScript and
#    JSON, not arbitrary assets, so those files have to travel beside the
#    artifact at the path the bundled module still believes they are at.
#    That path is <artifact dir>/node_modules/blessed/usr, because a bundled
#    module's __dirname keeps the identity it had at build time.
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

# Every widget blessed can load by name. Derived from the directory rather
# than hardcoded, so a blessed upgrade that adds one does not silently drop
# it -- the failure would otherwise be a MODULE_NOT_FOUND at run time, in
# whichever layout first used the new widget.
INCLUDES=()
for widget in node_modules/blessed/lib/widgets/*.js; do
  INCLUDES+=("--include=./node_modules/blessed/lib/widgets/$(basename "$widget" .js)")
done

"$HERMES_NODE" --build-bundle="$OUT_DIR/gtop.hbb" "${INCLUDES[@]}" monitor.js

# The terminfo data files, at the path the bundled blessed still resolves.
TERMINFO_DST="$OUT_DIR/node_modules/blessed"
mkdir -p "$TERMINFO_DST"
cp -R node_modules/blessed/usr "$TERMINFO_DST/"

if [ -f "$KIT_DIR/kit.manifest" ]; then
  "$HERMES_NODE" --build-exe="$OUT_DIR/gtop" --kit="$KIT_DIR" "$OUT_DIR/gtop.hbb"
else
  echo "note: no link kit at $KIT_DIR, so no executable was built."
  echo "      cmake --build $BUILD_DIR --target hermes-node-kit"
fi

echo
echo "wrote to $OUT_DIR:"
( cd "$OUT_DIR" && find . -type f | sed 's|^\./||' | sort | while read -r f; do
    printf "  %-56s %8.1f KB\n" "$f" "$(echo "scale=1; $(wc -c < "$f") / 1024" | bc)"
  done )
echo
echo "note: the artifact is more than one file. The terminfo database under"
echo "      node_modules/blessed/usr is data, not JavaScript, so it ships"
echo "      beside the bundle rather than inside it."
echo
echo "run it with:"
echo "  $HERMES_NODE --bundle=$OUT_DIR/gtop.hbb"
if [ -f "$OUT_DIR/gtop" ]; then
  echo "  $OUT_DIR/gtop        # no hermes-node, no node_modules"
fi
