#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT bundle of ast.js and leaves it in the output directory.
# run.sh calls this with a temporary directory and then checks what came
# out; run it yourself to keep the artifact.
#
# The output is one file. That is the whole point of the pairing with
# examples/hermes-parser-ast, which does the same job with the native addon
# and produces a bundle plus a shared object that have to travel together.
# Here the parser is a 665 KB wasm module base64-encoded inside an ordinary
# JavaScript file, so the producer sees nothing but JavaScript, needs no
# --include, and emits no warnings -- and the container carries the parser.
#
# There is no smoke test here: run.sh in this directory is the verification
# path, and it diffs the bundled AST against the unbundled one and asserts
# the artifact really is a single file.
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

# WebAssembly is a build option. A hermes-node built without it has no
# WebAssembly global, and this example's failure would otherwise be a
# TypeError from inside emscripten glue rather than a sentence saying why.
if ! "$HERMES_NODE" -e 'if (typeof WebAssembly !== "object") process.exit(1)'; then
  echo "ERROR: $HERMES_NODE has no WebAssembly." 1>&2
  echo "  Reconfigure with -DHERMES_ENABLE_WASM=ON (it is on by default; a" 1>&2
  echo "  build directory created before that default keeps its cached OFF)." 1>&2
  exit 1
fi

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

cd "$HERE"

# No --include, and nothing staged first. hermes-parser's wasm build reaches
# HermesParserWASM.js through a plain require(), which the scanner follows,
# and that file carries the module as a base64 data: URI rather than loading
# anything from disk.
"$HERMES_NODE" --build-bundle="$OUT_DIR/ast.hbb" ast.js

human() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1048576) printf "%.1f MB", b / 1048576
    else if (b >= 1024) printf "%.1f KB", b / 1024
    else printf "%d B", b
  }'
}

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
echo "  $HERMES_NODE --bundle=$OUT_DIR/ast.hbb <file-to-parse.js>"
