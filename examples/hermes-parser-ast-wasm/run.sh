#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Parses sample.js with the WebAssembly build of the Hermes parser four ways
# -- from disk, from an AOT bundle, as a build-native executable whose Wasm
# module is compiled to native code, and (when the sibling example is
# installed) with the native addon build of the same parser -- and checks
# every run produces byte-identical AST output.
#
# This example is the third corner of a triangle. All three parse a file and
# print an ESTree AST; they differ in what the parser is and therefore in
# what a bundle of them contains:
#
#   babel-parser         pure JavaScript      one file
#   hermes-parser-ast    native .node addon   one file + a sidecar .so
#   hermes-parser-wasm   WebAssembly          one file
#
# The first and third look alike from outside and are not alike inside: this
# one carries a 665 KB wasm module, base64-encoded inside an ordinary
# JavaScript file, which hermes-node compiles to Hermes bytecode at run time.
# So the bundle is one file the way the pure-JavaScript one is, while the
# thing doing the parsing is the same C++ that the native addon runs.
#
# When the sibling hermes-parser-ast example is installed too, the last
# check compares the two parsers' output directly. That is the assertion
# worth having: the two vendored parsers are meant to be interchangeable,
# and nothing else in the tree checks it end to end.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"
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
OUT_NATIVE="$HERE/out-native"

cleanup() {
  rm -rf "$OUT" "$OUT_NATIVE"
  # Only ever restore a tree this script moved itself.
  if [ -d "$HERE/.node_modules_hidden" ]; then
    rm -rf "$HERE/node_modules"
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
}
trap cleanup EXIT

# Asserted rather than assumed, because everything below fails confusingly
# without it and the cause is a build option rather than anything here.
echo "runtime:"
if "$HERMES_NODE" -e 'if (typeof WebAssembly !== "object") process.exit(1)'; then
  echo "  ok: hermes-node has WebAssembly"
else
  echo "FAIL: $HERMES_NODE has no WebAssembly (build with -DHERMES_ENABLE_WASM=ON)" 1>&2
  exit 1
fi

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

echo "hermes-node --build-bundle:"
rm -rf "$OUT"
if ! build_log="$("$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  echo "FAIL: build-bundle.sh" 1>&2
  exit 1
fi

if [ ! -f "$OUT/ast.hbb" ]; then
  echo "FAIL: $OUT/ast.hbb was not produced" 1>&2
  exit 1
fi

# The single-file property, asserted rather than described. This is the one
# check the sibling native example cannot make, and it is what would break
# first if the wasm module ever stopped travelling inside the container --
# if HermesParserWASM.js grew a real .wasm file beside it, say.
file_count="$(find "$OUT" -type f | wc -l)"
if [ "$file_count" -ne 1 ]; then
  echo "FAIL: expected the bundle to be exactly one file, found $file_count:" 1>&2
  find "$OUT" -type f 1>&2
  exit 1
fi
echo "  ok: the bundle is one file, $(wc -c < "$OUT/ast.hbb") bytes, no sidecar"

# Says the same thing from the container's side rather than the filesystem's.
if "$HERMES_NODE" --bundle="$OUT/ast.hbb" --verify-natives 2>&1 |
    grep -qi "no native addons recorded"; then
  echo "  ok: the container records no native addons"
else
  echo "FAIL: --verify-natives did not report an addon-free container" 1>&2
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

