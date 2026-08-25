#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Recompiles the ditz2 submodule as CommonJS into ./dist-cjs.
#
# This step exists because hermes-node has no ES module loader yet, and ditz2
# is published as ESM ("type": "module"). Running its shipped dist/ here fails
# in the CJS loader's ESM hand-off:
#
#   TypeError: undefined is not a function
#       at getOrInitializeCascadedLoader (internal/modules/esm/loader)
#
# Nothing in ditz2's source is patched to make this work -- the recompile
# changes two tsconfig settings and nothing else (see tsconfig.cjs.json). It
# is a plain `tsc` over the submodule's own sources, so what runs in all three
# modes is ditz2's real logic and not a port of it. The source happens to
# contain no `import.meta` and no top-level await, either of which would have
# made this a rewrite instead.
#
# Delete this script the day the ESM loader lands; the example should then
# point straight at the submodule's dist/.
#
# Usage: ./build-cjs.sh

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [ ! -f "$HERE/ditz2/package.json" ]; then
  echo "ERROR: the ditz2 submodule is not checked out. From the repo root:" 1>&2
  echo "  git submodule update --init examples/ditz2/ditz2" 1>&2
  exit 1
fi

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

rm -rf "$HERE/dist-cjs"
./node_modules/.bin/tsc -p tsconfig.cjs.json

# dist-cjs/ needs no package.json of its own: the nearest one walking up is
# this example's, which is "type": "commonjs". The submodule's ESM-declaring
# package.json is below this directory, not above dist-cjs, so it does not
# apply to the emitted files.

echo "wrote $HERE/dist-cjs ($(find "$HERE/dist-cjs" -name '*.js' | wc -l | tr -d ' ') files)"
