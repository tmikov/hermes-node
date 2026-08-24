# Issues, Workarounds, and Unimplemented Features

Summary of problems encountered, workarounds applied, and features not yet available after completing Phases 1–4 (Steps 1–33).

---

## Hermes Engine Limitations

These are fundamental gaps in the Hermes JS engine that affected our implementation.

### Async Generators (`async function*`)
Hermes supports async generators but they are off by default. Enabled via `RuntimeConfig::Builder().withEnableAsyncGenerators(true)` (CLI flag: `-Xasync-generators`). All vendored Node.js files using async generators work unmodified.

**Known Hermes bug:** The async generator prototype chain is flat (`gen.prototype.__proto__` is `Object.prototype`, missing the intermediate `AsyncGeneratorPrototype` and `AsyncIteratorPrototype` objects). This does not affect functionality but means `AsyncIteratorPrototype` cannot be derived from the prototype chain as Node does. Our `primordials.js` provides it as a standalone spec-compliant object.

### No `FinalizationRegistry`
**Impact: Low.** Used by Node's `internal/event_target.js` and `internal/abort_controller.js` for leak-prevention cleanup.
- **Workaround:** No-op polyfill in `primordials.js` (guarded by `typeof FinalizationRegistry === 'undefined'`, auto-removed when Hermes adds native support). All 3 Node.js uses (`abort_controller`, `event_target`, `process/finalization`) are leak-prevention — no correctness impact if callbacks never fire.
- The `abort_controller.js` shim is still needed independently due to the `internal/event_target` dependency chain (see below).

### No `Atomics`
**Impact: Low** for Phases 1–4. Would matter for `worker_threads` (Phase 6).

### No `AbortSignal`/`AbortController` Globals
**Impact: Medium.** Used by `events.js`, `stream` pipeline, and others.
- **Workaround:** Created `internal/abort_controller.js` shim with minimal `AbortController`/`AbortSignal` built on EventEmitter + a `DOMException` polyfill. The original depends on `internal/event_target` (needs `internalBinding('performance')`), `internal/webidl`, and `internal/worker/js_transferable` -- a chain we haven't ported. Supports basic abort signaling but not the full EventTarget-based API.

### No `Promise.withResolvers`
**Impact: Low.** Used by stream operators at runtime.
- Stream operators (`.map()`, `.filter()`, `.drop()`, `.take()`, `.flatMap()`) parse and load correctly (async generators work) but fail at runtime when they reach `Promise.withResolvers()` via the `compose -> duplexify -> fromAsyncGen` path. `Duplex.from()` is similarly affected.

### No `Error.captureStackTrace` / `Error.stackTraceLimit`
**Impact: Medium.** V8-isms used pervasively in `internal/errors.js` and throughout the codebase.
- **Workaround:** Polyfilled both in `primordials.js` before the intrinsics enumeration loop:
  - `Error.captureStackTrace(target)`: creates a temporary Error to capture the stack, sets a lazy `.stack` getter on the target object.
  - `Error.stackTraceLimit`: defined as writable property (default 10).
  - Both are picked up by `copyPropsRenamed(Error, ...)` as `ErrorCaptureStackTrace` / `ErrorStackTraceLimit`.

### Strict Mode Undeclared Global Warnings
**Impact: Low.** Hermes warns about undeclared globals in strict mode IIFEs.
- **Workaround:** Use `var X = globalThis.X` at the top of IIFEs to declare globals before use.

---

## Hermes NAPI Bugs

Actual bugs in Hermes's Node-API implementation.

### `napi_get_all_property_names` Returns Strings as Symbols
When both `plusIncludeSymbols().plusKeepSymbols()` and `plusIncludeNonSymbols()` are set (via `napi_key_all_properties`), string property names are returned as Hermes internal SymbolIDs (exposed as JS Symbols).
- **Fixed upstream** in Hermes NAPI (`454e8a3c1`). Workaround removed.

### `napi_create_string_utf8` Rejects Invalid UTF-8
Unlike V8 (which produces U+FFFD replacement characters), Hermes raises a RangeError and returns `napi_generic_failure`.
- **Fixed upstream** in Hermes NAPI (`db4fdc0fe`). Workarounds removed from `string_decoder`, `buffer`, and `encoding_binding`.

