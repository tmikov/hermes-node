#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Regenerates ../package/dist from ../package/src. Not run by any default
# build: dist/ is committed precisely so that building this repository
# needs no JavaScript toolchain. Run this after editing src/, then commit
# the result.

set -xe -o pipefail

THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd "$THIS_DIR/.." && pwd)/package"
INCLUDE_PATH="$1"

if [[ ! -d "$INCLUDE_PATH" ]]; then
  echo "usage: regen-dist.sh <hermes-include-path>" 1>&2
  exit 1
fi

if ! command -v npm >/dev/null; then
  echo "ERROR: npm is required to regenerate dist/." 1>&2
  exit 1
fi

# package.json's "overrides" block pins most of the @babel/* transitive
# closure, not just the direct devDependencies. Babel's own generated
# helper code (e.g. whether an exported `const` gets folded into its
# declaration, whether blank lines from src/ survive into dist/) changed
# across @babel/generator, @babel/traverse and friends between the 7.16.x
# family this was built against and later 7.x releases, even though all of
# those are semver-compatible. Caret ranges alone let npm pull whatever is
# newest today, which reliably reproduces *working* output but not the
# committed dist/ byte-for-byte. If a regeneration from an unmodified src/
# ever stops being byte-identical to the committed dist/ (aside from
# build-manifest.json's generatedAt/prebuilds fields), the fix is almost
# always a version that drifted out from under one of these pins, not the
# plugin list in babel.config.js.
(cd "$THIS_DIR" && npm install --no-audit --no-fund)

# Regenerate the hash that guards against ESTree.def drift.
node "$THIS_DIR/genKindHash.js" "$INCLUDE_PATH"

DIST_DIR="$PACKAGE_DIR/dist"
rm -rf "$DIST_DIR"
cp -r "$PACKAGE_DIR/src" "$DIST_DIR"

find "$DIST_DIR" -type f -name "*.js" | while read -r file; do
  if grep -q " @flow" "$file"; then
    [ -f "${file}.flow" ] || cp "$file" "${file}.flow"
  fi
done

rsync -a --include="*/" --include="*.js" --exclude="*" \
  "$PACKAGE_DIR/src" "$DIST_DIR"

"$THIS_DIR/node_modules/.bin/babel" \
  --config-file="$THIS_DIR/babel.config.js" \
  "$DIST_DIR" --out-dir="$DIST_DIR"

# Written last on purpose: a run that dies earlier leaves no manifest, and
# a missing manifest reads as stale rather than as up to date.
node "$THIS_DIR/distManifest.js" "$PACKAGE_DIR"
