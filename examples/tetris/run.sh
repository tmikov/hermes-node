#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the game three ways -- from disk, from an AOT bundle, and as a
# standalone executable with node_modules moved out of the way -- and checks
# each one actually draws a playing field.
#
# A TUI will not start without a terminal: tetris-cli calls
# stdin.setRawMode() on its fifth line, which is undefined when stdin is a
# pipe. So each run goes through ../pty-run.py, which gives it a real pty of
# a known size. script(1) would also do it, but its arguments differ between
# Linux and macOS and this has to pass on both.
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
PTY="$HERE/../pty-run.py"

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
cleanup() {
  rm -rf "$OUT"
  # Only ever restore a tree this script moved itself.
  if [ -d "$HERE/.node_modules_hidden" ]; then
    rm -rf "$HERE/node_modules"
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
}
trap cleanup EXIT

# The field's floor and the key legend: both come from the game's own draw
# routine, so neither can appear unless it started and rendered. 's' starts
# it, which is also what makes a piece appear.
expect_game() {
  local label="$1"
  shift
  local out
  out="$(python3 "$PTY" 3 100 30 --send 's' -- "$@" 2>/dev/null)" || true
  if printf '%s' "$out" | grep -q 'SPACE: drop' \
      && printf '%s' "$out" | grep -q 'rotate'; then
    echo "  ok: $label"
  else
    echo "FAIL: $label drew no playing field" 1>&2
    printf '%s' "$out" | tail -c 400 1>&2
    exit 1
  fi
}

echo "from disk:"
expect_game "play.js" "$HERMES_NODE" "$HERE/play.js"

echo "hermes-node --build-bundle:"
rm -rf "$OUT"
if ! build_log="$("$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  echo "FAIL: build-bundle.sh" 1>&2
  exit 1
fi
if [ ! -f "$OUT/tetris.hbb" ]; then
  echo "FAIL: $OUT/tetris.hbb was not produced" 1>&2
  exit 1
fi
# The producer emits no warnings for this graph. If that ever changes it is
# worth noticing, because it is the reason this example is the simple one.
if printf '%s' "$build_log" | grep -q '^warning:'; then
  echo "FAIL: the producer warned; this graph is supposed to be fully static" 1>&2
  printf '%s' "$build_log" | grep '^warning:' 1>&2
  exit 1
fi
echo "  ok: $OUT/tetris.hbb built with no producer warnings"

echo "hermes-node --bundle:"
expect_game "tetris.hbb" "$HERMES_NODE" --bundle="$OUT/tetris.hbb"

if [ -f "$OUT/tetris" ]; then
  echo "standalone executable:"
  # The point of the executable is that neither the runtime nor the package
  # tree is needed. Moving node_modules away is what proves it rather than
  # asserts it; the trap above puts it back however this exits.
  mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
  expect_game "dist/tetris with node_modules moved away" "$OUT/tetris"
  rm -rf "$HERE/node_modules"
  mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
else
  echo "standalone executable: skipped (no link kit in $BUILD_DIR)"
fi

echo "PASS: tetris"
