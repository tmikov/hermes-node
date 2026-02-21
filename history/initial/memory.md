# Implementation Memory

Non-obvious gotchas and patterns. For project overview, build instructions, bootstrap sequence,
module loader, JS limitations, and test infrastructure, see `CLAUDE.md`.

## Build Gotchas
- ASAN flags: Hermes sets them inside its subdirectory scope only. Top-level CMakeLists.txt must propagate.
- LLVH includes: gtest headers need `llvh/Support/raw_ostream.h`. `add_node_compat_unittest()` helper adds the paths.
- **hermes_napi.h is heavyweight**: pulls in all VM internals. Avoid in public headers. Use `hermes_napi_event_loop.h` for the event loop struct.
- **GC define not propagated**: `HERMESVM_GC_${HERMESVM_GCKIND}` is subdirectory-scoped. Our targets need `target_compile_definitions`.
- **Handle scopes required**: All NAPI calls creating JS values require `napi_open_handle_scope`.
- **Include order**: `hermes_napi.h` must come before `uv_event_loop.h` (struct redefinition).

## NAPI Gotchas
- `napi_default` (=0) = non-enumerable. Use `napi_enumerable` for bindings whose exports are spread in JS.
- `napi_get_value_string_utf8` writes null terminator — can't write directly into an ArrayBuffer of exact string length; use temp buffer + memcpy.
- `napi_property_descriptor` getter: must be `napi_callback` (fn ptr), NOT `napi_value`.
- `napi_add_finalizer` on getter functions: WRONG. Attach to target object instead (getter outlives finalizer).

## Event Loop Adapter
- `UvEventLoop` class (PIMPL): init/run/close lifecycle
- `uv_async_t` handle: unref'd when idle, ref'd when tasks pending (so loop exits cleanly)
- Task queue: mutex + singly-linked list, LIFO push, reversed to FIFO on drain

## FS Gotchas
- Stats layout: Float64Array(36), 18 fields per entry. Promise-mode stats must use fresh (non-shared) typed arrays — shared Float64Array causes race conditions with concurrent callback-mode ops.
- `uv_dirent_t.name` points to `uv_fs_t` request-owned memory. Build JS result BEFORE `uv_fs_req_cleanup`.
- Recursive mkdir: stat after EEXIST to detect file-vs-dir. After loop, stat final path.
- `readFileUtf8`/`writeFileUtf8`: compound open+read/write+close in C++.
- `internalModuleStat(path)`: 0=file, 1=directory, negative=error (no throw).
- `legacyMainResolve(pkgPath, main?, base?)`: returns int index 0-9 into extensions table. Phase 1 (with main): tries `pkgPath/main` + `['', '.js', '.json', '.node', '/index.js', '/index.json', '/index.node']`. Phase 2 (fallback): tries `pkgPath/index` + `['.js', '.json', '.node']`. Throws `ERR_MODULE_NOT_FOUND` if no file found. Used by ESM resolver for package.json "main" field resolution.

## Lit Test Config
- `test/lit.cfg` `config.excludes` list: add directory names to exclude from test discovery. `fixtures` excluded to prevent fixture .js files from being picked up as tests.

## V8 API Polyfills
- `Error.captureStackTrace`: polyfilled in `primordials.js`. Creates Error for stack, sets lazy getter.
- `Error.stackTraceLimit`: writable property (default 10) if missing.
- Both picked up by primordials `copyPropsRenamed(Error, ...)`.

## Key Shims
- `internal/abort_controller.js`: minimal AbortController/AbortSignal using EventEmitter (original depends on `event_target` -> `internalBinding('performance')`)
- `internal/event_target.js`: exports `kWeakHandler`/`kResistStopPropagation` symbols only
- `internal/bootstrap/realm.js`: `BuiltinModule` class with 31 public module names. `getSchemeOnlyModuleNames()` returns `[]`. `exists`/`isBuiltin`/`normalizeRequirableId` work against known set. `map` populated with instances.
- `internal/readline/interface.js`: stub (original chain: readline -> repl/history -> os -> credentials)
- `internal/worker/js_transferable.js`: stub (original needs internalBinding('messaging'))
- `internal/blob.js`, `internal/process/permission.js`: stubs
- `internal/url.js`: comprehensive Ada-backed URL shim (not a stub)

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`, drain with `runtime->drainJobs()`
- `napi_run_script` does global eval with `compileFlags.strict = false`
- Hermes supports `//# sourceURL=` for custom filenames in stack traces