# A native build compiles the module with shermes instead of to bytecode, and
# the producer only learns the module's bytes from a run: hermes-parser
# decodes them from a string at run time, where no static scan can see them.
# So this is two steps -- record, then build from the recording -- and the
# checks are that exactly one module was compiled natively, that the run is
# served by that native unit rather than by a bytecode compile, and that
# nothing on either side warned. A warning here would be the artifact saying
# it fell back to bytecode, which is the one thing this arm exists to rule
# out.
if [ -x "$BUILD_DIR/kit/shermes" ]; then
  echo "hermes-node build-native:"
  # The record writer creates no parent directories.
  rm -rf "$OUT_NATIVE" && mkdir -p "$OUT_NATIVE"
  if ! "$HERMES_NODE" --no-compile-cache --record-wasm="$OUT_NATIVE/ast.wasmrec" \
      "$HERE/ast.js" "$HERE/sample.js" > /dev/null; then
    echo "FAIL: the recording run" 1>&2
    exit 1
  fi
  if ! build_log="$("$HERMES_NODE" build-native "$HERE/ast.js" -o "$OUT_NATIVE/ast" \
      --bake-wasm="$OUT_NATIVE/ast.wasmrec" --kit="$BUILD_DIR/kit" 2>&1)"; then
    echo "$build_log" 1>&2
    echo "FAIL: build-native" 1>&2
    exit 1
  fi
  if printf '%s' "$build_log" | grep -q 'warning:'; then
    echo "FAIL: build-native warned" 1>&2
    printf '%s\n' "$build_log" | grep 'warning:' 1>&2
    exit 1
  fi
  native_lines="$(printf '%s\n' "$build_log" | grep -c '^wasm: native ' || true)"
  if [ "$native_lines" -ne 1 ]; then
    echo "FAIL: expected exactly one 'wasm: native' line, found $native_lines" 1>&2
    printf '%s\n' "$build_log" 1>&2
    exit 1
  fi
  echo "  ok: $(printf '%s\n' "$build_log" | grep '^wasm: native '), no producer warnings"

  echo "build-native executable:"
  # Moving node_modules away proves the artifact needs neither it nor the
  # runtime; the trap above puts it back however this exits. The compile
  # cache is off so a hit cannot come from a disk entry an earlier run left.
  mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
  NATIVE_ERR="$OUT_NATIVE/stderr"
  if ! NATIVE_OUT="$(HERMES_NODE_DISABLE_COMPILE_CACHE=1 \
      HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE \
      "$OUT_NATIVE/ast" "$HERE/sample.js" 2>"$NATIVE_ERR")"; then
    cat "$NATIVE_ERR" 1>&2
    echo "FAIL: the build-native executable" 1>&2
    exit 1
  fi
  rm -rf "$HERE/node_modules"
  mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  if diff <(echo "$UNBUNDLED_OUT") <(echo "$NATIVE_OUT") >/dev/null; then
    echo "  ok: native AST is byte-identical to the unbundled AST"
  else
    echo "FAIL: native and unbundled ASTs differ" 1>&2
    diff <(echo "$UNBUNDLED_OUT") <(echo "$NATIVE_OUT") | head -40 1>&2 || true
    exit 1
  fi
  if ! grep -q 'wasm native hit' "$NATIVE_ERR"; then
    echo "FAIL: the module was not served by its native unit" 1>&2
    cat "$NATIVE_ERR" 1>&2
    exit 1
  fi
  if grep -q 'warning:' "$NATIVE_ERR"; then
    echo "FAIL: the build-native executable warned" 1>&2
    grep 'warning:' "$NATIVE_ERR" 1>&2
    exit 1
  fi
  echo "  ok: served by the native unit (wasm native hit), no warnings"
else
  echo "build-native executable: skipped (no shermes in $BUILD_DIR/kit)"
fi

# Opportunistic, and skipped rather than failed when the sibling is not
# installed: examples/ is offline by default and nothing here should force
# a second npm install. When it is installed this is the check that the two
# vendored parsers really are interchangeable.
SIBLING="$ROOT/examples/hermes-parser-ast"
echo "cross-check against the native addon:"
if [ -d "$SIBLING/node_modules" ]; then
  # The sibling's addon override must not leak in from a shell that ran
  # examples/flow-bundler; the sibling's own run.sh unsets it for the same
  # reason.
  NATIVE_OUT="$(unset HERMES_PARSER_NATIVE_ADDON; \
    "$HERMES_NODE" "$SIBLING/ast.js" "$HERE/sample.js")"
  if diff <(echo "$UNBUNDLED_OUT") <(echo "$NATIVE_OUT") >/dev/null; then
    echo "  ok: wasm and native parsers produce byte-identical ASTs"
  else
    echo "FAIL: wasm and native parsers disagree" 1>&2
    diff <(echo "$UNBUNDLED_OUT") <(echo "$NATIVE_OUT") | head -40 1>&2 || true
    exit 1
  fi
else
  echo "  skip: examples/hermes-parser-ast is not installed"
fi

echo "PASS: hermes-parser-ast-wasm"
