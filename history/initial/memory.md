# Implementation Memory

Non-obvious gotchas and patterns. For project overview, build config, bootstrap sequence,
module loader, and test infrastructure basics, see `CLAUDE.md`.

## Build Gotchas
- ASAN flags: Hermes sets them inside its subdirectory scope only. Top-level CMakeLists.txt must propagate.
- LLVH includes: gtest headers need `llvh/Support/raw_ostream.h`. `add_node_compat_unittest()` helper adds the paths.
- **hermes_napi.h is heavyweight**: pulls in all VM internals. Avoid in public headers. Use `hermes_napi_event_loop.h` for the event loop struct.
- **GC define not propagated**: `HERMESVM_GC_${HERMESVM_GCKIND}` is subdirectory-scoped. Our targets need `target_compile_definitions`.
- **Handle scopes required**: All NAPI calls creating JS values require `napi_open_handle_scope`.
- **Include order**: `hermes_napi.h` must come before `uv_event_loop.h` (struct redefinition).
- **New shim files require CMake reconfigure** (shim resolution uses `EXISTS` at configure time).
- **HERMES_ENABLE_DEBUGGER**: Set ON via FORCE in top-level CMakeLists.txt. Hermes's `add_definitions(-DHERMES_ENABLE_DEBUGGER)` is subdirectory-scoped — our targets don't inherit it. Add `target_compile_definitions(... PRIVATE HERMES_ENABLE_DEBUGGER)` to any of our targets that need `#ifdef HERMES_ENABLE_DEBUGGER` guards.

## NAPI Gotchas
- `napi_default` (=0) = non-enumerable. Use `napi_enumerable` for bindings whose exports are spread in JS.
- `napi_get_value_string_utf8` writes null terminator — can't write directly into an ArrayBuffer of exact string length; use temp buffer + memcpy.
- `napi_property_descriptor` getter: must be `napi_callback` (fn ptr), NOT `napi_value`.
- `napi_add_finalizer` on getter functions: WRONG. Attach to target object instead (getter outlives finalizer).
- `napi_create_buffer_copy`/`napi_create_buffer`: returns plain Uint8Array in Hermes NAPI, NOT a Node.js Buffer. Wrap with `Buffer.from()` if Buffer methods needed.
- `napi_create_buffer_copy` with `len=0` and `data=nullptr`: returns Uint8Array aliasing previous buffer. Always pass valid non-null pointer.
- **CRITICAL**: Do NOT call `napi_call_function` (re-enter JS) from inside a NAPI callback to wrap buffers. Causes ASAN stack-use-after-return due to interpreter GCScope lifetime on fake stack. Do buffer wrapping in JS instead.

## Hermes VM Bugs (not fixable in NAPI layer)
- `_decodeUTF8SlowPath` OOB read: `napi_create_string_utf8` with truncated multi-byte UTF-8 (e.g. `[0xc3]` — lead byte with no continuation) reads past buffer.
- `napi_create_buffer_copy` with `len=0` and `data=nullptr`: returns a Uint8Array aliasing the previous buffer instead of an empty one.

## Hermes JS Limitations (beyond CLAUDE.md)
- No `Symbol.asyncDispose`/`Symbol.dispose` -- polyfilled in primordials.js
- No `Array.prototype.toSorted()` -- polyfilled in primordials.js
- `Promise.withResolvers`, `WeakRef`, private class fields, BigInt, RegExp advanced: all supported

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`, drain with `runtime->drainJobs()`
- `napi_run_script` does global eval with `compileFlags.strict = false`
- Hermes supports `//# sourceURL=` for custom filenames in stack traces
- Hermes NAPI compile API: `hermes_compile_to_bytecode()` / `hermes_run_bytecode()` / `hermes_free_bytecode()`

