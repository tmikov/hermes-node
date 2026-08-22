#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Parses sample.js with the native Hermes parser addon (hermes-parser), from
# disk and then from an AOT bundle, and checks the two runs produce
# byte-identical AST output.
#
# examples/babel-parser/ast.js is the pure-JavaScript sibling of this
# example: same job, one argument, JSON on stdout. The difference is what
# each one bundles to. @babel/parser is a plain require(), so its bundle is
# one file. hermes-parser's own loader
# (external/hermes-parser-native/package/dist/HermesParserAddon.js) reaches
# its .node through three computed require() calls, tried in a try/catch
# loop over candidate paths -- nothing a static scanner can follow -- so the
# addon has to be named explicitly with --include, and the bundle it
# produces is a file plus a shared object (the addon's sidecar).
#
# The bundle is built by build-bundle.sh, into out/, which this script
# deletes on the way out. That script is also how a person gets the bundle
# to keep -- ./build-bundle.sh writes it to ./dist. The addon staging and
# the --include live there and only there, so the artifact you ship and the
# artifact this script checks are built the same way.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)

set -e

# examples/flow-bundler/run.sh exports this to point hermes-parser at a build
# tree path, so a shell that ran that example still has it set. Here it would
# make the *disk* run load an addon from somewhere this example never chose,
# and the bundled run fail with an unexplained MODULE_NOT_FOUND. The bundled
# run with it set is asserted deliberately further down; it must not leak in
# from the environment.
unset HERMES_PARSER_NATIVE_ADDON

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

OUT="$HERE/out"

# The bundle directory is removed on every run so a stale sidecar from a
# previous build can never be mistaken for this one's. Unlike
# babel-parser's node_modules, the prebuilds/ copy that build-bundle.sh
# makes is left in place: it is inside the gitignored node_modules tree
# already, costs nothing to leave, and a following run overwrites it.
cleanup() {
  rm -rf "$OUT"
}
trap cleanup EXIT

# --- BEGIN vendored native parser addon ------------------------------------
# Delete everything between these two markers when the vendored addon goes
# away and this example switches to the published hermes-parser package. See
# "When to delete this directory" in
# external/hermes-parser-native/README.md; that recipe refers to these
# markers, so keep them in sync with it. build-bundle.sh in this directory
# has a marker block of its own -- it is the one that stages the addon where
# --include can name it; this one only resolves the path, for the negative
# HERMES_PARSER_NATIVE_ADDON check further down.
ADDON="$BUILD_DIR/external/hermes-parser-native/hermes-parser.node"

if [ ! -f "$ADDON" ]; then
  echo "ERROR: missing $ADDON -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node hermes-parser-napi" 1>&2
  exit 1
fi
# --- END vendored native parser addon --------------------------------------

# Every check below asserts something specific, so a run that dies halfway
# and still exits 0 cannot be mistaken for a success.

# Node types drawn from sample.js's own content: a class private field
# (PropertyDefinition with a PrivateIdentifier key) and an optional chain
# (wrapped in a ChainExpression), in ESTree ("babel: false") form. Any old
# JSON would not contain all three.
expect_ast() {
  local label="$1"
  shift
  local out
  out="$("$@")" || { echo "FAIL: $label" 1>&2; exit 1; }
  if echo "$out" | grep -q '"type": "Program"' \
      && echo "$out" | grep -q '"PropertyDefinition"' \
      && echo "$out" | grep -q '"PrivateIdentifier"' \
      && echo "$out" | grep -q '"ChainExpression"'; then
    echo "  ok: $label"
  else
    echo "FAIL: $label" 1>&2
    exit 1
  fi
}

# Built first, not because the bundle is checked first -- it is not -- but
# because build-bundle.sh is what copies the freshly built addon into the
# package's prebuilds/ directory, and the from-disk run below has to load
# that copy rather than whatever npm install happened to leave there.
echo "hermes-node --build-bundle:"
rm -rf "$OUT"
# The producer's output (bundle root, the native sidecar note, the file
# summary) is what a person building an artifact wants to see and is noise
# in a check, so it is captured and shown only if the build fails.
if ! build_log="$("$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  echo "FAIL: build-bundle.sh" 1>&2
  exit 1
fi

if [ ! -f "$OUT/ast.hbb" ]; then
  echo "FAIL: $OUT/ast.hbb was not produced" 1>&2
  exit 1
fi
if [ ! -f "$OUT/hermes-parser.node" ]; then
  echo "FAIL: $OUT/hermes-parser.node (the native addon's sidecar) was not produced" 1>&2
  exit 1
fi
if [ -e "$OUT/node_modules" ]; then
  echo "FAIL: $OUT contains node_modules -- the bundle is not self-contained" 1>&2
  exit 1
fi
echo "  ok: $OUT holds ast.hbb + hermes-parser.node, no node_modules"

echo "from disk:"
expect_ast "ast.js sample.js" "$HERMES_NODE" "$HERE/ast.js" "$HERE/sample.js"
UNBUNDLED_OUT="$("$HERMES_NODE" "$HERE/ast.js" "$HERE/sample.js")"

# ast.js with no argument is a usage error, not silence dressed as success.
if err="$("$HERMES_NODE" "$HERE/ast.js" 2>&1 1>/dev/null)"; then
  echo "FAIL: ast.js with no argument should not exit 0" 1>&2
  exit 1
fi
if echo "$err" | grep -qi usage; then
  echo "  ok: ast.js with no argument fails with a usage message"
else
  echo "FAIL: ast.js with no argument printed no usage message" 1>&2
  exit 1
fi

echo "hermes-node --bundle:"
BUNDLED_OUT="$("$HERMES_NODE" --bundle="$OUT/ast.hbb" "$HERE/sample.js")"
if [ -z "$BUNDLED_OUT" ]; then
  echo "FAIL: bundled run produced no output" 1>&2
  exit 1
fi
if diff <(echo "$UNBUNDLED_OUT") <(echo "$BUNDLED_OUT") >/dev/null; then
  echo "  ok: bundled AST is byte-identical to the unbundled AST"
else
  echo "FAIL: bundled and unbundled ASTs differ" 1>&2
  diff <(echo "$UNBUNDLED_OUT") <(echo "$BUNDLED_OUT") 1>&2 || true
  exit 1
fi

# The override, demonstrated rather than only described. hermes-parser's
# loader does `require(path.resolve(override))` outside its try/catch, so in
# a bundle -- where an absolute build-machine path is not an identity the
# container records -- it throws and the process exits non-zero. The disk
# run above is what shows the same override is fine outside a bundle.
echo "hermes-node --bundle with HERMES_PARSER_NATIVE_ADDON set:"
if HERMES_PARSER_NATIVE_ADDON="$ADDON" \
    "$HERMES_NODE" --bundle="$OUT/ast.hbb" "$HERE/sample.js" >/dev/null 2>&1; then
  echo "FAIL: a bundled run with HERMES_PARSER_NATIVE_ADDON set should not exit 0" 1>&2
  exit 1
fi
echo "  ok: the bundled run refuses an addon path the container does not record"

echo "hermes-node --bundle --verify-natives:"
if "$HERMES_NODE" --bundle="$OUT/ast.hbb" --verify-natives; then
  echo "  ok: --verify-natives exits 0"
else
  echo "FAIL: --verify-natives reported a problem" 1>&2
  exit 1
fi

echo "PASS: hermes-parser-ast"
