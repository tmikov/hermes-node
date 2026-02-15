# Implementation Progress

Tracks progress on `history/initial/2026-02-14-hermes-node-compat-detailed-plan.md`.

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
| Step 1 | Create repo and CMake scaffolding | — | done | |
| Step 2 | Vendor libuv | 1 | done | |
| Step 3 | Implement libuv-backed event loop adapter | 2 | done | |
| Step 4 | Implement primordials thin shim | — | done | |
| Step 5 | Implement internalBinding registry | 1 | done | |
| Step 6 | Implement internal module loader | 5 | done | |
| Step 7 | Implement process object (basic properties) | 5 | done | |
| Step 8 | Implement bootstrap sequence | 3, 4, 6, 7 | done | |
| Step 9 | Port constants binding | 5 | done | |
| Step 10 | Port types binding | 5 | done | |
| Step 11 | Port util binding | 5 | done | |
| Step 12 | Port string_decoder binding | 5 | done | |
| Step 13 | Port errors binding | 5 | done | |
| Step 14 | Port config binding | 5 | done | |
| Step 15 | Port symbols binding | 5 | done | |
| Step 16 | Implement internal/options shim | 6 | done | |
| Step 17 | Verify bootstrap modules load | 8, 9–16 | done | |
| Step 18 | Port buffer binding | 5 | done | |
| Step 19 | Port encoding_binding | 5 | done | |
| Step 20 | Port async_wrap binding (stub) | 5 | done | |
| Step 21 | Implement process.nextTick | 3, 7 | done | |
| Step 22 | Implement timers binding | 3, 5 | | |
| Step 23 | Implement process.stdout/stderr (minimal) | 7, 21 | | |
| Step 24 | Verify core modules load and work | 17–23 | | |
| Step 25 | Port stream_wrap binding (minimal) | 5, 3 | | |
| Step 26 | Verify streams work | 24, 25 | | |
| Step 27 | Port fs binding — sync operations | 2, 5, 9 | | |
| Step 28 | Port fs binding — async operations | 3, 27 | | |
| Step 29 | Port fs_dir binding | 27 | | |
| Step 30 | Port fs_event_wrap binding | 3, 5 | | |
| Step 31 | Verify fs sync operations | 27, 9 | | |
| Step 32 | Verify fs async operations | 28, 29 | | |
| Step 33 | Run Node.js fs test subset | 28, 29, 30 | | |

## Context Notes

