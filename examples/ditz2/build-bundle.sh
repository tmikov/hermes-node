#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT bundle, and -- when a link kit is available -- the
# standalone executable, into ./dist.
#
# Nothing here is special-cased for this package. Once ditz2 is CommonJS, its
# graph has no dynamic requires and no native addons, so the scanner finds
# every module and the producer emits no warnings at all. Its three runtime
# dependencies (commander, uuid, yaml) are literal requires the whole way
# down. That is why there is not an --include flag in sight, where gtop next
# door needs a page of them.
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

if [ ! -f "$HERE/dist-cjs/cli/main.js" ]; then
  echo "ERROR: no CommonJS build. Run './build-cjs.sh' in $HERE first." 1>&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
cd "$HERE"

"$HERMES_NODE" --build-bundle="$OUT_DIR/ditz2.hbb" dist-cjs/cli/main.js

# The kit is EXCLUDE_FROM_ALL, so a plain build does not produce one and a
# released hermes-node does not ship one yet. Without it the bundle is still
# the deliverable; the executable is the extra step.
if [ -f "$KIT_DIR/kit.manifest" ]; then
  "$HERMES_NODE" --build-exe="$OUT_DIR/dz" --kit="$KIT_DIR" "$OUT_DIR/ditz2.hbb"
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
echo "run it with:"
echo "  $HERMES_NODE --bundle=$OUT_DIR/ditz2.hbb --help"
if [ -f "$OUT_DIR/dz" ]; then
  echo "  $OUT_DIR/dz --help        # no hermes-node, no node_modules"
fi
