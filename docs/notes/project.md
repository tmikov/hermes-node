# Project: Hermes Node.js Compatibility Layer

You are implementing Node.js built-in module compatibility (`process`, `fs`, `net`, `http`, etc.) for the Hermes JS engine. This is not a full Node.js port — it is selective module compatibility for Hermes-based environments.

You reuse Node's `lib/*.js` files directly and port Node's C++ native bindings from the V8 API to Node-API (which Hermes implements). The `lib/*.js` files talk to native bindings through an `internalBinding()` bridge that you provide.

This works because Node's modules are structured as a JS layer plus thin native bindings over libuv, OpenSSL, etc. The JS is reusable as-is (with minor modifications); the native bindings are a mechanical port from V8 API calls to Node-API equivalents.

---

## Goal

Add Node.js built-in module compatibility (e.g. `process`, `fs`, `net`, `http`) to the Hermes JavaScript engine, enabling Hermes-based environments to run JavaScript code that depends on Node.js APIs.

This is **not** a full Node.js port. The goal is selective module compatibility — reusing Node's well-tested JavaScript implementations (`lib/*.js`) and porting the underlying C++ native bindings to work with Hermes via Node-API. The result is a runtime layer that can be embedded in React Native, standalone CLI tools, or other Hermes-based environments.

## Approach

### Why reuse Node's code rather than reimplement from scratch

Node's built-in modules represent over a decade of battle-tested edge case handling, platform-specific workarounds, and spec compliance fixes. Reimplementing them from scratch would mean rediscovering the same bugs Node already fixed. The modules are structured in a way that makes selective reuse feasible: a JavaScript layer (`lib/*.js`) communicates with native bindings through a narrow `internalBinding()` interface, and the native bindings are mostly thin wrappers around libuv and other standalone C libraries.

### Why port to Node-API rather than write fresh JSI bindings

Node's existing C++ bindings in `src/node_*.cc` encode the precise contract that the `lib/*.js` files expect — argument order, error handling behavior, result shapes, async completion semantics, and platform-specific behavior. Porting these to Node-API is a mechanical transformation that preserves all of this behavior. Writing fresh JSI bindings would require reverse-engineering the same contracts from the C++ source anyway, with the added risk of subtle behavioral differences.

Hermes already has a Node-API implementation, which provides the target API surface. The porting work is primarily replacing V8 API calls (`v8::Local<Value>`, `FunctionCallbackInfo<Value>`, etc.) with their Node-API equivalents (`napi_value`, `napi_callback_info`, etc.) and adapting the internal infrastructure that Node's bindings depend on (primarily the `Environment` context object and the `AsyncWrap` request lifecycle).

### Keep C++ close to Node's implementation

When porting native bindings, keep the C++ implementation as close to Node's `src/node_*.cc` as reasonable. In particular, when Node uses a third-party library (simdutf for SIMD-accelerated string operations, Ada for URL/IDNA, llhttp for HTTP parsing, c-ares for DNS, etc.), vendor and use that same library rather than hand-rolling equivalent functionality. This ensures:

- **Behavioral parity** — identical edge-case handling and spec compliance
- **Performance** — battle-tested optimizations (e.g. SIMD acceleration) for free
- **Easier future porting** — our code structure mirrors Node's, making diffs and updates mechanical

The vendoring pattern is: `external/$lib/` with a wrapper `CMakeLists.txt` + `README.md`, and the unmodified upstream source in `external/$lib/$lib/`.

### Architecture overview

```
┌──────────────────────────────────────────────────┐
│  Node's lib/*.js (reused with minimal patching)  │
├──────────────────────────────────────────────────┤
│             internalBinding() bridge             │
├──────────────────────────────────────────────────┤
│  Node's src/node_*.cc (ported to Node-API)       │
├──────────────────────────────────────────────────┤
│          Hermes Node-API implementation          │
├──────────────────────────────────────────────────┤
│               Hermes JS engine                   │
├──────────────────────────────────────────────────┤
│    libuv, llhttp, OpenSSL, zlib, c-ares, ICU     │
│    simdutf, Ada, ...                             │
└──────────────────────────────────────────────────┘
```

---

## Dependency Map

Understanding the dependency graph between modules is critical for ordering the work. Each layer depends only on layers above it.