### Step 1: Create repo and CMake scaffolding
- **Files**: created `CMakeLists.txt`, `lib/placeholder/CMakeLists.txt`, `lib/placeholder/placeholder.cpp`, `tools/hermes-node/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `unittests/CMakeLists.txt`, `libjs-node/README.md`. Vendored 347 JS files from Node.js into `libjs-node/`.
- **Decisions**:
  - Use Hermes cmake macros (`add_hermes_library`, `add_hermes_tool`, `add_unittest`) for consistency with Hermes build conventions.
  - `check-hermes-node` custom target depends on `NodeCompatUnitTests` (unit test suite target).
  - `add_node_compat_unittest()` helper function in `unittests/CMakeLists.txt` mirrors Hermes's `add_hermes_unittest()`.
- **What was done**: Created full directory structure (external/, include/hermes/node-compat/, lib/placeholder/, libjs/, libjs-node/, tools/hermes-node/, unittests/, test/). Top-level CMakeLists.txt adds hermes submodule, includes Hermes cmake modules, builds placeholder lib and hermes-node tool. Vendored Node.js v24.13.0 lib/ tree (commit def0bdf8) into libjs-node/ with provenance README. Built and verified: `hermes-node` binary prints usage with no args and exits 0; `check-hermes-node` target succeeds.
- **Notes for next step**: The `hermesNodePlaceholder` library is temporary; replace with real libraries as they are implemented. Hermes key targets: `hermesNapi` (NAPI lib), `hermesvm_a` (static VM), `gtest_main` (testing). LLVH_SOURCE_DIR must be set for gtest includes.

### Step 2: Vendor libuv
- **Files**: created `external/libuv/README.md`, `external/libuv/CMakeLists.txt`, `external/libuv/libuv/` (vendored source). Created `unittests/UvIntegrationTest.cpp`. Modified `CMakeLists.txt` (top-level), `unittests/CMakeLists.txt`.
- **Decisions**:
  - Vendored libuv 1.51.0 from Node.js v24.13.0 `deps/uv/` (same version our target Node uses).
  - Wrapper CMakeLists.txt disables shared lib build and tests (`LIBUV_BUILD_SHARED OFF`, `BUILD_TESTING OFF`).
  - Added ASAN/sanitizer flag propagation in top-level CMakeLists.txt because Hermes sets `CMAKE_CXX_FLAGS` only within its subdirectory scope; our targets outside hermes/ need them explicitly.
  - Added LLVH include directories to `add_node_compat_unittest()` helper since gtest headers reference `llvh/Support/raw_ostream.h`.
- **What was done**: Vendored libuv source tree from Node.js checkout. Created wrapper CMake that builds only the static `uv_a` target. Added `external/libuv` to top-level build. Wrote 3 GTest tests (version check, version string, loop init/close). Fixed two build issues: ASAN flag propagation and LLVH include paths for gtest. All tests pass under ASAN.
- **Notes for next step**: Link against `uv_a` for libuv. The `uv_a` target exports its include directories. libuv warning in `linux.c:1853` (const qualifier) is harmless upstream issue.

### Step 3: Implement libuv-backed event loop adapter
- **Files**: created `include/hermes/node-compat/event-loop/uv_event_loop.h`, `include/hermes/node-compat/event-loop/hermes_napi_event_loop.h`, `lib/event-loop/uv_event_loop.cpp`, `lib/event-loop/CMakeLists.txt`, `unittests/UvEventLoopTest.cpp`. Modified `CMakeLists.txt` (top-level), `unittests/CMakeLists.txt`.
- **Decisions**:
  - Used PIMPL pattern to avoid exposing Hermes VM internals in our public header. The header only needs `<uv.h>` and the standalone `hermes_napi_event_loop` struct definition.
  - Created a standalone `hermes_napi_event_loop.h` header that duplicates the struct definition from `hermes/API/napi/hermes_napi.h` to avoid dragging in all Hermes VM headers (which need LLVH, generated configs, etc.).
  - `cancel_work` returns false (cancellation not supported). The NAPI spec allows this, and `napi_cancel_async_work` returns `napi_generic_failure`.
  - Task queue uses a mutex-protected singly-linked list (LIFO push, reversed on drain for FIFO).
  - `uv_async_t` handle is unref'd when idle (so the loop can exit), ref'd when tasks are pending (so the loop stays alive to process them).
- **What was done**: Implemented `UvEventLoop` class backing `hermes_napi_event_loop` with libuv. `post_work` uses `uv_queue_work` (threadpool). `post_task` uses `uv_async_t` + task queue. Wrote 10 GTest tests covering lifecycle, post_work (single and multiple), cancel_work, post_task (same thread, cross-thread, multiple), and combined work+task. All pass under ASAN.
- **Notes for next step**: Link against `hermesNodeEventLoop` for the event loop. The `hermes_napi_event_loop.h` standalone header must stay in sync with `hermes_napi.h` if the struct changes. When building against Hermes NAPI internals, need LLVH includes + `libhermesvm-config.h` from `cmake-build-asan/hermes/lib/config/`.

### Step 4: Implement primordials thin shim
- **Files**: created `libjs/primordials.js`, `test/primordials.js`, `test/run-primordials-test.sh`. Modified `CMakeLists.txt` (top-level).
- **Decisions**:
  - Reused Node's algorithmic approach (enumerate prototypes with `Reflect.ownKeys` + `getOwnPropertyDescriptor`, create uncurried copies). This ensures we get exactly the right property names without manual enumeration.
  - Skip `FinalizationRegistry` and `Atomics` (not available in Hermes). They are conditionally included if present.
  - Skip `AsyncIteratorPrototype` (requires async generators, unsupported by Hermes). Provide minimal stub with `[Symbol.asyncIterator]`.
  - `Safe*` variants are just the originals (SafeMap === Map, etc.). No freezing.
  - `hardenRegExp` is identity function.
  - `makeSafe` copies prototype/statics but doesn't freeze.
  - Test runs by concatenating primordials.js + test file and running with stock `hermes` CLI.
- **What was done**: Implemented full primordials shim covering: uncurryThis/applyBind, all built-in constructors and their prototypes (Array, String, Object, Map, Set, RegExp, Promise, Error types, TypedArrays, BigInt, DataView, etc.), namespace objects (Math, JSON, Reflect), abstract intrinsics (TypedArray, ArrayIterator, StringIterator, IteratorPrototype), Safe* variants, SafePromise helpers (All/Race/Any/AllSettled and void variants), SafeArrayIterator/SafeStringIterator, hardenRegExp, SafeStringPrototypeSearch, SafeArrayPrototypePushApply. Test has 156 assertions covering all major categories. Added `check-hermes-node-js` CMake target for JS tests.
- **Notes for next step**: Hermes limitations: no `FinalizationRegistry`, no `Atomics`, no async generators. The `hermes` CLI binary must be built (target `hermes`) for JS tests. Hermes warns about undeclared globals in strict mode — we added `var Promise = globalThis.Promise` etc. in the IIFE to suppress these in the shim itself.

### Step 5: Implement internalBinding registry
- **Files**: created `include/hermes/node-compat/binding-registry/binding_registry.h`, `lib/binding-registry/binding_registry.cpp`, `lib/binding-registry/CMakeLists.txt`, `unittests/BindingRegistryTest.cpp`. Modified `CMakeLists.txt` (top-level), `unittests/CMakeLists.txt`.
- **Decisions**:
  - `BindingRegistry` class stores `name -> Entry` map where Entry has `napi_addon_register_func initFunc` and `napi_ref cachedRef`.
  - Lazy initialization: `getBinding()` calls initFunc only on first access, caches result as strong `napi_ref`.
  - `createInternalBindingFunction()` creates a JS function that wraps `getBinding()`, passing the registry pointer as NAPI callback data.
  - Public header depends only on `<node_api_types.h>` (lightweight), not `hermes_napi.h`.
  - `attach()`/`detach()` lifecycle: `detach()` deletes all cached `napi_ref`s before env destruction.
  - Binding name buffer is 256 bytes (sufficient for all Node binding names).
- **What was done**: Implemented `BindingRegistry` with `registerBinding`, `getBinding` (lazy init + cache), `createInternalBindingFunction` (JS-callable `internalBinding(name)` function). CMake library `hermesNodeBindingRegistry` is STATIC, depends only on NAPI headers. Wrote 8 GTest tests: basic get, caching, unknown name throws, multiple bindings, throwing init function, JS function creation and invocation, JS error on unknown, detach/reattach.
- **Issues**: Tests that include `hermes_napi.h` (heavyweight) need: (1) `${CMAKE_BINARY_DIR}/hermes/lib/config` for `libhermesvm-config.h`, (2) `-DHERMESVM_GC_${HERMESVM_GCKIND}` compile definition since Hermes's `add_definitions()` is scoped to its subdirectory.
- **Notes for next step**: Link against `hermesNodeBindingRegistry` for the binding registry. Tests using `hermes_napi_create_env` need `hermesvm_a` (bundles all Hermes), `hermes/API/napi/` include, `libhermesvm-config.h` include, and `HERMESVM_GC_HADES` define. All NAPI calls require an open handle scope (`napi_open_handle_scope`).

### Step 6: Implement internal module loader
- **Files**: created `include/hermes/node-compat/module-loader/module_loader.h`, `lib/module-loader/module_loader.cpp`, `lib/module-loader/CMakeLists.txt`, `libjs/loader.js`, `unittests/ModuleLoaderTest.cpp`, `unittests/module-loader-testdata/` (8 test JS files). Modified `CMakeLists.txt` (top-level), `unittests/CMakeLists.txt`.
- **Decisions**:
  - Split into C++ (`ModuleLoader` class) and JS (`loader.js`) sides. C++ reads loader.js from disk, evaluates it to get a setup function, then calls setup with native helpers.
  - C++ provides `readFileSync(path)` native function to JS for reading module source from disk.
  - JS side handles: module resolution, caching, circular dependencies, wrapping.
  - Module wrapper: `(function(exports, require, module, __filename, __dirname) { ... })`. primordials and internalBinding are set as globals rather than wrapper parameters, matching how Node modules access them as free variables.
  - Shim override mechanism: loader checks `libjs/shims/<name>.js` before `libjs-node/<name>.js`, enabling overrides for modules like `internal/options`.
  - `//# sourceURL=<filepath>` appended to wrapped source for Hermes stack traces.
  - `(0, eval)(wrapped)` used for indirect eval (global scope) in the loader.
  - Public header depends only on `<node_api_types.h>` (lightweight).
