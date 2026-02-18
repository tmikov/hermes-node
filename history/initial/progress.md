# Implementation Progress

Tracks progress on `history/initial/2026-02-18-repl-plan.md`.

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
| R1 | Stub `internalBinding('modules')` | — | done | |
| R2 | Shim `internal/modules/helpers.js` | R1 | done | |
| R3 | Enhance `BuiltinModule` shim for REPL | — | done | |
| R4 | Stub `internal/modules/esm/formats.js` | — | done | |
| R5 | Stub `domain` module | — | done | |
| R6 | Remove `internal/readline/interface.js` shim | — | done | |
| R7 | Implement minimal contextify -- ContextifyScript | — | done | |
| R8 | Stub `startSigintWatchdog` / `stopSigintWatchdog` | R7 | done | Already implemented in R7 |
| R9 | Stub `makeContext` and `compileFunction` | R7 | done | |
| R10 | Stub `internal/modules/esm/utils.js` if needed | — | done | Not needed -- lazy loaded |
| R11 | Verify `vm` module loads | R7, R8, R9 | done | |
| R12 | Verify `readline` module loads | R6 | done | Test already exists from R6 |
| R13 | Verify `domain` module loads | R5 | done | Test already exists from R5 |
| R14 | Shim CJS loader `Module` class for REPL | R2 | done | |
| R15 | Wire REPL entry point in `hermes-node.cpp` | R7, R11 | | |
| R16 | Handle `repl.js` line 216 -- `vm.runInNewContext` | R9 | | |
| R17 | Integration test -- REPL loads | R1-R6, R7-R9, R14-R16 | | |
| R18 | REPL entry point test (pipe mode) | R15, R17 | | |
| R19 | Implement SIGINT watchdog | R7, R8 | | |
| R20 | Verify REPL features | R17, R18 | | |
| R21 | REPL history persistence | R6, R17 | | |

## Context Notes

