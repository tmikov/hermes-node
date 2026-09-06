#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Bundles the fixture with the Flow bundler running under hermes-node, then
# checks the result against expected/.
#
# This repository vendors the Hermes parser twice -- as a Node-API addon
# (external/hermes-parser-native) and as a WebAssembly module
# (external/hermes-parser-wasm) -- and both are meant to be exercised. With
# no FLOW_BUNDLER_PARSER set this runs the bundler once with each, which is
# the interesting result: the same six bundles either way, so a parser that
# disagrees fails here the way a broken bundler would. Costs a few seconds
# (measured 2.1s native, 6.9s wasm on Linux x64 Release), which is why both
# run by default rather than one.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)
#   FLOW_BUNDLER_PARSER=native|wasm   run only that one
#   FLOW_BUNDLER_VERBOSE=1            have babel-register say which it chose

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"

HERMES_NODE="$BUILD_DIR/bin/hermes-node"

if [ ! -x "$HERMES_NODE" ] && [ ! -f "$HERMES_NODE" ]; then
  echo "ERROR: missing $HERMES_NODE -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node" 1>&2
  exit 1
fi

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

case "${FLOW_BUNDLER_PARSER:-}" in
  '')       PARSERS="native wasm" ;;
  native)   PARSERS="native" ;;
  wasm)     PARSERS="wasm" ;;
  *)
    echo "ERROR: FLOW_BUNDLER_PARSER must be 'native' or 'wasm', got '$FLOW_BUNDLER_PARSER'" 1>&2
    exit 1
    ;;
esac

# --- BEGIN vendored native parser addon ------------------------------------
# Delete everything between these two markers when the vendored addon goes
# away and this example runs the WebAssembly parser alone. See "When to
# delete this directory" in external/hermes-parser-native/README.md; that
# recipe refers to these markers, so keep them in sync with it. Removing
# this block means dropping "native" from the PARSERS cases above too.
ADDON="$BUILD_DIR/external/hermes-parser-native/hermes-parser.node"

if [[ " $PARSERS " == *" native "* ]] && [ ! -f "$ADDON" ]; then
  echo "ERROR: missing $ADDON -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node hermes-parser-napi" 1>&2
  exit 1
fi
# --- END vendored native parser addon --------------------------------------

# WebAssembly is a build option, and a hermes-node without it fails somewhere
# inside emscripten glue rather than saying so.
if [[ " $PARSERS " == *" wasm "* ]]; then
  if ! "$HERMES_NODE" -e 'if (typeof WebAssembly !== "object") process.exit(1)'; then
    echo "ERROR: $HERMES_NODE has no WebAssembly." 1>&2
    echo "  Reconfigure with -DHERMES_ENABLE_WASM=ON (it is on by default; a" 1>&2
    echo "  build directory created before that default keeps its cached OFF)." 1>&2
    exit 1
  fi
fi

run_one() {
  local parser="$1"
  echo "parser: $parser"
  rm -rf "$HERE/out"

  if [ "$parser" = native ]; then
    # Pin the addon to the build directory being tested rather than relying
    # on the package's prebuilds/ lookup, which several build directories
    # share.
    export HERMES_PARSER_NATIVE_ADDON="$ADDON"
  else
    # The wasm package never reads it, but a stale value from a previous
    # native run would be misleading in a stack trace.
    unset HERMES_PARSER_NATIVE_ADDON
  fi

  FLOW_BUNDLER_PARSER="$parser" "$HERMES_NODE" -r "$HERE/babel-register.js" \
    "$HERE/bundler/buildBundleCLI.js" -c "$HERE/build.config.js"

  local status=0
  for f in "$HERE"/expected/*.js; do
    local name
    name="$(basename "$f")"
    if ! cmp -s "$f" "$HERE/out/$name"; then
      echo "MISMATCH: $name" 1>&2
      status=1
    fi
  done

  if [ "$status" -ne 0 ]; then
    echo "FAIL: bundler output differs from expected/ with the $parser parser" 1>&2
    return 1
  fi

  echo "  ok: 6 bundles match expected/"
}

for parser in $PARSERS; do
  run_one "$parser"
done

echo "PASS: flow-bundler ($PARSERS)"