- **What was done**: Implemented `ModuleLoader` class with `setLibJsPath`, `setLibJsNodePath`, `init`, `require`, `detach`. JS loader.js implements CJS-style module system with cache, circular dependency support, and shim overrides. CMake library `hermesNodeModuleLoader` is STATIC. Wrote 8 GTest tests: basic require, caching, transitive require, circular require, nested paths (internal/foo), primordials accessible, internalBinding accessible, nonexistent module throws. All tests pass under ASAN.
- **Notes for next step**: Link against `hermesNodeModuleLoader` for the module loader. The loader sets `globalThis.primordials` and `globalThis.internalBinding` during init so that loaded modules can access them. Test data uses `TEST_DATA_PATH` CMake define for module resolution base path.

### Step 7: Implement process object (basic properties)
- **Files**: created `include/hermes/node-compat/process/node_process.h`, `lib/process/node_process.cpp`, `lib/process/CMakeLists.txt`, `unittests/NodeProcessTest.cpp`. Modified `CMakeLists.txt` (top-level), `unittests/CMakeLists.txt`.
- **Decisions**:
  - `NodeProcess` class owns the process object lifecycle: `setArgv`/`setExecPath` -> `create(env)` -> `detach(env)`.
  - `process.env` uses a JS `Proxy` with native trap functions (get/set/deleteProperty/has/ownKeys/getOwnPropertyDescriptor) that call `getenv`/`setenv`/`unsetenv`/`uv_os_environ` directly. No intermediate KVStore abstraction.
  - `process.title` uses `napi_property_descriptor` with `napi_callback` getter/setter (function pointers, NOT `napi_value` JS functions).
  - `process.hrtime()` returns `[seconds, nanoseconds]` array; `process.hrtime.bigint()` returns BigInt via `napi_create_bigint_uint64`.
  - `process.memoryUsage()` provides RSS via `uv_resident_set_memory`; heap stats stubbed to 0 (no Hermes heap stats API via NAPI).
  - `process.uptime()` uses `NodeProcess*` as callback data to access start time recorded in constructor.
  - `process.kill(pid, sig)` takes numeric signal (JS-side signal name lookup deferred to bootstrap).
- **What was done**: Implemented `NodeProcess` class with all non-I/O properties and methods. Properties: pid, ppid, platform, arch, version, versions, argv, execPath, title, env. Methods: cwd, chdir, hrtime, hrtime.bigint, cpuUsage, memoryUsage, uptime, exit, abort, umask, kill. CMake library `hermesNodeProcess` links `uv_a` publicly. Wrote 24 GTest tests covering all properties and methods. All pass under ASAN.
- **Notes for next step**: Link against `hermesNodeProcess`. The process object is created via C++ and set as a JS global. Step 8 (bootstrap) will integrate it with the module loader. Deferred: `process.nextTick` (Step 21), `process.stdout/stderr` (Step 23), signal handling, `process.on('exit')`.

