# Implementation Progress

Tracks progress on `history/initial/cjs-module-resolution-plan.md`.

The file has two sections: "Status" and "Context Notes".

**Status Section**:

Each row contains the step label from the detailed, plan, a brief description, list of
dependency labels, Status (initially empty), optional brief note (initially empty).

The status of a row is one of:
- "" (empty) initially, before work has started
- "wip" as soon as work on that raw has started.
- "done" when work has completed successfully. Rarely "Brief Note" may contain very brief
explanation. More details in "Context Notes".
- "blocked" when work cannot proceed for some reason. "Brief Note" must contain a brief
explanation. More details in "Context Notes".

Example on start:

| Step 11 | Port util binding | 5 |  |  |

**Context Notes**:

After completing work on a step, either successfully or by blocking, a section for that
step is added. It needs to have the format from the following example (empty bullets can
be omitted):

```
### Step 11: Port util binding
- **Files**: created `foo.c`, modified `bar.c`.
- **Decisions**:
-- Decision 1 concise explanation
-- Decision 2 concise explanation
- **What was done**: ...
- **Issues**: ...
- **Notes for next step**: ...
```

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| S1 | Add module_wrap binding stub | — | done | |
| S2 | Implement real readPackageJSON in modules binding | — | done | |
| S3 | Implement real compileFunctionForCJSLoader in contextify binding | — | done | |
| S4 | Add legacyMainResolve to fs binding | — | done | |
| S5 | Embed required modules | S1, S2 | done | |
| S6 | Create/update shims for newly embedded modules | S5 | done | |
| S7 | Integrate Node's CJS loader with bootstrap | S3, S6 | done | |
| S8 | Test: basic node_modules resolution | S7 | done | |
| S9 | Test: package.json "main" field | S7 | done | |
| S10 | Test: package.json "exports" field | S7 | done | |
| S11 | Test: .json file loading | S7 | done | |
| S12 | Test: nested node_modules | S7 | done | |
| S13 | Test: circular deps across node_modules | S7 | done | |
| S14 | Test: require.resolve | S7 | done | |
| S15 | Test: real npm package | S8–S14 | done | |

## Context Notes

### Step S1: Add module_wrap binding stub
- **Files**: created `lib/bindings/node_module_wrap.cpp`, `include/hermes/node-compat/bindings/node_module_wrap.h`; modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **What was done**: New `module_wrap` native binding exporting `kEvaluated = 4` (integer constant) and `createRequiredModuleFacade` (function that throws `ERR_REQUIRE_ESM`). Registered in hermes-node.cpp, added to CMake build. All 107 existing tests pass.

