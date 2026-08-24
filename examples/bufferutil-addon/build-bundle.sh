#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds an AOT bundle of mask.js and leaves it in the output directory.
#
# bufferutil is loaded by node-gyp-build, which is one of the loaders a
# bundle cannot serve on its own. It does not require-and-catch; it
# readdirSync()s prebuilds/<platform>-<arch>/ and requires only the winner.
# A bundle is closed for modules and open for data: the require() would be
# answered from the container, but the readdirSync() that precedes it reads
# the real filesystem and finds nothing, so bufferutil's index.js catches
# and silently loads its pure-JavaScript ./fallback -- and mask.js's own
# "is this really the native addon?" check then throws.
#
# So this script does two things rather than one:
#
#   --include names the addon, which packages it and drops it beside the
#   bundle as a sidecar. That is what makes the container hold the real
#   machine code.
#
#   The addon is ALSO copied to its identity path under the output
#   directory, so node-gyp-build's readdirSync() has something to find.
#   This is the escape hatch the native-addon design calls out by name
#   (docs/superpowers/specs/2026-08-21-bundle-natives-design.md, "The escape hatch,
#   which costs no code"): place the real file at its identity path and the
#   stat succeeds, after which the require() is answered from the
#   container. That copy is the difference between "the native addon runs"
#   and "a fallback quietly runs instead" -- rebuild without it and the
#   bundled mask.js exits 1, source tree present or not.
#
#   It has to be the real 14 KB file and not a zero-byte placeholder that
#   satisfies the stat. The design's own wording is that "whether we
#   intercept the following require or let it load from disk, the bytes are
#   the same": the interception is not a guarantee the format makes, so a
#   stub would work only for as long as nothing loads that path directly,
#   and would fail as an unreadable ELF rather than as a missing file.
#
# What the smoke test below does NOT prove is which of the two loaded.
# node-gyp-build resolves against the BUNDLE ROOT -- require.resolve
# ('bufferutil') is answered from the container, so the directory it stats
# is <out-dir>/node_modules/bufferutil, never this example's own
# node_modules. Whether $HERE/node_modules exists is inert for it. The
# staged copy above is the whole discriminator.
#
# node_modules is hidden anyway, for the other claim, the one every bundle
# makes: that it needs no source tree at all. Every JavaScript module in
# this program comes from the container, and the run below is what says so.
#
# This example has no run.sh, so this script is the only place the artifact
# gets exercised.
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

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

cd "$HERE"

# The smoke test moves node_modules aside, so an interrupted run would
# otherwise leave the example uninstalled.
cleanup() {
  if [ -d "$HERE/.node_modules_hidden" ]; then
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
}
trap cleanup EXIT

# Ask node-gyp-build itself which file it would pick, rather than guessing a
# platform-arch tuple and a filename. Run through hermes-node, so the answer
# matches the runtime that will load it and not the host's node.
ADDON="$("$HERMES_NODE" -e \
  'const p = require("path");
   const dir = p.dirname(require.resolve("bufferutil"));
   console.log(require("node-gyp-build").path(dir));')"
case "$ADDON" in
  "$HERE"/*) ;;
  *)
    echo "ERROR: node-gyp-build resolved $ADDON, outside $HERE" 1>&2
    exit 1
    ;;
esac
# Relative to the example directory, which is both what --include wants and
# the identity the container will record.
REL="${ADDON#$HERE/}"

"$HERMES_NODE" --build-bundle="$OUT_DIR/mask.hbb" --include="./$REL" mask.js

mkdir -p "$OUT_DIR/$(dirname "$REL")"
cp "$ADDON" "$OUT_DIR/$REL"

echo
echo "smoke test (no source tree):"
mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
status=0
out="$("$HERMES_NODE" --bundle="$OUT_DIR/mask.hbb")" || status=$?
mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
if [ "$status" -ne 0 ]; then
  echo "FAIL: the bundled mask.js exited $status" 1>&2
  echo "$out" 1>&2
  exit 1
fi
# mask.js prints the addon path it resolved and then PASS. Both matter: the
# path shows node-gyp-build's stat was satisfied by the staged copy -- it
# throws, unguarded, when it is not -- and PASS shows the masking
# round-trip came out right.
if echo "$out" | grep -q '^PASS$' && echo "$out" | grep -q '^native addon: .*\.node$'; then
  echo "  ok: mask.hbb runs with no source tree, native addon from the container"
else
  echo "FAIL: the bundled mask.js did not report a native addon and PASS" 1>&2
  echo "$out" 1>&2
  exit 1
fi

human() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1048576) printf "%.1f MB", b / 1048576
    else if (b >= 1024) printf "%.1f KB", b / 1024
    else printf "%d B", b
  }'
}

# What was written, and therefore what has to travel together. The addon
# appears twice on purpose -- see the header.
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
echo "note: the addon is present twice. $(basename "$ADDON") beside the bundle is"
echo "      the sidecar the container records and loads; $REL"
echo "      exists so node-gyp-build's readdirSync() finds something. Ship both."
echo
echo "run it with:"
echo "  $HERMES_NODE --bundle=$OUT_DIR/mask.hbb"
