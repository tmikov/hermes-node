#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the examples that have a run.sh and can verify themselves. Examples
# needing a network install are skipped when they have not been installed,
# so this stays usable offline.
#
# Usage: ./run-examples.sh <build-dir>

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$1"

if [ -z "$BUILD_DIR" ]; then
  echo "usage: run-examples.sh <build-dir>" 1>&2
  exit 1
fi

ran=0
skipped=0

# The bundler takes over ten minutes under ASAN, which makes it useless as
# a check. Sanitizer coverage of the addon comes from the unit tests in
# check-hermes-node instead.
if [ -f "$BUILD_DIR/CMakeCache.txt" ] &&
   grep -q "^HERMES_ENABLE_ADDRESS_SANITIZER:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
  echo "SKIP all examples: $BUILD_DIR is an ASAN build (too slow)."
  echo "Use a Release build: cmake --build cmake-build-release --target check-hermes-node-examples"
  exit 0
fi

for runner in "$HERE"/*/run.sh; do
  [ -f "$runner" ] || continue
  dir="$(dirname "$runner")"
  name="$(basename "$dir")"
  if [ ! -d "$dir/node_modules" ]; then
    echo "SKIP $name: not installed (run 'npm install' in $dir)"
    skipped=$((skipped + 1))
    continue
  fi
  echo "RUN  $name"
  "$runner" "$BUILD_DIR"
  ran=$((ran + 1))
done

echo "examples: $ran ran, $skipped skipped"
