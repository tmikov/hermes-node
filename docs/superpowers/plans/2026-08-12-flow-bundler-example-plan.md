# Flow Bundler Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the Hermes Flow bundler under `hermes-node` as a self-contained example with mechanically verified output, by vendoring the native parser addon into this repository.

**Architecture:** The addon's C++ (`external/hermes-parser-native/napi/`) builds as a normal CMake `MODULE` target against the Hermes front-end libraries already built here. Its npm package (`external/hermes-parser-native/package/`) is vendored with `dist/` committed, so building this repository never needs a JavaScript toolchain. `examples/flow-bundler/` copies the bundler and its fixture outright and depends on the vendored package through a `file:` path.

**Tech Stack:** CMake + Ninja, Clang, Node-API, npm, Babel.

**Design doc:** `docs/superpowers/specs/2026-08-12-flow-bundler-example-design.md`

## Global Constraints

- Always build with Clang, never GCC.
- Primary configuration is `cmake-build-asan` (Debug + ASAN). Verify there unless a step says otherwise.
- **Exception:** the Flow bundler example itself is verified in `cmake-build-release`. Under ASAN the bundler run exceeds ten minutes (measured), which makes it useless as a check. The addon and its unit tests are still verified under ASAN.
- Commit messages: ASCII only, no emojis.
- Run `./utils/format.sh -f` before any commit that touches C++.
- The `hermes/` submodule pin does not change in this plan.
- Copy source is the `parser-native` branch worktree at `/home/tmikov/work/hermes-parser-native`. Referred to below as `$FORK`. Record its exact commit SHA in the vendored README.
- Never copy a file whose header says "Confidential and proprietary". Two such files exist in the source set: `benchmarks/build-helpers/flow-bundler/babel-register.js` and `benchmarks/MiniReact/no-objects/build.config.js`. Both are authored fresh here.
- Unit test target names must end in `Test` (singular). `unittests/lit.cfg` discovers GoogleTest binaries by that suffix; a name ending in `Tests` is silently never collected.
- Set `export FORK=/home/tmikov/work/hermes-parser-native` and `export ASAN_OPTIONS=detect_leaks=0` in the shell used for these tasks.

---

### Task 1: Vendor and build the C++ addon

**Files:**
- Create: `external/hermes-parser-native/napi/` (12 files copied)
- Create: `external/hermes-parser-native/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add one `add_subdirectory`)

**Interfaces:**
- Produces: a CMake target `hermes-parser-napi` whose output file is `<build>/external/hermes-parser-native/hermes-parser.node`. Task 3 and Task 5 consume that path.

- [ ] **Step 1: Copy the addon sources verbatim**

```bash
cd /home/tmikov/work/hermes-node-compat
mkdir -p external/hermes-parser-native
cp -r "$FORK/tools/hermes-parser-native" external/hermes-parser-native/napi
rm -rf external/hermes-parser-native/napi/__tests__
ls external/hermes-parser-native/napi
```

Expected: `CMakeLists.txt ContainerWriter.h HermesParserDiagHandler.cpp HermesParserDiagHandler.h HermesParserJSSerializer.cpp HermesParserJSSerializer.h KindHash.h SourcePositionMap.h StringTable.h hermes-parser-napi.cpp`

The copied `napi/CMakeLists.txt` is kept for provenance but is **not** used; the wrapper below replaces it. It hardcodes `${CMAKE_SOURCE_DIR}/include/hermes/napi`, which resolves correctly only inside the Hermes tree.

- [ ] **Step 2: Write the wrapper CMakeLists**

Create `external/hermes-parser-native/CMakeLists.txt`:

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Wrapper CMakeLists.txt for the vendored hermes-parser-native addon.
#
# Declares the target here rather than using napi/CMakeLists.txt: that file
# hardcodes ${CMAKE_SOURCE_DIR}/include/hermes/napi, which points at the
# Hermes root in the tree it came from and at this repository's root here.
#
# A Node-API addon, built as a MODULE library named "hermes-parser.node".
# The napi_* symbols are deliberately left undefined: they resolve from the
# host process at dlopen time, which is what lets one binary work in both
# Node and hermes-node.
add_library(hermes-parser-napi MODULE
  napi/hermes-parser-napi.cpp
  napi/HermesParserJSSerializer.cpp
  napi/HermesParserDiagHandler.cpp
)

target_link_libraries(hermes-parser-napi
  hermesAST
  hermesParser
  hermesSema
  LLVHSupport
)

set_target_properties(hermes-parser-napi PROPERTIES
  PREFIX ""
  OUTPUT_NAME "hermes-parser"
  SUFFIX ".node"
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hermes-parser-napi PRIVATE
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
)

target_compile_definitions(hermes-parser-napi PRIVATE
  NODE_GYP_MODULE_NAME=hermes_parser
)

if (APPLE)
  # Allow napi_* to be resolved by the loading process.
  target_link_options(hermes-parser-napi PRIVATE
    "SHELL:-undefined dynamic_lookup")
endif ()
```

