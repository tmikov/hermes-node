#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT container -- with yoga-layout's WebAssembly layout engine
# baked into it ahead of time -- and, when a link kit is available, the
# standalone executable built from that container, into ./dist.
#
# Baking matters specifically here. yoga-layout's layout engine is
# WebAssembly, base64-decoded from inside a JavaScript module rather than
# read from a .wasm file, and Ink loads it as soon as the bundle's module
# graph loads -- so a bundled run with nothing baked in still pays the
# whole Wasm compile at every launch. Measured on this exact container,
# both sides with --no-compile-cache (the shipped-artifact case: a fresh
# machine has no warm disk cache to fall back on):
#
#   plain --build-bundle              container:   950,712 bytes
#     bundled run: 0.46s (wasm container miss, then wasm store)
#   --record-wasm, then --bake-wasm   container: 1,207,952 bytes
#     bundled run: 0.09s (wasm container hit, no store)
#
# So this script always records and bakes; there is no plain-bundle mode
# left to fall back to. See README.md for how those numbers were taken.
#
# The record run needs a real pty (raw-mode input) and a keypress to quit
# cleanly, exactly like run.sh's own checks -- see ../pty-run.py.
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
PTY="$HERE/../pty-run.py"

if [ ! -f "$HERMES_NODE" ]; then
  echo "ERROR: missing $HERMES_NODE -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node" 1>&2
  exit 1
fi

if [ ! -f "$HERE/dist-cjs/app.cjs" ]; then
  echo "ERROR: no CommonJS build. Run './build-cjs.sh' in $HERE first." 1>&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
cd "$HERE"

APP="dist-cjs/app.cjs"

# The recording is a build-time intermediate, not part of the shipped
# artifact -- its entries end up inside the container, so it does not need
# to travel beside it the way the natives sidecar or gtop's terminfo do.
WASM_REC="$(mktemp /tmp/hermes-node-ink-wasm.XXXXXX)"
trap 'rm -f "$WASM_REC"' EXIT

python3 "$PTY" 2 80 24 --send 'q' -- "$HERMES_NODE" --no-compile-cache \
  --record-wasm="$WASM_REC" "$APP" >/dev/null

"$HERMES_NODE" --build-bundle="$OUT_DIR/ink.hbb" --bake-wasm="$WASM_REC" "$APP"

if [ -f "$KIT_DIR/kit.manifest" ]; then
  "$HERMES_NODE" --build-exe="$OUT_DIR/ink" --kit="$KIT_DIR" "$OUT_DIR/ink.hbb"
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
echo "  $HERMES_NODE --bundle=$OUT_DIR/ink.hbb"
if [ -f "$OUT_DIR/ink" ]; then
  echo "  $OUT_DIR/ink        # no hermes-node, no node_modules"
fi