### Step 8: Implement bootstrap sequence
- **Files**: modified `tools/hermes-node/hermes-node.cpp`, `tools/hermes-node/CMakeLists.txt`, `CMakeLists.txt` (top-level), `include/hermes/node-compat/event-loop/hermes_napi_event_loop.h`. Created `test/test-boot.js`, `test/run-boot-test.sh`.
- **Decisions**:
  - Implemented a minimal `console` object (log/info/warn/error) via NAPI in hermes-node.cpp since Hermes VM's `installConsoleBindings` uses internal VM APIs not accessible via NAPI. `console.log`/`info` write to stdout; `console.warn`/`error` write to stderr.
  - `--node-lib-path <dir>` option points to the project root; paths to `libjs/` and `libjs-node/` are derived from it. Default path resolution uses executable location (`bin/../`).
  - Runtime created with `withMicrotaskQueue(true)` and `runtime->drainJobs()` before event loop to flush Promise microtasks.
  - Removed `hermesNodePlaceholder` library from the build (no longer needed).
  - Added `#ifndef HERMES_NAPI_HERMES_NAPI_H` guard around struct definition in standalone `hermes_napi_event_loop.h` to prevent redefinition when both headers are included.
- **What was done**: Full bootstrap sequence: create Hermes runtime (microtasks enabled) -> init libuv event loop -> create napi_env -> install console -> register bindings (empty for now) -> load primordials.js -> create process global -> init module loader -> execute user script -> drain microtasks -> run event loop -> cleanup. hermes-node tool now links `hermesNodeEventLoop`, `hermesNodeBindingRegistry`, `hermesNodeModuleLoader`, `hermesNodeProcess`, `hermesvm_a`. Bootstrap test (`test/test-boot.js`) verifies console.log, process.platform, process.pid, process.cwd(), process.argv, process.version. Shell script `test/run-boot-test.sh` added to `check-hermes-node-js` target. All tests pass under ASAN.
- **Notes for next step**: Future steps (10-15) register native bindings in the `BindingRegistry` before bootstrap. When including `hermes_napi.h` alongside our `uv_event_loop.h`, `hermes_napi.h` must come first in include order (or use the guard). The tool needs LLVH include dirs + GC define + config include, same as unit tests.

### Step 9: Port constants binding
- **Files**: created `include/hermes/node-compat/bindings/node_constants.h`, `lib/bindings/node_constants.cpp`, `lib/bindings/CMakeLists.txt`, `test/test-constants.js`, `test/run-hermes-node-test.sh`. Modified `CMakeLists.txt` (top-level), `tools/hermes-node/hermes-node.cpp`, `tools/hermes-node/CMakeLists.txt`.
- **Decisions**:
  - Created `lib/bindings/` directory for all native bindings with a single `hermesNodeBindings` library target. Future bindings (types, util, etc.) will add source files to this same library.
  - Binding init function follows `napi_addon_register_func` signature: `initConstantsBinding(env, exports)` returns populated exports object.
  - Constants organized in nested objects matching Node's layout: `os.errno`, `os.signals`, `os.priority`, `os.dlopen`, `fs`, `crypto`, `zlib`, `trace`.
  - crypto, zlib, trace: empty objects (stubbed). No OpenSSL/zlib dependencies.
  - Used `SET_CONST` macro with `napi_create_int32`/`napi_set_named_property` for each constant. All constants guarded by `#ifdef` for portability.
  - Created generic `run-hermes-node-test.sh` test runner (takes test script as argument, checks for PASS output) for reuse by future binding tests.
- **What was done**: Implemented `initConstantsBinding` covering all POSIX errno codes, signal numbers, libuv priority/dirent/fs constants, file open flags, permission bits, access flags, copy file flags, and dlopen constants. Registered in bootstrap via `registry.registerBinding("constants", initConstantsBinding)`. JS test verifies signal values (SIGINT=2, SIGTERM=15), errno codes, fs constants (O_RDONLY=0), access flags, dirent types, and stub objects.
- **Notes for next step**: Add new binding source files to `lib/bindings/CMakeLists.txt` and register in `hermes-node.cpp`. The `hermesNodeBindings` library links `uv_a` publicly for libuv constants. Use `run-hermes-node-test.sh` for future binding JS tests.