- [ ] **Step 3: Add the subdirectory to the top-level build**

In `CMakeLists.txt`, next to the other `external/` subdirectories, add:

```cmake
add_subdirectory(external/hermes-parser-native)
```

Place it after the existing `external/` entries so the Hermes targets it links are already declared.

- [ ] **Step 4: Build it and verify the output exists**

```bash
cmake -B cmake-build-asan
cmake --build cmake-build-asan --target hermes-parser-napi
ls -la cmake-build-asan/external/hermes-parser-native/hermes-parser.node
```

Expected: the file exists. If CMake cannot find `hermesAST`, the `add_subdirectory` was placed before `add_subdirectory(hermes)`; move it later.

- [ ] **Step 5: Verify hermes-node can dlopen it**

```bash
cmake --build cmake-build-asan --target hermes-node
./cmake-build-asan/bin/hermes-node -e '
  const addon = require("./cmake-build-asan/external/hermes-parser-native/hermes-parser.node");
  console.log("exports:", Object.keys(addon).sort().join(","));
'
```

Expected: a non-empty list of exported function names. An error mentioning `napi_register_module_v1` means the addon built without the Node-API entry point; re-check Step 2.

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
git add external/hermes-parser-native CMakeLists.txt
git commit -m "Vendor the hermes-parser native addon C++ sources

The Flow bundler example parses through hermes-parser, whose published
build is WebAssembly, and hermes-node has none. The native Node-API addon
lives on a Hermes branch the submodule cannot point at, so it is vendored
here instead. Temporary: it goes away when hermes-node supports wasm.