### R1: Stub `internalBinding('modules')`
- **Files**: created `lib/bindings/node_modules.cpp`, `include/hermes/node-compat/bindings/node_modules.h`, `test/test-modules-binding.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **What was done**: Created stub `modules` binding with no-op functions: `enableCompileCache`, `getCompileCacheDir`, `flushCompileCache`, `readPackageJSON`, `getPackageScopeConfig`, `getPackageType`, `getNearestParentPackageJSONType`, `setLazyPathHelpers`. Also exports `compileCacheStatus` array `["FAILED", "ENABLED", "ALREADY_ENABLED", "DISABLED"]` (consumed by `helpers.js` to build name->index map).
- **Decisions**: Included stubs for `readPackageJSON`/`getPackageScopeConfig`/`getPackageType`/`getNearestParentPackageJSONType`/`setLazyPathHelpers` beyond what `helpers.js` needs, since `package_json_reader.js`, `run_main.js`, and `esm/initialize_import_meta.js` also destructure from this binding.

### R2: Shim `internal/modules/helpers.js`
- **Files**: created `libjs/shims/internal/modules/helpers.js`, `test/test-modules-helpers.js`. Modified `lib/embedded-modules/embedded-modules.txt`.
- **What was done**: Created shim providing `makeRequireFunction` and `addBuiltinLibsToObject` for the REPL, plus all other exports from Node's original as stubs. Both key functions lazily load `Module` from `internal/modules/cjs/loader` (will be provided by R14). Also exports `constants`, `compileCacheStatus`, `stripBOM`, `enableCompileCache`, `getCjsConditions`, etc. as stubs.
- **Decisions**: Shimmed rather than using the real `helpers.js` because the original has heavy dependencies (`internal/fs/utils`, `internal/assert`, `internal/url`, `internalBinding('url')`, `internal/modules/package_json_reader.js`, debuglog). Our shim is self-contained, depending only on primordials, `internal/validators`, `internal/util`, and `internalBinding('modules')`.
- **Notes for next step**: `makeRequireFunction` and `addBuiltinLibsToObject` cannot be called until R14 provides the `Module` class via `internal/modules/cjs/loader` shim. The test verifies exports exist and stubs work, but does not exercise the lazy Module loading path. CMake reconfigure required when adding new shim files (build uses `EXISTS` check at configure time).

### R3: Enhance `BuiltinModule` shim for REPL
- **Files**: modified `libjs/shims/internal/bootstrap/realm.js`, created `test/test-builtin-module-shim.js`.
- **What was done**: Enhanced the `BuiltinModule` shim with a list of 31 public built-in module names. Added `getSchemeOnlyModuleNames()` (returns `[]`), `normalizeRequirableId()`, `getAllBuiltinModuleIds()`, constructor. Made `exists`/`canBeRequiredByUsers`/`canBeRequiredWithoutScheme`/`isBuiltin` work against the known module set. Populated `BuiltinModule.map` with instances.
- **Decisions**: Returned empty array from `getSchemeOnlyModuleNames()` since we don't support any scheme-only modules (test, sea, sqlite, quic). Included all modules we currently support plus a few we'll add soon (readline/promises, stream/web, etc.).

### R4: Stub `internal/modules/esm/formats.js`
- **Files**: created `libjs/shims/internal/modules/esm/formats.js`, `test/test-esm-formats.js`. Modified `lib/embedded-modules/embedded-modules.txt`.
- **What was done**: Created shim providing `extensionFormatMap` (static map: `.cjs`->`commonjs`, `.js`->`module`, `.json`->`json`, `.mjs`->`module`, `.wasm`->`wasm`), `mimeToFormat` (regex-based MIME detection), and `getFormatOfExtensionlessFile` (stub returning `'module'`).
- **Decisions**: Shimmed rather than using the real module because it depends on `internalBinding('constants').internal` (our constants binding lacks the `internal` sub-object) and `fsBindings.getFormatOfExtensionlessFile` (not implemented). The REPL only needs `extensionFormatMap`.

### R5: Stub `domain` module
- **Files**: created `libjs/shims/domain.js`, `test/test-domain-basic.js`. Modified `lib/embedded-modules/embedded-modules.txt`.
- **What was done**: Created minimal domain shim with Domain class extending EventEmitter, providing `enter`, `exit`, `run`, `bind`, `intercept`, `add`, `remove`, `_errorHandler` methods. Exports `create()`, `createDomain()`, `Domain`, `active: null`. Sets `process.domain` as getter/setter. Test covers all exported functionality.
- **Decisions**: Shimmed rather than using the real `domain.js` because it depends on `async_hooks` -> `internal/async_hooks` -> `internalBinding('async_context_frame')` (not implemented) and `internal/util.WeakReference`. Also calls `process.hasUncaughtExceptionCaptureCallback()` at load time (not on our process object). The REPL uses domain only for error isolation (`_domain.bind(eval_)` and `_domain.on('error', ...)`), so a minimal shim suffices.

### R6: Remove `internal/readline/interface.js` shim
- **Files**: deleted `libjs/shims/internal/readline/interface.js`, created `test/test-readline-basic.js`. Modified `lib/embedded-modules/embedded-modules.txt`, `libjs/primordials.js`.
- **What was done**: Removed the stub shim that threw on construction, allowing the real Node `internal/readline/interface.js` to be used. Added all readline-related modules to the embedded modules list: `readline`, `readline/promises`, `internal/readline/callbacks`, `internal/readline/emitKeypressEvents`, `internal/readline/promises`, `internal/readline/utils`, `internal/repl/history`. Added `Array.prototype.toSorted` polyfill to primordials (Hermes lacks it; readline/utils.js uses `ArrayPrototypeToSorted` for tab-completion prefix matching). Test verifies `readline.createInterface` works with programmatic input, fires 'line' events, and emits 'close'.
- **Decisions**: Polyfilled `toSorted` in primordials (same pattern as `Symbol.dispose`/`FinalizationRegistry`) rather than patching `internal/readline/utils.js`, keeping Node source unmodified.

### R7: Implement minimal contextify -- ContextifyScript
- **Files**: created `lib/bindings/node_contextify.cpp`, `include/hermes/node-compat/bindings/node_contextify.h`, `test/test-vm-basic.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **What was done**: Created the `contextify` binding with `ContextifyScript` class (constructor + `runInContext` + `createCachedData`), plus stub exports for all other functions needed by `vm.js` and `internal/vm.js` to load: `makeContext`, `compileFunction`, `startSigintWatchdog`, `stopSigintWatchdog`, `watchdogHasPendingSigint`, `measureMemory`, `compileFunctionForCJSLoader`, `containsModuleSyntax`, `constants`. ContextifyScript stores source code with `//# sourceURL=filename` on the JS object and evaluates via `napi_run_script` in `runInContext`. Both `runInThisContext` (sandbox=null) and `runInContext` (sandbox=object) evaluate in the global context since we don't support real sandboxing. `compileFunction` creates a function wrapper via eval.
- **Decisions**: Stored source as a non-enumerable property (`__contextifySource`) on the JS script object rather than using `napi_wrap` data, keeping it simpler. Included all binding exports (including R8/R9 stubs) in this step because `vm.js` destructures `makeContext`/`constants`/`measureMemory` and `internal/vm.js` destructures `compileFunction` at the top level -- they'd fail to load without these exports.
- **Notes for next step**: R8 (SIGINT stubs) and R9 (makeContext + compileFunction) are already stubbed in the binding. R8/R9 can focus on enhancing the stubs (e.g., makeContext setting the private symbol for isContext(), compileFunction parameter handling). The `__contextifySource` property key is a plain string, not a symbol -- collision risk is negligible in our controlled environment.

