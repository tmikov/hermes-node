# Implementation Memory

## Conventions
- Copyright: "Copyright (c) Tzvetan Mikov." (NOT Meta)
- Vendoring: always include upstream LICENSE

## Hermes Build
- CMake macros: `add_hermes_library`, `add_hermes_tool`, `add_hermes_executable`
- GTest: `add_unittest` (needs `LLVH_SOURCE_DIR` = `hermes/external/llvh`)
- Link targets: `hermesNapi`, `hermesvm_a`, `gtest_main`
- NAPI headers: public `hermes/include/hermes/napi/`, internal `hermes/API/napi/hermes_napi.h`
- Our test target: `check-hermes-node` -> `NodeCompatUnitTests`

## Build Gotchas
- ASAN flags: Hermes sets `-fsanitize=address` via `CMAKE_CXX_FLAGS` inside its subdirectory scope only. Our top-level CMakeLists.txt must propagate them via `add_compile_options`/`add_link_options` when `HERMES_ENABLE_ADDRESS_SANITIZER` is ON.
- LLVH includes: gtest headers need `llvh/Support/raw_ostream.h`. The `add_node_compat_unittest()` helper adds LLVH include dirs (`hermes/external/llvh/include`, `gen/include`, build-dir `include`).
- **hermes_napi.h is heavyweight**: It includes `hermes/VM/Runtime.h` etc., pulling in all VM internals (needs LLVH + `libhermesvm-config.h` from `cmake-build-*/hermes/lib/config/`). Avoid including it in public headers. Use our standalone `hermes_napi_event_loop.h` for the event loop struct.
- **GC define not propagated**: Hermes uses `add_definitions(-DHERMESVM_GC_${HERMESVM_GCKIND})` scoped to its subdirectory. Our targets that include Hermes VM headers need `target_compile_definitions(... PRIVATE HERMESVM_GC_${HERMESVM_GCKIND})`.
- **Handle scopes required**: All NAPI calls that create JS values require an open handle scope (`napi_open_handle_scope`). Tests must open one in SetUp and close in TearDown.

## Vendored Deps
- libuv 1.51.0 in `external/libuv/libuv/`, static target `uv_a`

## Event Loop Adapter
- `hermesNodeEventLoop` lib in `lib/event-loop/`, links `uv_a` (PUBLIC)
- `UvEventLoop` class (PIMPL): init/run/close lifecycle, getEventLoop() for NAPI, getLoop() for libuv
- `uv_async_t` handle: unref'd when idle, ref'd when tasks pending (so loop exits cleanly)
- Task queue: mutex + singly-linked list, LIFO push, reversed to FIFO on drain

## Hermes JS Engine Limitations
- No `FinalizationRegistry` (typeof is undefined)
- No `Atomics` (typeof is undefined)
- No async generators (SyntaxError)
- `WeakRef`: supported
- Private class fields (`#x`): supported
- `BigInt`, `BigInt64Array`: supported
- RegExp: lookbehind, named groups, hasIndices/d flag all supported
- `AggregateError`: supported
- Hermes warns about undeclared globals in strict mode — use `var X = globalThis.X` in IIFEs

## JS Test Infrastructure
- `check-hermes-node-js` CMake target runs: `run-primordials-test.sh` (hermes CLI) + `run-boot-test.sh` + binding tests (hermes-node)
- `run-hermes-node-test.sh <binary> <src-dir> <test.js>`: generic test runner, checks for PASS output
- Primordials tests use stock `hermes` CLI binary; bootstrap/binding tests use `hermes-node`
- Hermes doesn't support multiple file args; concatenate files before running
- Hermes doesn't have `load()` function

## Primordials
- `libjs/primordials.js`: thin shim, sets `globalThis.primordials`
- Reuses Node's algorithmic approach (enumerate+copy prototypes)
- Safe* variants are just the originals (no tamper-resistance)
- 156 test assertions in `test/primordials.js`

## Binding Registry
- `hermesNodeBindingRegistry` lib in `lib/binding-registry/`, depends only on NAPI headers (lightweight)
- `BindingRegistry::registerBinding(name, initFunc)` + `getBinding(env, name, &result)` with lazy init + `napi_ref` cache
- `createInternalBindingFunction(env, &fn)` creates JS `internalBinding(name)` function
- `attach(env)` / `detach(env)` lifecycle; detach clears all cached refs