## OS Binding
- `os.js` depends on `internalBinding('credentials')` at load time (for `getTempDir`) -- both bindings must exist.
- Error reporting: ctx-object pattern. Set errno/message/syscall/code on ctx, return undefined. JS side uses `getCheckedFunction()` to throw `ERR_SYSTEM_ERROR`.
- `getInterfaceAddresses` returns flat array [name, addr, netmask, family, mac, internal, scopeid, ...] -- 7 fields per interface. JS reassembles into nested object.
- `getCPUs` returns flat array [model, speed, user, nice, sys, idle, irq, ...] -- 7 fields per CPU. JS reassembles.
- `getOSInformation` returns [sysname, version, release, machine]. Called once at module load; type/version/release/machine are cached as module-level constants.
- `getUserInfo` takes (options, ctx) -- options may be null/object. Returns {uid, gid, username, homedir, shell}.
- `credentials` binding: `getTempDir()` checks TMPDIR/TMP/TEMP env vars; `safeGetenv(key)` is simplified getenv.

## Constants Binding
- `node_constants.cpp`: os (signals, errno, priority, dlopen, UV_UDP_*, SOCK_*), fs, crypto/zlib/trace (stubs)
- Node only puts `UV_UDP_REUSEADDR` in `os_constants`; we also add IPV6ONLY/PARTIAL/REUSEPORT + SOCK_* for completeness
- `dgram.js` gets `UV_UDP_REUSEADDR` from `constants.os`; other UDP constants from `udp_wrap` binding directly

