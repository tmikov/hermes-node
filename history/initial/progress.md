# Implementation Progress

Tracks progress on `history/initial/2026-02-16-phase5-networking-plan.md`.

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
| N5.1 | Port `os` binding | — | done | Also added credentials binding |
| N5.2 | Port `credentials` binding | — | done | Done as part of N5.1 |
| N5.3 | Port `tty_wrap` binding | N5.6 | done | |
| N5.4 | Integrate simdutf into buffer/encoding | — | done | Already mostly integrated; replaced last hand-rolled UTF-8 trim |
| N5.5 | Integrate Ada into url binding | — | done | Native binding + comprehensive URL shim |
| N5.6 | Implement HandleWrap + LibuvStreamBase | — | done | Base classes for all native stream types |
| N5.7 | Vendor c-ares | — | done | |
| N5.8 | Implement `dns.lookup()` via libuv | N5.7 | done | |
| N5.9 | Implement c-ares DNS queries | N5.7, N5.8 | done | |
| N5.10 | Port `tcp_wrap` binding | N5.6 | | |
| N5.11 | Port `pipe_wrap` binding | N5.6 | | |
| N5.12 | Verify `net` module works | N5.8, N5.10, N5.11 | | |
| N5.13 | Port `udp_wrap` binding | N5.6 | | |
| N5.14 | Vendor llhttp | — | | |
| N5.15 | Port `http_parser` binding | N5.6, N5.14 | | |
| N5.16 | Verify HTTP works | N5.12, N5.15 | | |
| N5.17 | Port `process_wrap` binding | N5.6, N5.11 | | |
| N5.18 | Port `spawn_sync` binding | N5.17 | | |
| N5.19 | Verify `child_process` module works | N5.17, N5.18 | | |
| N5.20 | Implement `process.stdin` | N5.3, N5.6 | | |
| N5.21 | Add missing `os` constants | N5.1 | | |
| N5.22 | Run Node.js net test subset | N5.12 | | |
| N5.23 | Run Node.js http test subset | N5.16 | | |
| N5.24 | Run Node.js child_process test subset | N5.19 | | |

## Context Notes