```
Layer 0 — Bootstrap
    primordials
    internal module loader
    internal/errors
    internal/validators
    internal/util

Layer 1 — Core Objects (no libuv)
    events          (← primordials)
    path            (← primordials) [pure JS, no native binding]
    buffer          (← primordials, internal/validators) [native binding: memory ops only]
    util            (← primordials, internal/errors, buffer) [native binding: type checks]
    process         (← primordials, events) [native binding: env, argv, cwd, pid, hrtime]

Layer 2 — Event Loop
    libuv loop integration
    timers          (← process, primordials) [native binding: uv_timer_t]
    process.nextTick queue
    setImmediate

Layer 3 — Streams & File System
    stream          (← events, buffer, process.nextTick) [minimal native deps]
    fs              (← buffer, path, stream, events, util) [native binding: uv_fs_*]
    fs/promises     (← fs, same native binding)

Layer 4 — Networking
    dns             (← events) [native binding: uv_getaddrinfo / c-ares]
    net             (← stream, events, buffer, dns) [native binding: uv_tcp_t, uv_pipe_t]
    dgram           (← events, buffer) [native binding: uv_udp_t]
    http            (← net, stream, buffer, events) [native binding: llhttp parser]
    tls             (← net, stream, crypto) [native binding: OpenSSL]
    https           (← http, tls)

Layer 5 — Extended
    child_process   (← events, stream, net, fs) [native binding: uv_spawn]
    zlib            (← stream, buffer) [native binding: zlib/brotli]
    crypto          (← buffer) [native binding: OpenSSL]
    worker_threads  [requires significant Hermes-specific design]
```

---

## Phase 1 — Bootstrap Infrastructure

**Goal:** Establish the module loading system that allows Node's `lib/*.js` files to run on Hermes. No I/O capability yet — this phase is purely about getting the internal dependency machinery working.

### 1.1 Primordials

Nearly every `lib/*.js` file destructures from a `primordials` object as its first action:

```js
const {
  ArrayPrototypeSlice,
  ObjectDefineProperty,
  Promise,
  SafeMap,
  StringPrototypeSlice,
} = primordials;
```

Node creates this object during bootstrap (`lib/internal/per_context/primordials.js`) by copying every built-in prototype method into a flat namespace and creating `Safe*` variants of `Map`, `Set`, `WeakMap`, etc. that cannot be corrupted by user code modifying prototypes.

**Decision required: full primordials or elimination transform.**

- **Option A — Full primordials:** Run Node's `primordials.js` bootstrap during initialization. This is the most compatible approach and means `lib/*.js` files work unmodified. Requires Hermes to support all the reflection APIs used in the bootstrap (`Object.getOwnPropertyDescriptors`, `Reflect.apply`, etc.).

- **Option B — Thin primordials shim:** Create a `primordials` object that re-exports the standard built-ins without the safety wrappers. `SafeMap` is just `Map`, `ArrayPrototypeSlice` is just `Array.prototype.slice.call.bind(Array.prototype.slice)`, etc. Simpler to implement, but any `lib/*.js` code that depends on tamper-resistance won't actually be tamper-resistant. This is unlikely to matter in practice for this use case.

- **Option C — Source transform:** Mechanically rewrite `lib/*.js` files to replace primordials usage with direct built-in calls. `StringPrototypeSlice(s, i)` becomes `s.slice(i)`, `SafeMap` becomes `Map`, etc. This eliminates the primordials dependency entirely but requires maintaining the transform and re-applying it when updating to newer Node source. Tools like jscodeshift could automate this.

**Recommendation:** Option B. It is minimal effort, maintains source compatibility with Node's `lib/*.js` files, and the tamper-resistance guarantees are irrelevant when the engine host controls the environment.

### 1.2 Internal module loader

Node's `lib/*.js` files reference each other via `require('internal/foo')` and access native bindings via `internalBinding('foo')`. These are not the public `require()` — they are internal mechanisms set up during bootstrap.

Implement:

- **`internalBinding(name)`** — Returns the native binding object registered under `name`. This is the bridge to the ported C++ code. Internally, maintain a registry mapping names (`'fs'`, `'buffer'`, `'util'`, `'constants'`, etc.) to Node-API modules.

- **`require(name)` for internal modules** — Loads and caches `lib/internal/*.js` files. Does not need to implement Node's full public module resolution (node_modules lookup, package.json `"main"`, etc.) — it only needs to resolve the internal module namespace. A simple map from module ID to file path or pre-bundled source is sufficient.

- **`NativeModule` / built-in module wrapper** — Each `lib/*.js` file is wrapped in a function that receives `exports`, `require`, `module`, `process`, `internalBinding`, and `primordials` as arguments, matching Node's internal module wrapper signature.