## NAPI Test Pattern
- Tests needing `napi_env`: link `hermesvm_a`, include `hermes/API/napi/`, add `HERMESVM_GC_${HERMESVM_GCKIND}` define, add `${CMAKE_BINARY_DIR}/hermes/lib/config` include
- Create Runtime via `vm::Runtime::create(config)`, env via `hermes_napi_create_env(*rt_)`
- Open handle scope in SetUp, close in TearDown

## Module Loader
- `hermesNodeModuleLoader` lib in `lib/module-loader/`, depends only on NAPI headers
- `ModuleLoader` class: `setLibJsPath/setLibJsNodePath` -> `init(env, primordials, internalBindingFn)` -> `require(env, name, &result)` -> `detach(env)`
- JS side (`libjs/loader.js`): IIFE returning setup function; setup returns require function
- Shim override: checks `libjs/shims/<name>.js` before `libjs-node/<name>.js`
- Sets `globalThis.primordials` and `globalThis.internalBinding` during init
- `//# sourceURL=<path>` for Hermes stack traces in wrapped modules
- `(0, eval)(wrapped)` for global-scope indirect eval
- Tests use `TEST_DATA_PATH` and `LIBJS_PATH` CMake compile definitions for filesystem paths

## Process Object
- `hermesNodeProcess` lib in `lib/process/`, links `uv_a` publicly
- `NodeProcess` class: `setArgv`/`setExecPath` -> `create(env, &result)` -> `detach(env)`
- `process.env` is a JS `Proxy` with native C trap functions (get/set/delete/has/ownKeys/getOwnPropertyDescriptor)
- `process.title` uses `napi_property_descriptor` with `napi_callback` getter/setter (function pointers, NOT `napi_value`)
- `process.hrtime.bigint()` uses `napi_create_bigint_uint64`
- `process.memoryUsage()` RSS via `uv_resident_set_memory`; heap stats stubbed (no Hermes heap API via NAPI)
- `process.uptime()` passes `NodeProcess*` as callback data to access `getStartTime()`

