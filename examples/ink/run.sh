#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the demo three ways -- from disk, from an AOT container, and as a
# standalone executable with node_modules and dist-cjs moved out of the
# way -- and checks each one the same three ways: that its
# grapheme-segmentation exhibit renders exactly the way node renders it,
# that a real 'q' keypress exits cleanly (useApp().exit()), and that a
# multi-character paste does not. The container is also checked for what
# it exists to prove: that its baked-in Wasm compile is actually used
# (a container hit, never a store) rather than merely present.
#
# The segmentation check and the keypress checks are deliberately run as
# separate captures. Ink calls stdin.setRawMode(true) from a useEffect,
# which only runs after the first frame is already on the wire -- so a
# keystroke sent the moment output starts (pty-run.py sends as soon as it
# sees any) can race the terminal driver's own echo of it into the *middle*
# of that first frame, at whatever byte offset happened to be in flight.
# That is a real difference in how the two engines flush stdout (see
# CLAUDE.md's "queued vs synchronous" note, and dz 01a08eac-0d4b for the
# general case), not a segmentation bug, and it is not the thing this
# example is trying to prove -- so the segmentation capture below sends no
# keystroke at all, which removes the race, and the keypress checks only
# grep for a fixed marker, which does not care where in the stream a stray
# echoed key landed.
#
# useInput needs raw mode, which does not exist on a pipe, so every run
# goes through ../pty-run.py, exactly as examples/tetris and examples/gtop
# do.
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
if ! command -v node >/dev/null 2>&1; then
  echo "ERROR: node not found on PATH -- it is the control this example" 1>&2
  echo "       compares hermes-node against." 1>&2
  exit 1
fi

cd "$HERE"
"$HERE/build-cjs.sh" >/dev/null
APP="$HERE/dist-cjs/app.cjs"