## simdutf Integration
- Fully integrated in `node_buffer.cpp` and `node_encoding.cpp`: UTF-8/ASCII validation, base64, Latin-1↔UTF-8, UTF-16 length calc.
- `utf8WriteStaticCb` uses `simdutf::trim_partial_utf8()` for boundary truncation.
- Hex codec and substring search remain hand-rolled (simdutf doesn't support these).
- UCS2 slice/write use NAPI UTF-16 functions directly (no manual transcoding).

## HandleWrap + LibuvStreamBase
- `HandleWrapBase` (`handle_wrap_base.h/cpp`): base class for all uv handle wraps. Manages ref/unref/hasRef/close lifecycle with GC-safe finalizer. Uses `napi_wrap` + prevent-GC ref.
- `LibuvStreamBase` (`libuv_stream_base.h/cpp`): extends HandleWrapBase for stream I/O. Provides readStart/readStop/write*/writev/shutdown/getWriteQueueSize/setBlocking.
- **Usage pattern**: subclass constructors call `initStream(env, jsObj, stream)` after `uv_*_init()`. Call `addStreamMethods(env, prototype)` on the constructor's prototype.
- **JS callback flow**: read data arrives via `handle.onread(arrayBuffer)` with `streamBaseState[kReadBytesOrError]` set. Write completion calls `reqObj.oncomplete(status)`. Both are set by JS (stream_base_commons.js).
- **streamBaseState sharing**: `initStreamWrapBinding` creates the Int32Array and passes the pointer to `LibuvStreamBase::setStreamBaseState()`.
- **Event loop**: `setHandleWrapEventLoop(loop)` must be called in hermes-node.cpp before any handle wraps are created.
- **GC safety**: `napi_remove_wrap` in doClose() transfers ownership from GC finalizer to uv_close callback, preventing double-free.
- **getJsObject()**: public method on HandleWrapBase to get JS object from prevent-GC ref. Needed by libuv callbacks to call JS methods on the handle.
- **Latin1 write limitation**: `writeLatin1String` uses UTF-8 extraction (no `napi_get_value_string_latin1` in NAPI). Acceptable for networking text but won't preserve exact byte values for chars > 127.

## TTY Wrap
- `TTYWrap` inherits `LibuvStreamBase`, wraps `uv_tty_t`. Constructor: `TTY(fd, ctx)`, static: `isTTY(fd)`, instance: `getWindowSize(out)`, `setRawMode(flag)`.
- On `uv_tty_init` failure, set error on ctx and do NOT call `initStream` — HandleWrapBase finalizer checks `state_ == kClosed` and just deletes.
- `uv_tty_init` can succeed on non-TTY fds (e.g. pipes). Don't assume it fails when `isTTY()` returns false.
- `require('tty')` needs `net.js` which needs `cares_wrap`/`tcp_wrap`/`pipe_wrap`. Can't test `tty` module until those are implemented.
- Pattern for new stream wraps: inherit `LibuvStreamBase`, call `initStream()` after `uv_*_init()`, call `addStreamMethods(env, prototype)`, use `napi_define_class` for constructor.

## Vendored c-ares
- c-ares 1.34.6 in `external/cares/cares/`, static target `c-ares`, alias `cares_a`
- When `CARES_SHARED=OFF`, static target is named `c-ares` (no `_static` suffix). The suffix is only set inside the `IF (CARES_SHARED)` block.
- `ares_init()` is deprecated; use `ares_init_options()` instead.
- c-ares uses its own CMake build (897 lines). We delegate via `add_subdirectory` like libuv.

## cares_wrap Binding (DNS)
- `initCaresWrapBinding` in `node_cares_wrap.cpp`: `getaddrinfo`, `getnameinfo`, `canonicalizeIP`, `strerror`, constructors, constants, ChannelWrap with full c-ares integration
- `setCaresWrapEventLoop(loop)` called in hermes-node.cpp before binding init
- `caresWrapShutdown()` called before `eventLoop.close()` — closes all live ChannelWraps
- Hostname IDNA: `ada::idna::to_ascii()` before `uv_getaddrinfo` (Ada already linked)
- Async pattern: `GetAddrInfoReq` struct with `napi_ref` to JS request obj, `uv_getaddrinfo_t`, order. Callback builds IP string array via `uv_inet_ntop`, calls `reqObj.oncomplete(status, addresses)`.
- ChannelWrap: wraps `ares_channel_t` with per-socket `uv_poll_t` (AresTask) and `uv_timer_t` for timeout processing. Uses prevent-GC ref (`selfRef_`) — released in `closeChannel()` after all handles closed.
- **GC safety**: ChannelWrap must NOT be GC'd while libuv handles are open — embedded `uv_timer_t` would be freed but `uv_close` callback fires later. Prevent-GC ref solves this. Static `s_channels` set tracks all live instances for shutdown.
- Query types: A, AAAA, MX, NS, TXT, SRV, CNAME, PTR, NAPTR, SOA, CAA, reverse (getHostByAddr). Uses deprecated `ares_query()` + `ares_parse_*_reply()` API (simpler, Node v24 compatible).
- `dns.js` needs `internal/perf/observe` shim (no-op `hasObserver`/`startPerf`/`stopPerf`)
- `initializeDns()` not called in our bootstrap; `dnsOrder` defaults to `undefined`, falls through to verbatim — works fine
- `strerror`: uses `ares_strerror` for c-ares codes (0..ARES_ECANCELLED), `uv_strerror` for libuv codes (negative)

## Hermes VM Bugs (not fixable in NAPI layer)
- `_decodeUTF8SlowPath` OOB read: `napi_create_string_utf8` with truncated multi-byte UTF-8 (e.g. `[0xc3]` — lead byte with no continuation) reads past buffer. Avoid passing truncated multi-byte sequences to `napi_create_string_utf8`.
- `napi_create_buffer_copy` with `len=0` and `data=nullptr`: returns a Uint8Array aliasing the previous buffer instead of an empty one. Always pass a valid non-null pointer, even for zero-length buffers.

## TCP Wrap
- `TCPWrap` inherits `LibuvStreamBase`, wraps `uv_tcp_t`. Constructor: `TCP(type)` where type is SOCKET(0) or SERVER(1).
- Instance methods: bind, bind6, listen, connect, connect6, open, getsockname, getpeername, setNoDelay, setKeepAlive, reset.
- **OnConnection**: stores module-level `napi_ref` to TCP constructor. When connection arrives, `napi_new_instance()` creates client TCPWrap, `uv_accept()` transfers the connection, calls `server.onconnection(status, clientHandle)`.
- **ConnectReqData**: heap-allocated struct with `uv_connect_t`, `napi_env`, `napi_ref` to JS req. AfterConnect callback calls `reqObj.oncomplete(status, handle, req, readable, writable)`.
- **AddressToJS**: helper converting `sockaddr` to JS object `{address, family, port}`.
- **cluster shim**: `net.js` → `cluster` → `child_process` → `dgram` → `udp_wrap`. Shim with `isPrimary: true` breaks the chain (standalone CLI is always primary).
- **convertIpv6StringToBuffer**: added to cares_wrap. `net.js` line 65 needs it. Uses `uv_inet_pton(AF_INET6, ...)` → 16-byte buffer.

## Pipe Wrap Binding
- `PipeWrap` inherits `LibuvStreamBase`, wraps `uv_pipe_t`. Constructor: `Pipe(type)` where type is SOCKET(0), SERVER(1), or IPC(2). IPC flag passed to `uv_pipe_init`.
- Instance methods: open, bind, listen, connect, fchmod, getsockname, getpeername.
- **uv_pipe_connect**: does NOT return an error code (unlike `uv_tcp_connect`). Connect always returns 0; errors reported asynchronously via callback.
- **getsockname/getpeername**: for pipes, return `{address: <path>}` (no port/family fields, just the socket path).
- **OnConnection**: same pattern as TCPWrap — module-level `napi_ref` to Pipe constructor.
- `net.createServer`/`net.connect` with Unix domain socket paths now fully functional.

## Net Module (verified)
- `require('net')` loads and works for TCP and Unix domain sockets.
- TCP echo, concurrent connections, server address, socket properties, setTimeout, setNoDelay/setKeepAlive, error handling (ECONNREFUSED), DNS hostname lookup integration all verified.
- `net.isIP`/`net.isIPv4`/`net.isIPv6` work correctly.
- 4 Node.js tests ported and passing: server-close, socket-timeout, write-slow, pipe-connect-errors.
- `process.getuid()` not wired on process object. Use `internalBinding('credentials').getuid()` as fallback.
- Write-slow test: original uses 2MB data with 20ms delays; under ASAN this hangs. Reduce data size for ASAN CI.
- Test common module now has `localhostIPv4` and `PIPE` properties for net tests.

## Vendored llhttp
- llhttp 9.3.0 in `external/llhttp/llhttp/`, static target `llhttp_a`
- Simple wrapper CMake (3 source files), not delegating to llhttp's own CMake
- API: `llhttp_init(parser, type, settings)`, `llhttp_execute(parser, data, len)`. Callbacks via `llhttp_settings_t` function pointers.
- Parser state: `parser->method`, `parser->status_code`, `parser->http_major/minor`, `parser->upgrade`, `parser->content_length`

## HTTP Parser Binding
- `initHttpParserBinding` in `node_http_parser.cpp`: HTTPParser constructor, ConnectionsList, methods/allMethods arrays, constants
- HTTPParser wraps `llhttp_t`. Callbacks via indexed properties: `parser[kOnMessageBegin]`, `parser[kOnHeadersComplete]`, etc.
- Header accumulation: 32-pair buffer, flushed via `kOnHeaders` callback. Flat array `[name, value, name, value, ...]`.
- `kOnHeadersComplete` returns: 0=continue, 1=skip body, 2=pause for upgrade
- `execute(buffer)` returns byte count on success, Error object with code/reason/bytesParsed on failure
- `consume()`/`unconsume()`: state tracking only (no C++ stream interception). Data flows JS-side via `socketOnData` -> `parser.execute()`.
- `maxHttpHeaderSize`: 0 = use default (80KB). Node JS passes `req.maxHeaderSize || 0`.
- Parser/ConnectionsList destructors must NOT call `napi_delete_reference` (GC finalizer runs after env destroy).
- `llhttp_a` linked via PRIVATE in CMakeLists.txt.

## Hermes JS Limitations (additions)
- No `Symbol.asyncDispose`/`Symbol.dispose` -- polyfilled in primordials.js
- No `Array.prototype.toSorted()` -- polyfilled in primordials.js (was patched in `http.js` to use `.slice().sort()`, now globally available)

## Readline Module
- `require('readline')` loads and works with the real Node implementation (no shim needed)
- Dependency chain: `readline` -> `internal/readline/{interface,callbacks,emitKeypressEvents,utils,promises}`, `readline/promises`, `internal/repl/history`, `string_decoder`
- All deps must be in `embedded-modules.txt` (module loader can only load embedded or disk files)
- `internal/readline/utils.js` uses `ArrayPrototypeToSorted` -- requires the `toSorted` polyfill

## HTTP Module (verified)
- `require('http')` loads and works for both server and client
- GET, POST with body, chunked transfer encoding, keep-alive, headers, status codes, streaming, timeouts, DNS hostname resolution all verified
- 12 Node.js tests ported and passing: request-end, status-code, client-get-url, date-header, methods, head-response-has-no-body, content-length, set-cookies, write-empty-string, no-content-length, contentLength0, keep-alive
- `test/node-tests/common/countdown.js` helper added for tests needing sequential request counting
- HTTP Agent keep-alive works (multiple requests on same connection)
- 204 No Content correctly omits body
- Client `timeout` option works (fires 'timeout' event, then destroy to abort)
- `http.METHODS` includes QUERY (added in llhttp 9.3.0 per RFC 9110)

## Process Wrap Binding
- `ProcessWrap` inherits `HandleWrapBase` (NOT LibuvStreamBase -- processes are not streams).
- `uv_process_t` is NOT initialized in constructor. `uv_spawn` both initializes the handle AND starts the process. `HandleWrapBase::init()` is called only after successful `uv_spawn`.
- GC finalizer: uses `napi_wrap` directly in constructor (not via HandleWrapBase::init). If state is kClosed (never spawned), just deletes. If initialized, calls doClose().
- OnExit callback: `onexit(exitStatus, signalName)`. exitStatus as double (int64_t range), signal as string ("SIGTERM" etc.) via `signoString()`.
- ParseStdioOptions: handles ignore/pipe/overlapped/wrap/fd. For pipe: extracts `uv_stream_t*` from JS handle via `HandleWrapBase::unwrap() -> handle()`.
## Spawn Sync Binding
- `SyncProcessRunner` in `node_spawn_sync.cpp`: creates temp `uv_loop_t`, spawns child, runs loop until exit, collects output. Stack-allocated runner, loop destroyed after use.
- `StdioPipe`: internal `uv_pipe_t` on temp loop. For sync spawn, JS does NOT create PipeWrap handles -- C++ creates pipes internally.
- OutputBuffer: linked list of 64KB chunks for capturing pipe output.
- Timeout: `uv_timer_t` on temp loop (unref'd). On timeout, kills child with configured signal, closes all pipes, sets UV_ETIMEDOUT error.
- maxBuffer: tracks total buffered output, kills child on overflow (UV_ENOBUFS).
- **CRITICAL**: Do NOT call `napi_call_function` (re-enter JS) from inside a NAPI callback to wrap buffers. Causes ASAN stack-use-after-return crash due to interpreter GCScope lifetime on fake stack. Do buffer wrapping in JS instead.
- Output Buffer wrapping: done in `internal/child_process.js` spawnSync() -- wraps Uint8Array outputs with `Buffer.from()`.

## Child Process Module
- `require('child_process')` loads and works for both async and sync operations.
- Async: spawn, exec, execFile, kill all work.
- Sync: spawnSync, execSync, execFileSync all work. Supports stdin input, timeout, maxBuffer, cwd, env.
- `cluster.js` shim still needed (breaks `child_process` -> `dgram` -> `udp_wrap` chain at load time).

## Process Stdio Streams
- `process.stdin/stdout/stderr` are proper Node.js streams since N5.20. Lazy-initialized via getters.
- `libjs/setup-stdio.js`: bootstrap script, runs after module loader/timers/debuglog but before console.
- Uses `guessHandleType(fd)` from `internal/util` (backed by `uv_guess_handle` in util binding).
- TTY -> `tty.WriteStream`/`tty.ReadStream`, PIPE/TCP -> `net.Socket`, FILE -> `SyncWriteStream`/`fs.ReadStream`.
- stdin starts paused (readStop). stdout/stderr have `dummyDestroy` override to prevent fd closure.
- **Shutdown**: must close native `_handle.close()` on stdio streams BEFORE event loop destroy. Otherwise GC finalizer triggers UAF.
- `internal/process/signal.js` shimmed (no-op). No `signal_wrap` binding yet.

## Module Wrap Binding (CJS loader prereq)
- `initModuleWrapBinding` in `node_module_wrap.cpp`: stub for ESM module wrapping
- Exports: `kEvaluated` (int 4), `createRequiredModuleFacade` (throws ERR_REQUIRE_ESM)
- CJS loader destructures both at top level: `const { kEvaluated, createRequiredModuleFacade } = internalBinding('module_wrap')`

## Modules Binding (REPL prereq + CJS loader prereq)
- `initModulesBinding` in `node_modules.cpp`: compile cache stubs + real `readPackageJSON`.
- Exports: `enableCompileCache`, `getCompileCacheDir`, `flushCompileCache`, `readPackageJSON`, `getPackageScopeConfig`, `getPackageType`, `getNearestParentPackageJSONType`, `setLazyPathHelpers`, `compileCacheStatus` (array).
- `compileCacheStatus` is `["FAILED", "ENABLED", "ALREADY_ENABLED", "DISABLED"]` -- JS builds name->index map.
- `readPackageJSON(path, isESM, base, specifier)`: reads file via libuv sync, parses with JSON.parse, returns 6-element array `[name, main, type, imports, exports, file_path]` or undefined. Type normalized to "commonjs"/"module"/"none". Imports/exports stringified for objects/arrays (JS lazily re-parses).
- Other functions still stubs: `getPackageScopeConfig`/`getPackageType`/`getNearestParentPackageJSONType` return `undefined`. JS `package_json_reader.js` handles undefined gracefully (exists=false, type='none').

## Modules Helpers Shim (REPL prereq)
- `libjs/shims/internal/modules/helpers.js`: provides `makeRequireFunction` and `addBuiltinLibsToObject` for the REPL
- Both functions lazily load `Module` from `internal/modules/cjs/loader` (R14 provides this)
- Also exports stubs for `constants`, `compileCacheStatus`, `stripBOM`, `enableCompileCache`, etc.
- 14 modules in libjs-node import `internal/modules/helpers` -- the shim covers all their needs

## ESM Formats Shim (REPL prereq)
- `libjs/shims/internal/modules/esm/formats.js`: static `extensionFormatMap`, `mimeToFormat`, stub `getFormatOfExtensionlessFile`
- Real module needs `internalBinding('constants').internal` (not in our constants binding) and `fsBindings.getFormatOfExtensionlessFile` (not implemented)
- REPL only uses `extensionFormatMap`

## Domain Shim (REPL prereq)
- `libjs/shims/domain.js`: minimal Domain class extending EventEmitter
- Real `domain.js` blocked by: `async_hooks` -> `internal/async_hooks` -> `internalBinding('async_context_frame')` (missing), `internal/util.WeakReference`, `process.hasUncaughtExceptionCaptureCallback()` (not on our process)
- REPL uses domain for: `_domain.bind(eval_)`, `_domain.on('error', ...)`, `process.domain` check
- Shim provides: `create`, `createDomain`, `Domain`, `active`, `enter/exit/run/bind/intercept/add/remove/_errorHandler`

## CJS Loader Module Shim (REPL prereq)
- `libjs/shims/internal/modules/cjs/loader.js`: minimal `Module` class for REPL and helpers.js
- REPL uses: `builtinModules`, `_nodeModulePaths`, `_resolveLookupPaths`, `_resolveFilename`, `_extensions`, `_cache`, `globalPaths`, constructor, `require()`
- `Module.builtinModules` derived from `BuiltinModule.getAllBuiltinModuleIds()` (frozen)
- **CRITICAL**: `Module.prototype.require` uses `_loaderRequire` (captured at shim load time), NOT `globalThis.require`. The REPL overwrites `globalThis.require` with `makeRequireFunction` wrapper that calls `Module.prototype.require`, creating a circular loop if we use `globalThis.require`.
- `_resolveFilename` returns request as-is for non-builtins (our loader handles resolution)
- Real `internal/modules/cjs/loader.js` (2000+ lines) is too complex to use directly
- **BuiltinModule shim** now includes `domain` and `vm` (33 modules total)

## Embedded Module Build Gotchas
- Shim resolution (`libjs/shims/` vs `libjs-node/`) uses CMake `EXISTS` check at configure time. Adding a new shim file requires `cmake` reconfigure before `cmake --build`.

## VM Module
- `require('vm')` works: `runInThisContext`, `Script`, `createContext`/`isContext`, `runInNewContext`, `compileFunction` all functional
- `vm.js` also needs `internalBinding('symbols')` for `vm_dynamic_import_main_context_default`, `vm_context_no_contextify` etc. -- already present in our symbols binding
- `vm.runInNewContext` evaluates in global context (no sandboxing) -- acceptable for REPL

## Contextify Binding (vm module prereq)
- `initContextifyBinding` in `node_contextify.cpp`: ContextifyScript class + all functions
- ContextifyScript stores source (with `//# sourceURL=`) as `__contextifySource` property on JS object
- `runInContext(sandbox, timeout, displayErrors, breakOnSigint, breakFirstLine)`: evaluates via `napi_run_script`. Both null sandbox (runInThisContext) and object sandbox evaluate in global context (no real sandboxing).
- `compileFunction`: creates wrapper `(function(params) { code })` via eval, returns `{function, cachedDataProduced: false}`
- `makeContext`: sets `contextify_context_private_symbol` on sandbox (lazy-cached via `napi_ref`), returns sandbox. No real sandboxing.
- `startSigintWatchdog`/`stopSigintWatchdog`: real implementation (R19). Uses POSIX `sigaction` for SIGINT handler + Hermes async break to interrupt eval. `contextifyScriptRunInContext` converts uncatchable Hermes timeout error to catchable `ERR_SCRIPT_EXECUTION_INTERRUPTED`.
- **SIGINT watchdog architecture**: callback pattern (`TriggerAsyncBreakFn`) avoids Hermes VM header dependency in bindings lib. `hermes-node.cpp` provides lambda wrapping `triggerTimeoutAsyncBreak()`.
- **Hermes async break**: `triggerTimeoutAsyncBreak()` is thread-safe (atomic flag). Interpreter checks at `AsyncBreakCheck` instruction, calls `notifyTimeout()` which raises **uncatchable** `TimeoutError`. Must intercept at NAPI boundary and rethrow as catchable error.
- `vm.js` destructures `ContextifyScript`, `makeContext`, `constants`, `measureMemory` at top level
- `internal/vm.js` destructures `ContextifyScript`, `compileFunction` at top level
- `internal/vm.js` `isContext()`: checks `object[contextify_context_private_symbol] !== undefined`
- **Cross-binding symbol access**: contextify gets the private symbol from util binding by calling `globalThis.internalBinding('util')` from native code, then caches via `napi_ref`.
- `compileFunctionForCJSLoader(content, filename, is_sea_main, should_detect_module)`: wraps raw source as `(function(exports, require, module, __filename, __dirname) { <content> })`, evaluates via `napi_run_script`. Returns `{function, sourceMapURL, sourceURL, cachedDataRejected, canParseAsESM}`. Uses `napi_run_script` (not `hermes_compile_to_bytecode`) because the returned function references bytecode internally -- freeing it causes UAF.
- **CJS loader `wrapSafe()` code path**: when `patched=false` (default), calls `compileFunctionForCJSLoader` directly with raw unwrapped source. The native side adds the CJS wrapper. The `patched=true` path uses `Module.wrap()` + `makeContextifyScript` instead.

## REPL Entry Point
- `hermes-node` with no args starts the REPL via `require('internal/repl').createInternalRepl(process.env, cb)`. This handles `NODE_REPL_HISTORY`, `NODE_REPL_HISTORY_SIZE`, `NODE_REPL_MODE`, `NODE_NO_READLINE` env vars and calls `setupHistory()` for file persistence.
- Exit handler waits for history flushing: `repl.historyManager.isFlushing` -> wait for `flushHistory` event before `process.exit()`.
- `process.features` object added: `{inspector: false, tls: false, ipv6: false}`. Required by `internal/repl/utils.js` which checks `process.features.inspector` for terminal preview features.
- Process event emitter extended with: `prependListener`, `prependOnceListener`, `addListener` (alias for `on`), `rawListeners`, `removeAllListeners`, `newListener` event emission. REPL needs `prependListener('newListener', ...)` for domain error isolation.
- Acorn parser vendored in `libjs-node/internal/deps/acorn/` (acorn.js 6262 lines, walk.js 455 lines). Required by REPL for syntax checking and recovery (isRecoverableError).
- `internal/vm/module` shim: passthrough `importModuleDynamicallyWrap` (real module needs `internalBinding('module_wrap')`). Called when REPL eval registers dynamic import callback.
- `internal/modules/esm/utils` shim: no-op `registerModule` (real module needs full ESM loader).
- `internal/util/inspector.js` uses the real Node file (loads fine since `internalBinding('config').hasInspector === false` causes `sendInspectorCommand` to always call onError callback).
- `let`/`const` don't persist across REPL lines (Hermes eval limitation -- each `napi_run_script` is a separate script context). `var` works.
- `repl.js` line 216 `vm.runInNewContext('Object.getOwnPropertyNames(globalThis)')` runs at load time. Returns main context globals (no sandboxing) -- acceptable for tab-completion filtering.

## REPL History Persistence
- History only works with `terminal: true`. In non-terminal mode, `[kNormalWrite]` -> `[kOnLine]` directly (emits `line` event) but never calls `[kLine]` -> `[kAddHistory]`, so history array stays empty. Matches Node's behavior.
- `ReplHistory` in `internal/repl/history.js`: opens file async, reads existing entries, truncates, flushes on `line` event (15ms debounce).
- `setupHistory({filePath, size, onHistoryFileLoaded})`: creates `ReplHistory`, calls `initialize()` which does the async file open. `onHistoryFileLoaded` callback fires when ready.
- Default history file: `~/.node_repl_history`. Set `NODE_REPL_HISTORY` env var to override. Empty string disables.
- `REPLServer.close()` waits for flush only when `terminal: true`.
- Testing: must use `terminal: true` + `r.write('...\n')` for history to work in programmatic tests.

## REPL Testing
- `test-repl-entry.js`: 10 pipe-mode tests via `execSync` (spawn hermes-node with printf input). Pattern: `replExec(input)` with `JSON.stringify` for shell escaping.
- `test-repl-features.js`: 10 programmatic tests via Readable/Writable streams. Covers: multi-line input (continuation prompt), .help/.break commands, error recovery, require(), var persistence, util.inspect output, function definition, exception handling.
- `test-repl-basic.js`: 5 programmatic tests (arithmetic, strings, var, error recovery, require).
- `test-repl-history.js`: 5 tests for history persistence (write, load, append, disable, createInternalRepl + NODE_REPL_HISTORY). Uses `terminal: true` with programmatic streams.
- Multi-line input works: incomplete expressions like `(1 +` get `... ` continuation prompt, completing with `)` evaluates correctly.
- `.break` command cancels multi-line input and returns to normal prompt.
- **Known issue**: `test-repl-features.js` tests 1-2 (multi-line) silently fail -- assertion error in async REPL exit handler, process exits 0, lit reports PASS. Pre-existing from R20.

## Event Loop Tick Draining
- **Critical**: `process.nextTick` callbacks are drained by both a `uv_prepare_t` (before poll) and `uv_check_t` (after poll) handle. Without the prepare handle, ticks scheduled during native callbacks (pending I/O callbacks phase) would be stuck until the poll phase times out — which can be minutes with long timeouts.
- Node.js uses `InternalCallbackScope`/`MakeCallback` to drain ticks after every native callback. We approximate this with the two-handle approach.
- Root cause of the fixed `test-child-process-exec-timeout.js` flakiness: `uv_shutdown` callback called `process.nextTick(finish)`, but poll blocked for the full 2M ms timeout.

## Test Infrastructure Gotchas
- **Test timeout rule**: `check-hermes-node` should complete in under 3 minutes. If tests take longer, they have locked up. Use `timeout 180` or equivalent when running the test target.

## CJS Module Resolution -- Embedded Modules (S5)
- 17 new modules embedded for CJS loader: `package_json_reader`, `customization_hooks`, `typescript`, `run_main` + 13 ESM resolver modules
- All compiled to Hermes bytecode successfully (no syntax issues)

## CJS Module Resolution -- Shims (S6)
- **Real CJS loader**: `internal/modules/cjs/loader.js` loads successfully (shim removed). All key methods work: `_resolveFilename`, `_findPath`, `_load`, `_compile`, `_nodeModulePaths`.
- **Shims still needed** (real modules have missing deps):
  - `helpers.js`: real needs `_enableCompileCache` returning array (ours returns object), `stringify()` needs `internal/encoding`
  - `esm/formats.js`: real needs `internalBinding('constants').internal`
  - `esm/utils.js`: real needs `ModuleWrap`, `setImportModuleDynamicallyCallback` from `module_wrap`
  - `typescript.js`: real needs `internal/deps/amaro` (WASM parser), compile cache bindings
  - `run_main.js`: real needs `internal/process/execution`, `internalBinding('errors').triggerUncaughtException`
- **`initializeCJS()`**: called during bootstrap step 11e. Sets `Module.builtinModules`, initializes CJS conditions, sets up global paths. MUST be called before REPL or user code.
- **`BuiltinModule.compileForPublicLoader()`**: loads module via captured bootstrap `require`, caches `.exports`. Called by `loadBuiltinModule()` in helpers.
- **Module constructor**: real loader does NOT set `this.paths` in constructor (unlike old shim). Paths are set during `Module.prototype.load()`.
- **ESM resolver works**: `internal/modules/esm/resolve.js` loads lazily. `packageExportsResolve` and `packageImportsResolve` available. Used by CJS loader for `"exports"` field resolution.
- Missing runtime deps not yet shimmed/embedded: `internal/encoding`, `internal/process/execution`, `internal/deps/cjs-module-lexer/lexer`. Will surface when those code paths are hit.

## CJS Module Resolution -- Bootstrap Integration (S7)
- **`__loadUserScript`**: uses `Module._load(path.resolve(filepath), null, true)`. Resolves relative paths to absolute via `path.resolve()` before passing to CJS loader.
- **Fallback mechanism**: `Module._load` wrapped in `loader.js` to fall back to bootstrap loader on MODULE_NOT_FOUND. Checks bootstrap cache first, then `loadBytecodeModule()`. Allows `require('internal/...')` from user code/tests.
- **TypeScript**: `.ts` extension handler registered via `Module._extensions['.ts']`. Uses bootstrap's `compileAndRun(wrapped, filename, true)` with `Module.wrap()` for CJS wrapper.
- **builtinIds**: includes `async_hooks`, `cluster`, `diagnostics_channel`, `repl` (added in S7). Missing from list = MODULE_NOT_FOUND for that module (unless fallback catches it).
- **`hasStartedUserCJSExecution`**: set to `true` by `Module._compile` during user script loading. Tests that check this flag should expect `true`.
- **Built-in resolution path**: `require('fs')` -> `Module._resolveFilename` -> `BuiltinModule.normalizeRequirableId` -> `loadBuiltinWithHooks` -> `loadBuiltinModule` -> `BuiltinModule.compileForPublicLoader()` -> bootstrap `requireModule('fs')`.
- **User script resolution**: `require('./foo')` -> `Module._resolveFilename` -> `_findPath(request, [parentDir])` -> `path.resolve(parentDir, request)` -> try extensions -> `fs.readFileSync` -> `compileFunctionForCJSLoader` -> execute.

## CJS Test Pattern (S8-S14)
- node_modules tests need scripts in fixture dirs (resolution starts from requiring file's dir)
- Lit test file in `test/` has RUN/CHECK directives; actual test logic in `test/fixtures/<name>/main.js`
- Pattern: `// RUN: %hermes-node %source_dir/test/fixtures/<name>/main.js | %FileCheck %s`
- `fixtures` already excluded in lit.cfg, so .js files in fixture dirs aren't picked up as tests

## CJS Exports Resolution (verified S10)
- Conditional exports (`"require"` vs `"import"` conditions) work correctly -- `require()` picks the `"require"` condition
- Subpath exports (`"./utils": "./utils.js"`) resolve correctly
- Wildcard/pattern exports (`"./lib/*": "./lib/*.js"`) resolve correctly
- Simple string exports (`"exports": "./main.js"`) work -- bypasses index.js
- Non-exported subpaths throw `ERR_PACKAGE_PATH_NOT_EXPORTED` as expected
- ESM resolver (`internal/modules/esm/resolve.js`) handles all these cases via `packageExportsResolve`

## Unverified
- `Duplex.from()` (in `duplexify.js`) may still have issues