The loader should be implemented primarily in C++ (via Node-API) to set up the initial execution environment before any JS runs. The flow is:

1. Initialize Hermes runtime
2. Register all native bindings in the `internalBinding` registry
3. Create the `primordials` object
4. Execute the internal module loader JS
5. Bootstrap `process`, `events`, and other foundational modules
6. Hand control to user code

### 1.3 `internal/errors`

Node's custom error system. Defines error codes (`ERR_INVALID_ARG_TYPE`, `ERR_INVALID_ARG_VALUE`, etc.) and error classes that include the code in the message. Almost every module depends on this.

**Dependencies:** primordials only.

**Native bindings:** None. This is pure JavaScript.

**Porting notes:** Should work as-is once primordials and the internal module loader are in place. Verify that Hermes supports all the metaprogramming patterns used (subclassing `Error`, `Object.defineProperty` on error instances, `Error.captureStackTrace` — this last one is a V8-ism that Node polyfills but may need attention).

### 1.4 `internal/validators`

Argument validation utilities used throughout the codebase: `validateInteger`, `validateString`, `validateCallback`, `validateAbortSignal`, `validateBoolean`, `validateObject`, etc.

**Dependencies:** primordials, `internal/errors`.

**Native bindings:** None.

**Porting notes:** Pure JavaScript, should work as-is.

### 1.5 `internal/util`

Internal utility functions distinct from the public `util` module. Includes `deprecate()`, `spliceOne()`, `getSystemErrorName()`, `once()`, `sleep()`, various type-checking helpers.

**Dependencies:** primordials, `internal/errors`. Has a native binding (`internalBinding('util')`) for some type-checking fast paths (`isArrayBufferView`, `isTypedArray`, `isDate`, etc.) and V8-specific introspection (`getPromiseDetails`, `getProxyDetails`).

**Porting notes:** The native binding needs to be ported. The type-checking functions are straightforward to implement in Node-API. V8-specific introspection functions (`getPromiseDetails`, `getProxyDetails`, `previewEntries`) may not have equivalents in Hermes — these are primarily used for `util.inspect` rendering and can return stub values initially.

---

## Phase 2 — Core Modules (No libuv)

**Goal:** Establish the core object model that all I/O modules depend on. After this phase, code using `EventEmitter`, `Buffer`, `path`, and basic `process` properties will work.

### 2.1 `events`

The `EventEmitter` class. Nearly every object in Node that does anything asynchronous inherits from this.

**Dependencies:** primordials, `internal/errors`, `internal/validators`.

**Native bindings:** Effectively none. There is a minor native dependency for `kMaxEventTargetListeners` and some performance-sensitive paths, but the core `EventEmitter` is pure JavaScript.

**Porting notes:** Should work with minimal changes. Verify that `AbortSignal` / `AbortController` usage (added in recent Node versions) is compatible with Hermes's implementation or polyfillable.

### 2.2 `path`

Path manipulation: `join`, `resolve`, `dirname`, `basename`, `extname`, `normalize`, `parse`, `format`, `relative`, `isAbsolute`.

**Dependencies:** primordials, `internal/validators`.

**Native bindings:** None. Completely pure JavaScript.

**Porting notes:** Works as-is. The only consideration is `path.win32` vs `path.posix` — the module detects the platform at load time via `process.platform`.

### 2.3 `buffer`

`Buffer` and related classes. This is the first module with a substantial native binding.

**Dependencies:** primordials, `internal/errors`, `internal/validators`, `internal/util`.

**Native binding (`internalBinding('buffer')`) provides:**
- `createFromString(string, encoding)` — fast native string-to-buffer conversion
- `byteLengthUtf8(string)` — fast byte length calculation
- `compare(buf1, buf2)` — memcmp wrapper
- `fill(buf, val, start, end, encoding)` — optimized fill
- `indexOfString(buf, val, byteOffset, encoding, isForward)` — search
- `indexOfBuffer(buf, val, byteOffset, isForward)` — search
- `swap16(buf)`, `swap32(buf)`, `swap64(buf)` — byte-swap in place
- `decodeUTF8(input, ignore_bom)` — UTF-8 decoding
- Various encoding/decoding fast paths (hex, base64, latin1)

**Porting notes:** These are all pure memory operations with no libuv dependency. They operate on `ArrayBuffer` / `TypedArray` backing stores. The Node-API typed array functions (`napi_get_typedarray_info`, `napi_get_arraybuffer_info`) provide access to the underlying data pointer, making the port straightforward. This is a good first real test of the Node-API porting workflow because the functions are self-contained and independently testable.

