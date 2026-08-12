#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Bundles the fixture with the Flow bundler running under hermes-node, then
# checks the result against expected/.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"

HERMES_NODE="$BUILD_DIR/bin/hermes-node"
ADDON="$BUILD_DIR/external/hermes-parser-native/hermes-parser.node"

for f in "$HERMES_NODE" "$ADDON"; do
  if [ ! -x "$f" ] && [ ! -f "$f" ]; then
    echo "ERROR: missing $f -- build it first:" 1>&2
    echo "  cmake --build $BUILD_DIR --target hermes-node hermes-parser-napi" 1>&2
    exit 1
  fi
done

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

# Pin the addon to the build directory being tested rather than relying on
# the package's prebuilds/ lookup, which several build directories share.
export HERMES_PARSER_NATIVE_ADDON="$ADDON"

rm -rf "$HERE/out"
"$HERMES_NODE" -r "$HERE/babel-register.js" \
  "$HERE/bundler/buildBundleCLI.js" -c "$HERE/build.config.js"

status=0
for f in "$HERE"/expected/*.js; do
  name="$(basename "$f")"
  if ! cmp -s "$f" "$HERE/out/$name"; then
    echo "MISMATCH: $name" 1>&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "FAIL: bundler output differs from expected/" 1>&2
  exit 1
fi

echo "PASS: 6 bundles match expected/"