fail() {
  echo "FAIL: $1" 1>&2
  shift
  [ $# -gt 0 ] && printf '%s\n' "$*" | tail -c 800 1>&2
  exit 1
}

# Strips CSI escape sequences (cursor moves, line clears, bold/reset) so
# what is left is the text Ink actually drew.
strip_ansi() {
  python3 -c "
import re, sys
data = sys.stdin.buffer.read().decode('utf-8', errors='replace')
sys.stdout.write(re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', data))
"
}

# The four lines that matter for the segmentation exhibit: the title (a
# fixed anchor) and the three rows, each still wrapped in its own box
# borders. Excludes the tick/last-key line (timing-dependent) and the
# border-only rows (whose width depends on the outer box, not on
# segmentation). sort -u collapses the duplicate copies a re-render leaves
# behind -- how many ticks land inside the capture window is itself
# timing-dependent and not what this is checking.
extract_exhibit() {
  strip_ansi | tr -d '\r' | grep -E 'hermes-node \+ Ink|family|wave|hello' | sort -u
}

# The reference every run mode is checked against: node's own rendering,
# taken once, with no keystroke sent (see the header comment).
node_exhibit="$(python3 "$PTY" 1.5 80 24 -- node "$APP" 2>&1 | extract_exhibit)"
if [ -z "$node_exhibit" ] || [ "$(printf '%s\n' "$node_exhibit" | wc -l)" -lt 4 ]; then
  fail "node (the control) never rendered the segmentation exhibit"
fi

# ---- Check 1: the segmentation exhibit matches node's, exactly ----
expect_exhibit() {
  local label="$1"
  shift
  local out
  out="$(python3 "$PTY" 1.5 80 24 -- "$@" 2>&1 | extract_exhibit)"
  if [ -z "$out" ] || [ "$(printf '%s\n' "$out" | wc -l)" -lt 4 ]; then
    fail "$label: the segmentation exhibit did not render"
  fi
  if [ "$out" != "$node_exhibit" ]; then
    echo "--- $label ---" 1>&2
    printf '%s\n' "$out" 1>&2
    echo "--- node ---" 1>&2
    printf '%s\n' "$node_exhibit" 1>&2
    fail "$label: segmentation exhibit differs from node's"
  fi
  echo "  ok: $label renders the segmentation exhibit identically to node"
}

# ---- Check 2: a real 'q' keypress exits cleanly ----
expect_clean_exit() {
  local label="$1"
  shift
  local out
  out="$(python3 "$PTY" 3 80 24 --send 'q' -- "$@" 2>&1)" || true
  printf '%s' "$out" | grep -q 'EXITED CLEANLY' \
    || fail "$label: did not exit cleanly on a real 'q' keypress" "$out"
  echo "  ok: $label exits cleanly on a real 'q' keypress"
}

# ---- Check 3: a multi-character paste ("abcq") is not the same as 'q' ----
# --send delivers its whole argument as one keypress, so "abcq" !== 'q' and
# must not trigger the quit handler that check 2 just exercised. The timer
# advancing past tick 0 is what proves the process kept running rather than
# exiting for some unrelated reason.
expect_keeps_running() {
  local label="$1"
  shift
  local out
  out="$(python3 "$PTY" 2 80 24 --send 'abcq' -- "$@" 2>&1)" || true
  printf '%s' "$out" | grep -q 'EXITED CLEANLY' \
    && fail "$label: exited on 'abcq', which is not the keypress 'q'"
  printf '%s' "$out" | grep -qE 'tick [1-9]' \
    || fail "$label: the timer never advanced past tick 0" "$out"
  printf '%s' "$out" | grep -q '"abcq"' \
    || fail "$label: never registered the 'abcq' keypress via useInput"
  echo "  ok: $label keeps running (and re-rendering) on a multi-character paste"
}

echo "from disk:"
expect_exhibit "dist-cjs/app.cjs" "$HERMES_NODE" "$APP"
expect_clean_exit "dist-cjs/app.cjs" "$HERMES_NODE" "$APP"
expect_keeps_running "dist-cjs/app.cjs" "$HERMES_NODE" "$APP"

echo "hermes-node --build-bundle (with the Wasm layout engine baked in):"
OUT="$HERE/out"
rm -rf "$OUT"
if ! build_log="$("$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  fail "build-bundle.sh"
fi
[ -f "$OUT/ink.hbb" ] || fail "$OUT/ink.hbb was not produced"
# The three warnings below are expected and correct: bufferutil and
# utf-8-validate are optional native accelerators `ws` probes for and
# neither is installed, and react-devtools-core is only reachable from
# Ink's dead DEV-only import. All three are left to the run-time loader by
# design, which is what an optional-dependency probe expects. Anything
# else warned is new and worth looking at.
expected_warnings=3
actual_warnings="$(printf '%s' "$build_log" | grep -c '^warning:' || true)"
if [ "$actual_warnings" -ne "$expected_warnings" ]; then
  printf '%s' "$build_log" | grep '^warning:' 1>&2
  fail "expected exactly $expected_warnings producer warnings, saw $actual_warnings"
fi
for name in bufferutil utf-8-validate react-devtools-core; do
  printf '%s' "$build_log" | grep -q "not packaging '$name'" \
    || fail "expected a warning naming '$name'" "$build_log"
done
echo "  ok: $OUT/ink.hbb built with exactly the 3 expected warnings"

# The point of baking: the container's Wasm entry is actually consulted and
# used, not merely present. A good container run is a hit with no store
# following it -- proven from HERMES_NODE_DEBUG_NATIVE tracing, never from
# timing, matching the rest of this suite's rule for the compile cache.
# --no-compile-cache is the shipped-artifact case: no warm disk cache to
# fall back on, so a hit here can only have come from the container.
trace="$(env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE python3 "$PTY" 2 80 24 \
  --send 'q' -- "$HERMES_NODE" --no-compile-cache --bundle="$OUT/ink.hbb" 2>&1)" || true
printf '%s' "$trace" | grep -q 'wasm container hit' \
  || fail "the container's baked Wasm entry was never consulted" "$trace"
printf '%s' "$trace" | grep -q 'wasm store' \
  && fail "the container's baked Wasm entry was refused and recompiled" "$trace"
echo "  ok: --bundle hits the baked Wasm entry, with no compile"

echo "hermes-node --bundle:"
expect_exhibit "ink.hbb" "$HERMES_NODE" --bundle="$OUT/ink.hbb"
expect_clean_exit "ink.hbb" "$HERMES_NODE" --bundle="$OUT/ink.hbb"
expect_keeps_running "ink.hbb" "$HERMES_NODE" --bundle="$OUT/ink.hbb"

if [ -f "$OUT/ink" ]; then
  echo "standalone executable:"
  # The point of the executable is that neither the runtime, the package
  # tree, nor the transpiled source is needed. Moving all three away is
  # what proves it rather than asserts it.
  mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
  mv "$HERE/dist-cjs" "$HERE/.dist_cjs_hidden"
  restore_trees() {
    rm -rf "$HERE/node_modules" "$HERE/dist-cjs"
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
    mv "$HERE/.dist_cjs_hidden" "$HERE/dist-cjs"
  }
  trap restore_trees EXIT

  expect_exhibit "dist/ink" "$OUT/ink"
  expect_clean_exit "dist/ink" "$OUT/ink"
  expect_keeps_running "dist/ink" "$OUT/ink"

  restore_trees
  trap - EXIT
else
  echo "standalone executable: skipped (no link kit in $BUILD_DIR)"
fi

rm -rf "$OUT"
echo "PASS: ink"