### 2.4 `util`

The public `util` module: `util.inspect`, `util.format`, `util.promisify`, `util.types`, `util.deprecate`, `util.inherits`, `util.isDeepStrictEqual`, `util.TextEncoder`, `util.TextDecoder`.

**Dependencies:** primordials, `internal/errors`, `internal/util`, `internal/validators`, `buffer`, `events`.

**Native binding (`internalBinding('util')`) provides:**
- Type-checking functions: `isDate`, `isRegExp`, `isMap`, `isSet`, `isTypedArray`, `isArrayBuffer`, `isSharedArrayBuffer`, `isExternal`, `isAnyArrayBuffer`, `isDataView`, `isPromise`, etc.
- `getPromiseDetails(promise)` — returns `[state, result]` (used by `util.inspect`)
- `getProxyDetails(proxy)` — returns `[target, handler]` (used by `util.inspect`)
- `previewEntries(iterator)` — gets entries from Map/Set iterators (used by `util.inspect`)
- `getOwnNonIndexProperties(obj, filter)` — used by `util.inspect`
- Various formatting and comparison helpers

**Porting notes:** The type-checking functions map directly to Node-API's `napi_typeof`, `napi_is_typedarray`, `napi_is_date`, etc. The introspection functions (`getPromiseDetails`, `getProxyDetails`, `previewEntries`) are V8-specific. Check if Hermes's Node-API implementation exposes promise state; if not, these can return undefined/empty initially — they only affect `util.inspect` output for those object types.

### 2.5 `process` (partial)

The `process` global object. In Node, this is partially constructed in C++ during bootstrap and extended in JavaScript. For this phase, implement the non-I/O properties and methods.

**Properties (set from native):**
- `process.pid` — `getpid()`
- `process.ppid` — `getppid()`
- `process.platform` — `'linux'`, `'darwin'`, `'win32'`, etc.
- `process.arch` — `'x64'`, `'arm64'`, `'arm'`, etc.
- `process.version` — version string (define your own scheme)
- `process.versions` — object with component versions (hermes, libuv, openssl, etc.)
- `process.argv` — command line arguments
- `process.execPath` — path to the executable
- `process.env` — environment variable accessor (uses a Proxy or getter/setter pattern over `getenv`/`setenv`)
- `process.title` — get/set process title