## Inspector (CDP Debugger)
- Config fields: `HermesNodeConfig::inspect`, `inspectBrk`, `inspectHost`, `inspectPort` (in `hermes_node_runtime.h`).
- `--inspect-brk` implies `inspect = true`. Port 0 = OS-assigned.
- Flags parsed in `hermes-node.cpp`, `parseInspectHostPort()` handles `PORT` or `HOST:PORT`.
- CDP creation: `CDPDebugAPI::create(*hermesRT)` then `CDPAgent::create(1, *cdpDebugAPI, enqueueTask, messageCallback)`. Call `enableRuntimeDomain()` after creation. Destruction order: cdpAgent -> cdpDebugAPI -> napi_env -> hermesRT.
- `EnqueueRuntimeTaskFunc` = `std::function<void(RuntimeTask)>`, `RuntimeTask` = `std::function<void(HermesRuntime&)>`. Both in `hermes/RuntimeTaskRunner.h`.
- `OutboundMessageFunc` = `std::function<void(const std::string&)>`. In `hermes/cdp/CDPAgent.h`.

## HermesRuntime (JSI) vs vm::Runtime
- `makeHermesRuntime(rtConfig)` returns `unique_ptr<HermesRuntime>` (JSI-level). `hermes/hermes.h` header.
- `hermesRT->getVMRuntimeUnsafe()` returns `void*`, cast to `vm::Runtime*` for NAPI.
- `HermesRuntime` inherits `IHermes` directly -- no need for `castInterface<IHermes>()`.
- `CDPDebugAPI::create()` requires `HermesRuntime&` (JSI level), not `vm::Runtime*`.
- `hermes_napi_create_env()` requires `vm::Runtime*` (low level).
- `hermes/API` include path already in `lib/runtime/CMakeLists.txt` (PRIVATE).

## V8 API Polyfills
- `Error.captureStackTrace`: polyfilled in `primordials.js`. Creates Error for stack, sets lazy getter.
- `Error.stackTraceLimit`: writable property (default 10) if missing.
- Both picked up by primordials `copyPropsRenamed(Error, ...)`.

## Event Loop Adapter
- `UvEventLoop` class (PIMPL): init/run/close lifecycle
- `uv_async_t` handle: unref'd when idle, ref'd when tasks pending (so loop exits cleanly)
- Task queue: mutex + singly-linked list, LIFO push, reversed to FIFO on drain
- **Two drain points**: `uv_prepare_t` (before poll) + `uv_check_t` (after poll); both unref'd. Without prepare handle, nextTick from native callbacks blocks until poll timeout.
- Bootstrap: load `internal/process/task_queues` -> `setupTaskQueue()` -> nextTick + _tickCallback
- Must call `initializeDebugEnv()` before timers load (debuglog dep)

## FS Binding Gotchas
- Stats layout: Float64Array(36), 18 fields per entry. Promise-mode stats must use fresh (non-shared) typed arrays — shared causes race conditions.
- `uv_dirent_t.name` points to `uv_fs_t` request-owned memory. Build JS result BEFORE `uv_fs_req_cleanup`.
- Recursive mkdir: stat after EEXIST to detect file-vs-dir. After loop, stat final path.
- `readFileUtf8`/`writeFileUtf8`: compound open+read/write+close in C++.
- `internalModuleStat(path)`: 0=file, 1=directory, negative=error (no throw).
- `legacyMainResolve(pkgPath, main?, base?)`: returns int index 0-9 into extensions table. Phase 1 (with main): tries `['', '.js', '.json', '.node', '/index.js', '/index.json', '/index.node']`. Phase 2 (fallback): tries `pkgPath/index` + `['.js', '.json', '.node']`.

