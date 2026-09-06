#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Regenerates ../package/dist from ../package/src plus an emscripten-built
# parser. Not run by any default build: dist/ is committed precisely so that
# building this repository needs no JavaScript toolchain and no emsdk. Run
# this after editing src/ or rebuilding the wasm, then commit the result.
#
# The wasm parser is built out of tree, because it needs emscripten and a
# Hermes checkout rather than anything in this repository:
#
#   source /path/to/emsdk/emsdk_env.sh
#   emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm --target hermes-parser-wasm
#   regen-dist.sh "$PWD/build-wasm/bin/hermes-parser-wasm.js"
#
# See ../README.md for which Hermes commit the committed dist/ was built from.

set -xe -o pipefail

THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd "$THIS_DIR/.." && pwd)/package"
WASM_PARSER="$1"

if [[ ! -f "$WASM_PARSER" ]]; then
  echo "usage: regen-dist.sh <path-to-hermes-parser-wasm.js>" 1>&2
  exit 1
fi

if ! command -v npm >/dev/null; then
  echo "ERROR: npm is required to regenerate dist/." 1>&2
  exit 1
fi

# npm ci, not npm install: with a committed lockfile, `npm install` will
# silently re-resolve and rewrite it if package.json and the lockfile ever
# drift, which defeats reproducible output. `npm ci` fails loudly instead.
(cd "$THIS_DIR" && npm ci --no-audit --no-fund)

DIST_DIR="$PACKAGE_DIR/dist"
rm -rf "$DIST_DIR"
cp -r "$PACKAGE_DIR/src" "$DIST_DIR"

find "$DIST_DIR" -type f -name "*.js" | while read -r file; do
  if grep -q " @flow" "$file"; then
    [ -f "${file}.flow" ] || cp "$file" "${file}.flow"
  fi
done

# Note the missing trailing slash on the source: this copies the `src`
# directory itself into dist/, producing dist/src/. That looks like a
# mistake and is not one to correct here -- it is what upstream's build.sh
# does, so the published package has a dist/src/ too, and matching it keeps
# this dist/ diffable against npm's.
rsync -a --include="*/" --include="*.js" --exclude="*" \
  "$PACKAGE_DIR/src" "$DIST_DIR"

# Strip Flow with flow-remove-types before Babel, which is what upstream's
# build.sh does for its BOOTSTRAP_PACKAGES -- and hermes-parser is one of
# them. Upstream's reason is a bootstrap cycle (the parser cannot use
# babel-plugin-syntax-hermes-parser to build itself); ours is fidelity.
# flow-remove-types blanks removed annotations to whitespace rather than
# reprinting the file, so comments survive verbatim -- including the MIT
# license header on every file, which Babel's flow-strip-types drops. That
# is visible in npm's published dist/, whose headers read " *  strict"
# where "@flow strict" was blanked, and it is how this dist/ stays
# comparable to the published one.
find "$DIST_DIR" -type f -name "*.js" | while read -r file; do
  "$THIS_DIR/node_modules/.bin/flow-remove-types" --quiet --pretty \
    --remove-empty-imports --out-file "$file.stripped" "$file"
  mv "$file.stripped" "$file"
done

"$THIS_DIR/node_modules/.bin/babel" \
  --config-file="$THIS_DIR/babel.config.js" \
  "$DIST_DIR" --out-dir="$DIST_DIR"

# The emscripten output goes in verbatim under a header, exactly as
# upstream's genWasmParser.js does it. It is never transpiled: it is
# generated code that already targets the environments it supports, and
# running Babel over 900 KB of it would cost minutes and change nothing.
node "$THIS_DIR/genWasmParser.js" "$WASM_PARSER"

# Written last on purpose: a run that dies earlier leaves no manifest, and
# a missing manifest reads as stale rather than as up to date.
node "$THIS_DIR/distManifest.js" "$PACKAGE_DIR"
