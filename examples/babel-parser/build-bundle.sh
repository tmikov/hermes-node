#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT bundles for this example and leaves them in the output
# directory. run.sh calls this with a temporary directory and then checks
# what came out; run it yourself to keep the artifacts.
#
# All four scripts are bundled, because the four are the example: parse.js
# and ast.js are @babel/parser alone, transform-static.js and transform.js
# are the same @babel/core transform reached two different ways, and the
# pair is the point (see README.md). They cost about two seconds together
# and the flags differ between them, which is the reason this file exists --
# so the flags are written down once and run.sh uses the same ones.
#
# rollup-out.hbb is deliberately NOT built here. Producing rollup-out.cjs
# needs a Node install and a devDependency, so a script that built it would
# fail on the "npm install --omit=dev" tree this example supports. run.sh
# builds it when rollup happens to be present, as a comparison rather than
# as an artifact of this example.
#
# There is no smoke test here: run.sh in this directory is the verification
# path, and it checks every bundle below with the source tree hidden.
#
# Usage: ./build-bundle.sh [out-dir] [build-dir]
#   out-dir    where to write the bundles   (default: ./dist)
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

"$HERMES_NODE" --build-bundle="$OUT_DIR/parse.hbb" parse.js
# @babel/parser is a plain require() here too, so ast.js needs no --include.
"$HERMES_NODE" --build-bundle="$OUT_DIR/ast.hbb" ast.js
# transform-static.js requires its preset, so the scanner finds it.
"$HERMES_NODE" --build-bundle="$OUT_DIR/transform-static.hbb" transform-static.js
# transform.js names the same preset in a string, which no static scanner
# can follow. --include packages it anyway: the idiomatic source, unedited,
# still ends up self-contained. This is the one flag in this file that is
# load-bearing rather than a default.
"$HERMES_NODE" --build-bundle="$OUT_DIR/transform.hbb" \
  --include=@babel/preset-env transform.js

human() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1048576) printf "%.1f MB", b / 1048576
    else if (b >= 1024) printf "%.1f KB", b / 1024
    else printf "%d B", b
  }'
}

# What was written, and therefore what has to travel together. These four
# have no native sidecars, so each bundle is one self-contained file.
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
echo "run them with:"
echo "  $HERMES_NODE --bundle=$OUT_DIR/parse.hbb"
echo "  $HERMES_NODE --bundle=$OUT_DIR/transform.hbb"
echo "  $HERMES_NODE --bundle=$OUT_DIR/transform-static.hbb"
echo "  $HERMES_NODE --bundle=$OUT_DIR/ast.hbb <file-to-parse.js>"