### R8: Stub `startSigintWatchdog` / `stopSigintWatchdog`
- **What was done**: Already fully implemented as part of R7. `startSigintWatchdog` returns `true`, `stopSigintWatchdog` returns `false`, `watchdogHasPendingSigint` returns `false`. Test coverage included in `test-vm-basic.js`.

### R9: Stub `makeContext` and `compileFunction`
- **Files**: modified `lib/bindings/node_contextify.cpp`, `test/test-vm-basic.js`.
- **What was done**: Enhanced `makeContext` to set the `contextify_context_private_symbol` on the sandbox object, making `isContext()` (in `internal/vm.js`) return `true` for contextified objects. The private symbol is obtained lazily from `internalBinding('util').privateSymbols` on first call and cached as a `napi_ref`. `compileFunction`, `constants`, and `measureMemory` were already functional from R7.
- **Decisions**: Used lazy initialization via `napi_ref` cache for the private symbol rather than passing it during binding init. This avoids coupling between contextify and util binding initialization order. The symbol is retrieved by calling `globalThis.internalBinding('util')` from native code on first `makeContext` call.

### R10: Stub `internal/modules/esm/utils.js` if needed
- **What was done**: Investigation only -- no code changes needed. The `require('internal/modules/esm/utils')` in `internal/vm.js` line 106 is inside the `registerImportModuleDynamically` function body (lazy-loaded), and only executes when `importModuleDynamically` is a non-undefined, non-symbol value. The REPL doesn't use ESM dynamic imports, so `importModuleDynamically` is always undefined and the function returns early at line 100-103 without requiring the module. Neither `vm` nor `internal/vm` are in the embedded modules list yet (will be added in R11).
- **Decisions**: No shim needed for `internal/modules/esm/utils.js`.

### R11: Verify `vm` module loads
- **Files**: created `test/test-vm-module.js`. Modified `lib/embedded-modules/embedded-modules.txt`.
- **What was done**: Added `vm` and `internal/vm` to the embedded modules list. Created test exercising the full `require('vm')` public API: `runInThisContext`, `Script` constructor with `runInThisContext`, `createContext`/`isContext`, `runInNewContext`, and `compileFunction`. All work correctly. CMake reconfigure was required since embedded module resolution uses `EXISTS` at configure time.
- **Notes for next step**: `vm.runInNewContext` evaluates in the global context (no real sandboxing) per our design. This is sufficient for the REPL's use on line 216 (`Object.getOwnPropertyNames(globalThis)`).

### R12: Verify `readline` module loads
- **What was done**: Verified that the existing `test/test-readline-basic.js` (created in R6) passes. The test covers `require('readline')`, `createInterface` with programmatic input, 'line' event firing, and 'close' event. All 97 JS tests pass including this one.
- **Notes for next step**: No new files needed. R6 already performed the verification as part of removing the readline/interface shim.

### R13: Verify `domain` module loads
- **What was done**: Verified that the existing `test/test-domain-basic.js` (created in R5) passes. The test covers `require('domain')`, `create()`, `enter`/`exit`, `run`, `bind`, `add`/`remove`, error handling, and `intercept`. All 97 JS tests pass.
- **Notes for next step**: No new files needed. R5 already created a comprehensive test.

### R14: Shim CJS loader `Module` class for REPL
- **Files**: created `libjs/shims/internal/modules/cjs/loader.js`, `test/test-cjs-loader-module.js`. Modified `lib/embedded-modules/embedded-modules.txt`, `libjs/shims/internal/bootstrap/realm.js`.
- **What was done**: Created shim providing `Module` class with all properties/methods the REPL needs: `builtinModules` (from BuiltinModule), `_nodeModulePaths` (POSIX node_modules path generation), `_resolveLookupPaths`, `_resolveFilename`, `_extensions`, `_cache`, `globalPaths`, and constructor with `require()` method delegating to `globalThis.require`. Also added `domain` and `vm` to the BuiltinModule shim's known module list (now 33 modules). Test verifies all Module class features plus integration with `makeRequireFunction` and `addBuiltinLibsToObject` from the helpers shim.
- **Decisions**: Shimmed rather than using real `internal/modules/cjs/loader.js` because the original (2000+ lines) has deep dependencies on `internal/modules/package_json_reader`, `internal/assert`, `internal/source_map`, V8-specific APIs, and the full CJS resolution algorithm. Our shim is ~130 lines providing just what the REPL needs. `Module.prototype.require` delegates to `globalThis.require` (our loader). `_resolveFilename` returns the request as-is for non-builtins since our loader handles resolution.
- **Notes for next step**: The `_nodeModulePaths` implementation skips segments named `node_modules` (matching Node behavior). `_resolveLookupPaths` handles built-in (returns null), non-relative (uses parent paths + globalPaths), relative with no parent (returns `['.']`), and relative with parent filename (returns parent dir). This is sufficient for REPL tab-completion which uses `_extensions`, `_resolveLookupPaths`, and `globalPaths`.