**Methods (native implementation):**
- `process.cwd()` — `uv_cwd()` (technically libuv, but it's a synchronous one-shot call)
- `process.chdir(dir)` — `uv_chdir()`
- `process.hrtime()` / `process.hrtime.bigint()` — `uv_hrtime()`
- `process.memoryUsage()` — `uv_resident_set_memory()` plus Hermes heap stats
- `process.cpuUsage()` — `uv_getrusage()`
- `process.exit(code)` — cleanup and `exit()`
- `process.abort()` — `abort()`
- `process.umask([mask])` — `umask()`
- `process.uptime()` — time since process start

**Deferred to Phase 3:**
- `process.nextTick()` — requires event loop integration
- `process.stdout` / `process.stderr` / `process.stdin` — require stream + tty bindings
- Signal handling (`process.on('SIGINT', ...)`) — requires libuv signal handles

**Porting notes:** Many of these are trivial one-liner native functions. `process.env` is the most complex because Node implements it as a Proxy-like object where property access calls `getenv()` and property assignment calls `setenv()`. In Node-API, this can be done via `napi_define_properties` with getter/setter pairs, or by creating a wrapper object. The enumeration behavior (iterating `process.env` lists all environment variables via `environ`) also needs implementation.

---

## Phase 3 — Event Loop Integration

**Goal:** Wire libuv's event loop into the Hermes runtime, enabling async operations. After this phase, timers, `nextTick`, and the foundational async machinery will work.

### 3.1 libuv loop initialization and run strategy

This is the most architecturally significant piece. All async I/O depends on a running `uv_loop_t`.

**For standalone (CLI/server) use:**
```c
uv_loop_t loop;
uv_loop_init(&loop);
// ... register bindings, bootstrap JS ...
// Enter the event loop
uv_run(&loop, UV_RUN_DEFAULT);
// Cleanup
uv_loop_close(&loop);
```
The loop runs until there are no more active handles/requests, matching Node's behavior.

**For React Native / embedded use:**
The host already has an event loop (the platform's run loop on iOS/Android). Options:
- Run libuv on a dedicated background thread, marshaling JS callbacks back to the Hermes thread
- Use `uv_run(UV_RUN_NOWAIT)` polled from the host's run loop
- Use libuv's `uv_backend_fd()` to integrate with the host's event notification mechanism (epoll/kqueue fd)

The choice depends on the embedding context. Design the integration layer so the loop management is pluggable.

**Microtask draining:** After each libuv callback that enters JavaScript, drain Hermes's microtask queue before returning to the event loop. This matches Node's behavior where promise continuations run to completion between event loop phases. In Node this is done via V8's `MicrotasksPolicy::kExplicit` — the equivalent with Hermes is calling the microtask drain function after each JS invocation.

### 3.2 `timers`

`setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`.

**Native binding wraps:**
- `uv_timer_init(loop, handle)`
- `uv_timer_start(handle, callback, timeout, repeat)`
- `uv_timer_stop(handle)`
- `uv_timer_again(handle)`

**Porting notes:** The JS layer in `lib/timers.js` and `lib/internal/timers.js` is sophisticated — Node uses a priority queue and timer list optimization to avoid creating a uv_timer for every setTimeout call. Multiple timers with the same delay share a single uv_timer. This JS code can be reused; the native binding is small.

### 3.3 `setImmediate` / `clearImmediate`

Runs a callback after I/O events in the current event loop iteration.

**Porting notes:** In Node, this was originally a `uv_check_t` handle but has been optimized over the years. The JS-side implementation maintains its own queue. The native side just needs to signal "drain the immediate queue" at the right point in the loop.

### 3.4 `process.nextTick`

Not a timer and not backed by libuv. It is a JS-level queue that drains at specific points: after each callback from the event loop, before I/O polling, and at other defined checkpoints. The queue must fully drain (including ticks enqueued during draining) before the loop continues.

**Porting notes:** Implement as a JS array-based queue. The critical integration point is ensuring the drain function is called at the right times — after libuv callbacks return and after microtasks drain. The ordering is: libuv callback → microtask queue drain → nextTick queue drain → return to loop.

### 3.5 `process.stdout` / `process.stderr` / `process.stdin`

These are streams wrapping file descriptors 0, 1, 2. Their type depends on what the fd points to:
- TTY → `tty.WriteStream` / `tty.ReadStream`
- Pipe → `net.Socket`
- File → `fs.WriteStream` / `fs.ReadStream`

**Porting notes:** For initial implementation, wrapping with simple synchronous writes for stdout/stderr is sufficient. Full async TTY support can come later. This unblocks `console.log` working through Node's process streams rather than Hermes's built-in console.

---

## Phase 4 — Streams and File System

**Goal:** Full file system access and the stream infrastructure that all I/O modules build on.

### 4.1 `stream`

The stream module is spread across many files:
- `lib/stream.js` — public entry point
- `lib/internal/streams/readable.js`
- `lib/internal/streams/writable.js`
- `lib/internal/streams/duplex.js`
- `lib/internal/streams/transform.js`
- `lib/internal/streams/pipeline.js`
- `lib/internal/streams/compose.js`
- `lib/internal/streams/operators.js`
- `lib/internal/streams/destroy.js`
- `lib/internal/streams/state.js`
- `lib/internal/streams/end-of-stream.js`
- `lib/internal/streams/utils.js`

**Dependencies:** `events`, `buffer`, `process` (for `nextTick`), primordials.

**Native bindings:** Minimal. The `StreamBase`/`StreamWrap` C++ classes in Node provide a base for native streams (TCP, pipes, TTY), but the core JS stream state machine is independent. The native `stream_wrap` binding provides `WriteWrap` and `ShutdownWrap` classes used by native stream implementations — these will need porting when `net` is implemented but are not needed for the basic stream module.

**Porting notes:** This is mostly a matter of loading many JS files. The stream code is complex but self-contained. Test with `Transform` streams and piping between `Readable`/`Writable` instances to verify correctness.

### 4.2 `fs`

The file system module. This is the largest and most important native binding port.

**JS files:**
- `lib/fs.js` — callback API
- `lib/internal/fs/promises.js` — promise API
- `lib/internal/fs/utils.js` — shared utilities, `Stats` object construction
- `lib/internal/fs/dir.js` — `Dir` / `Dirent` for directory iteration
- `lib/internal/fs/watchers.js` — `fs.watch` / `fs.watchFile`
- `lib/internal/fs/streams.js` — `createReadStream` / `createWriteStream`
- `lib/internal/fs/read/context.js` — `FileHandle.read` context
- `lib/internal/fs/cp/` — recursive copy

**Native binding (`internalBinding('fs')`) wraps the `uv_fs_*` family:**

| JS function | libuv function |
|---|---|
| `open` | `uv_fs_open` |
| `close` | `uv_fs_close` |
| `read` | `uv_fs_read` |
| `write` | `uv_fs_write` |
| `stat` / `lstat` / `fstat` | `uv_fs_stat` / `uv_fs_lstat` / `uv_fs_fstat` |
| `rename` | `uv_fs_rename` |
| `unlink` | `uv_fs_unlink` |
| `mkdir` | `uv_fs_mkdir` |
| `rmdir` | `uv_fs_rmdir` |
| `readdir` / `opendir` | `uv_fs_scandir` / `uv_fs_opendir` |
| `chmod` / `fchmod` | `uv_fs_chmod` / `uv_fs_fchmod` |
| `chown` / `fchown` | `uv_fs_chown` / `uv_fs_fchown` |
| `link` / `symlink` | `uv_fs_link` / `uv_fs_symlink` |
| `readlink` / `realpath` | `uv_fs_readlink` / `uv_fs_realpath` |
| `ftruncate` | `uv_fs_ftruncate` |
| `utimes` / `futimes` / `lutimes` | `uv_fs_utime` / `uv_fs_futime` / `uv_fs_lutime` |
| `mkdtemp` | `uv_fs_mkdtemp` |
| `copyfile` | `uv_fs_copyfile` |
| `access` | `uv_fs_access` |

**Key porting challenges:**

**`FSReqBase` class hierarchy.** Each async fs operation creates a request object that wraps the `uv_fs_t` request. In Node, `FSReqCallback` (for callback API) and `FSReqPromise` (for promise API) both inherit from `FSReqBase` which inherits from `ReqWrap<uv_fs_t>` which inherits from `AsyncWrap`. For the Node-API port:
- Create a request wrapper using `napi_wrap` to associate a C struct/object with a JS object
- The struct holds the `uv_fs_t`, a reference to the JS callback or promise, and any buffers that need to stay alive during the async operation
- On completion, the libuv callback unwraps the JS object, resolves the promise or calls the callback, and cleans up

**`Stats` object population.** The `uv_fs_stat` result (`uv_stat_t`) has ~20 fields that need to be set on the JS `Stats` object. Node does this by writing into a shared `Float64Array` (the "stats array buffer") and letting the JS side read from it, avoiding per-field JS object property sets. This optimization can be replicated: allocate a `Float64Array`, pass it to the native binding, have the native side write stat fields into it, and have the JS side read them out. This avoids many `napi_set_named_property` calls in the hot path.

**Sync vs async.** Each function has both sync and async variants. The sync path calls `uv_fs_*` synchronously (libuv supports this — the callback parameter is `NULL`). The async path submits to the libuv thread pool and fires the callback/promise on completion. Both paths go through the same native binding; a parameter indicates which mode.

**File watchers.** `fs.watch` wraps `uv_fs_event_t`. `fs.watchFile` uses polling via `uv_fs_poll_t`. These are libuv handle types, not one-shot requests.

### 4.3 `fs` streams

`fs.createReadStream` and `fs.createWriteStream` depend on both `fs` and `stream`. Implemented in `lib/internal/fs/streams.js`. These are JS classes extending `Readable` / `Writable` that call the `fs` binding for reads/writes.

**Porting notes:** Entirely JavaScript. Should work once `fs` and `stream` are functional.

---

## Phase 5 — Networking

**Goal:** TCP/UDP networking, HTTP client and server capability.

### 5.1 `dns`

**Native binding wraps:**
- `uv_getaddrinfo` — async DNS resolution (the common case)
- c-ares library — for `dns.resolve*` functions (MX, TXT, SRV, AAAA, etc.)

**Porting notes:** The c-ares integration is its own native binding (`internalBinding('cares_wrap')`). It is moderately complex — c-ares has its own event loop integration via `uv_poll_t` handles watching c-ares's file descriptors. Port the `uv_getaddrinfo` path first as it covers `dns.lookup()` which is what `net.connect()` uses.

### 5.2 `net`

TCP and IPC (Unix domain socket / Windows named pipe) client and server.

**Native binding (`internalBinding('tcp_wrap')` and `internalBinding('pipe_wrap')`) wraps:**
- `uv_tcp_init`, `uv_tcp_bind`, `uv_tcp_connect`, `uv_listen`, `uv_accept`
- `uv_pipe_init`, `uv_pipe_bind`, `uv_pipe_connect`
- `uv_read_start`, `uv_read_stop`, `uv_write`, `uv_shutdown`
- `uv_tcp_nodelay`, `uv_tcp_keepalive`

**Key porting challenge — `StreamBase`/`StreamWrap`:**
Node has an internal C++ class hierarchy for native streams. `StreamWrap` inherits from `HandleWrap` (which wraps `uv_handle_t` lifecycle) and `StreamBase` (which provides the read/write interface). `StreamBase` uses a `StreamListener` interface to push data to the JS layer.

This is the most complex native binding to port. The approach:
1. Define a `HandleWrap` equivalent that associates a `uv_handle_t` with a JS object via `napi_wrap`, handling ref/unref and the handle close sequence
2. Define a `StreamWrap` equivalent that manages `uv_read_start` allocation callbacks and `uv_write` completion
3. The JS `net.Socket` interacts with this through `StreamBase`'s methods exposed on the wrapped JS object

### 5.3 `dgram`

UDP sockets. Simpler than TCP since there are no streams.

**Native binding wraps:**
- `uv_udp_init`, `uv_udp_bind`, `uv_udp_send`, `uv_udp_recv_start`, `uv_udp_recv_stop`
- Multicast: `uv_udp_set_membership`, `uv_udp_set_multicast_ttl`, etc.

### 5.4 `http` (and `llhttp`)

**Native binding (`internalBinding('http_parser')`) wraps `llhttp`:**
- `llhttp` is a standalone C library (extracted from Node) that parses HTTP/1.x requests and responses
- The binding creates a parser object that receives data chunks and fires JS callbacks for `onHeadersComplete`, `onBody`, `onMessageComplete`, etc.

**Porting notes:** `llhttp` has no V8 or libuv dependency — it's pure C. The binding is a relatively clean wrapper. The JS layer (`lib/_http_server.js`, `lib/_http_client.js`, `lib/_http_incoming.js`, `lib/_http_outgoing.js`, `lib/_http_common.js`, `lib/_http_agent.js`) is substantial but depends only on `net`, `stream`, `buffer`, `events`, and `url`.

### 5.5 `tls` and `https`

**Native binding wraps OpenSSL:**
- `SSL_CTX_new`, `SSL_new`, `SSL_read`, `SSL_write`, `SSL_do_handshake`
- Certificate handling, verification, SNI, ALPN

**Porting notes:** This is a large and security-sensitive binding. OpenSSL integration is complex and the attack surface for bugs is significant. Consider whether your use case requires TLS. If it does, port carefully and consider reusing Node's test suite for this module specifically.

`https` is a thin wrapper over `http` + `tls` and requires no additional native work.

---

## Phase 6 — Extended Modules (As Needed)

These modules are ported based on specific use-case requirements.

### 6.1 `child_process`

**Native binding wraps:**
- `uv_spawn` — process creation with stdio redirection
- IPC channel support via `uv_pipe_t` for `.send()` / `'message'` event

**Porting notes:** The `uv_spawn` wrapper is moderately complex due to stdio configuration (inherit, pipe, ignore, fd). The IPC serialization for `child_process.fork()` uses a custom wire format over pipes. Port if you need subprocess management.

### 6.2 `crypto`

The largest native binding by surface area. Wraps OpenSSL for:
- Hash (SHA-256, etc.), HMAC, cipher/decipher, sign/verify
- Key generation, key derivation (PBKDF2, HKDF, scrypt)
- DH, ECDH, X25519
- Random bytes, UUID generation
- Certificate parsing (X509)

**Porting notes:** Consider porting only the subset you need. A minimal useful set might be: `crypto.createHash`, `crypto.createHmac`, `crypto.randomBytes`, `crypto.randomUUID`, `crypto.pbkdf2`. Each function is relatively self-contained within the native binding.

### 6.3 `zlib`

**Native binding wraps zlib and brotli:**
- `deflate`, `inflate`, `gzip`, `gunzip`, `brotliCompress`, `brotliDecompress`
- Implemented as Transform streams

**Porting notes:** Straightforward. The binding is a thin wrapper around `z_stream` and brotli state objects. The JS layer handles the stream integration.

### 6.4 `worker_threads`

**This requires significant Hermes-specific design.** Node's implementation creates new V8 isolates per worker. Hermes would need its own threading/isolation model. Key questions:
- Can Hermes run multiple runtime instances in separate threads?
- How is `SharedArrayBuffer` handled?
- How is structured clone serialization implemented for `postMessage`?
- How is the `MessagePort` / `MessageChannel` abstraction mapped?

**Recommendation:** Defer this unless specifically needed. It is the most architecturally divergent module from Node's implementation.

---

## Cross-Cutting Concerns

### Async Hooks

Node's `async_hooks` module tracks the lifecycle of async resources. The internal `AsyncWrap` class annotates every async operation with an async ID and a trigger ID, firing `init`, `before`, `after`, `destroy`, and `promiseResolve` hooks.

In the native bindings, every class that wraps a libuv handle or request inherits from `AsyncWrap`. During the Node-API port, decide whether to:
- **Implement async hooks from the start** — More work upfront, but maintains compatibility with APM tools, diagnostics, and the `AsyncLocalStorage` API
- **Stub it out initially** — `AsyncWrap` becomes a no-op base class. Everything works, but `async_hooks` and `AsyncLocalStorage` do not. These can be added later.

**Recommendation:** Stub initially. Async hooks add complexity to every single async operation and are rarely needed outside of APM/diagnostics tooling.

### Error handling conventions

Node's native bindings follow specific patterns for reporting errors:
- System errors (from libuv) are thrown as objects with `code` (e.g. `'ENOENT'`), `errno` (negative integer), `syscall`, `path`, and `message` properties
- The `UVException` and `ErrnoException` helpers in `lib/internal/errors.js` construct these from the numeric error code

Ensure the ported native bindings pass libuv error codes back to JavaScript consistently, so the JS error construction code works unchanged.

### Constants

Many modules depend on `internalBinding('constants')` which provides a large object of platform constants: file open flags (`O_RDONLY`, `O_WRONLY`), signal numbers (`SIGINT`, `SIGTERM`), errno values, socket constants, etc. Node populates this from C headers at compile time.

Implement this binding early — it is a simple object with numeric properties and has no behavioral complexity, but many modules destructure from it.

### Testing strategy

Node has an extensive test suite under `test/`. For each ported module, run the corresponding test files:
- `test/parallel/test-fs-*.js` for `fs`
- `test/parallel/test-net-*.js` for `net`
- `test/parallel/test-stream-*.js` for `stream`
- etc.

These tests will require the Node test harness (`test/common/index.js` and friends), which itself has dependencies. Setting up the test infrastructure is worthwhile — it is the definitive validation that the ported behavior matches Node.

---

## Estimated Complexity

| Component | Lines of C++ to port (approx) | Difficulty | External deps |
|---|---|---|---|
| `buffer` binding | ~800 | Medium | None |
| `util` binding | ~600 | Low-Medium | None |
| `process` binding | ~1200 | Medium | libuv (sync calls) |
| `constants` binding | ~400 | Low | None (compile-time) |
| `fs` binding | ~2500 | High | libuv |
| `timers` binding | ~300 | Low | libuv |
| `tcp_wrap` / `pipe_wrap` | ~1500 | High | libuv |
| `stream_wrap` / `handle_wrap` | ~1000 | High | libuv |
| `udp_wrap` | ~600 | Medium | libuv |
| `dns` / `cares_wrap` | ~1200 | Medium | libuv, c-ares |
| `http_parser` binding | ~500 | Medium | llhttp |
| `tls_wrap` / `crypto` | ~8000+ | Very High | OpenSSL |
| `zlib` binding | ~800 | Medium | zlib, brotli |
| `spawn_sync` / `process_wrap` | ~1000 | Medium | libuv |

These are rough estimates of the C++ code that needs to be understood and converted to Node-API equivalents. The JS code (`lib/*.js`) is reused and does not count toward porting effort.

---

## Open Questions

1. **Hermes Node-API completeness.** Which Node-API functions does Hermes currently implement, and are there gaps that would block specific modules? A systematic audit against the functions used in each binding is needed before committing to the porting order.

2. **ES feature compatibility.** Which ES features used by Node's `lib/*.js` files does Hermes not yet support? A scan of the `lib/` codebase for `WeakRef`, `FinalizationRegistry`, `RegExp` lookbehind/named groups, optional chaining in patterns not yet supported, etc. should be done early.

3. **Node version target.** Which Node.js version to port from? LTS versions (e.g. Node 20 or 22) are more stable but may have accumulated more V8-specific assumptions. The choice affects the `lib/*.js` files and the native binding interfaces.

4. **Embedding model.** Is the primary target standalone CLI, React Native integration, or both? This affects Phase 3 (event loop integration) design decisions significantly.

5. **Licensing.** Node.js is MIT-licensed. Confirm that reusing `lib/*.js` files and ported `src/node_*.cc` derivatives is compatible with Hermes's licensing and distribution model.