## Bootstrap Sequence
- `hermes-node` binary: full bootstrap in `tools/hermes-node/hermes-node.cpp`
- Sequence: runtime (microtasks on) -> event loop -> napi_env -> console -> bindings -> primordials -> process -> module loader -> user script -> drainJobs -> uv_run -> cleanup
- `--node-lib-path <dir>` overrides project root for libjs/libjs-node paths; default: `bin/../`
- Minimal console via NAPI (Hermes VM's `installConsoleBindings` uses internal VM APIs)
- **Include order**: `hermes_napi.h` must come before `uv_event_loop.h` to avoid `hermes_napi_event_loop` struct redefinition
- hermes-node CMake: links `hermesvm_a` + all our libs; needs LLVH includes + GC define + config include (same as unit tests)

## Native Bindings
- `hermesNodeBindings` lib in `lib/bindings/`, links `uv_a` publicly
- Add new binding source files to `lib/bindings/CMakeLists.txt`
- Register in `hermes-node.cpp` via `registry.registerBinding("name", initFunc)` before `registry.attach(env)`
- Init function signature: `napi_value init(napi_env env, napi_value exports)` (same as `napi_addon_register_func`)
- Public headers: `include/hermes/node-compat/bindings/`
- Constants binding: `initConstantsBinding` — os.errno, os.signals, os.priority, os.dlopen, fs, crypto/zlib/trace (stubs)
- Types binding: `initTypesBinding` — 30 type-check functions (`isMap`, `isDate`, `isPromise`, etc.)
- Util binding: `initUtilBinding` — getOwnNonIndexProperties, privateSymbols, constants, sleep, guessHandleType, stubs for V8-specific introspection

## Type Checking Patterns (types binding)
- Direct NAPI: `napi_is_arraybuffer`, `napi_is_dataview`, `napi_is_date`, `napi_is_promise`, `napi_is_typedarray`, `napi_is_error`
- `napi_instanceof` with global ctor: Map, Set, WeakMap, WeakSet, RegExp, SharedArrayBuffer
- `Object.prototype.toString` tag: boxed primitives, function subtypes (AsyncFunction, GeneratorFunction), iterators (Map/Set Iterator), GeneratorObject, Arguments
- Stubs: `isProxy` (no NAPI detection), `isModuleNamespaceObject` (V8-specific)

## String Decoder Binding
- `initStringDecoderBinding` — state machine for multi-byte character decoding across chunk boundaries
- Decoder state is 7-byte Uint8Array: [0-3] incomplete char buf, [4] missing bytes, [5] buffered bytes, [6] encoding enum
- Encoding enum matches Node: ASCII=0, UTF8=1, BASE64=2, UCS2=3, LATIN1=4, HEX=5, BUFFER=6, BASE64URL=7
- Used by `internal/util.js` for `encodingsMap` and by `string_decoder.js`

## Errors Binding
- `initErrorsBinding` — triggerUncaughtException (prints + exit(1)), noSideEffectsToString, 6 stub callbacks, exitCodes object
- Exit codes from Node's `ExitCode` enum: kNoFailure=0, kGenericUserError=1, kInvalidCommandLineArgument=9, kBootstrapFailure=10, kAbort=134, etc. (13 total)
- Widely used: `exitCodes.kGenericUserError` in async_hooks, process/execution, test_runner; `triggerUncaughtException` in diagnostics_channel, promise_hooks, promises

## Config Binding
- `initConfigBinding` — boolean feature flags + `bits` (pointer size) + `getDefaultLocale()` function
- All crypto/inspector/intl/tracing flags false; `hasNodeOptions` true
- Used by: `internal/navigator.js` (getDefaultLocale), `buffer.js` (hasIntl), `internal/bootstrap/node.js`, `internal/process/pre_execution.js` (hasInspector, noBrowserGlobals), `internal/main/print_help.js` (hasIntl, hasSmallICU, hasNodeOptions)

## Symbols Binding
- `initSymbolsBinding` — 21 unique symbols from Node's `PER_ISOLATE_SYMBOL_PROPERTIES`
- Used by: `internal/async_hooks.js` (owner_symbol, resource_symbol, async_id_symbol, trigger_async_id_symbol), `internal/modules/cjs/loader.js` (imported_cjs_symbol), `internal/worker/js_transferable.js` (messaging_*_symbol), `vm.js` (vm_* symbols)

## Options Shim
- `libjs/shims/internal/options.js`: pure JS shim replacing Node's C++-backed `internal/options`
- Static `optionsMap` with ~90 option defaults; `getOptionValue(name)` does map lookup
- Exports: `getOptionValue`, `refreshOptions` (no-op), `getEmbedderOptions`, `getCLIOptionsInfo`, `getOptionsAsFlagsFromBinding`, `getAllowUnauthorized`, `generateConfigJsonSchema`
- `globalThis.require` exposed by `loader.js` so user scripts can call `require()`

## V8 API Polyfills
- `Error.captureStackTrace`: polyfilled in `primordials.js` before intrinsics loop. Creates Error for stack, sets lazy getter on target.
- `Error.stackTraceLimit`: defined as writable property (default 10) if missing.
- Both picked up by `copyPropsRenamed(Error, ...)` as `ErrorCaptureStackTrace`/`ErrorStackTraceLimit`.
- `internal/v8/startup_snapshot` shim in `libjs/shims/internal/v8/startup_snapshot.js`: `isBuildingSnapshot()` returns false.

## NAPI Property Attributes
- `napi_default` (=0) makes properties non-enumerable, non-writable, non-configurable.
- `napi_set_named_property` creates enumerable+writable+configurable properties.
- Bindings whose exports are spread (`...internalBinding('x')`) must use `napi_enumerable` attribute.
- Currently only the types binding is spread (in `internal/util/types.js`).

## Bootstrap Module Dependencies
- `internal/errors` -> `internal/assert` (eager), `internal/v8/startup_snapshot` (lazy), `internalBinding('util')` (eager for privateSymbols), `internalBinding('uv')` (lazy)
- `internal/util` -> `internal/errors`, `internal/options`, `internal/assert` (eager); `internalBinding('util')`, `internalBinding('types')`, `internalBinding('string_decoder')` (eager); `internalBinding('uv')` (lazy)
- `internal/validators` -> `internal/errors`, `internal/util`, `internal/util/types` (eager); `internalBinding('constants').os` (eager)
- `internal/util/types` -> `internalBinding('types')` (spread, needs enumerable)

## Hermes NAPI Bugs/Workarounds
- **`napi_get_all_property_names` with mixed string+symbol**: When both `plusIncludeSymbols().plusKeepSymbols()` and `plusIncludeNonSymbols()` are set (via `napi_key_all_properties` without skip flags), string property names are returned as Hermes internal SymbolIDs (exposed as JS Symbols). Workaround: make two separate calls — one with `napi_key_skip_symbols` for strings, one with `napi_key_skip_strings` for symbols.
- **`napi_create_string_utf8` rejects invalid UTF-8**: Unlike V8 (which produces replacement chars), Hermes raises a RangeError and returns `napi_generic_failure`. Workaround: catch failure, clear exception, sanitize bytes by replacing invalid sequences with U+FFFD, retry.

## Buffer Binding
- `initBufferBinding` — 34 functions + 2 constants (kMaxLength, kStringMaxLength)
- Core ops: byteLengthUtf8, compare/compareOffset, copy, fill, indexOf{Buffer,Number,String}, swap{16,32,64}, isUtf8, isAscii, atob, btoa
- Slice methods (7): called as methods on Buffer via `this` — asciiSlice, latin1Slice, utf8Slice, hexSlice, base64Slice, base64urlSlice, ucs2Slice
- Write methods: "static" variants (asciiWriteStatic, latin1WriteStatic, utf8WriteStatic) take `(buf, string, offset, length)`; method-style (base64Write, hexWrite, ucs2Write, base64urlWrite) use `this`
- `setBufferPrototype`: no-op; `createUnsafeArrayBuffer`: zero-initialized (NAPI limitation)
- `copyArrayBuffer`: raw memcpy between ArrayBuffers
- Users: `buffer.js`, `internal/buffer.js`, `internal/blob.js`, `internal/webstreams/util.js`, `internal/util/comparisons.js`

## Encoding Binding
- `initEncodingBinding` — 6 functions + 1 property (encodeIntoResults Uint32Array)
- `encodeUtf8String(str)`: returns Uint8Array of UTF-8 bytes
- `encodeInto(str, dest)`: writes UTF-8 into pre-allocated Uint8Array, updates encodeIntoResults[0]=charsRead, [1]=bytesWritten
- `decodeUTF8(buf, ignoreBOM, fatal)`: UTF-8 bytes to string; BOM strip, fatal validation, Hermes sanitization fallback
- `decodeLatin1(buf, ignoreBOM, fatal)`: Latin-1 bytes to string via UTF-8 conversion
- `toASCII`/`toUnicode`: stubbed (pass-through, no ADA/IDNA library)
- Users: `internal/encoding.js` (TextEncoder/TextDecoder), `url.js` (toASCII for IDNA domains)
- **Gotcha**: `napi_get_value_string_utf8` writes null terminator — cannot write directly into an ArrayBuffer of exact string length; use temp buffer + memcpy

## Async Wrap Binding (stub)
- `initAsyncWrapBinding` — all functions are no-ops; shared arrays are zero-initialized typed arrays
- `async_hook_fields`: Uint32Array(9), `async_id_fields`: Float64Array(4), `execution_async_resources`: empty JS array, `async_ids_stack`: empty Float64Array(0)
- `constants`: 13 values from `AsyncHooks::Fields` (kInit=0..kUsesExecutionAsyncResource=8) + `UidFields` (kExecutionAsyncId=0..kDefaultTriggerAsyncId=3)
- `Providers`: 48 non-crypto provider types (NONE=0 through ZLIB); crypto providers omitted (no OpenSSL)
- Used by: `internal/async_hooks.js` (primary consumer), `internal/promise_hooks.js` (setPromiseHooks), `internal/bootstrap/node.js` (setupHooks call)

## Task Queue Binding
- `initTaskQueueBinding` — tickInfo Uint32Array(2), runMicrotasks, setTickCallback, enqueueMicrotask, setPromiseRejectCallback, promiseRejectEvents
- `setTaskQueueDrainMicrotasks(fn, data)`: host sets drain callback before binding init (avoids hermes_napi.h dependency in bindings lib)
- `enqueueMicrotask`: uses `Promise.resolve().then(fn)` (portable NAPI approach)
- promiseRejectEvents: kPromiseRejectWithNoHandler=0, kPromiseHandlerAddedAfterReject=1, kPromiseResolveAfterResolved=2, kPromiseRejectAfterResolved=3
- Used by: `internal/process/task_queues.js` (setupTaskQueue, processTicksAndRejections)

## Async Context Frame Binding (stub)
- `initAsyncContextFrameBinding` — getContinuationPreservedEmbedderData (returns undefined), setContinuationPreservedEmbedderData (no-op)
- Used by: `internal/async_context_frame.js` (inactive when `--async-context-frame` option is false)

## Event Loop Tick Integration
- `uv_check_t` handle runs after I/O polling: drains microtasks then calls tick callback (runNextTicks)
- Handle is unref'd (doesn't keep loop alive), properly closed with `uv_close` + `UV_RUN_NOWAIT` before loop teardown
- Post-script-execution: explicitly drain microtasks and call tick callback before entering event loop
- Bootstrap: load `internal/process/task_queues` -> `setupTaskQueue()` -> set `process.nextTick` and `process._tickCallback`

