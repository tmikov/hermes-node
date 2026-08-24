#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the monitor three ways -- from disk, from an AOT bundle, and as a
# standalone executable with node_modules moved out of the way -- and checks
# each one draws its dashboard.
#
# blessed will not lay out without a window size, so every run goes through
# ../pty-run.py, which gives it a real pty of a known size. See
# examples/tetris/run.sh for why that is not script(1).
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
  if [ -d "$HERE/.node_modules_hidden" ]; then
    rm -rf "$HERE/node_modules"
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
}
trap cleanup EXIT

# Panels *and* the numbers in them. Titles alone are not enough: gtop's
# module body builds the whole layout, so every panel draws whether or not
# anything ever fills it -- an empty dashboard looks like a working one at a
# glance, and did, until monitor.js started calling init(). The percentages
# come from the CPU list and the donuts, and only exist once a monitor has
# ticked, so requiring them is what distinguishes a running dashboard from a
# drawn one.
#
# Eight seconds because the monitors tick at 1s and the process table at 3s,
# and a loaded machine can miss the first tick.
expect_dashboard() {
  local label="$1"
  shift
  local out missing="" pcts
  out="$(python3 "$PTY" 8 160 48 -- "$@" 2>/dev/null)" || true
  for panel in CPU Memory Swap Network Processes; do
    printf '%s' "$out" | grep -q "$panel" || missing="$missing $panel"
  done
  if [ -n "$missing" ]; then
    echo "FAIL: $label is missing panels:$missing" 1>&2
    printf '%s' "$out" | tail -c 400 1>&2
    exit 1
  fi
  pcts="$(printf '%s' "$out" | tr -cd '%' | wc -c)"
  if [ "$pcts" -lt 5 ]; then
    echo "FAIL: $label drew its panels but no data ($pcts percent signs)" 1>&2
    echo "      the layout renders even when no monitor is running -- check" 1>&2
    echo "      that monitor.js calls init()" 1>&2
    exit 1
  fi
  echo "  ok: $label ($pcts readings)"
}

echo "from disk:"
expect_dashboard "monitor.js" "$HERMES_NODE" "$HERE/monitor.js"

echo "hermes-node --build-bundle:"
rm -rf "$OUT"
if ! build_log="$("$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  echo "FAIL: build-bundle.sh" 1>&2
  exit 1
fi
if [ ! -f "$OUT/gtop.hbb" ]; then
  echo "FAIL: $OUT/gtop.hbb was not produced" 1>&2
  exit 1
fi
if [ ! -f "$OUT/node_modules/blessed/usr/xterm" ]; then
  echo "FAIL: blessed's terminfo was not staged beside the bundle" 1>&2
  exit 1
fi
# Only data may travel beside the bundle. A .js file here would mean some
# module was left on disk rather than packaged, and the artifact would be
# self-contained only by accident of running from its build tree.
if find "$OUT/node_modules" -name '*.js' | grep -q .; then
  echo "FAIL: JavaScript was staged beside the bundle; it belongs inside" 1>&2
  find "$OUT/node_modules" -name '*.js' 1>&2
  exit 1
fi
echo "  ok: $OUT holds gtop.hbb plus terminfo data, and no JavaScript"

echo "hermes-node --bundle:"
expect_dashboard "gtop.hbb" "$HERMES_NODE" --bundle="$OUT/gtop.hbb"

if [ -f "$OUT/gtop" ]; then
  echo "standalone executable:"
  mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
  expect_dashboard "dist/gtop with node_modules moved away" "$OUT/gtop"
  rm -rf "$HERE/node_modules"
  mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
else
  echo "standalone executable: skipped (no link kit in $BUILD_DIR)"
fi

echo "PASS: gtop"