The wrapper CMakeLists declares the target rather than using the copied
one, which hardcodes an include path relative to the Hermes root."
```

---

### Task 2: Vendor the addon's unit tests

**Files:**
- Create: `unittests/HermesParserNative/` (5 test sources + `CMakeLists.txt`)
- Modify: `unittests/CMakeLists.txt` (add one `add_subdirectory`)

**Interfaces:**
- Consumes: `external/hermes-parser-native/napi/` from Task 1.
- Produces: a GTest binary `cmake-build-asan/unittests/HermesParserNativeTest`, collected by the Lit GoogleTest suite.

- [ ] **Step 1: Copy the test sources**

```bash
cd /home/tmikov/work/hermes-node-compat
mkdir -p unittests/HermesParserNative
cp "$FORK"/unittests/HermesParserNative/*.cpp unittests/HermesParserNative/
ls unittests/HermesParserNative/
```

Expected: `ContainerWriterTest.cpp KindHashTest.cpp SerializerTest.cpp SourcePositionMapTest.cpp StringTableTest.cpp`

- [ ] **Step 2: Write the test CMakeLists**

Create `unittests/HermesParserNative/CMakeLists.txt`:

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Note the singular "Test" suffix: unittests/lit.cfg discovers GoogleTest
# binaries by that suffix, so the fork's "HermesParserNativeTests" would
# never be collected here.
set(HermesParserNativeTestSources
  StringTableTest.cpp
  KindHashTest.cpp
  ContainerWriterTest.cpp
  SerializerTest.cpp
  SourcePositionMapTest.cpp
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi/HermesParserJSSerializer.cpp
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi/HermesParserDiagHandler.cpp
)

add_node_compat_unittest(HermesParserNativeTest
  ${HermesParserNativeTestSources}
)

target_include_directories(HermesParserNativeTest PRIVATE
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi
)

target_link_libraries(HermesParserNativeTest
  hermesAST
  hermesParser
  hermesSema
  LLVHSupport
)
```

- [ ] **Step 3: Register the subdirectory**

In `unittests/CMakeLists.txt`, alongside the other `add_node_compat_unittest` calls, add:

```cmake
add_subdirectory(HermesParserNative)
```

- [ ] **Step 4: Build and run the binary directly**

```bash
cmake -B cmake-build-asan
cmake --build cmake-build-asan --target HermesParserNativeTest
./cmake-build-asan/unittests/HermesParserNativeTest 2>&1 | tail -3
```

Expected: `[  PASSED  ] N tests.` with N > 0 and no failures.

- [ ] **Step 5: Verify Lit collects it**

```bash
cmake --build cmake-build-asan --target check-hermes-node-unit 2>&1 | tail -4
```

Expected: the "Expected Passes" count is higher than before this task, and the run mentions no failures. If the count is unchanged, the target name does not end in `Test`; fix Step 2.

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
git add unittests/HermesParserNative unittests/CMakeLists.txt
git commit -m "Vendor the native parser addon unit tests

Renamed from the fork's HermesParserNativeTests: Lit's GoogleTest
discovery matches the suffix Test, so the plural form would never be
collected. These need no network and no JavaScript toolchain, so unlike
the example they run in check-hermes-node."
```

---

### Task 3: Vendor the npm package

**Files:**
- Create: `external/hermes-parser-native/package/` (`src/`, `dist/`, `package.json`, `README.md`, `LICENSE`)
- Create: `external/hermes-parser-native/.gitignore`
- Modify: `external/hermes-parser-native/CMakeLists.txt` (add the `POST_BUILD` copy)

**Interfaces:**
- Consumes: the `hermes-parser-napi` target from Task 1.
- Produces: a package directory usable as `"hermes-parser": "file:../../external/hermes-parser-native/package"`, whose `dist/index.js` is the entry point.

- [ ] **Step 1: Copy the package, excluding build and test artifacts**

```bash
cd /home/tmikov/work/hermes-node-compat
SRC="$FORK/tools/hermes-parser/js/hermes-parser-native"
mkdir -p external/hermes-parser-native/package
cp -r "$SRC/src" "$SRC/dist" external/hermes-parser-native/package/
cp "$SRC/package.json" "$SRC/README.md" "$SRC/LICENSE" external/hermes-parser-native/package/
find external/hermes-parser-native/package -type f | wc -l
```

Expected: 129 files (34 under `src/`, 92 under `dist/`, plus the 3 top-level files). `node_modules/`, `prebuilds/`, `__tests__/`, `__test_utils__/`, `__benchmarks__/` and `yarn.lock` are deliberately not copied — the JavaScript test suites are out of scope per the design.

- [ ] **Step 2: Add the vendored .gitignore**

Create `external/hermes-parser-native/.gitignore`:

```gitignore
# The addon is copied here by the build (see CMakeLists.txt) so the
# package's own loader finds it without an environment variable.
package/prebuilds/

# Only needed when regenerating dist/ (see scripts/).
scripts/node_modules/
```

Note the divergence from the fork, where `dist/` is gitignored. Here `dist/` is committed so building this repository never requires a JavaScript toolchain.

- [ ] **Step 3: Copy the built addon into the package on build**

Append to `external/hermes-parser-native/CMakeLists.txt`:

```cmake
# The package's loader (package/dist/HermesParserAddon.js) looks first at
# package/prebuilds/<platform>-<arch>/hermes-parser.node. Putting the build
# output there lets the vendored package behave exactly like a published
# one, so the example needs no environment variable.
#
# The directory is gitignored. Note that several build directories write to
# the same place, so the last build wins; anything that cares which addon it
# loads should set HERMES_PARSER_NATIVE_ADDON explicitly.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(HPN_PLATFORM "darwin")
else ()
  set(HPN_PLATFORM "linux")
endif ()
if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(HPN_ARCH "arm64")
else ()
  set(HPN_ARCH "x64")
endif ()

set(HPN_PREBUILD_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/package/prebuilds/${HPN_PLATFORM}-${HPN_ARCH})

add_custom_command(TARGET hermes-parser-napi POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${HPN_PREBUILD_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:hermes-parser-napi>
          ${HPN_PREBUILD_DIR}/hermes-parser.node
  COMMENT "Staging hermes-parser.node into the vendored package"
  VERBATIM
)
```

- [ ] **Step 4: Build and verify the staged copy**

```bash
cmake -B cmake-build-asan
cmake --build cmake-build-asan --target hermes-parser-napi
ls -la external/hermes-parser-native/package/prebuilds/*/hermes-parser.node
git status --short external/hermes-parser-native/package/prebuilds
```

Expected: the file exists, and `git status` shows nothing (it is ignored).

- [ ] **Step 5: Verify the package loads and parses under hermes-node**

Use a script file, not `hermes-node -e`. Requiring anything that pulls in a
`.node` addon from eval mode hits a pre-existing hermes-node bug: eval-mode
`require` has no `.node` handling and tries to compile the shared library as
JavaScript, giving `SyntaxError: unrecognized Unicode character \u7f` (the
first byte of the ELF magic). This reproduces with the unrelated
`hello_addon.node` too, so it is not caused by anything in this plan.

**Fixed after this plan was executed**, in "Give -e code a real require
instead of the bootstrap loader": eval code now gets a require built for a
module named `[eval]` in the current directory, so `.node` and `.json` both
resolve. The script-file form above still works and is left as written; the
warning is kept for the record of why it was there.

```bash
mkdir -p /tmp/hpn-check && cd /tmp/hpn-check
cat > package.json <<'EOF'
{ "name": "hpn-check", "version": "1.0.0",
  "dependencies": { "hermes-parser": "file:/home/tmikov/work/hermes-node-compat/external/hermes-parser-native/package" } }