## Timers Binding
- `initTimersBinding` — setupTimers, getLibuvNow, scheduleTimer, toggleTimerRef, toggleImmediateRef, immediateInfo Uint32Array(3), timeoutInfo Int32Array(1)
- `setTimersEventLoop(uv_loop_t*)`: host sets loop before binding init (same pattern as task_queue)
- Three libuv handles: `uv_timer_t` (timer scheduling), `uv_check_t` (immediate drain after I/O), `uv_idle_t` (prevent poll blocking for refed immediates)
- `timerBase` = `uv_now()` at init; `getLibuvNow()` returns relative time
- `processTimers(now)` return: 0=no timers, >0=next expiry refed, <0=next expiry unrefed
- `closeTimersHandles()`: must call before event loop close
- Bootstrap: load `internal/timers` -> `getTimerCallbacks(runNextTicks)` -> `setupTimers(processImmediate, processTimers)` -> load `timers` -> set 6 globals
- `initializeDebugEnv(process.env.NODE_DEBUG)` must be called before timers load (debuglog dependency)

## Trace Events Binding (stub)
- `initTraceEventsBinding` — getCategoryEnabledBuffer (returns zero Uint8Array), trace (no-op), setTraceCategoryStateUpdateHandler (no-op)
- Required by: `internal/util/debuglog.js`