### Step 10: Port types binding
- **Files**: created `include/hermes/node-compat/bindings/node_types.h`, `lib/bindings/node_types.cpp`, `test/test-types.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - Direct NAPI calls for: `isArrayBuffer`, `isDataView`, `isDate`, `isPromise`, `isTypedArray` (via `napi_is_*`), `isExternal` (via `napi_typeof`), `isNativeError` (via `napi_is_error`).
  - `instanceof` for: `isMap`, `isSet`, `isWeakMap`, `isWeakSet`, `isRegExp`, `isSharedArrayBuffer` (via `napi_instanceof` with global constructor).
  - `Object.prototype.toString` tag checking for: boxed primitives (`isNumberObject`, `isStringObject`, `isBooleanObject`, `isBigIntObject`, `isSymbolObject`, `isBoxedPrimitive`), function subtypes (`isAsyncFunction`, `isGeneratorFunction`), iterator types (`isMapIterator`, `isSetIterator`), `isGeneratorObject`, `isArgumentsObject`.
  - Stubs returning false for: `isProxy` (no NAPI way to detect proxies), `isModuleNamespaceObject` (V8-specific).
  - Also exported: `isAnyArrayBuffer` (ArrayBuffer || SharedArrayBuffer), `isArrayBufferView` (TypedArray || DataView), `isUint8Array` (via `napi_get_typedarray_info`).
- **What was done**: Implemented `initTypesBinding` with 30 type-checking functions covering all types from Node's `node_types.cc` plus extras needed by `internal/util/types.js`. JS test covers all functions with positive and negative cases. Registered in bootstrap. All tests pass under ASAN.
- **Notes for next step**: The `instanceof`-based checks (Map, Set, etc.) can be fooled by `Symbol.hasInstance` overrides, but this matches the pragmatic non-tamper-resistant approach. `isProxy` always returns false -- this only affects `util.inspect` display of Proxy objects.

### Step 11: Port util binding
- **Files**: created `include/hermes/node-compat/bindings/node_util.h`, `lib/bindings/node_util.cpp`, `test/test-util.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - V8-specific introspection functions stubbed: `getPromiseDetails` (returns undefined), `getProxyDetails` (returns undefined), `previewEntries` (returns undefined), `getCallerLocation` (returns undefined), `getCallSites` (returns empty array). These only affect `util.inspect` display.
  - `getOwnNonIndexProperties` implemented via two separate `napi_get_all_property_names` calls (one for strings, one for symbols) to work around Hermes NAPI bug where `plusKeepSymbols()` flag causes string property names to be returned as SymbolIDs.
  - `privateSymbols` object created with regular JS Symbols (not V8 private symbols) using descriptive names matching Node's `PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES`. All 20 private symbols included.
  - `constants` sub-object includes: Promise states (kPending/kFulfilled/kRejected), ExitInfo fields (kExiting/kExitCode/kHasExitCode), PropertyFilter flags (ALL_PROPERTIES through SKIP_SYMBOLS), TransferMode flags (kDisallowCloneAndTransfer/kTransferable/kCloneable).
  - `shouldAbortOnUncaughtToggle` is a Uint32Array(1) initialized to 0.
  - `guessHandleType` returns integer index (0-5) matching Node's JS-side `handleTypes` array.
  - `isInsideNodeModules` returns the default value (2nd arg) since we lack V8 stack introspection.
  - `defineLazyProperties` uses NAPI getter descriptors with `napi_add_finalizer` for cleanup.
  - `constructSharedArrayBuffer` delegates to JS `new SharedArrayBuffer(length)`.
  - `parseEnv` stubbed (returns empty object).
  - `arrayBufferViewHasBuffer` always returns true (Hermes always has backing buffer).
- **What was done**: Implemented `initUtilBinding` with 15 functions and 3 sub-objects covering all exports from Node's `node_util.cc` that `lib/*.js` files use. Registered in bootstrap. JS test covers sleep, guessHandleType, getOwnNonIndexProperties (including ONLY_ENUMERABLE filter), privateSymbols (type checks + property key usage), all constants values, shouldAbortOnUncaughtToggle, stubs, getConstructorName, arrayBufferViewHasBuffer, isInsideNodeModules, getCallSites, parseEnv. All tests pass under ASAN.
- **Issues**: Hermes NAPI `napi_get_all_property_names` with both `plusIncludeSymbols().plusKeepSymbols()` and `plusIncludeNonSymbols()` causes string property names to be returned as Hermes internal SymbolIDs (exposed as JS Symbols). Worked around by making two separate calls -- one for strings (skip_symbols), one for symbols (skip_strings).
- **Notes for next step**: The `defineLazyProperties` implementation creates getter descriptors that call `require(id)[key]` on access. It uses `napi_add_finalizer` for cleanup. The `privateSymbols` are regular JS Symbols, not truly private -- this is acceptable in our controlled environment.