### Step S2: Implement real readPackageJSON in modules binding
- **Files**: modified `lib/bindings/node_modules.cpp`; created `test/test-read-package-json.js`, `test/fixtures/test-package.json`, `test/fixtures/test-package-cjs.json`, `test/fixtures/test-package-minimal.json`, `test/fixtures/test-package-string-exports.json`, `test/fixtures/test-package-bad-type.json`.
- **Decisions**:
-- Used libuv sync fs + JSON.parse/JSON.stringify via NAPI (no simdjson) -- simpler, reuses existing JSON parser.
-- For imports/exports fields: stringify objects/arrays back to JSON strings (lazy parsing in JS), pass strings as-is, undefined for missing/other types.
-- Type field normalized to "commonjs", "module", or "none" (matching Node behavior).
-- BOM stripping added for robustness (JSON.parse doesn't handle BOM).
-- Other stubs (getPackageScopeConfig, getPackageType, getNearestParentPackageJSONType) left as-is for now -- they return undefined which JS side handles gracefully.
- **What was done**: Replaced `readPackageJSONCb` stub with full implementation. Reads file via libuv sync API, parses JSON, extracts name/main/type/imports/exports/file_path into 6-element array matching Node's native API contract. Added 6-case test covering: full package.json, CJS type, minimal fields, non-existent file, string exports, invalid type normalization. All 108 tests pass.
- **Notes for next step**: S5 (embed modules) depends on this. The `getPackageScopeConfig` native function may need real implementation for ESM resolver modules (S5/S6); currently stub returns undefined which JS wraps as `{pjsonPath: undefined, exists: false, type: 'none'}`.

### Step S3: Implement real compileFunctionForCJSLoader in contextify binding
- **Files**: modified `lib/bindings/node_contextify.cpp`; created `test/test-compile-function-cjs.js`.
- **Decisions**:
-- Used `napi_run_script` (not `hermes_compile_to_bytecode` + `hermes_run_bytecode`) because the returned function continues to reference bytecode internally. Using `napi_run_script` avoids bytecode buffer lifetime issues (UAF with ASAN).
-- Wraps source as `(function(exports, require, module, __filename, __dirname) { <source> })` with `//# sourceURL=<filename>` for stack traces. Matches V8's `ScriptCompiler::CompileFunction` with CJS parameters.
-- `canParseAsESM` always false (no ESM detection). `cachedDataRejected` always false (no compile cache). `sourceMapURL`/`sourceURL` always undefined (no source map support yet).
- **What was done**: Replaced stub (returned undefined) with full implementation. Wraps raw source in CJS parameter function, evaluates via `napi_run_script`, returns result object with `{function, sourceMapURL, sourceURL, cachedDataRejected, canParseAsESM}`. Added 6-case test covering: basic compilation, CJS parameter passing, module.exports assignment, SyntaxError propagation, empty source, require() inside compiled function. All 109 tests pass.
- **Notes for next step**: S7 (integration) depends on this. The `wrapSafe()` function in Node's loader will call this with raw unwrapped source. Our implementation adds the wrapper, so Node's `Module.wrap()` must NOT be used (only the `patched=false` code path).

### Step S4: Add legacyMainResolve to fs binding
- **Files**: modified `lib/bindings/node_file.cpp`, `test/lit.cfg`; created `test/test-legacy-main-resolve.js`, `test/fixtures/legacy-main-resolve/` (5 fixture packages).
- **Decisions**:
-- Used `std::filesystem::path::append` + `lexically_normal()` for path resolution (already available in the file).
-- Used `uv_fs_stat` with nullptr loop (synchronous) + `S_ISDIR` check (matches Node's `FilePathIsFile` pattern).
-- Throws `ERR_MODULE_NOT_FOUND` when no file found (matches Node behavior; JS caller doesn't handle undefined return).
-- No permission checks (we don't have Node's permission model).
- **What was done**: Added `fsLegacyMainResolve` function and helper `filePathIsFile`. Tries extensions 0-6 for `pkgPath/main`, then 7-9 for `pkgPath/index`. Returns integer index. Added 8-case test covering: exact main match, main+.js, main+/index.js, main+.json, fallback index.js, fallback after main miss, throw on no match with base arg, throw on empty package. Also added `fixtures` to lit.cfg excludes to prevent fixture .js files from being picked up as tests. All 110 tests pass.
- **Notes for next step**: S5 (embed modules) is now fully unblocked. The ESM resolver (`resolve.js`) calls `FSLegacyMainResolve` via `internalBinding('fs').legacyMainResolve`.

### Step S5: Embed required modules
- **Files**: modified `lib/embedded-modules/embedded-modules.txt`.
- **What was done**: Added 17 new modules to the embedded module manifest: 4 CJS loader direct dependencies (`package_json_reader`, `customization_hooks`, `typescript`, `run_main`) and 13 ESM resolver modules (`resolve`, `assert`, `get_format`, `load`, `loader`, `hooks`, `module_map`, `module_job`, `translators`, `create_dynamic_module`, `initialize_import_meta`, `shared_constants`, `worker`). Reorganized the manifest sections: CJS module resolution support, ESM resolver, REPL support. All 17 modules compiled to bytecode successfully on first try with no syntax issues. All 110 existing tests pass.
- **Decisions**:
-- Added only the modules specified in the plan. Transitive runtime dependencies (e.g. `internal/encoding`, `internal/deps/cjs-module-lexer/lexer`) will be handled in S6 (shims) or S7 (integration) as needed at runtime.
-- Existing shims for `internal/modules/cjs/loader`, `internal/modules/helpers`, `internal/modules/esm/formats`, `internal/modules/esm/utils` remain in place (the build system's shim-first resolution picks them up over the libjs-node originals).
- **Notes for next step**: S6 (create/update shims) is now unblocked. Key concern: many of these modules have runtime dependencies on bindings or features we don't have (e.g. `module_wrap` full API, `inspector`, `worker_threads`). S6 will need to create shims for modules that fail to load at runtime. The `cjs-module-lexer` vendored dep is missing from our tree -- S6 will need a shim for it.

### Step S6: Create/update shims for newly embedded modules
- **Files**: created `libjs/shims/internal/modules/typescript.js`, `libjs/shims/internal/modules/run_main.js`; modified `libjs/shims/internal/modules/esm/utils.js`, `libjs/shims/internal/modules/helpers.js`, `libjs/shims/internal/bootstrap/realm.js`, `libjs/loader.js`, `tools/hermes-node/hermes-node.cpp`; removed `libjs/shims/internal/modules/cjs/loader.js`; modified tests `test/test-cjs-loader-module.js`, `test/test-modules-helpers.js`.
- **Decisions**:
-- Removed CJS loader shim: real Node `internal/modules/cjs/loader.js` (2074 lines) now loads successfully with all key methods (`_resolveFilename`, `_findPath`, `_load`, `_compile`, `_nodeModulePaths`, `_extensions`, `load`, `require`).
-- Kept helpers shim: real `internal/modules/helpers.js` needs `_enableCompileCache` to return an array (our binding returns object), and `stringify()` lazily needs `internal/encoding` (not embedded). Shim updated with real `loadBuiltinModule`, `getCjsConditions`, `normalizeReferrerURL`, `urlToFilename`, `getBuiltinModule`.
-- Kept ESM formats shim: real module needs `internalBinding('constants').internal` (not in our constants binding).
-- Created typescript shim: stubs `stripTypeScriptModuleTypes` (real module needs `internal/deps/amaro` WASM-based parser and compile cache bindings).
-- Created run_main shim: stubs `executeUserEntryPoint` (real module needs `internal/process/execution` and `internalBinding('errors').triggerUncaughtException`).
-- Updated ESM utils shim: added `getConditionsSet`, `getDefaultConditions`, `getDefaultConditionsSet`, `initializeDefaultConditions`, `initializeESM`, `requestTypes`, `compileSourceTextModule`. The ESM resolver uses `getConditionsSet` for package.json exports resolution.
-- Updated realm.js shim: added `compileForPublicLoader()` to BuiltinModule. Loads modules via captured bootstrap `require` and caches `.exports`.
-- Added CJS init step (11e) to bootstrap in hermes-node.cpp: calls `initializeCJS()` which sets `Module.builtinModules`, initializes CJS conditions, sets up global paths, and assigns `Module.runMain`.
- **What was done**: Made Node's real CJS loader (`internal/modules/cjs/loader.js`) loadable and initialized. The real loader, ESM resolver, customization_hooks, and package_json_reader all load successfully at runtime. `Module.builtinModules` is populated (35 modules). `loadBuiltinModule` and `getBuiltinModule` actually load modules via bootstrap require. All 110 existing tests pass.
- **Notes for next step**: S7 (bootstrap integration) needs to wire `__loadUserScript()` to use `Module._load()` instead of the bootstrap loader's `loadModule()`. `initializeCJS()` is already called during bootstrap. `BuiltinModule.compileForPublicLoader()` is available for the helpers `loadBuiltinModule`.

### Step S7: Integrate Node's CJS loader with bootstrap
- **Files**: modified `libjs/loader.js`, `libjs/shims/internal/bootstrap/realm.js`, `test/test-modules-helpers.js`; created `test/test-cjs-loader-integration.js`.
- **Decisions**:
-- `__loadUserScript` uses `Module._load(path.resolve(filepath), null, true)`. `path.resolve()` converts relative CLI paths to absolute (CJS loader requires absolute paths for proper resolution).
-- `Module._load` wrapped with fallback to bootstrap loader: on MODULE_NOT_FOUND, checks bootstrap cache and `loadBytecodeModule()`. This allows `require('internal/...')` in tests and for any embedded module not in the public builtins list.
-- Registered `.ts` extension handler (`Module._extensions['.ts']`) using bootstrap loader's `compileAndRun` with `enableTS=true`. Uses `Module.wrap()` for CJS wrapper.
-- Added `async_hooks`, `cluster`, `diagnostics_channel`, `repl` to `builtinIds` in realm.js (were missing; embedded but not listed as public builtins).
-- Updated test-modules-helpers.js: `hasStartedUserCJSExecution()` now returns `true` because user test scripts go through `Module._compile` which sets the flag.
- **What was done**: Wired `__loadUserScript()` to use Node's `Module._load()` instead of bootstrap loader's `loadModule()`. User scripts now get full CJS module resolution: `node_modules/` lookup, `package.json` main/exports, `.json` loading, `require.resolve()`, `Module._cache`, `process.mainModule`, and `module.filename`/`__dirname`. Built-in modules resolve via `BuiltinModule.compileForPublicLoader()` -> bootstrap require. Internal/embedded modules resolve via fallback in wrapped `Module._load`. TypeScript files handled via custom `.ts` extension handler using Hermes's native TS compilation. All 111 tests pass (110 existing + 1 new integration test).
- **Notes for next step**: S8-S14 are test steps that create fixture directories with `node_modules/` structures to verify the full resolution algorithm. The infrastructure is now in place. Key code paths exercised: `Module._resolveFilename` -> `_findPath` -> `resolveExports` for exports field, `Module._nodeModulePaths` for directory traversal, `Module._extensions['.json']` for JSON loading.

### Step S8: Test basic node_modules resolution
- **Files**: created `test/test-cjs-node-modules-basic.js`, `test/fixtures/node-modules-basic/main.js`, `test/fixtures/node-modules-basic/node_modules/my-package/index.js`.
- **What was done**: Created fixture directory with `node_modules/my-package/` containing a simple package (`{hello: 'world', version: '1.0.0'}`). Test script (`main.js`) verifies: require('my-package') resolves from node_modules, module caching works (second require returns same instance), module.filename/__dirname are set, require.resolve returns correct path, and Module._cache contains the resolved module. Lit test file runs main.js via `%hermes-node %source_dir/...` pattern. All 112 tests pass (111 existing + 1 new).
- **Decisions**:
-- Test script lives in fixtures dir (not test/) because node_modules resolution starts from the requiring file's directory. Lit test file in test/ just has the RUN/CHECK directives.
- **Notes for next step**: S9-S14 follow the same pattern: fixture dir with node_modules structure + main.js + lit test wrapper in test/.

### Step S9: Test package.json "main" field
- **Files**: created `test/test-cjs-node-modules-main.js`, `test/fixtures/node-modules-main/main.js`, `test/fixtures/node-modules-main/node_modules/my-package/` (package.json + lib/entry.js + index.js decoy), `test/fixtures/node-modules-main/node_modules/no-ext-package/` (package.json + src/index.js).
- **What was done**: Created two fixture packages: `my-package` with `"main": "lib/entry.js"` (explicit .js extension) and `no-ext-package` with `"main": "src/index"` (extensionless). `my-package` also has an `index.js` decoy to verify it's NOT loaded when "main" is set. Test verifies: (1) require loads from "main" path not index.js, (2) require.resolve returns path to "main" entry, (3) extensionless "main" resolves by appending .js, (4) require.resolve for extensionless also works. All 113 tests pass (112 existing + 1 new).

### Step S10: Test package.json "exports" field
- **Files**: created `test/test-cjs-node-modules-exports.js`, `test/fixtures/node-modules-exports/main.js`, `test/fixtures/node-modules-exports/node_modules/my-package/` (package.json + cjs.js + utils.js + lib/helper.js + index.js decoy + esm.mjs decoy), `test/fixtures/node-modules-exports/node_modules/simple-exports/` (package.json + main.js + index.js decoy).
- **What was done**: Created two fixture packages: `my-package` with conditional exports (`"require"` vs `"import"` conditions), subpath exports (`"./utils"`), and wildcard pattern exports (`"./lib/*"`); `simple-exports` with string exports (`"exports": "./main.js"`). Test verifies 7 cases: (1) conditional exports pick "require" condition over "import", (2) require.resolve respects exports field, (3) subpath export `./utils` resolves correctly, (4) wildcard export `./lib/*` resolves `lib/helper.js`, (5) simple string exports loads correct entry, (6) require.resolve for string exports works, (7) non-exported subpath throws ERR_PACKAGE_PATH_NOT_EXPORTED. All 114 tests pass (113 existing + 1 new).

### Step S11: Test .json file loading
- **Files**: created `test/test-cjs-require-json.js`, `test/fixtures/require-json/main.js`, `test/fixtures/require-json/data.json`, `test/fixtures/data.json`, `test/fixtures/require-json/node_modules/json-pkg/index.json`, `test/fixtures/require-json/node_modules/json-main-pkg/package.json`, `test/fixtures/require-json/node_modules/json-main-pkg/config.json`.
- **What was done**: Created fixture directory with JSON data file and two node_modules packages. Test verifies 6 cases: (1) `require('./data.json')` loads and parses JSON correctly (string, number, nested object/array, null fields), (2) nested objects and arrays are preserved, (3) second require returns same cached reference, (4) `require.resolve('./data.json')` returns absolute path, (5) JSON from node_modules via `index.json` works (`require('json-pkg')`), (6) package with `"main"` pointing to a `.json` file works (`require('json-main-pkg')` loads `config.json`). All 115 tests pass (114 existing + 1 new).
- **Notes for next step**: JSON loading uses `Module._extensions['.json']` from Node's real CJS loader (reads file, strips BOM, calls `JSON.parse`). No special handling needed on our side -- it just works.

### Step S12: Test nested node_modules
- **Files**: created `test/test-cjs-node-modules-nested.js`, `test/fixtures/node-modules-nested/main.js`, `test/fixtures/node-modules-nested/node_modules/pkg-a/index.js`, `test/fixtures/node-modules-nested/node_modules/pkg-a/node_modules/pkg-b/index.js`, `test/fixtures/node-modules-nested/node_modules/pkg-c/index.js`, `test/fixtures/node-modules-nested/node_modules/pkg-b-hoisted/index.js`.
- **What was done**: Created fixture directory with 2 levels of node_modules nesting. Structure: top-level has `pkg-a`, `pkg-c`, `pkg-b-hoisted`; `pkg-a` has nested `pkg-b` in its own `node_modules/`. Test verifies 7 cases: (1) `pkg-a` loads from top-level node_modules, (2) `pkg-a`'s nested `pkg-b` loads from `pkg-a/node_modules/pkg-b/`, (3) nested `pkg-b` finds top-level `pkg-c` by walking up the directory tree, (4) `pkg-a` finds hoisted `pkg-b-hoisted` from top-level, (5) `require('pkg-b')` from top level throws MODULE_NOT_FOUND (not visible at top level), (6) `require.resolve('pkg-a')` returns correct path, (7) caching across nesting levels works (same `pkg-c` instance). All 116 tests pass (115 existing + 1 new).

### Step S13: Test circular deps across node_modules
- **Files**: created `test/test-cjs-node-modules-circular.js`, `test/fixtures/node-modules-circular/main.js`, `test/fixtures/node-modules-circular/node_modules/pkg-a/index.js`, `test/fixtures/node-modules-circular/node_modules/pkg-b/index.js`.
- **What was done**: Created fixture directory with two mutually-dependent packages: `pkg-a` sets early exports then requires `pkg-b`; `pkg-b` requires `pkg-a` (circular) and captures what's visible. Test verifies 8 cases: (1) no infinite loop, (2) pkg-b loaded by pkg-a, (3) pkg-b saw pkg-a's early exports (name, early value), (4) pkg-b did NOT see pkg-a's late exports at require time (undefined), (5) late export visible via shared exports reference after resolution, (6) pkg-a's late export directly accessible, (7) module caching (same pkg-a instance), (8) cached pkg-b instance matches. All 117 tests pass (116 existing + 1 new).

### Step S14: Test require.resolve
- **Files**: created `test/test-cjs-require-resolve.js`, `test/fixtures/require-resolve/main.js`, `test/fixtures/require-resolve/local-module.js`, `test/fixtures/require-resolve/node_modules/resolve-pkg/package.json`, `test/fixtures/require-resolve/node_modules/resolve-pkg/lib/entry.js`, `test/fixtures/require-resolve/node_modules/resolve-pkg/index.js`.
- **What was done**: Created fixture directory with `node_modules/resolve-pkg/` (has `"main": "lib/entry.js"` and decoy `index.js`) and a local module. Test verifies 10 cases: (1) `require.resolve('resolve-pkg')` returns absolute path respecting "main" field, (2) `require.resolve('./local-module')` returns absolute path for relative modules, (3) resolving with/without `.js` extension gives same result, (4) `require.resolve` is consistent with what `require()` loads, (5) `require.resolve.paths('resolve-pkg')` returns array of search paths including local `node_modules/`, (6) `require.resolve.paths('fs')` returns null for builtins, (7) `require.resolve('fs')` returns `'fs'` for builtins, (8) `require.resolve` for non-existent package throws MODULE_NOT_FOUND, (9) `require.resolve` with `{ paths: [...] }` option works, (10) `require.resolve` with `{ paths: [] }` throws for non-builtins. All 118 tests pass (117 existing + 1 new).

### Step S15: Test real npm package
- **Files**: created `test/test-cjs-real-npm-package.js`, `test/fixtures/real-npm-package/main.js`, `test/fixtures/real-npm-package/package.json`, `test/fixtures/real-npm-package/package-lock.json`, `test/fixtures/real-npm-package/node_modules/minimist/` (full npm install).
- **What was done**: Installed `minimist@1.2.8` via `npm install` in a fixture directory. Created test script verifying 9 cases: (1) `require('minimist')` loads the real npm package (returns a function), (2) basic argument parsing (--name, --count, positionals), (3) boolean/string options and grouped short flags (-abc, --beep=boop), (4) default values, (5) alias option (-v -> verbose), (6) `--` stops parsing, (7) module caching (same reference), (8) require.resolve returns correct path, (9) loading package.json via require to verify metadata. All 119 tests pass (118 existing + 1 new).
- **Decisions**:
-- Chose `minimist` (1.2.8): zero dependencies, single file (~260 lines), widely-used, exercises multiple JS features (object manipulation, type coercion, array handling).
-- Committed full `node_modules/minimist/` directory as test fixture (136KB, 24 files). Acceptable size for integration test.
-- Kept npm-generated `package.json` and `package-lock.json` in fixture dir (realistic npm project structure).