## Bootstrap Realm Shim
- `libjs/shims/internal/bootstrap/realm.js`: minimal `BuiltinModule` class (exists/canBeRequiredByUsers/isBuiltin all return false)
- Required by: `internal/util/inspect.js` (stack trace formatting)

## Stdio Binding
- `initStdioBinding` — writeString(fd, str), writeBuffer(fd, buf), getHandleType(fd)
- `process.stdout`/`process.stderr`: created in bootstrap as plain objects, not Writable streams
- Minimal event emitter stubs: on/once/removeListener (return this), listenerCount (returns 0)
- `.write(data, cb)`: sync `uv_fs_write`, handles strings/typed arrays, calls cb synchronously
- `.isTTY`: `uv_guess_handle(fd) == UV_TTY`
- Node's `console` requires: `.write()`, `.listenerCount()`, `.once()`, `.removeListener()`
- Upgrade to proper Writable streams when stream module is available (Step 25-26)

## Core Module Verification (Step 24)
- `events`, `path`, `buffer`, `util` all load and work with no additional shims or fixes
- Full dependency chain resolves cleanly: internal/errors, internal/util, internal/validators, internal/util/types, internal/util/inspect, internal/event_target, async_hooks, etc.
- EventEmitter fully functional (on/emit/once/removeListener/listenerCount)
- No `process.emitWarning` issues encountered in basic usage (timers.js warns for overflow/NaN but not in normal operation)

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h:269-300): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- `Runtime::create(RuntimeConfig)` returns `shared_ptr<Runtime>`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`, drain with `runtime->drainJobs()`
- `napi_run_script` does global eval with `compileFlags.strict = false`; no way to set source URL via API, use `//# sourceURL=` comment instead
- Hermes supports `//# sourceURL=` for custom filenames in stack traces