### Step 12: Port string_decoder binding
- **Files**: created `include/hermes/node-compat/bindings/node_string_decoder.h`, `lib/bindings/node_string_decoder.cpp`, `test/test-string-decoder.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - Ported Node's `StringDecoder` state machine to work with NAPI. The decoder state is a 7-byte buffer (Uint8Array) matching Node's layout: bytes 0-3 incomplete char buffer, byte 4 missing bytes, byte 5 buffered bytes, byte 6 encoding enum.
  - Encoding enum values match Node's `enum encoding`: ASCII=0, UTF8=1, BASE64=2, UCS2=3, BINARY/LATIN1=4, HEX=5, BUFFER=6, BASE64URL=7.
  - String creation from raw bytes uses NAPI directly: `napi_create_string_utf8` (UTF-8), `napi_create_string_latin1` (ASCII/Latin1), `napi_create_string_utf16` (UCS2/UTF16LE). Hex and Base64/Base64URL are encoded to ASCII strings in C++.
  - String concatenation (prepend + body) uses `String.prototype.concat` via NAPI.
  - Hermes `napi_create_string_utf8` rejects invalid UTF-8 (raises RangeError). Added fallback: on failure, sanitize bytes by replacing invalid sequences with U+FFFD replacement character, then retry.
- **What was done**: Implemented `initStringDecoderBinding` with: constants (kIncompleteCharactersStart through kNumFields, kSize), encodings array (8 entries), decode() and flush() functions. Full multi-byte character boundary handling for UTF-8, UCS2, Base64/Base64URL. Registered in bootstrap. JS test covers all constants, encodings array, UTF-8 (simple, multi-byte, split across chunks, flush), Latin1, ASCII, Hex, Base64 (including split), UCS2 (including odd-byte split). All tests pass under ASAN.
- **Issues**: Hermes `napi_create_string_utf8` returns `napi_generic_failure` for invalid UTF-8 (unlike V8 which produces replacement chars). Worked around with UTF-8 sanitization that replaces invalid byte sequences with U+FFFD.
- **Notes for next step**: The `encodings` array and `encodingsMap` in `internal/util.js` are used by `string_decoder.js` and `buffer.js`. The string_decoder JS module (`libjs-node/string_decoder.js`) uses `Buffer.alloc(kSize)` for state, so it depends on the buffer module (Step 18).

### Step 13: Port errors binding
- **Files**: created `include/hermes/node-compat/bindings/node_errors.h`, `lib/bindings/node_errors.cpp`, `test/test-errors.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - `triggerUncaughtException(error, fromPromise)` prints the error's stack trace (or stringified value) to stderr and calls `std::exit(1)`. Sufficient for initial use; proper `process._fatalException` integration deferred.
  - `noSideEffectsToString(value)` uses `napi_coerce_to_string`. Not truly side-effect-free (toString can be overridden), but sufficient for error formatting use case.
  - Source map / stack trace callbacks stubbed as no-ops: `setPrepareStackTraceCallback`, `setGetSourceMapErrorSource`, `setSourceMapsEnabled`, `setMaybeCacheGeneratedSourceMap`, `setEnhanceStackForFatalException`. No source map support in Hermes.
  - `getErrorSourcePositions` stubbed returning undefined (requires V8 internal APIs).
  - `exitCodes` object contains all 13 exit codes from Node's `ExitCode` enum in `node_exit_code.h`.
- **What was done**: Implemented `initErrorsBinding` with `triggerUncaughtException`, `noSideEffectsToString`, 6 stub functions, and `exitCodes` object. Registered in bootstrap. JS test verifies all function types, stub invocations don't throw, `noSideEffectsToString` for various types, and all 13 exit code values. All tests pass under ASAN.

### Step 14: Port config binding
- **Files**: created `include/hermes/node-compat/bindings/node_config.h`, `lib/bindings/node_config.cpp`, `test/test-config.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - All feature flags reflect Hermes build capabilities: `hasOpenSSL`, `hasInspector`, `hasIntl`, `hasSmallICU`, `hasTracing` all false. `hasNodeOptions` true. `noBrowserGlobals` false. `fipsMode` false. `openSSLIsBoringSSL` false.
  - `isDebugBuild` derived from `NDEBUG` preprocessor macro.
  - `bits` is `8 * sizeof(intptr_t)` (32 or 64).
  - `getDefaultLocale()` reads `LC_ALL`/`LC_MESSAGES`/`LANG` env vars, strips encoding suffix, converts underscore to hyphen for BCP 47 format. Falls back to "en-US".
- **What was done**: Implemented `initConfigBinding` with 10 boolean properties, `bits` integer, and `getDefaultLocale` function. Registered in bootstrap. JS test verifies all property types and values. All tests pass under ASAN.

### Step 15: Port symbols binding
- **Files**: created `include/hermes/node-compat/bindings/node_symbols.h`, `lib/bindings/node_symbols.cpp`, `test/test-symbols.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - All 21 symbols from Node's `PER_ISOLATE_SYMBOL_PROPERTIES` (src/env_properties.h) created via `napi_create_symbol` with descriptive string names.
  - Used X-macro `SYMBOL_PROPERTIES(V)` for maintainability — adding/removing symbols requires changing only the macro.
  - Symbol descriptions match Node's exact strings (e.g., `handle_onclose_symbol` has description "handle_onclose").
- **What was done**: Implemented `initSymbolsBinding` creating 21 unique symbols. Registered in bootstrap. JS test verifies all symbols exist, are typeof 'symbol', are unique, have descriptive toString, and are usable as property keys. All tests pass under ASAN.

### Step 16: Implement internal/options shim
- **Files**: created `libjs/shims/internal/options.js`, `test/test-options.js`. Modified `libjs/loader.js`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - Pure JS shim (not native binding) using the loader's shim override mechanism: `libjs/shims/internal/options.js` is loaded instead of `libjs-node/internal/options.js`.
  - Static `optionsMap` with sensible defaults for all ~90 options queried by Node's `lib/*.js` modules. Boolean flags default to false, string flags to '', array flags to []. Node v24 defaults used where relevant (e.g., `--experimental-require-module` true, `--experimental-detect-module` true).
  - `getOptionValue(name)` does a simple map lookup, returns undefined for unknown options.
  - `getEmbedderOptions()` returns `{ noBrowserGlobals: false, hasEmbedderPreload: false, noGlobalSearchPaths: false }`.
  - `getCLIOptionsInfo()`, `getOptionsAsFlagsFromBinding()`, `getAllowUnauthorized()`, `refreshOptions()`, `generateConfigJsonSchema()` all stubbed with minimal implementations.
  - Added `globalThis.require = requireModule` in `loader.js` so user scripts (and tests) can call `require()`.
