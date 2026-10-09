#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the Flow bundler into four artifacts, in ./dist by default:
#
#   flow-bundler.hbb             AOT container        (--build-bundle)
#   flow-bundler                 standalone executable (--build-exe; needs the kit)
#   flow-bundler-native          native executable    (build-native; needs shermes)
#   flow-bundler-native-bcwasm   the same, --bytecode-wasm
#
# Each carries the WebAssembly hermes-parser and build.config.js. The first
# two and flow-bundler-native-bcwasm carry the parser's compiled bytecode
# baked in; flow-bundler-native compiles the parser itself to native code,
# like everything else in it. The two native executables differ in nothing
# else, so they measure what native WebAssembly costs and buys. An artifact reads <its dir>/fixture/src and writes
# <its dir>/out, so the out-dir gets a fixture/src link and runs in place:
#
#   hermes-node --bundle=dist/flow-bundler.hbb -- -c dist/build.config.js
#   dist/flow-bundler -c dist/build.config.js
#
# The -c path names a file that need not exist: the config is packaged, and
# a bundle loads code only from itself. See "Bundling it" in README.md for
# why each step below exists; check-artifacts.sh verifies the result.
#
# Usage: ./build-bundle.sh [out-dir] [build-dir]

set -e

# The artifacts carry the WebAssembly parser only; a value left over from a
# native run.sh would be misleading in a stack trace.
unset HERMES_PARSER_NATIVE_ADDON

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
if ! "$HERMES_NODE" -e 'if (typeof WebAssembly !== "object") process.exit(1)'; then
  echo "ERROR: $HERMES_NODE has no WebAssembly." 1>&2
  echo "  Reconfigure with -DHERMES_ENABLE_WASM=ON." 1>&2
  exit 1
fi

# The staging redirects below replace the top-level hermes-parser and
# prettier only. A nested copy -- which a different npm resolution could
# produce -- would keep the replaced package in some corner of the graph,
# silently.
nested="$(find "$HERE/node_modules" -mindepth 2 -type d \
  \( -path "$HERE/node_modules/*/node_modules/hermes-parser" \
  -o -path "$HERE/node_modules/*/node_modules/prettier" \))"
if [ -n "$nested" ]; then
  echo "ERROR: nested copies the staging redirects would miss:" 1>&2
  echo "$nested" 1>&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
STAGE="$OUT_DIR/stage"

# --- 1. Stage --------------------------------------------------------------
# A node_modules made of symlinks into the real one, with two exceptions.
# The producer never resolves symlinks, so each is packaged under the name
# everything requires, and the package it replaces never enters the graph:
#   hermes-parser -> hermes-parser-wasm, the parser the artifacts carry;
#   prettier      -> prettier-stub, since nothing here formats code and
#                    prettier would otherwise be half the artifact.
# The real node_modules is left alone.
rm -rf "$STAGE"
mkdir -p "$STAGE/node_modules" "$STAGE/fixture"
for entry in "$HERE"/node_modules/*; do
  name="$(basename "$entry")"
  if [ "$name" = hermes-parser ] || [ "$name" = prettier ]; then
    continue
  fi
  ln -s "$entry" "$STAGE/node_modules/$name"
done
ln -s "$HERE/node_modules/hermes-parser-wasm" "$STAGE/node_modules/hermes-parser"
ln -s "$HERE/prettier-stub" "$STAGE/node_modules/prettier"
cp "$HERE/build.config.js" "$STAGE/build.config.js"
ln -s "$HERE/fixture/src" "$STAGE/fixture/src"

# --- 2. Transpile ----------------------------------------------------------
FLOW_BUNDLER_PARSER=wasm "$HERMES_NODE" "$HERE/transpile.js" "$STAGE/bundler"

# --- 3. Record the compiled WebAssembly parser -----------------------------
# Run the staged tree from disk and record the Wasm module it compiles. The
# -r is not optional: at run time the disk loader DOES resolve symlinks, so
# without it packages loaded through the farm would find the native parser
# beside their real paths -- correct output, empty record, and artifacts
# that compile the parser on every launch.
echo "recording the WebAssembly parser:"
( cd "$STAGE" && FLOW_BUNDLER_PARSER=wasm "$HERMES_NODE" -r "$HERE/parser-select.js" \
    --record-wasm="$STAGE/parser.wasmrec" \
    bundler/buildBundleCLI.js -c "$STAGE/build.config.js" )
rm -rf "$STAGE/out"
if ! "$HERMES_NODE" --dump-wasm="$STAGE/parser.wasmrec" | grep -q '^WASM (1)$'; then
  "$HERMES_NODE" --dump-wasm="$STAGE/parser.wasmrec" 1>&2
  echo "ERROR: expected exactly one recorded Wasm module" 1>&2
  exit 1
fi

# --- 4. Build --------------------------------------------------------------
# The artifacts' input, so they run where they are built.
mkdir -p "$OUT_DIR/fixture"
ln -sfn "$HERE/fixture/src" "$OUT_DIR/fixture/src"

# --include resolves from the entry's directory, stage/bundler. The three
# plugins are reached only through build.config.js's plugin() helper -- a
# computed require.resolve the scanner cannot follow.
INCLUDES=(
  --include=../build.config.js
  --include=@babel/plugin-transform-flow-strip-types
  --include=@babel/plugin-transform-class-properties
  --include=@babel/plugin-transform-classes
)

cd "$STAGE"

echo "hermes-node --build-bundle:"
"$HERMES_NODE" --build-bundle="$OUT_DIR/flow-bundler.hbb" \
  --bake-wasm="$STAGE/parser.wasmrec" "${INCLUDES[@]}" bundler/buildBundleCLI.js

# The kit is EXCLUDE_FROM_ALL, so a plain build does not produce one.
if [ -f "$KIT_DIR/kit.manifest" ]; then
  echo "hermes-node --build-exe:"
  "$HERMES_NODE" --build-exe="$OUT_DIR/flow-bundler" --kit="$KIT_DIR" \
    "$OUT_DIR/flow-bundler.hbb"
else
  echo "note: no link kit at $KIT_DIR, so no executables were built."
  echo "      cmake --build $BUILD_DIR --target hermes-node-kit"
fi

# Minutes, not seconds: about 1,250 modules through shermes and cc, and, in
# the first build, the parser's 768 KB Wasm module through them too.
if [ -x "$KIT_DIR/shermes" ]; then
  echo "hermes-node build-native:"
  "$HERMES_NODE" build-native bundler/buildBundleCLI.js \
    -o "$OUT_DIR/flow-bundler-native" --kit="$KIT_DIR" \
    --bake-wasm="$STAGE/parser.wasmrec" "${INCLUDES[@]}"
  echo "hermes-node build-native --bytecode-wasm:"
  "$HERMES_NODE" build-native bundler/buildBundleCLI.js \
    -o "$OUT_DIR/flow-bundler-native-bcwasm" --kit="$KIT_DIR" \
    --bake-wasm="$STAGE/parser.wasmrec" --bytecode-wasm "${INCLUDES[@]}"
elif [ -f "$KIT_DIR/kit.manifest" ]; then
  echo "note: no shermes in $KIT_DIR, so no native executable was built."
fi

echo
echo "wrote to $OUT_DIR:"
for f in flow-bundler.hbb flow-bundler flow-bundler-native \
  flow-bundler-native-bcwasm; do
  if [ -f "$OUT_DIR/$f" ]; then
    printf "  %-24s %8.1f MB\n" "$f" "$(echo "scale=1; $(wc -c < "$OUT_DIR/$f") / 1000000" | bc)"
  fi
done
