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

## V8 API Polyfills
- `Error.captureStackTrace`: polyfilled in `primordials.js`. Creates Error for stack, sets lazy getter.
- `Error.stackTraceLimit`: writable property (default 10) if missing.
- Both picked up by primordials `copyPropsRenamed(Error, ...)`.

## Key Shims
- `internal/abort_controller.js`: minimal AbortController/AbortSignal using EventEmitter (original depends on `event_target` -> `internalBinding('performance')`)
- `internal/event_target.js`: exports `kWeakHandler`/`kResistStopPropagation` symbols only
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
- No `Array.prototype.toSorted()` -- patched `http.js` to use `.slice().sort()`

## HTTP Module (verified)
- `require('http')` loads and works for both server and client
- GET, POST with body, chunked transfer encoding, keep-alive, headers, status codes, streaming, timeouts, DNS hostname resolution all verified
- 4 Node.js tests ported and passing: request-end, status-code, client-get-url, date-header
- `test/node-tests/common/countdown.js` helper added for tests needing sequential request counting
- HTTP Agent keep-alive works (multiple requests on same connection)
- 204 No Content correctly omits body
- Client `timeout` option works (fires 'timeout' event, then destroy to abort)

## Unverified
- `Duplex.from()` (in `duplexify.js`) may still have issues