### Step N5.1: Port `os` binding
- **Files**: created `include/hermes/node-compat/bindings/node_os.h`, `lib/bindings/node_os.cpp`, `include/hermes/node-compat/bindings/node_credentials.h`, `lib/bindings/node_credentials.cpp`, `test/test-os.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **Decisions**:
-- Implemented credentials binding (N5.2) alongside os binding because `libjs-node/os.js` imports `getTempDir` from `internalBinding('credentials')` at load time -- os module cannot load without it.
-- credentials binding includes POSIX credential functions (getuid/geteuid/getgid/getegid/getgroups) in addition to getTempDir/safeGetenv.
-- Error reporting uses the ctx-object pattern matching Node's `getCheckedFunction` in os.js: set errno/message/syscall/code on ctx object, return undefined on error.
- **What was done**: Full `os` binding with 13 functions (getHostname, getOSInformation, getLoadAvg, getUptime, getCPUs, getFreeMem, getTotalMem, getHomeDirectory, getUserInfo, getInterfaceAddresses, setPriority, getPriority, getAvailableParallelism) + isBigEndian property. Full `credentials` binding with getTempDir, safeGetenv, and POSIX credentials. JS test covering all os module APIs.
- **Notes for next step**: N5.2 is already done. Constants (priority, dlopen, signals, errno) were already in node_constants.cpp from earlier phases.

### Step N5.2: Port `credentials` binding
- Done as part of N5.1 (see above).

### Step N5.5: Integrate Ada into url binding
- **Files**: created `include/hermes/node-compat/bindings/node_url.h`, `lib/bindings/node_url.cpp`, `test/test-url.js`. Modified `libjs/shims/internal/url.js` (full rewrite), `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **Decisions**:
-- Wrote comprehensive self-contained `internal/url.js` shim rather than using Node's 1700-line original (too many dependencies: url_pattern, querystring, data_url, mime, etc.).
-- Native binding uses Ada C++ API (`ada::url_aggregator` for parsing/component access, `ada::url` for format function). Ada was already vendored at `external/ada/`.
-- URL globals (`URL`, `URLSearchParams`) set during bootstrap (step 11a3) after Buffer setup.
-- `url_pattern` binding is a stub exporting `URLPattern: undefined`.
-- Component offsets stored in shared Int32Array(9) following Node's layout.
- **What was done**: Full native url binding with 8 functions (parse, update, canParse, domainToASCII, domainToUnicode, getOrigin, format, pathToFileURL). Comprehensive JS URL class with all getters/setters, URLSearchParams with full API, utility functions (fileURLToPath, pathToFileURL, toPathIfFileURL, urlToHttpOptions, isURL, domainToASCII/Unicode, encodeStr). Protocol sets for url.js compatibility. Test covering all URL/URLSearchParams APIs, legacy url module, and globals.
- **Issues**: Ada's `host_start` offset points at `@` when credentials exist (not the first host char). Fixed by checking for `@` in host/hostname getters, matching Node's approach. Ada was already linked as PRIVATE dep of hermesNodeBindings (from encoding binding's IDNA usage).

### Step N5.4: Integrate simdutf into buffer/encoding
- **Files**: modified `lib/bindings/node_buffer.cpp`, `test/test-buffer.js`, `test/test-encoding.js`.
- **What was done**: simdutf was already integrated for UTF-8/ASCII validation, base64 encode/decode, Latin-1↔UTF-8 conversions, and UTF-16 length calculations. Replaced the last hand-rolled code: UTF-8 boundary trimming in `utf8WriteStaticCb` now uses `simdutf::trim_partial_utf8()` instead of manual byte-walking. Added edge-case tests for simdutf code paths: truncated/invalid UTF-8 sequences, base64 edge cases (empty, padding-only, single byte roundtrip), UTF-8 write boundary truncation (2/3/4-byte chars at various maxLength limits), hex write edge cases, emoji encodeInto, Latin-1 full-range decode.
- **Decisions**:
-- Hex codec and substring search remain hand-rolled (simdutf has no support for these).
-- UCS2 slice/write use NAPI UTF-16 functions directly (no transcoding needed).
- **Issues**: Hermes `_decodeUTF8SlowPath` has an OOB read when `napi_create_string_utf8` is called with truncated multi-byte UTF-8 (e.g. buffer = `[0xc3]`). This is a VM-internal bug, not fixable in the NAPI layer. Worked around by testing only full-length invalid sequences (e.g. `[0xc3, 0x00]`) in fatal mode and skipping non-fatal truncated sequence tests.

### Step N5.6: Implement HandleWrap + LibuvStreamBase
- **Files**: created `include/hermes/node-compat/bindings/handle_wrap_base.h`, `lib/bindings/handle_wrap_base.cpp`, `include/hermes/node-compat/bindings/libuv_stream_base.h`, `lib/bindings/libuv_stream_base.cpp`. Modified `lib/bindings/node_stream_wrap.cpp`, `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **Decisions**:
-- Implemented as reusable C++ base classes using NAPI rather than inheritance through V8's template system. HandleWrapBase manages uv_handle_t lifecycle (ref/unref/hasRef/close). LibuvStreamBase extends it with stream I/O (readStart/readStop/write*/shutdown).
-- WriteWrap/ShutdownWrap remain plain JS objects (constructors in stream_wrap binding). Native stream methods create their own uv_write_t/uv_shutdown_t internally via WriteReqData/ShutdownReqData structs. The oncomplete callback is invoked on the JS request object after async completion.
-- The shared streamBaseState Int32Array pointer is passed from initStreamWrapBinding to LibuvStreamBase::setStreamBaseState() so native methods can update it directly.
-- Read callback fires handle.onread(arrayBuffer) with streamBaseState[kReadBytesOrError] set to nread and kArrayBufferOffset set to 0. Data is copied into a new ArrayBuffer.
-- GC finalizer on HandleWrapBase async-closes the handle if JS object is collected before close() is called. Uses napi_remove_wrap to transfer ownership to uv_close callback.
-- getJsObject() method on HandleWrapBase provides access to the JS object from the prevent-GC reference, needed by libuv callbacks to call JS methods.
- **What was done**: Full HandleWrapBase with ref/unref/hasRef/close/getAsyncId and GC-safe lifecycle. Full LibuvStreamBase with readStart/readStop, writeBuffer/writeUtf8String/writeLatin1String/writeAsciiString/writeUcs2String/writev, shutdown, getWriteQueueSize, setBlocking. Enhanced stream_wrap binding to share streamBaseState pointer. All 39 existing tests pass.
- **Notes for next step**: TCP/Pipe/TTY wraps should inherit LibuvStreamBase and call addStreamMethods() on their prototypes. The `onread` property is set by JS (net.js sets handle.onread = onStreamRead from stream_base_commons.js). The `owner_symbol` linking is also done in JS. String write methods use UTF-8 extraction for latin1/ascii (acceptable for networking; exact latin1 byte preservation would need napi_get_value_string_latin1 which doesn't exist in NAPI).

### Step N5.3: Port `tty_wrap` binding
- **Files**: created `include/hermes/node-compat/bindings/node_tty_wrap.h`, `lib/bindings/node_tty_wrap.cpp`, `test/test-tty-wrap.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **What was done**: TTYWrap class inheriting LibuvStreamBase, wrapping `uv_tty_t`. Constructor `TTY(fd, ctx)` with error context pattern. Static `isTTY(fd)` using `uv_guess_handle`. Instance methods `getWindowSize(out)` and `setRawMode(flag)`. All stream methods inherited from LibuvStreamBase via `addStreamMethods()`.
- **Decisions**:
-- Error reporting uses same ctx-object pattern as os binding (errno/message/syscall/code).
-- Constructor always allocates TTYWrap; on `uv_tty_init` failure, sets ctx error info and does NOT call `initStream` (so HandleWrapBase finalizer safely deletes without uv_close).
-- `setRawMode` uses `UV_TTY_MODE_RAW_VT` (not `UV_TTY_MODE_RAW`), matching Node's approach for better VT control sequence support.
- **Issues**: `require('tty')` cannot be tested yet because `tty.js` depends on `net.js` which requires `cares_wrap`, `tcp_wrap`, and `pipe_wrap` bindings (N5.8/N5.10/N5.11). Test only covers the native `tty_wrap` binding directly.
- **Notes for next step**: N5.20 (process.stdin) depends on this + N5.6 (done). Once tcp_wrap/pipe_wrap/cares_wrap are implemented, the tty module will be testable end-to-end. The binding pattern used here (inherit LibuvStreamBase, call `addStreamMethods`, use `napi_define_class`) is the template for tcp_wrap and pipe_wrap.

### Step N5.7: Vendor c-ares
- **Files**: created `external/cares/README.md`, `external/cares/CMakeLists.txt`, `external/cares/cares/` (vendored source), `unittests/CAresIntegrationTest.cpp`. Modified `CMakeLists.txt`, `unittests/CMakeLists.txt`.
- **What was done**: Vendored c-ares 1.34.6 from Node.js v24.13.0 `deps/cares/`. Wrapper CMake sets `CARES_STATIC=ON`, `CARES_SHARED=OFF`, `CARES_BUILD_TOOLS=OFF`, `CARES_BUILD_TESTS=OFF`, `CARES_INSTALL=OFF` and delegates to c-ares's own CMake. Alias target `cares_a` for consistency with `uv_a`. GTest with 3 tests: library init/cleanup, version check, channel init/destroy.
- **Decisions**:
-- Used `add_subdirectory` with c-ares's own CMake build (same pattern as libuv), rather than hand-rolling source lists.
-- When `CARES_SHARED=OFF`, c-ares names the static target `c-ares` (no `_static` suffix). The `_static` suffix is only appended when shared is also being built.
- **Notes for next step**: Link against `cares_a` (alias for `c-ares`) or `c-ares::cares_static`. The `ares_init()` API is deprecated; use `ares_init_options()` instead.

### Step N5.8: Implement `dns.lookup()` via libuv
- **Files**: created `include/hermes/node-compat/bindings/node_cares_wrap.h`, `lib/bindings/node_cares_wrap.cpp`, `libjs/shims/internal/perf/observe.js`, `test/test-dns.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **Decisions**:
-- `dns.lookup()` uses `uv_getaddrinfo` (libuv OS resolver), not c-ares directly. c-ares queries (N5.9) will extend this binding.
-- Hostname is converted to ASCII via `ada::idna::to_ascii()` before passing to `uv_getaddrinfo`, matching Node's approach for internationalized domain names.
-- `ChannelWrap`, `QueryReqWrap`, and `getnameinfo` are included as stubs so `dns.js` and `dns/callback_resolver.js` can load without errors. These will be fleshed out in N5.9.
-- Created `internal/perf/observe.js` shim (no-op) since `dns.js` imports `hasObserver`/`startPerf`/`stopPerf` for performance observation. We don't have `internalBinding('performance')`.
-- `initializeDns()` from `dns/utils.js` is not called during our bootstrap (Node calls it in pre_execution). `dnsOrder` defaults to `undefined`, which falls through to `DNS_ORDER_VERBATIM` in `dns.lookup()`.
- **What was done**: Full `cares_wrap` binding with: `getaddrinfo` (async via `uv_getaddrinfo`), `getnameinfo` (async via `uv_getnameinfo`), `canonicalizeIP`, `strerror`, `GetAddrInfoReqWrap`/`GetNameInfoReqWrap`/`QueryReqWrap` constructors, `ChannelWrap` stub with `getServers`/`setServers`/`cancel`/`setLocalAddress` stubs, and constants (`AF_INET`, `AF_INET6`, `AF_UNSPEC`, `AI_*`, `DNS_ORDER_*`). Test covers binding API, `dns.lookup()` with localhost/IP passthrough/all mode/family filtering/empty hostname, and `ChannelWrap` stub.
- **Notes for next step**: N5.9 (c-ares DNS queries) should extend this binding's `ChannelWrap` to use real `ares_channel` with `uv_poll_t` integration. `QueryReqWrap` needs oncomplete callback support. `strerror` currently uses `uv_strerror` — for c-ares-specific errors it should use `ares_strerror`.

### Step N5.9: Implement c-ares DNS queries
- **Files**: modified `lib/bindings/node_cares_wrap.cpp`, `include/hermes/node-compat/bindings/node_cares_wrap.h`, `tools/hermes-node/hermes-node.cpp`. Created `test/test-dns-resolve.js`.
- **Decisions**:
-- Used deprecated `ares_query()` + `ares_parse_*_reply()` API rather than newer `ares_query_dnsrec()` (simpler, and Node v24 still uses the older API path).
-- ChannelWrap uses prevent-GC ref (`selfRef_`) pattern to prevent GC from collecting the ChannelWrap JS object while libuv handles (timer, poll watchers) are open. Released in `closeChannel()` after all handles are closed.
-- `caresWrapShutdown()` closes all live ChannelWrap instances before event loop is destroyed (called in hermes-node.cpp before `eventLoop.close()`). Static `s_channels` set tracks all live instances.
-- Socket I/O integration: per-socket `AresTask` struct with `uv_poll_t`, managed by `sockStateCb` from c-ares. `uv_timer_t` for timeout processing via `ares_timeout()`.
-- `strerror` dispatches to `ares_strerror` for c-ares error codes (0 to ARES_ECANCELLED) and `uv_strerror` for libuv error codes (negative).
- **What was done**: Full ChannelWrap implementation wrapping `ares_channel_t` with libuv I/O integration. Query methods for all DNS record types: A, AAAA, MX, NS, TXT, SRV, CNAME, PTR, NAPTR, SOA, CAA, Any. Reverse DNS via `getHostByAddr` (`ares_gethostbyaddr`). Real `getServers`/`setServers`/`cancel`/`setLocalAddress` implementations. Parse functions for all record types building JS objects matching Node's format. Test covering binding API, dns.Resolver, dns.resolve*/reverse at JS API level, cancel, strerror for c-ares codes.
- **Issues**: ASAN heap-use-after-free when GC collected ChannelWrap during JS execution: destructor tried to `uv_close` the embedded `uv_timer_t` member, but after `delete this` the timer memory was freed, so when `uv_run` processed the close callback it accessed freed memory. Fixed by adding prevent-GC ref to keep ChannelWrap alive until all handles are properly closed via `closeChannel()`.
- **Notes for next step**: `dns.resolve*` and `dns.reverse` are now functional. The `dns/promises` API should also work (it's built on the callback API). The `queryAny` method returns a "not implemented" error for now (c-ares doesn't have a single-call API for ANY queries). N5.10/N5.11 (tcp_wrap/pipe_wrap) are the next unblocked steps.