- **What was done**: Implemented full options shim covering all exports from Node's `internal/options.js`. Added JS test verifying all exported functions, option value types (boolean/string/number/array), defaults, and unknown option behavior. All tests pass under ASAN.

### Step 17: Verify bootstrap modules load
- **Files**: modified `libjs/primordials.js` (ErrorCaptureStackTrace polyfill, Error.stackTraceLimit), `lib/bindings/node_types.cpp` (enumerable properties), `CMakeLists.txt`. Created `test/test-bootstrap.js`, `libjs/shims/internal/v8/startup_snapshot.js`. Modified `test/primordials.js` (5 new assertions).
- **Decisions**:
  - Added `Error.captureStackTrace` polyfill to primordials: creates an Error to capture stack, sets lazy `.stack` getter on target object. Placed before intrinsics loop so `copyPropsRenamed` picks it up as `ErrorCaptureStackTrace`.
  - Added `Error.stackTraceLimit` property (defaults to 10) since Hermes doesn't have this V8 property.
  - Fixed types binding (`node_types.cpp`): changed `napi_default` to `napi_enumerable` so `...internalBinding('types')` spread works in `internal/util/types.js`.
  - Created `internal/v8/startup_snapshot` shim: `isBuildingSnapshot()` returns false, all snapshot functions are stubs/no-ops. Required by `internal/errors.js` lazily via `isErrorStackTraceLimitWritable()`.
- **What was done**: All four bootstrap modules load successfully: `internal/assert`, `internal/errors`, `internal/util`, `internal/validators`. Plus `internal/util/types`. Functional tests verify ERR_INVALID_ARG_TYPE error creation, validateString validation, normalizeEncoding. 9 test assertions in `test-bootstrap.js`, 161 primordials assertions (was 156).
- **Issues**: `napi_default` (attribute=0) makes properties non-enumerable. The spread operator only copies enumerable own properties. Any binding whose exports are spread in JS code needs `napi_enumerable` attribute. Currently only the types binding is spread.
- **Notes for next step**: The `internalBinding('uv')` binding is referenced lazily by `internal/util.js` and `internal/errors.js` (via `lazyUv()`) -- it will be needed when error formatting code paths are exercised. Node v24 renamed `validateCallback` to `validateFunction` in validators.

