#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs ditz2 three ways -- from disk, from an AOT bundle, and as a standalone
# executable with node_modules and dist-cjs moved out of the way -- and drives
# a real workflow through each one, checking what it wrote rather than just
# that it exited 0.
#
# No pty is needed here, unlike the two TUI examples: ditz2 is a plain CLI
# that reads no keys and draws no screen.
#
# DZ_AUTHOR is set so the run does not depend on the machine having a git
# identity configured. Without it ditz2 probes git and Sapling, and `doctor`
# reports NO_AUTHOR on a machine that has neither -- a real diagnostic, but
# not what this example is testing.
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
if [ ! -f "$HERE/ditz2/package.json" ]; then
  echo "ERROR: the ditz2 submodule is not checked out. From the repo root:" 1>&2
  echo "  git submodule update --init examples/ditz2/ditz2" 1>&2
  exit 1
fi

export DZ_AUTHOR="Example Runner <runner@example.com>"

OUT="$HERE/out"
WORK=""
cleanup() {
  rm -rf "$OUT"
  [ -n "$WORK" ] && rm -rf "$WORK"
  # Only ever restore trees this script moved itself.
  if [ -d "$HERE/.node_modules_hidden" ]; then
    rm -rf "$HERE/node_modules"
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
  if [ -d "$HERE/.dist_cjs_hidden" ]; then
    rm -rf "$HERE/dist-cjs"
    mv "$HERE/.dist_cjs_hidden" "$HERE/dist-cjs"
  fi
}
trap cleanup EXIT

fail() {
  echo "FAIL: $1" 1>&2
  shift
  [ $# -gt 0 ] && printf '%s\n' "$*" | tail -c 600 1>&2
  exit 1
}

# Drives a full workflow and checks the tracker actually recorded it. Every
# assertion is on ditz2's own output or on the files it wrote, so none of it
# can pass without the program having really run.
expect_workflow() {
  local label="$1"
  shift
  local dz=("$@")
  local proj
  proj="$(mktemp -d)"
  WORK="$proj"
  (
    cd "$proj"
    "${dz[@]}" init --name demo >/dev/null
    "${dz[@]}" add "Parser drops trailing newline" --type bug -m "repro here" >/dev/null
    "${dz[@]}" add "Add --json flag to list" --type feature >/dev/null
  ) || fail "$label: init/add exited non-zero"

  local listing
  listing="$(cd "$proj" && "${dz[@]}" list 2>&1)" || fail "$label: list exited non-zero"
  printf '%s' "$listing" | grep -q 'Parser drops trailing newline' \
    || fail "$label: list did not show the bug" "$listing"
  printf '%s' "$listing" | grep -q 'Add --json flag to list' \
    || fail "$label: list did not show the feature" "$listing"

  # One issue file per `add`, which is the whole point of this tracker.
  local count
  count="$(find "$proj/dz/issues" -name '*.md' | wc -l | tr -d ' ')"
  [ "$count" = "2" ] || fail "$label: expected 2 issue files, found $count"

  # Close one by id prefix and confirm the status really changed on disk.
  local id
  id="$(cd "$proj" && "${dz[@]}" list --json 2>/dev/null \
        | grep -o '"id": "[0-9a-f-]*"' | head -1 | cut -d'"' -f4)"
  [ -n "$id" ] || fail "$label: --json listing produced no id"
  (cd "$proj" && "${dz[@]}" close "$id" --as fixed >/dev/null) \
    || fail "$label: close exited non-zero"
  local closed
  closed="$(cd "$proj" && "${dz[@]}" list --all 2>&1)"
  printf '%s' "$closed" | grep -q 'closed' \
    || fail "$label: the issue did not come back closed" "$closed"

  # doctor is ditz2's own consistency check over what the run produced.
  local report
  report="$(cd "$proj" && "${dz[@]}" doctor 2>&1)" \
    || fail "$label: doctor reported problems" "$report"
  printf '%s' "$report" | grep -q 'no problems found' \
    || fail "$label: doctor did not certify the project" "$report"

  rm -rf "$proj"
  WORK=""
  echo "  ok: $label"
}

echo "transpiling the submodule to CommonJS:"
"$HERE/build-cjs.sh" | sed 's/^/  /'

echo "from disk:"
expect_workflow "dist-cjs/cli/main.js" "$HERMES_NODE" "$HERE/dist-cjs/cli/main.js"

echo "hermes-node --build-bundle:"
rm -rf "$OUT"
if ! build_log="$("$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  fail "build-bundle.sh"
fi
[ -f "$OUT/ditz2.hbb" ] || fail "$OUT/ditz2.hbb was not produced"
# The producer emits no warnings for this graph. If that ever changes it is
# worth noticing: it means a dynamic require appeared, and the closed world
# would need an --include to cover it.
if printf '%s' "$build_log" | grep -q '^warning:'; then
  printf '%s' "$build_log" | grep '^warning:' 1>&2
  fail "the producer warned; this graph is supposed to be fully static"
fi
echo "  ok: $OUT/ditz2.hbb built with no producer warnings"

echo "hermes-node --bundle:"
expect_workflow "ditz2.hbb" "$HERMES_NODE" --bundle="$OUT/ditz2.hbb"

if [ -f "$OUT/dz" ]; then
  echo "standalone executable:"
  # The point of the executable is that neither the runtime nor the package
  # tree nor the transpiled source is needed. Moving all three away is what
  # proves it rather than asserts it; the trap above puts them back however
  # this exits.
  mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
  mv "$HERE/dist-cjs" "$HERE/.dist_cjs_hidden"
  expect_workflow "dist/dz with node_modules and dist-cjs moved away" "$OUT/dz"
  rm -rf "$HERE/node_modules" "$HERE/dist-cjs"
  mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  mv "$HERE/.dist_cjs_hidden" "$HERE/dist-cjs"
else
  echo "standalone executable: skipped (no link kit in $BUILD_DIR)"
fi

echo "PASS: ditz2"