---

## Build and Integration Issues

### ASAN Flags Not Propagated
Hermes sets `-fsanitize=address` via `CMAKE_CXX_FLAGS` inside its subdirectory scope only. Our top-level targets don't inherit these flags.
- **Fix:** Top-level `CMakeLists.txt` explicitly adds `add_compile_options(-fsanitize=address)` and `add_link_options(-fsanitize=address)` when `HERMES_ENABLE_ADDRESS_SANITIZER` is ON.

### GC Define Not Propagated
Hermes uses `add_definitions(-DHERMESVM_GC_${HERMESVM_GCKIND})` scoped to its subdirectory. Our targets that include Hermes VM headers need this define.
- **Fix:** `target_compile_definitions(... PRIVATE HERMESVM_GC_${HERMESVM_GCKIND})` on each target.

### Hermes NAPI Header Organization
The public Hermes NAPI headers (`hermes_napi.h`, `hermes_napi_compile.h` in `hermes/API/napi/`) are lightweight C headers that only depend on `node_api.h`. The heavyweight internal headers (`hermes_napi_internal.h`, `hermes_napi_impl.h`) are separate and not needed by bindings code.
- **Include path:** Add `${PROJECT_SOURCE_DIR}/hermes/API/napi` to `target_include_directories` for access to compile/run APIs.
- **Event loop plumbing:** Bindings library uses host-provided callbacks (`setTaskQueueDrainMicrotasks`, `setTimersEventLoop`, `setFsEventLoop`, etc.) set up in `hermes-node.cpp`.

### Event Loop Cleanup Order
`eventLoop.close()` must be called BEFORE `hermes_napi_destroy_env`. Otherwise, async callbacks from in-flight libuv operations fire on a destroyed NAPI environment, causing use-after-free.

