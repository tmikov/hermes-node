#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds the artifacts with build-bundle.sh, then runs each one ALONE -- in
# an empty directory holding only the artifact and a copy of fixture/src, no
# node_modules, no bundler/, no build.config.js -- and checks it produces
# the six bundles in expected/.
#
# It also checks that the WebAssembly parser was baked rather than compiled
# (from compile-cache tracing, never from timing), and that the container
# holds the WebAssembly parser and not the native one.
#
# Not part of run.sh, and so not part of check-hermes-node-examples:
# build-native alone takes minutes here. run.sh is the cheap check.
#
# Usage: ./check-artifacts.sh [build-dir]     (default: cmake-build-release)

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"
if [ -d "$BUILD_DIR" ]; then
  BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
fi

HERMES_NODE="$BUILD_DIR/bin/hermes-node"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="$WORK/dist"

"$HERE/build-bundle.sh" "$OUT" "$BUILD_DIR"
echo

# The staging redirect, checked directly. A Wasm hit below says the Wasm
# parser ran; this says the native one is not even there.
dump="$("$HERMES_NODE" --bundle="$OUT/flow-bundler.hbb" --dump)"
if ! printf '%s\n' "$dump" | grep -q 'node_modules/hermes-parser/dist/HermesParserWASM\.js'; then
  echo "FAIL: flow-bundler.hbb does not contain the WebAssembly parser" 1>&2
  exit 1
fi
if printf '%s\n' "$dump" | grep -q 'HermesParserAddon\.js\|^NATIVES'; then
  echo "FAIL: flow-bundler.hbb contains the native parser" 1>&2
  exit 1
fi
echo "ok: flow-bundler.hbb holds the WebAssembly parser and no native addon"

failed=0

# check_one <label> <artifact file> <command...>
# The command runs inside the artifact's directory; @DIR@ in it is replaced
# by that directory.
check_one() {
  local label="$1" artifact="$2"
  shift 2
  local dir="$WORK/run-$label"
  mkdir -p "$dir/fixture"
  cp "$artifact" "$dir/"
  cp -R "$HERE/fixture/src" "$dir/fixture/src"

  local cmd=("${@//@DIR@/$dir}")
  local log="$dir.log"
  local status=0
  ( cd "$dir" && HERMES_NODE_DISABLE_COMPILE_CACHE=1 \
      HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE "${cmd[@]}" ) > "$log" 2>&1 || status=$?

  local problems=()
  if [ "$status" -ne 0 ]; then
    problems+=("exited $status")
  fi
  local f name
  for f in "$HERE"/expected/*.js; do
    name="$(basename "$f")"
    if ! cmp -s "$f" "$dir/out/$name"; then
      problems+=("$name differs from expected/")
    fi
  done
  local hits stores
  hits="$(grep -c 'wasm container hit' "$log" || true)"
  stores="$(grep -c 'wasm store' "$log" || true)"
  if [ "$hits" -ne 1 ] || [ "$stores" -ne 0 ]; then
    problems+=("expected 1 wasm container hit and 0 wasm store, got $hits and $stores")
  fi

  if [ "${#problems[@]}" -eq 0 ]; then
    echo "ok: $label -- 6 bundles match expected/, Wasm parser baked"
  else
    echo "FAIL: $label" 1>&2
    printf '  %s\n' "${problems[@]}" 1>&2
    tail -n 20 "$log" | sed 's/^/  | /' 1>&2
    failed=1
  fi
}

check_one hbb "$OUT/flow-bundler.hbb" \
  "$HERMES_NODE" --bundle=@DIR@/flow-bundler.hbb -- -c @DIR@/build.config.js

for exe in flow-bundler flow-bundler-native; do
  if [ -f "$OUT/$exe" ]; then
    check_one "$exe" "$OUT/$exe" @DIR@/"$exe" -c @DIR@/build.config.js
  else
    echo "skipped: $exe (not built; see build-bundle.sh's notes above)"
  fi
done

if [ "$failed" -ne 0 ]; then
  echo "FAIL: flow-bundler artifacts" 1>&2
  exit 1
fi
echo "PASS: flow-bundler artifacts"