EOF
npm install --no-audit --no-fund
cat > check.js <<'EOF'
const {parse} = require('hermes-parser');
const ast = parse('const x: number = 1;', {flow: 'all'});
console.log('PASS', ast.body[0].type,
            ast.body[0].declarations[0].id.typeAnnotation != null);
EOF
/home/tmikov/work/hermes-node-compat/cmake-build-asan/bin/hermes-node check.js
cd - && rm -rf /tmp/hpn-check
```

Expected: `PASS VariableDeclaration true`. A "Cannot find module" naming a `.node` path means Step 3 staged to the wrong directory; compare it against what `package/dist/HermesParserAddon.js` computes.

- [ ] **Step 6: Commit**

```bash
cd /home/tmikov/work/hermes-node-compat
git add external/hermes-parser-native
git commit -m "Vendor the hermes-parser-native npm package

dist/ is committed, unlike upstream where it is generated and ignored, so
that building this repository never requires a JavaScript toolchain. src/
comes along for regeneration, wired up in the next commit.

The build stages the addon into package/prebuilds/<platform>-<arch>/,
which is where the package's own loader looks first, so consumers need no
environment variable."
```

---

### Task 4: Opt-in regeneration of `dist/`

**Files:**
- Create: `external/hermes-parser-native/scripts/regen-dist.sh`
- Create: `external/hermes-parser-native/scripts/package.json`
- Create: `external/hermes-parser-native/scripts/babel.config.js`
- Copy: `external/hermes-parser-native/scripts/genKindHash.js`, `distManifest.js`
- Modify: `external/hermes-parser-native/CMakeLists.txt` (add the opt-in target)

**Interfaces:**
- Consumes: `package/src/` and the `hermes/` submodule's `include/` from Task 3.
- Produces: a CMake target `hermes-parser-native-dist`, excluded from `ALL`.

- [ ] **Step 1: Copy the two generator scripts**

```bash
cd /home/tmikov/work/hermes-node-compat
mkdir -p external/hermes-parser-native/scripts
cp "$FORK/tools/hermes-parser/js/scripts/genKindHash.js" \
   "$FORK/tools/hermes-parser/js/scripts/distManifest.js" \
   external/hermes-parser-native/scripts/
```

- [ ] **Step 2: Write the regeneration toolchain manifest**

Create `external/hermes-parser-native/scripts/package.json`. These are the Babel packages the fork's workspace config uses, pinned so regeneration is reproducible:

```json
{
  "name": "hermes-parser-native-regen",
  "version": "1.0.0",
  "private": true,
  "description": "Toolchain for regenerating ../package/dist from ../package/src",
  "devDependencies": {
    "@babel/cli": "^7.24.0",
    "@babel/core": "^7.24.0",
    "@babel/preset-env": "^7.24.0",
    "@babel/plugin-syntax-flow": "^7.24.0",
    "@babel/plugin-transform-flow-strip-types": "^7.24.0",
    "@babel/plugin-proposal-class-properties": "^7.18.6",
    "babel-plugin-transform-flow-enums": "^0.0.2",
    "babel-plugin-syntax-hermes-parser": "0.37.0"
  }
}
```

- [ ] **Step 3: Write the Babel config**

Create `external/hermes-parser-native/scripts/babel.config.js`. This mirrors the fork's workspace config; the parser override is unconditional here because the plugin comes from npm rather than being built locally:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Mirrors tools/hermes-parser/js/babel.config.js from the Hermes fork this
// package was copied from. Kept in sync by hand; see ../README.md.

module.exports = {
  assumptions: {
    constantReexports: true,
    constantSuper: true,
    noClassCalls: true,
    noDocumentAll: true,
    noNewArrows: true,
    setPublicClassFields: true,
  },
  presets: [['@babel/preset-env', {targets: {node: '12.0.0'}}]],
  plugins: [
    ['@babel/plugin-syntax-flow', {enums: true}],
    'babel-plugin-transform-flow-enums',
    ['@babel/plugin-transform-flow-strip-types', {allowDeclareFields: true}],
    '@babel/plugin-proposal-class-properties',
    // hermes-parser as Babel's parser, so newer Flow syntax (e.g. `as`
    // casts) beyond what @babel/parser supports is understood.
    'babel-plugin-syntax-hermes-parser',
  ],
};
```

- [ ] **Step 4: Write the regeneration script**

Create `external/hermes-parser-native/scripts/regen-dist.sh`, `chmod +x` it:

```bash
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

# npm ci, not npm install: with a committed lockfile, `npm install` will
# silently re-resolve and rewrite it if package.json and the lockfile ever
# drift, which defeats reproducible output. `npm ci` fails loudly instead.
(cd "$THIS_DIR" && npm ci --no-audit --no-fund)

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
```

- [ ] **Step 5: Add the opt-in CMake target**

Append to `external/hermes-parser-native/CMakeLists.txt`:

```cmake
# Regenerating dist/ from src/ needs npm and Babel. Deliberately excluded
# from ALL: dist/ is committed so that a normal build needs no JavaScript
# toolchain. Run explicitly after editing src/, then commit the result.
add_custom_target(hermes-parser-native-dist
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/regen-dist.sh
          ${PROJECT_SOURCE_DIR}/hermes/include
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Regenerating vendored hermes-parser-native dist/"
  VERBATIM
)
```

- [ ] **Step 6: Verify regeneration reproduces the committed dist**

This is the real test of the mechanism: regenerating from unmodified `src/` must leave `dist/` byte-identical apart from the manifest's timestamp.

```bash
cd /home/tmikov/work/hermes-node-compat
cmake -B cmake-build-asan
cmake --build cmake-build-asan --target hermes-parser-native-dist
git diff --stat external/hermes-parser-native/package/dist/
```

Expected: the only changed file is `dist/build-manifest.json`, and its only changed field is `generatedAt`. If `.js` files differ, the Babel config in Step 3 does not match the one the committed `dist/` was built with; diff a changed file to see how.

- [ ] **Step 7: Restore the manifest timestamp and commit**

```bash
git checkout external/hermes-parser-native/package/dist/build-manifest.json
git status --short external/hermes-parser-native
git add external/hermes-parser-native/scripts external/hermes-parser-native/CMakeLists.txt
git commit -m "Add opt-in regeneration of the vendored dist/

Excluded from ALL: dist/ is committed so a normal build needs no
JavaScript toolchain. Verified by regenerating from an unmodified src/ and
confirming the output is byte-identical apart from the manifest timestamp."
```

---

### Task 5: The self-contained example

**Files:**
- Create: `examples/flow-bundler/bundler/` (8 files copied)
- Create: `examples/flow-bundler/fixture/src/` (21 files copied)
- Create: `examples/flow-bundler/expected/` (6 files copied)
- Create: `examples/flow-bundler/package.json`, `babel.config.js`, `babel-register.js`, `build.config.js`, `run.sh`, `README.md`

**Interfaces:**
- Consumes: `external/hermes-parser-native/package` from Task 3, and the addon built in Task 1.
- Produces: `examples/flow-bundler/run.sh`, which exits 0 on success and non-zero on any output mismatch. Task 6 invokes it.

- [ ] **Step 1: Copy the bundler, the fixture, and the expected output**

The sources come from the `hermes/` submodule, so it must be initialized
(`git submodule update --init --recursive`). This is the only task that reads
from it; nothing the example produces depends on it afterwards.

```bash
cd /home/tmikov/work/hermes-node-compat
B=hermes/benchmarks
mkdir -p examples/flow-bundler/{bundler,fixture,expected}
cp -r "$B/build-helpers/flow-bundler/src/." examples/flow-bundler/bundler/
cp "$B/build-helpers/flow-bundler/babel.config.js" examples/flow-bundler/
cp -r "$B/MiniReact/no-objects/src" examples/flow-bundler/fixture/
cp "$B"/MiniReact/no-objects/out/*.js examples/flow-bundler/expected/
ls examples/flow-bundler/expected/ | wc -l
grep -rl "Confidential" examples/flow-bundler/ || echo "no proprietary headers - good"
```

Expected: 6 expected files, and the grep prints the "good" line. `babel-register.js` and `build.config.js` are **not** copied; they carry proprietary headers and are written fresh below.

- [ ] **Step 2: Point the copied Babel config at this directory**

Edit `examples/flow-bundler/babel.config.js` so `NODE_MODULES` resolves here rather than in the bundler's original directory. The file's only change is the comment and that the path is now correct by construction:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

// Copied from the Hermes flow-bundler. Unchanged except for this comment:
// __dirname is now this example directory, so NODE_MODULES resolves to the
// node_modules created by `npm install` here.
const path = require('path');
const NODE_MODULES = path.resolve(__dirname, 'node_modules');

