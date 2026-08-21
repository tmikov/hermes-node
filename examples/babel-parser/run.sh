#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the Babel examples under hermes-node: the two scripts from disk, then
# their AOT bundles, checked with the source tree hidden. A bundle is a
# closed world -- it never reads a module off the disk -- so hiding the tree
# is what tells a self-contained container from one that is not.
#
# transform.js names its preset in a string, which no static bundler can
# follow, and is bundled with --include=@babel/preset-env. transform-static.js
# requires the preset instead and needs nothing. Both end up self-contained,
# by the two different routes, and that pair is the point of this example.
#
# The rollup section is optional. It runs only when rollup and node are both
# present, and reports SKIP otherwise, so `npm install --omit=dev` leaves a
# working example rather than a broken check.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"
# Absolute before the cd below, because run-examples.sh passes whatever the
# caller typed and that is commonly a relative path.
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

cd "$HERE"

# without_node_modules() below moves the tree aside, so an interrupted run
# would otherwise leave the example uninstalled. Restoring it here costs
# nothing and is the difference between a failed check and a broken
# checkout. The `if` rather than `&&` keeps the trap's own exit status out
# of the script's.
cleanup() {
  rm -f "$HERE"/*.hbb
  if [ -d "$HERE/.node_modules_hidden" ]; then
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
}
trap cleanup EXIT

# Every check below asserts the script's own PASS line, so a run that dies
# halfway and still exits 0 cannot be mistaken for a success.
expect_pass() {
  local label="$1"
  shift
  if "$@" | grep -q '^PASS$'; then
    echo "  ok: $label"
  else
    echo "FAIL: $label" 1>&2
    exit 1
  fi
}

# Hides node_modules for the duration of one command. A bundle that is
# genuinely self-contained does not notice; one with a hole in it fails with
# "Cannot find module ... Not in the bundle", which is the only way to tell
# the two apart.
without_node_modules() {
  local status=0
  mv node_modules .node_modules_hidden
  "$@" || status=$?
  mv .node_modules_hidden node_modules
  return $status
}

echo "from disk:"
expect_pass "parse.js" "$HERMES_NODE" parse.js
expect_pass "transform.js" "$HERMES_NODE" transform.js
expect_pass "transform-static.js" "$HERMES_NODE" transform-static.js

echo "hermes-node --build-bundle:"
"$HERMES_NODE" --build-bundle=parse.hbb parse.js >/dev/null 2>&1
"$HERMES_NODE" --build-bundle=transform-static.hbb transform-static.js >/dev/null 2>&1
# The unmodified transform.js, whose preset is named by a string the bundler
# cannot follow. --include packages it anyway. This is the case the closed
# world exists for: the idiomatic source, no edit, self-contained.
"$HERMES_NODE" --build-bundle=transform.hbb --include=@babel/preset-env \
  transform.js >/dev/null 2>&1
expect_pass "parse.hbb, no source tree" \
  without_node_modules "$HERMES_NODE" --bundle=parse.hbb
expect_pass "transform-static.hbb, no source tree" \
  without_node_modules "$HERMES_NODE" --bundle=transform-static.hbb
expect_pass "transform.hbb (--include), no source tree" \
  without_node_modules "$HERMES_NODE" --bundle=transform.hbb

# Optional: rollup is a devDependency, and running it needs node itself.
if [ ! -d "$HERE/node_modules/rollup" ]; then
  echo "rollup: SKIP (not installed; 'npm install' without --omit=dev)"
elif ! command -v node >/dev/null 2>&1; then
  echo "rollup: SKIP (no node on PATH)"
else
  echo "rollup:"
  npx --no-install rollup -c rollup.config.mjs >/dev/null 2>&1
  expect_pass "rollup-out.cjs, no source tree" \
    without_node_modules "$HERMES_NODE" rollup-out.cjs
  # Rollup collapses the graph into one module; the container then holds one
  # bytecode blob instead of 270, which is both smaller and faster to load.
  "$HERMES_NODE" --build-bundle=rollup-out.hbb rollup-out.cjs >/dev/null 2>&1
  expect_pass "rollup-out.hbb, no source tree" \
    without_node_modules "$HERMES_NODE" --bundle=rollup-out.hbb
fi

echo "PASS: babel-parser"