### libuv `uv_buf_t` in Async Operations
For `uv_fs_read`/`uv_fs_write` async paths, pass local `uv_buf_t` (or vector's `.data()`) directly. Do NOT pre-allocate `wrap->req.bufs` — libuv overwrites the pointer during async dispatch (copies to internal `bufsml`).

### `uv_dirent_t.name` Memory Ownership
`uv_dirent_t.name` points to memory owned by the `uv_fs_t` request. Must extract/copy directory entry names BEFORE calling `uv_fs_req_cleanup`.

---

## Shims and Stubs

These are JS files we created to replace Node internals that can't load in Hermes.

| Shim | Replaces | Reason |
|------|----------|--------|
| `libjs/shims/internal/options.js` | `internal/options` | Node's version backed by C++ options parser; ours is a static map of ~90 defaults |
| `libjs/shims/internal/bootstrap/realm.js` | `internal/bootstrap/realm` | Minimal `BuiltinModule` class (exists/canBeRequiredByUsers/isBuiltin return false) |
| `libjs/shims/internal/v8/startup_snapshot.js` | `internal/v8/startup_snapshot` | `isBuildingSnapshot()` returns false; all snapshot functions are stubs |
| `libjs/shims/internal/abort_controller.js` | `internal/abort_controller` | Minimal implementation using EventEmitter (original needs event_target + webidl + js_transferable chain) |
| `libjs/shims/internal/event_target.js` | `internal/event_target` | Exports `kWeakHandler`, `kResistStopPropagation` symbols and minimal stubs (original needs `internalBinding('performance')`) |
| `libjs/shims/internal/blob.js` | `internal/blob` | Stub: `createBlobFromFilePath` throws, `isBlob` returns false |
| `libjs/shims/internal/url.js` | `internal/url` | Minimal: `toPathIfFileURL`, `pathToFileURL`, `fileURLToPath`, `isURL` |
| `libjs/shims/internal/process/permission.js` | `internal/process/permission` | Stub: `isEnabled()` returns false, `has()` returns true |
| `libjs/shims/internal/readline/interface.js` | `internal/readline/interface` | Stub (original chain: readline -> repl/history -> os -> credentials). fs/promises uses Interface only for `FileHandle.readLines()` |
| `libjs/shims/internal/worker/js_transferable.js` | `internal/worker/js_transferable` | Stub (original needs `internalBinding('messaging')`). Provides Symbol-based kDeserialize/kTransfer/kTransferList and no-op markTransferMode |

## Vendored File Modifications

No vendored Node.js files (`libjs-node/`) are currently modified. All previous async generator workaround patches have been reverted now that Hermes supports async generators.

---

## Stubbed Native Bindings

These bindings are registered but provide no-op or minimal implementations:

| Binding | Status | Notes |
|---------|--------|-------|
| `async_wrap` | Full stub | All functions are no-ops; shared arrays zero-initialized. Providers enum complete. |
| `async_context_frame` | Full stub | `getContinuationPreservedEmbedderData` returns undefined |
| `trace_events` | Full stub | `getCategoryEnabledBuffer` returns zero Uint8Array; `trace` is no-op |
| `stream_wrap` | Partial stub | `WriteWrap`/`ShutdownWrap` constructors + state array. Sufficient for stream module to load; full impl needed for networking. |

## Stubbed Functions Within Real Bindings

| Binding | Function | Behavior |
|---------|----------|----------|
| `util` | `getPromiseDetails` | Returns undefined (V8-specific) |
| `util` | `getProxyDetails` | Returns undefined (V8-specific) |
| `util` | `previewEntries` | Returns undefined (V8-specific) |
| `util` | `getCallerLocation` | Returns undefined (V8-specific) |
| `util` | `getCallSites` | Returns empty array (V8-specific) |
| `util` | `parseEnv` | Returns empty object |
| `util` | `isInsideNodeModules` | Returns default value (no V8 stack introspection) |
| `types` | `isProxy` | Always returns false (no NAPI detection) |
| `types` | `isModuleNamespaceObject` | Always returns false (V8-specific) |
| `encoding_binding` | `toASCII` | Pass-through (no ADA/IDNA library) |
| `encoding_binding` | `toUnicode` | Pass-through (no ADA/IDNA library) |
| `buffer` | `createUnsafeArrayBuffer` | Zero-initialized (NAPI doesn't expose uninitialized allocation) |
| `config` | `hasOpenSSL` | false |
| `config` | `hasInspector` | false |
| `config` | `hasIntl` | false |
| `errors` | `triggerUncaughtException` | Prints error + `exit(1)` (no proper `process._fatalException`) |
| `errors` | Source map callbacks | All no-ops (no source map support) |
| `process` | `memoryUsage()` heap stats | Stubbed to 0 (no Hermes heap API via NAPI) |

---

## Not Yet Implemented (Beyond Phase 4)

### Deferred from Phases 1–4
- `process.stdin` — requires readable stream + event loop integration
- Signal handling (`process.on('SIGINT', ...)`) — requires libuv signal handles
- Proper `process._fatalException` integration
- Full Writable stream process.stdout/stderr (currently plain objects with stubs)
- Source map support
- ICU / Intl support

### Phase 5 — Networking (Not Started)
- `dns` (c-ares + `uv_getaddrinfo`)
- `net` (TCP/IPC: `tcp_wrap`, `pipe_wrap`, full `stream_wrap`, `handle_wrap`)
- `dgram` (UDP)
- `http` / `https` (llhttp parser + TLS)
- `tls` (OpenSSL)

### Phase 6 — Extended (Not Started)
- `child_process` (`uv_spawn`)
- `crypto` (OpenSSL)
- `zlib` (zlib + brotli)
- `worker_threads` (requires Hermes threading design)

### Permanently Out of Scope
- `inspector` (V8-specific)
- `vm` / `contextify` (V8-specific)
- `v8` module (V8-specific)
- `wasi`, `sea`, `permission`

---

## Performance Notes

- `assert.deepStrictEqual` on large buffers (1MB+) is extremely slow under ASAN. Use `Buffer.compare` instead in tests.
- No simdutf dependency: UTF-8/ASCII validation, base64, hex encode/decode all implemented in plain C++. Functional but slower than Node's simdutf-accelerated versions.
- `createUnsafeArrayBuffer` zero-initializes (NAPI limitation). Slightly slower than Node's uninitialized allocation path.