module.exports = {
  presets: [
    [
      path.join(NODE_MODULES, '@babel/preset-env'),
      {targets: {node: 'current'}},
    ],
    path.join(NODE_MODULES, '@babel/preset-flow'),
  ],
  plugins: [path.join(NODE_MODULES, 'babel-plugin-syntax-hermes-parser')],
  ignore: [/\/node_modules\//],
};
```

- [ ] **Step 3: Write the require hook**

Create `examples/flow-bundler/babel-register.js`. The upstream equivalent is one line, but it is marked proprietary and would not work here anyway: Babel resolves configuration relative to each compiled file, and the bundler sources now sit below this directory.

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Preloaded with `hermes-node -r`. Installs a require hook that compiles
// the bundler's Flow + ESM sources to CommonJS as they load, parsing them
// with hermes-parser by way of babel-plugin-syntax-hermes-parser.
//
// configFile and root are explicit: Babel otherwise looks for configuration
// relative to each file it compiles, and `only` keeps the hook off anything
// outside this example.

const path = require('path');

require('@babel/register')({
  ...require('./babel.config'),
  configFile: false,
  babelrc: false,
  root: __dirname,
  only: [path.join(__dirname, 'bundler'), path.join(__dirname, 'build.config.js')],
  extensions: ['.js'],
});
```

- [ ] **Step 4: Write the build config**

Create `examples/flow-bundler/build.config.js`. Mirrors the MiniReact config, with plugin resolution pointed at this directory:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Mirrors benchmarks/MiniReact/no-objects/build.config.js from the Hermes
// tree, with plugins resolved from this example's node_modules.

const path = require('path');

const HERE = __dirname;

function plugin(name) {
  return require.resolve(name, {paths: [HERE]});
}

function createConfig(benchmarkName) {
  return {
    root: path.join(HERE, 'fixture', 'src'),
    outDir: path.join(HERE, 'out'),
    entrypoints: [`./app/${benchmarkName}/index.js`],
    simpleJsxTransform: true,
    out: {
      [`${benchmarkName}.js`]: null,
      [`${benchmarkName}-stripped.js`]: {
        babelConfig: {
          plugins: [plugin('@babel/plugin-transform-flow-strip-types')],
        },
      },
      [`${benchmarkName}-lowered.js`]: {
        babelConfig: {
          plugins: [
            [plugin('@babel/plugin-transform-class-properties'), {enableBabelRuntime: false}],
            plugin('@babel/plugin-transform-flow-strip-types'),
            [plugin('@babel/plugin-transform-classes'), {enableBabelRuntime: false}],
          ],
          assumptions: {
            constantSuper: true,
            noClassCalls: true,
            setClassMethods: true,
            setPublicClassFields: true,
            superIsCallableConstructor: true,
          },
        },
      },
    },
  };
}

module.exports = {builds: [createConfig('simple'), createConfig('music')]};
```

- [ ] **Step 5: Write package.json**

Create `examples/flow-bundler/package.json`. The dependency list is the bundler's own, minus `flow-bin` (a large binary the bundler does not use) and `hermes-eslint` (unused here), plus the three transform plugins `build.config.js` resolves by name.

**Do not remove `hermes-estree` as redundant.** It is the vendored package's own
and only dependency, and npm 11 defaults to `install-links=false`, which
symlinks a `file:` dependency without installing its transitive dependencies.
Declaring it here is what makes a plain `npm install` work; without it the run
fails with `Cannot find module 'hermes-estree'`.

```json
{
  "name": "flow-bundler-example",
  "version": "1.0.0",
  "private": true,
  "description": "Runs the Hermes Flow bundler under hermes-node",
  "type": "commonjs",
  "dependencies": {
    "@babel/core": "^7.23.3",
    "@babel/generator": "^7.23.5",
    "@babel/plugin-transform-class-properties": "^7.23.3",
    "@babel/plugin-transform-classes": "^7.23.3",
    "@babel/plugin-transform-flow-strip-types": "^7.23.3",
    "@babel/preset-env": "^7.23.3",
    "@babel/preset-flow": "^7.23.3",
    "@babel/register": "^7.22.15",
    "@babel/types": "^7.23.5",
    "babel-plugin-syntax-hermes-parser": "0.37.0",
    "hermes-estree": "0.37.0",
    "hermes-parser": "file:../../external/hermes-parser-native/package",
    "hermes-transform": "0.37.0",
    "prettier": "2.8.8",
    "prettier-plugin-hermes-parser": "0.36.0",
    "string-width": "4.2.3",
    "yargs": "^17.7.2"
  }
}
```

- [ ] **Step 6: Write run.sh**

Create `examples/flow-bundler/run.sh`, `chmod +x` it:

```bash
#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Bundles the fixture with the Flow bundler running under hermes-node, then
# checks the result against expected/.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"

HERMES_NODE="$BUILD_DIR/bin/hermes-node"
ADDON="$BUILD_DIR/external/hermes-parser-native/hermes-parser.node"

for f in "$HERMES_NODE" "$ADDON"; do
  if [ ! -x "$f" ] && [ ! -f "$f" ]; then
    echo "ERROR: missing $f -- build it first:" 1>&2
    echo "  cmake --build $BUILD_DIR --target hermes-node hermes-parser-napi" 1>&2
    exit 1
  fi
done

if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

# Pin the addon to the build directory being tested rather than relying on
# the package's prebuilds/ lookup, which several build directories share.
export HERMES_PARSER_NATIVE_ADDON="$ADDON"

rm -rf "$HERE/out"
"$HERMES_NODE" -r "$HERE/babel-register.js" \
  "$HERE/bundler/buildBundleCLI.js" -c "$HERE/build.config.js"

status=0
for f in "$HERE"/expected/*.js; do
  name="$(basename "$f")"
  if ! cmp -s "$f" "$HERE/out/$name"; then
    echo "MISMATCH: $name" 1>&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "FAIL: bundler output differs from expected/" 1>&2
  exit 1
fi

echo "PASS: 6 bundles match expected/"
```

- [ ] **Step 7: Install and run it**

```bash
cd /home/tmikov/work/hermes-node-compat
cmake --build cmake-build-release --target hermes-node hermes-parser-napi
(cd examples/flow-bundler && npm install --no-audit --no-fund)
./examples/flow-bundler/run.sh cmake-build-release
```

Expected: `PASS: 6 bundles match expected/`.

If a bundle mismatches, diff it against the expectation — the bundles embed no paths, so a difference is a real behavioural difference, not a relocation artifact. If the run fails with "Cannot find module 'hermes-parser'", check that `npm install` created `node_modules/hermes-parser` (npm installs a `file:` dependency under the key, not the package's own name).

- [ ] **Step 8: Write the example README and register it**

Create `examples/flow-bundler/README.md` covering: what it demonstrates; that `bundler/` and `fixture/` are copies from the Hermes tree with the source commit recorded; that the configs are written fresh because the originals resolve paths relative to their own directory and two carry proprietary headers; how to run it; and that it needs `npm install`.

Add a matching entry to `examples/README.md` in the style of the existing `bufferutil-addon/` entry.

- [ ] **Step 9: Commit**

```bash
git add examples/flow-bundler examples/README.md
git commit -m "Add the Flow bundler example

Runs the Hermes Flow bundler under hermes-node end to end: hermes-node
loads the bundler through a -r require hook, Babel parses with
hermes-parser, and hermes-parser is the vendored native addon rather than
the published wasm build, which hermes-node cannot run.

Self-contained on purpose. The bundler and fixture are copied rather than
read out of the hermes submodule, whose benchmarks directory is not an API
and would break the example on a submodule bump. The configs are written
fresh: the originals resolve plugins relative to their own directory, and
both carry proprietary headers.

run.sh checks the six generated bundles against committed expected output.
The bundles embed no paths, so relocating the sources does not change them
and the expectations are the ones the Hermes tree ships."
```

---

### Task 6: Opt-in example test target

**Files:**
- Modify: `CMakeLists.txt` (add `check-hermes-node-examples`)
- Create: `examples/run-examples.sh`

**Interfaces:**
- Consumes: `examples/flow-bundler/run.sh` from Task 5.
- Produces: a CMake target `check-hermes-node-examples`, not part of `check-hermes-node`.

- [ ] **Step 1: Write the driver script**

Create `examples/run-examples.sh`, `chmod +x` it:

```bash
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
```

- [ ] **Step 2: Add the CMake target**

In `CMakeLists.txt`, after the `check-hermes-node` target, add:

```cmake
# Examples are not part of check-hermes-node: they need a network install of
# Babel and friends, while the default suite stays offline. Examples that
# have not been installed are skipped rather than failed.
add_custom_target(check-hermes-node-examples
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/examples/run-examples.sh
          ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS hermes-node hermes-parser-napi
  COMMENT "Running hermes-node examples"
  VERBATIM
)
```

- [ ] **Step 3: Verify it runs the installed example**

```bash
cd /home/tmikov/work/hermes-node-compat
cmake -B cmake-build-release
cmake --build cmake-build-release --target check-hermes-node-examples 2>&1 | tail -5
```

Expected: `RUN  flow-bundler`, then `PASS: 6 bundles match expected/`, then `examples: 1 ran, 0 skipped`.

- [ ] **Step 4: Verify both skip paths**

Not-installed:

```bash
mv examples/flow-bundler/node_modules /tmp/fb-node-modules
cmake --build cmake-build-release --target check-hermes-node-examples 2>&1 | tail -3
mv /tmp/fb-node-modules examples/flow-bundler/node_modules
```

Expected: `SKIP flow-bundler: not installed ...` and `examples: 0 ran, 1 skipped`, exit 0.

ASAN build:

```bash
cmake -B cmake-build-asan
cmake --build cmake-build-asan --target check-hermes-node-examples 2>&1 | tail -3
```

Expected: `SKIP all examples: ... is an ASAN build (too slow).` and exit 0, in
well under a minute. If it starts bundling instead, the CMakeCache grep in
Step 1 does not match this repository's variable name — check
`grep ADDRESS_SANITIZER cmake-build-asan/CMakeCache.txt`.

- [ ] **Step 5: Verify the default suite is unaffected**

```bash
export ASAN_OPTIONS=detect_leaks=0
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | grep -E "Expected Passes|Unexpected|FAILED"
```

Expected: both suites pass, and no example ran (the JS suite time stays around 20 s).

- [ ] **Step 6: Commit**

```bash
git add examples/run-examples.sh CMakeLists.txt
git commit -m "Add an opt-in check-hermes-node-examples target

Not part of check-hermes-node: the examples need a network install, while
the default suite stays offline and finishes in about twenty seconds. An
example that has not been installed is skipped with a clear message rather
than failing."
```

---

### Task 7: Provenance and documentation

**Files:**
- Create: `external/hermes-parser-native/README.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Capture the source commit**

```bash
git -C "$FORK" rev-parse HEAD
git -C "$FORK" describe --tags --always --dirty --match "v*"
```

Record both in the README written next.

- [ ] **Step 2: Write the vendored README**

Create `external/hermes-parser-native/README.md` covering, in this order:

1. **What this is** — the Hermes native parser addon, a Node-API replacement for the WebAssembly `hermes-parser`, vendored because `hermes-node` has no WebAssembly and the addon lives on a Hermes branch the submodule cannot point at.
2. **Provenance** — source repository `tmikov/hermes`, branch `parser-native`, the commit SHA from Step 1, and the two source paths copied (`tools/hermes-parser-native` → `napi/`, `tools/hermes-parser/js/hermes-parser-native` → `package/`).
3. **How to re-sync** — recopy the two directories, rerun `cmake --build <dir> --target hermes-parser-native-dist`, rerun the tests.
4. **Divergences from upstream** — `dist/` is committed here and ignored there; `__tests__`, `__test_utils__`, `__benchmarks__` and `yarn.lock` are not copied; the wrapper `CMakeLists.txt` replaces `napi/CMakeLists.txt`; `scripts/regen-dist.sh` plus its own `package.json` and `babel.config.js` replace upstream's `build-native.sh`, which assumes the Hermes JS workspace; the unit test target is singular.
5. **Limitations** — builds for the host platform only, `linux-x64` being the only one exercised; `dist/` is generated code, regenerate with the opt-in target after editing `src/`; and a consumer depending on this package through a `file:` path must either declare `hermes-estree` itself or install with `--install-links=true`, because npm 11 defaults to `install-links=false` and symlinks the package without installing its transitive dependencies. `examples/flow-bundler/package.json` takes the first route.
6. **The Sema landmine** — with the current submodule pin, `CheckImplicitReturn.cpp:248` asserts on any `try { } catch { } finally { }` in parsed input, because the split that would prevent it is gated on `compile_` at `SemanticResolver.cpp:794` and the parser path runs with `compile=false`. Assertions-enabled builds abort on such input; Release builds are unaffected because `resolveASTForParser` runs only the resolver and never reads the result. The example's own inputs contain none. The fix is on the `sema-implicit-return-fixes` branch, intended for upstream.
7. **When to delete this directory** — when `hermes-node` supports WebAssembly: `git rm -r external/hermes-parser-native unittests/HermesParserNative`, repoint the example's dependency at `hermes-parser` from npm, and drop the `POST_BUILD` copy, the `hermes-parser-native-dist` target, and the two `add_subdirectory` lines.

- [ ] **Step 3: Update CLAUDE.md**

Under "Test Infrastructure", note that `check-hermes-node-examples` runs the examples and is not part of `check-hermes-node`. Under "Conventions", note that `external/hermes-parser-native/` is a temporary vendored copy with its own README.

- [ ] **Step 4: Full verification**

```bash
cd /home/tmikov/work/hermes-node-compat
export ASAN_OPTIONS=detect_leaks=0
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | grep -E "Expected Passes|Unexpected|FAILED"
cmake --build cmake-build-release --target check-hermes-node-examples 2>&1 | tail -3
git status --short
```

Expected: both suites pass, the example passes, and the working tree is clean apart from `TODO.md`.

- [ ] **Step 5: Commit**

```bash
git add external/hermes-parser-native/README.md CLAUDE.md
git commit -m "Document the vendored parser addon and the examples target

Records where the copy came from, how to re-sync it, how it diverges from
upstream, and the exact steps to delete it once hermes-node supports
WebAssembly and it is no longer needed."
```
