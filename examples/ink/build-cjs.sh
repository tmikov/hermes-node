#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Bundles app.mjs and its dependencies (Ink, React, yoga-layout) into a
# single CommonJS file at ./dist-cjs/app.cjs, with esbuild.
#
# This step exists because Ink is ESM ("type": "module") and hermes-node
# has no ES module loader yet, and because two packages in this dependency
# tree use a top-level await, which esbuild will not lower to CommonJS on
# its own. build.mjs (run with plain node, not hermes-node -- this is a
# build-time step) handles both: an onLoad plugin drops one top-level
# await in ink/build/reconciler.js (dead code, gated on an env var this
# example never sets), and yoga-layout's is routed around by aliasing the
# package to yoga-shim.mjs. See build.mjs and yoga-shim.mjs for the detail,
# and ../../docs/notes/2026-08-24-ink-findings.md for how this was found.
#
# Usage: ./build-cjs.sh

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

rm -rf "$HERE/dist-cjs"
node build.mjs
