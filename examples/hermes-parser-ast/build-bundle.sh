#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the AOT bundle of ast.js and leaves it, plus the native addon's
# sidecar, in the output directory. run.sh calls this with a temporary
# directory and then checks what came out; run it yourself to keep the
# artifacts.
#
# The output is two files, not one: a bundle that needs a native addon
# drops the addon beside itself, and the pair has to travel together.
#
# There is no smoke test here: run.sh in this directory is the verification
# path, and it diffs the bundled AST against the unbundled one, asserts the
# sidecar exists, and runs --verify-natives.
#
# Usage: ./build-bundle.sh [out-dir] [build-dir]
#   out-dir    where to write the bundle    (default: ./dist)
#   build-dir  the hermes-node build tree   (default: cmake-build-release)

set -e

# examples/flow-bundler/run.sh exports this to point hermes-parser at a build
# tree path, so a shell that ran that example still has it set. Here it would
# send the producer's walk at an addon this example never chose. run.sh
# asserts separately that a bundled run refuses it.
unset HERMES_PARSER_NATIVE_ADDON

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

# --- BEGIN vendored native parser addon ------------------------------------
# Delete everything between these two markers when the vendored addon goes
# away and this example switches to the published hermes-parser package. See
# "When to delete this directory" in
# external/hermes-parser-native/README.md; that recipe refers to these
# markers, so keep them in sync with it. Note that PLATFORM_ARCH is used by
# the --include below, outside the markers.
ADDON="$BUILD_DIR/external/hermes-parser-native/hermes-parser.node"

if [ ! -f "$ADDON" ]; then
  echo "ERROR: missing $ADDON -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node hermes-parser-napi" 1>&2
  exit 1
fi

# Unlike flow-bundler, this example does NOT export
# HERMES_PARSER_NATIVE_ADDON: a bundle serves only what its container
# records, and that override is an absolute build-tree path that no
# container records. Copying the addon into the package's own prebuilds/
# directory instead puts it somewhere the walk can reach, where --include
# can name it and the container can record it. The platform-arch pair is
# derived from hermes-node itself, not the host node, so the copy matches
# the runtime that will load it -- and so this works on a machine the
# committed linux-x64 prebuilt does not fit.
PLATFORM_ARCH="$("$HERMES_NODE" -e 'console.log(process.platform + "-" + process.arch)')"
TARGET_DIR="$HERE/node_modules/hermes-parser/prebuilds/$PLATFORM_ARCH"
mkdir -p "$TARGET_DIR"
cp "$ADDON" "$TARGET_DIR/hermes-parser.node"
# --- END vendored native parser addon --------------------------------------

# hermes-parser's own require() calls for the addon are computed (a
# path.resolve() of an env var, and two path.join()s tried in a try/catch
# loop), so the scanner cannot see them; --include names the one this
# platform will actually load. Unlike bufferutil-addon, no copy of the addon
# has to be staged under the output directory as well: hermes-parser
# require()s its candidates and catches, rather than stat-ing first, so the
# container answers directly.
"$HERMES_NODE" --build-bundle="$OUT_DIR/ast.hbb" \
  --include="./node_modules/hermes-parser/prebuilds/$PLATFORM_ARCH/hermes-parser.node" \
  ast.js

human() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1048576) printf "%.1f MB", b / 1048576
    else if (b >= 1024) printf "%.1f KB", b / 1024
    else printf "%d B", b
  }'
}

# What was written, and therefore what has to travel together.
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
echo "  $HERMES_NODE --bundle=$OUT_DIR/ast.hbb --verify-natives"
