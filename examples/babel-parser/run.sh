#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the Babel examples under hermes-node: the scripts from disk, then
# their AOT bundles, checked with the source tree hidden. A bundle is a
# closed world for modules -- it never reads a module off the disk -- so
# hiding the tree is what tells a self-contained container from one that is
# not.
#
# transform.js names its preset in a string, which no static bundler can
# follow, and is bundled with --include=@babel/preset-env. transform-static.js
# requires the preset instead and needs nothing. Both end up self-contained,
# by the two different routes, and that pair is the point of this example.
#
# The bundles are built by build-bundle.sh, into a temporary directory this
# script deletes on the way out. That script is also how a person gets the
# bundles to keep -- ./build-bundle.sh writes them to ./dist. The flags live
# there and only there, so the artifact you ship and the artifact this
# script checks are built the same way.
#
# ast.js takes a file to parse as an argument. It shows the other half of
# "closed world": the bundle still won't read a *module* off disk, but the
# program's own input keeps coming from wherever its argument points, tree
# hidden or not.
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

# The bundles go somewhere temporary rather than into the example: what a
# person keeps comes from ./build-bundle.sh with no argument, and a check
# should not leave artifacts behind that look like it.
BUNDLES="$(mktemp -d)"

# without_node_modules() below moves the tree aside, so an interrupted run
# would otherwise leave the example uninstalled. Restoring it here costs
# nothing and is the difference between a failed check and a broken
# checkout. The `if` rather than `&&` keeps the trap's own exit status out
# of the script's.
cleanup() {
  rm -rf "$BUNDLES"
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

# Sibling to expect_pass, for ast.js: it has no PASS line, since its whole
# output is an AST. This checks that the output really is one -- the File
# node, plus two node types known to appear in the input -- rather than any
# old JSON. Every call below points ast.js at parse.js, so those two types
# are fixed.
expect_ast() {
  local label="$1"
  shift
  local out
  out="$("$@")" || { echo "FAIL: $label" 1>&2; exit 1; }
  if echo "$out" | grep -q '"type": "File"' \
      && echo "$out" | grep -q '"VariableDeclaration"' \
      && echo "$out" | grep -q '"ExpressionStatement"'; then
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
expect_ast "ast.js parse.js" "$HERMES_NODE" ast.js parse.js

echo "hermes-node --build-bundle:"
# The producer's warnings are expected here and explained in README.md, so
# the output is captured and shown only if the build actually fails.
if ! build_log="$("$HERE/build-bundle.sh" "$BUNDLES" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  echo "FAIL: build-bundle.sh" 1>&2
  exit 1
fi
expect_pass "parse.hbb, no source tree" \
  without_node_modules "$HERMES_NODE" --bundle="$BUNDLES/parse.hbb"
expect_pass "transform-static.hbb, no source tree" \
  without_node_modules "$HERMES_NODE" --bundle="$BUNDLES/transform-static.hbb"
expect_pass "transform.hbb (--include), no source tree" \
  without_node_modules "$HERMES_NODE" --bundle="$BUNDLES/transform.hbb"
# The point of this one: modules come from the container (node_modules is
# hidden), and the input file -- parse.js, an ordinary argument -- still
# comes from the disk.
expect_ast "ast.hbb, no source tree, parse.js from disk" \
  without_node_modules "$HERMES_NODE" --bundle="$BUNDLES/ast.hbb" parse.js

# ast.js with no argument is a usage error, not silence dressed as success.
if err="$("$HERMES_NODE" ast.js 2>&1 1>/dev/null)"; then
  echo "FAIL: ast.js with no argument should not exit 0" 1>&2
  exit 1
fi
if echo "$err" | grep -qi usage; then
  echo "  ok: ast.js with no argument fails with a usage message"
else
  echo "FAIL: ast.js with no argument printed no usage message" 1>&2
  exit 1
fi

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
  # bytecode blob instead of 574, which is both smaller and faster to load.
  # Not in build-bundle.sh: rollup is a devDependency and running it needs a
  # Node install, so this is a comparison rather than an artifact of the
  # example. It also needs no flags, so there is nothing here to drift.
  "$HERMES_NODE" --build-bundle="$BUNDLES/rollup-out.hbb" rollup-out.cjs >/dev/null 2>&1
  expect_pass "rollup-out.hbb, no source tree" \
    without_node_modules "$HERMES_NODE" --bundle="$BUNDLES/rollup-out.hbb"
fi

echo "PASS: babel-parser"