## simdutf Integration
- Fully integrated in `node_buffer.cpp` and `node_encoding.cpp`: UTF-8/ASCII validation, base64, Latin-1↔UTF-8, UTF-16 length calc.
- `utf8WriteStaticCb` uses `simdutf::trim_partial_utf8()` for boundary truncation.
- Hex codec and substring search remain hand-rolled (simdutf doesn't support these).

## HandleWrap + LibuvStreamBase
- `HandleWrapBase` (`handle_wrap_base.h/cpp`): base for all uv handle wraps. ref/unref/hasRef/close lifecycle. Uses `napi_wrap` + prevent-GC ref.
- `LibuvStreamBase` (`libuv_stream_base.h/cpp`): extends HandleWrapBase for stream I/O.
- **Usage pattern**: subclass calls `initStream(env, jsObj, stream)` after `uv_*_init()`. Call `addStreamMethods(env, prototype)`.
- **GC safety**: `napi_remove_wrap` in doClose() transfers ownership from GC finalizer to uv_close callback, preventing double-free.
- **Shutdown**: `UvEventLoop::close()` calls `uv_walk()` to force-close all remaining handles. `clearHandleWrapEventLoop()` nulls loop pointer.
- **Latin1 write limitation**: `writeLatin1String` uses UTF-8 extraction (no `napi_get_value_string_latin1` in NAPI). Won't preserve exact byte values for chars > 127.

## Stream Wrap Bindings (TTY/TCP/Pipe/UDP)
- All inherit `HandleWrapBase` or `LibuvStreamBase`. Recipe: inherit LibuvStreamBase, `uv_*_init()`, `initStream()`, `addStreamMethods(env, proto)`, `napi_define_class`.
- TTY: `uv_tty_init` can succeed on non-TTY fds. On init failure: set ctx error, do NOT call `initStream`.
- TCP/Pipe: module-level `napi_ref` to constructor for OnConnection. ConnectReqData: heap-allocated with `napi_ref`.
- Pipe: `uv_pipe_connect` does NOT return error (unlike `uv_tcp_connect`). Errors are async. `getsockname/getpeername` return `{address: <path>}` only.
- UDP: inherits HandleWrapBase (NOT stream). `uv_udp_try_send` returns `totalSize + 1` on success.
- **cluster shim**: `net.js` -> `cluster` -> `child_process` -> `dgram` -> `udp_wrap`. Shim `isPrimary: true` breaks chain.
- **convertIpv6StringToBuffer**: in cares_wrap. `net.js` line 65 needs it. Uses `uv_inet_pton(AF_INET6, ...)`.

## cares_wrap Binding (DNS)
- ChannelWrap: wraps `ares_channel_t`, per-socket `uv_poll_t` (AresTask), `uv_timer_t` for timeouts. Uses prevent-GC ref (`selfRef_`). Static `s_channels` set tracks all instances for shutdown.
- **GC safety**: ChannelWrap must NOT be GC'd while libuv handles open. Prevent-GC ref + `closeChannel()` pattern.
- `setCaresWrapEventLoop(loop)` in hermes-node.cpp before binding init. `caresWrapShutdown()` before `eventLoop.close()`.
- Hostname IDNA via `ada::idna::to_ascii()` before `uv_getaddrinfo` (Ada already linked).
- `strerror`: `ares_strerror` for c-ares codes (0..ARES_ECANCELLED), `uv_strerror` for libuv codes (negative).
- `dns.js` needs `internal/perf/observe` shim (no-op `hasObserver`/`startPerf`/`stopPerf`).

## OS & Credentials Bindings
- `os.js` depends on `internalBinding('credentials')` at load time (for `getTempDir`) -- both bindings must exist.
- Error reporting: ctx-object pattern (set errno/message/syscall/code, return undefined). JS uses `getCheckedFunction()` -> `ERR_SYSTEM_ERROR`.
- `getInterfaceAddresses`/`getCPUs`: return flat arrays (7 fields each), JS reassembles into objects.
- `getOSInformation`: returns [sysname, version, release, machine], cached at module load time.

## URL Binding
- Uses Ada C++ API: `ada::url_aggregator` for parsing, `ada::url` for format.
- **host_start offset**: Ada's `host_start` points AT `@` when credentials exist. Must check and skip.
- `internal/url.js` shim: self-contained ~860 lines, NOT Node's original (too many deps).

## Constants Binding
- `node_constants.cpp`: os (signals, errno, priority, dlopen, UV_UDP_*, SOCK_*), fs, crypto/zlib/trace (stubs).
- `dgram.js` gets `UV_UDP_REUSEADDR` from `constants.os`; other UDP constants from `udp_wrap` binding directly.

## HTTP Parser Binding
- HTTPParser wraps `llhttp_t`. Callbacks via indexed properties: `parser[kOnMessageBegin]`, `parser[kOnHeadersComplete]`, etc.
- Header accumulation: 32-pair buffer, flushed via `kOnHeaders` callback. Flat array `[name, value, ...]`.
- `consume()`/`unconsume()`: state tracking only (no C++ stream interception). Data flows JS-side.
- `maxHttpHeaderSize`: 0 = use default (80KB).
- Parser/ConnectionsList destructors must NOT call `napi_delete_reference` (GC finalizer runs after env destroy).

## Process Wrap Binding
- `ProcessWrap` inherits `HandleWrapBase` (NOT stream). `uv_spawn` both inits handle and starts process.
- **CRITICAL**: `uv_spawn` always calls `uv__handle_init` even on failure. Must call `init()` unconditionally to ensure `uv_close` happens during cleanup.
- `napi_wrap` double-wrap: constructor wraps first, `init()` tries re-wrap (fails silently). Works but is a design issue.
- ParseStdioOptions: handles ignore/pipe/overlapped/wrap/fd. For pipe: `HandleWrapBase::unwrap() -> handle()`.

## Spawn Sync Binding
- `SyncProcessRunner`: temp `uv_loop_t`, spawns child, runs loop, collects output.
- `StdioPipe`: internal `uv_pipe_t` on temp loop (JS does NOT create PipeWrap for sync). OutputBuffer: 64KB chunk linked list.
- Output wrapping: done in `internal/child_process.js` spawnSync() -- wraps Uint8Array outputs with `Buffer.from()`.

## Contextify Binding (vm module)
- `makeContext`: sets `contextify_context_private_symbol` on sandbox (lazy-cached `napi_ref` from util binding). No real sandboxing.
- `startSigintWatchdog`/`stopSigintWatchdog`: real POSIX `sigaction` + Hermes `triggerTimeoutAsyncBreak()` via callback pattern. Converts uncatchable timeout error to catchable `ERR_SCRIPT_EXECUTION_INTERRUPTED`.
- **Cross-binding symbol**: contextify calls `globalThis.internalBinding('util')` from native code on first `makeContext` call.
- `compileFunctionForCJSLoader(content, filename, ...)`: wraps as CJS function, evaluates via `napi_run_script`. Uses `napi_run_script` (not `hermes_compile_to_bytecode`) because returned function references bytecode internally -- freeing causes UAF.
- **CJS loader `wrapSafe()` code path**: `patched=false` (default) calls `compileFunctionForCJSLoader` directly. Native side adds CJS wrapper.

## Key Shims
- `internal/abort_controller.js`: minimal AbortController/AbortSignal using EventEmitter
- `internal/event_target.js`: exports `kWeakHandler`/`kResistStopPropagation` symbols only
- `internal/bootstrap/realm.js`: `BuiltinModule` with 33 module names (including `domain`, `vm`)
- `internal/options.js`: static defaults for ~90 CLI options
- `internal/v8/startup_snapshot.js`: `isBuildingSnapshot()` returns false
- `internal/url.js`: comprehensive Ada-backed URL shim
- `internal/perf/observe.js`, `internal/blob.js`, `internal/process/permission.js`, `internal/worker/js_transferable.js`: stubs

## CJS Loader Architecture
- **Real CJS loader** (`internal/modules/cjs/loader.js`) loads successfully. Key methods: `_resolveFilename`, `_findPath`, `_load`, `_compile`, `_nodeModulePaths`.
- **`initializeCJS()`**: called during bootstrap step 11e. MUST be called before REPL or user code.
- **`BuiltinModule.compileForPublicLoader()`**: loads module via captured bootstrap `require`, caches `.exports`.
- **Module constructor**: real loader does NOT set `this.paths` in constructor. Paths set during `Module.prototype.load()`.
- **`__loadUserScript`**: uses `Module._load(path.resolve(filepath), null, true)`.
- **Fallback mechanism**: `Module._load` wrapped in `loader.js` to fall back to bootstrap loader on MODULE_NOT_FOUND.
- **TypeScript**: `.ts` extension handler via `Module._extensions['.ts']`. Uses bootstrap's `compileAndRun()`.
- **ESM resolver**: `internal/modules/esm/resolve.js` loads lazily for `"exports"` field resolution.
- **Shims still needed** (real modules have missing deps): `helpers.js`, `esm/formats.js`, `esm/utils.js`, `typescript.js`, `run_main.js`. Missing runtime deps: `internal/encoding`, `internal/process/execution`, `internal/deps/cjs-module-lexer/lexer`.
- **CRITICAL (loader shim)**: `Module.prototype.require` uses `_loaderRequire` (captured at shim load time), NOT `globalThis.require`. REPL overwrites `globalThis.require` -> circular loop.

## Domain Shim (REPL prereq)
- Real `domain.js` blocked by: `async_hooks` -> `internalBinding('async_context_frame')` (missing)
- REPL uses domain for: `_domain.bind(eval_)`, `_domain.on('error', ...)`, `process.domain` check

## REPL
- Startup: `require('internal/repl').createInternalRepl(process.env, cb)`. Handles `NODE_REPL_HISTORY` etc.
- `process.features` required: `{inspector: false, tls: false, ipv6: false}`. Checked by `internal/repl/utils.js`.
- Acorn parser vendored in `libjs-node/internal/deps/acorn/` for syntax checking/recovery.
- `let`/`const` don't persist across REPL lines (Hermes eval: each `napi_run_script` is separate context). `var` works.
- History: only with `terminal: true`. Default file: `~/.node_repl_history`.
- Shims: `internal/vm/module` (passthrough), `internal/modules/esm/utils` (no-op `registerModule`).
- **Known issue**: `test-repl-features.js` tests 1-2 (multi-line) silently fail -- pre-existing from R20.

## Readline Module
- Dependency chain: `readline` -> `internal/readline/{interface,callbacks,emitKeypressEvents,utils,promises}`, `readline/promises`, `internal/repl/history`, `string_decoder`.
- All deps must be in `embedded-modules.txt`.
- `internal/readline/utils.js` uses `ArrayPrototypeToSorted` -- requires the `toSorted` polyfill.

## Test Infrastructure
- `assert.deepStrictEqual` on large buffers extremely slow under ASAN; use `Buffer.compare` instead.
- **FileCheck pipe hazard**: Tests with async child process cleanup must NOT pipe to FileCheck. Use exit-code-based testing.
- **Test timeout rule**: `check-hermes-node` should complete in under 3 minutes.
- Net write-slow test: reduce data size for ASAN (original 2MB with 20ms delays hangs).
- CJS test pattern: fixture dirs in `test/fixtures/<name>/main.js`, lit file has RUN/CHECK.

## Module Status Summary
- **Working**: events, path, buffer, util, stream (+ operators), fs, fs.promises, os, url, dns (lookup + resolve), net (TCP + Unix sockets), http (server + client), child_process (async + sync), tty, readline, vm, repl
- **CJS resolution complete**: node_modules traversal, package.json main/exports, conditional/subpath/wildcard exports, JSON loading, nested deps, circular deps, require.resolve, npm packages
- **Unverified**: `Duplex.from()` (in `duplexify.js`) may still have issues