### Step 18: Port buffer binding
- **Files**: created `include/hermes/node-compat/bindings/node_buffer.h`, `lib/bindings/node_buffer.cpp`, `test/test-buffer.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - No simdutf dependency: implemented UTF-8/ASCII validation, base64 encode/decode, hex encode/decode, and string search entirely in C++ without external libraries. Simpler but slower than Node's simdutf-accelerated versions.
  - `setBufferPrototype` is a no-op since we don't create buffers from native code.
  - `createUnsafeArrayBuffer` creates zero-initialized ArrayBuffer (NAPI doesn't expose uninitialized allocation). Safe but slightly slower than Node's uninitialized path.
  - String write "static" variants (asciiWriteStatic, latin1WriteStatic, utf8WriteStatic) take `(buf, string, offset, length)` as positional args. Non-static variants (base64Write, hexWrite, ucs2Write, base64urlWrite) use `this` as the buffer.
  - UTF-8 write truncation: backs off to valid UTF-8 boundary when maxLength cuts a multi-byte sequence.
- **What was done**: Implemented `initBufferBinding` with 34 functions and 2 constants covering all exports from Node's `node_buffer.cc` that `lib/buffer.js` and `lib/internal/buffer.js` use. Functions: byteLengthUtf8, compare, compareOffset, copy, fill, indexOfBuffer/Number/String, swap16/32/64, isUtf8, isAscii, atob, btoa, setBufferPrototype, copyArrayBuffer, createUnsafeArrayBuffer, 7 slice methods (ascii/base64/base64url/latin1/hex/ucs2/utf8), 7 write methods (3 static + 4 method-style). Constants: kMaxLength, kStringMaxLength. Registered in bootstrap. JS test covers all functions with positive and negative cases.
- **Notes for next step**: The buffer JS module (`libjs-node/buffer.js`) also requires `internal/util` (for encodingsMap, normalizeEncoding), `internal/util/types`, and `internal/errors`. The `internal/buffer.js` module adds 60+ read/write prototype methods in pure JS. `internal/blob.js` and `internal/webstreams/util.js` also use kMaxLength and copyArrayBuffer from this binding.

### Step 19: Port encoding_binding
- **Files**: created `include/hermes/node-compat/bindings/node_encoding.h`, `lib/bindings/node_encoding.cpp`, `test/test-encoding.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - No simdutf dependency: UTF-8 validation and Latin-1 to UTF-8 conversion implemented in C++ without external libraries.
  - No ADA library dependency: `toASCII` and `toUnicode` (IDNA domain name conversion) are stubbed as pass-through (return input unchanged). Correct for ASCII domains; internationalized domain names won't be converted to punycode.
  - `encodeIntoResults` is a Uint32Array(2) stored on the exports object and updated via `napi_set_element` (not a shared backing store like Node's `AliasedUint32Array`).
  - `encodeInto` captures `encodeIntoResults` via `napi_ref` callback data to update it after each call.
  - `decodeUTF8` handles BOM stripping (3-byte UTF-8 BOM EF BB BF) when `ignoreBOM=false`, fatal mode validation, and Hermes invalid UTF-8 sanitization fallback.
  - `decodeLatin1` converts Latin-1 bytes to UTF-8 (each byte 0x80-0xFF becomes 2-byte UTF-8 sequence).
- **What was done**: Implemented `initEncodingBinding` with 6 functions and 1 property: `encodeUtf8String` (string to Uint8Array), `encodeInto` (encode into pre-allocated buffer), `decodeUTF8` (UTF-8 bytes to string), `decodeLatin1` (Latin-1 bytes to string), `toASCII` (stub), `toUnicode` (stub), `encodeIntoResults` (Uint32Array). Registered in bootstrap. JS test covers all functions with 50+ assertions including multi-byte encoding, BOM handling, truncation, fatal mode, invalid UTF-8 handling.
- **Issues**: `napi_get_value_string_utf8` writes a null terminator, so writing directly into an ArrayBuffer of exact string length causes heap-buffer-overflow. Fixed by using a temporary buffer and memcpy.
- **Notes for next step**: The `internal/encoding.js` module also depends on `buffer`, `internal/errors`, `internal/validators`. When ICU is unavailable, TextDecoder only supports UTF-8 and Latin-1/windows-1252 fast paths (which we provide); other encodings fall through to a JS-based ICU converter that will fail without ICU.

### Step 20: Port async_wrap binding (stub)
- **Files**: created `include/hermes/node-compat/bindings/node_async_wrap.h`, `lib/bindings/node_async_wrap.cpp`, `test/test-async-wrap.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt` (top-level).
- **Decisions**:
  - All 10 functions are no-op stubs: `setupHooks`, `setCallbackTrampoline`, `pushAsyncContext`, `popAsyncContext`, `executionAsyncResource`, `clearAsyncIdStack`, `queueDestroyAsyncId`, `setPromiseHooks`, `getPromiseHooks`, `registerDestroyHook`.
  - Shared arrays match Node's layout: `async_hook_fields` (Uint32Array, 9 elements), `async_id_fields` (Float64Array, 4 elements), `execution_async_resources` (empty JS array), `async_ids_stack` (empty Float64Array).
  - `constants` object has all 13 fields/uid constants from Node's `AsyncHooks::Fields` and `AsyncHooks::UidFields` enums.
  - `Providers` object has all 48 provider types from `NODE_ASYNC_PROVIDER_TYPES` (non-crypto only, since we have no OpenSSL).
- **What was done**: Implemented `initAsyncWrapBinding` with stub functions, shared typed arrays, constants, and Providers enum. Registered in bootstrap. JS test verifies all function types, no-op invocations, shared array types/sizes/writability, all constant values, and Providers entries. All tests pass under ASAN.

### Step 21: Implement process.nextTick
- **Files**: created `include/hermes/node-compat/bindings/node_task_queue.h`, `lib/bindings/node_task_queue.cpp`, `include/hermes/node-compat/bindings/node_async_context_frame.h`, `lib/bindings/node_async_context_frame.cpp`, `test/test-nexttick.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `CMakeLists.txt`.
- **Decisions**:
  - `task_queue` binding uses a host-provided callback (`setTaskQueueDrainMicrotasks`) for `runMicrotasks` instead of directly including `hermes_napi.h`. This avoids the bindings library depending on heavyweight Hermes VM headers.
  - `enqueueMicrotask` uses `Promise.resolve().then(fn)` as a portable NAPI way to enqueue microtasks (no direct access to Hermes job queue from NAPI).
  - `async_context_frame` binding is a stub with `getContinuationPreservedEmbedderData` (returns undefined) and `setContinuationPreservedEmbedderData` (no-op). Required by `internal/async_context_frame.js`.
  - Bootstrap loads `internal/process/task_queues` via the module loader, calls `setupTaskQueue()`, and sets `process.nextTick` and `process._tickCallback` from the returned object.
  - Event loop integration via `uv_check_t` handle (runs after I/O polling): drains microtasks then calls tick callback. Handle is unref'd so it doesn't keep the loop alive. Properly closed with `uv_close` + `UV_RUN_NOWAIT` before loop teardown.
  - Post-script-execution: explicitly drains microtasks and calls tick callback before entering the event loop, so nextTick callbacks queued during script execution run immediately.
- **What was done**: Implemented `initTaskQueueBinding` (tickInfo Uint32Array, runMicrotasks, setTickCallback, enqueueMicrotask, setPromiseRejectCallback, promiseRejectEvents constants) and `initAsyncContextFrameBinding` (stub). Wired bootstrap to load task_queues module, set up process.nextTick, and integrate with event loop. JS test verifies binding exports, nextTick ordering, nested nextTick, argument passing, enqueueMicrotask, and strict mode this binding. All tests pass under ASAN.
