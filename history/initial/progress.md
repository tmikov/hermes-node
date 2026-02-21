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
| S5 | Embed required modules | S1, S2 | | |
| S6 | Create/update shims for newly embedded modules | S5 | | |
| S7 | Integrate Node's CJS loader with bootstrap | S3, S6 | | |
| S8 | Test: basic node_modules resolution | S7 | | |
| S9 | Test: package.json "main" field | S7 | | |
| S10 | Test: package.json "exports" field | S7 | | |
| S11 | Test: .json file loading | S7 | | |
| S12 | Test: nested node_modules | S7 | | |
| S13 | Test: circular deps across node_modules | S7 | | |
| S14 | Test: require.resolve | S7 | | |
| S15 | Test: real npm package | S8–S14 | | |

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

