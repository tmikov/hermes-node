# Hermes Node.js Compatibility Layer -- Design Document

## Overview

This project adds Node.js built-in module compatibility to the
[Hermes JavaScript engine](https://github.com/facebook/hermes).
It is **not** a full Node.js port. It enables Hermes-based environments to run
JavaScript that depends on Node.js APIs (`fs`, `net`, `http`, `child_process`,
`stream`, `buffer`, `path`, `events`, `os`, `dns`, `url`, `util`, `tty`, `vm`,
`repl`, etc.).

The key insight is that Node.js modules are structured as a **JavaScript layer**
(`lib/*.js`) communicating with **thin native C++ bindings** through a narrow
`internalBinding()` interface. The JS layer is reusable as-is; the native
bindings are a mechanical port from V8 API calls to Node-API equivalents. The
official Hermes repository does not yet provide Node-API; this project uses the
`n-api` branch of a [fork](https://github.com/tmikov/hermes/tree/n-api) that
adds a Node-API implementation.

**Current status:** ~156 embedded modules, ~36 native bindings, covering events,
streams, file system, networking (TCP, UDP, Unix sockets, HTTP), DNS, child
processes, TTY, URL, OS, crypto (hashing), timers, module, REPL. Full CJS module
resolution (`node_modules/`, `package.json` main/exports/imports) uses Node's
real loader. TLS/HTTPS, zlib, and worker_threads are not yet implemented.

---

## Architecture

```
+------------------------------------------------------------+
|  User JavaScript                                           |
+------------------------------------------------------------+
|  Node's lib/*.js (vendored from Node v24.13.0, unmodified) |
|  + shims for modules that cannot load under Hermes          |
+------------------------------------------------------------+
|  internalBinding() bridge                                   |
+------------------------------------------------------------+
|  Native bindings (ported from Node's src/node_*.cc)         |
|  using Node-API instead of V8 API                           |
+------------------------------------------------------------+
|  Hermes Node-API implementation                             |
+------------------------------------------------------------+
|  Hermes JavaScript engine                                   |
+------------------------------------------------------------+
|  libuv  |  llhttp  |  c-ares  |  Ada  |  simdutf  |  picohash  |
+------------------------------------------------------------+
```

The architecture has four layers, each with a clear responsibility:

1. **Vendored Node.js JavaScript** (`libjs-node/`): The actual `lib/*.js` files
   from Node v24.13.0. These implement the public API surface -- `EventEmitter`,
   `Stream`, `Buffer`, `fs.*`, `net.*`, `http.*`, etc. They are used unmodified.

2. **Shims** (`libjs/shims/`): Targeted replacements for Node internal modules
   that cannot load under Hermes because they depend on unimplemented bindings
   (e.g., `internalBinding('performance')`, `internalBinding('messaging')`). A
   shim provides the same exports the consuming code expects, but with a simpler
   implementation.

3. **Native bindings** (`lib/bindings/`): C++ implementations of the contracts
   that Node's JS files expect from `internalBinding('fs')`,
   `internalBinding('tcp_wrap')`, etc. These are ported from Node's
   `src/node_*.cc` to use Node-API instead of V8's C++ API. They wrap the same
   underlying C libraries (libuv, llhttp, c-ares, Ada, simdutf) that Node uses.

4. **Infrastructure** (`lib/event-loop/`, `lib/binding-registry/`,
   `lib/module-loader/`, `lib/process/`, `lib/embedded-modules/`): The glue that
   wires everything together -- event loop adapter, module loading, process
   object, and the bytecode embedding pipeline.

### Why Reuse Node's Code

Node's built-in modules represent over a decade of edge-case handling, platform
workarounds, and spec compliance fixes. The `lib/*.js` files encode subtle
behavioral contracts in argument validation, error handling, encoding edge
cases, platform-specific behavior, and async completion semantics.
Reimplementing them would mean rediscovering every bug Node already fixed.

### Why Port to Node-API

Node's existing C++ bindings in `src/node_*.cc` encode the precise contract that
the `lib/*.js` files expect. Porting to Node-API is a mechanical transformation:
replace `v8::Local<Value>` with `napi_value`, `FunctionCallbackInfo<Value>` with
`napi_callback_info`, etc. Writing fresh JSI bindings would require
reverse-engineering the same contracts from the C++ source, with the added risk
of subtle behavioral differences.

### Why Vendor the Same Third-Party Libraries

When Node uses a third-party library (simdutf for SIMD-accelerated string
operations, Ada for URL/IDNA parsing, llhttp for HTTP, c-ares for DNS), this
project vendors and uses that same library rather than hand-rolling equivalent
functionality. This ensures behavioral parity, gets battle-tested optimizations,
and makes our code structure mirror Node's for easier future porting.

---

## Component Details

### Module Loading and Resolution

Module loading has two phases: a **bootstrap loader** for internal modules and
Node's **real CJS loader** for user code. Understanding how they interact is
important because the boundary is non-obvious.

#### Bootstrap Loader (`libjs/loader.js`)

The bootstrap loader is a simple CJS implementation that runs during startup,
before Node's real CJS loader is available. It loads embedded bytecode modules
by ID and is the only loader available during the boot sequence.

Resolution order for `require('internal/foo')`:

1. Check cache (already loaded modules are returned immediately)
2. Try embedded bytecode via `loadBytecodeModule(id)` (compiled at build time)
3. Fall back to `readFileSync(filepath)` + `compileAndRun()` (disk, for user
   scripts only)

The bootstrap loader provides `globalThis.require`, `globalThis.primordials`,
and `globalThis.internalBinding` -- the three things every Node internal module
expects in its compilation scope.

#### Node's CJS Loader (`internal/modules/cjs/loader.js`)

After bootstrap completes, user scripts are loaded via Node's real CJS loader
(2000+ LOC, used unmodified from Node v24.13.0). This provides the full
resolution algorithm:

- `node_modules/` directory traversal via `Module._nodeModulePaths()`
- `package.json` `main`, `exports`, and `imports` fields
- Conditional exports (`require` vs `import` conditions)
- `.js`, `.json`, `.node` extension trying
- `require.resolve()` with `paths` option
- Circular dependency handling
- `Module._cache`, `module.filename`, `__dirname`

#### How the Two Loaders Interact

The transition from bootstrap to CJS happens during `__initCJS()` (called at
bootstrap step 15). This function:

1. Calls `initializeCJS()` on the real CJS loader, which populates
   `Module.builtinModules`, initializes global paths, and sets `Module.runMain`.
2. **Wraps `Module._load`** with a fallback to the bootstrap loader. When the
   CJS loader throws `MODULE_NOT_FOUND`, the wrapper checks the bootstrap
   cache and `loadBytecodeModule()`. This is critical because user code and npm
   packages may `require()` modules that are embedded but not listed as public
   builtins (e.g., `require('internal/...')` in tests).
3. Registers a `.ts` extension handler that compiles TypeScript via Hermes's
   native TS support.

After init, `__loadUserScript(filepath)` calls
`Module._load(path.resolve(filepath), null, true)`. From this point on, all
`require()` calls in user code go through Node's CJS loader, which calls back
into the bootstrap world only for built-in modules.

#### Built-in Module Resolution

When the CJS loader encounters `require('fs')` or `require('module')`, it
recognizes it as a built-in and calls `loadBuiltinModule()` (in our
`internal/modules/helpers` shim). This calls
`BuiltinModule.compileForPublicLoader()` (in our `internal/bootstrap/realm`
shim), which loads the module via the bootstrap loader's `require` and caches
the exports. The `BuiltinModule` class maintains a list of 41 public module
names that are available to user code.

The `module` built-in is notable: `require('module')` loads Node's real
`libjs-node/module.js`, which re-exports the `Module` class from the CJS loader
along with source map APIs, `register()` for ESM hooks, and compile cache
utilities. This is what tools like Babel use for `Module.createRequire()`.

#### Compilation of User Code

When the CJS loader loads a `.js` file from disk, it calls
`compileFunctionForCJSLoader()` from the `contextify` native binding. This
function:

1. Wraps the raw source in a CJS parameter function:
   `(function(exports, require, module, __filename, __dirname) { ... })`
2. Appends `//# sourceURL=<filename>` for stack traces
3. Evaluates via `napi_run_script` to get the wrapper function
4. Returns `{ function, sourceMapURL, sourceURL, cachedDataRejected: false,
   canParseAsESM: false }`

The CJS loader then calls the returned function with the module's `exports`,
`require`, and `module` objects. This is the same flow as in Node, except V8's
`ScriptCompiler::CompileFunction` is replaced with Hermes's `napi_run_script`.

#### Why ESM Modules Are Embedded

The `embedded-modules.txt` manifest includes ~15 ESM-related modules
(`internal/modules/esm/resolve`, `loader`, `module_job`, `translators`, etc.)
even though Hermes does not support ES modules. This is because **the CJS loader
depends on the ESM resolver for `package.json` `exports` field resolution**.

When `Module._findPath()` encounters a package with an `exports` field, it calls
`resolveExports()` which requires `packageExportsResolve` from
`internal/modules/esm/resolve`. The ESM resolver implements the full
[Node.js package exports algorithm](https://nodejs.org/api/packages.html#exports),
including conditional exports, subpath patterns, and wildcard expansions.

These ESM modules have their own dependencies that require the `module_wrap`
native binding to export certain constants. For example,
`internal/modules/esm/loader.js` destructures `kInstantiated`, `kErrored`,
`kSourcePhase`, and `throwIfPromiseRejected` at load time. The `module_wrap`
binding provides stub values for all of these -- the actual `ModuleWrap`
constructor throws if called, and the constants are only meaningful in ESM code
paths that we never execute. What matters is that the destructuring at module
load time succeeds without crashing.

#### Shim-First Resolution

At build time, the embedding pipeline checks `libjs/shims/<id>.js` before
`libjs-node/<id>.js`. This means any Node internal can be replaced without
modifying the vendored files. The shim provides the exports the consuming code
expects with a simpler implementation suitable for Hermes.

Several modules that originally required shims now load directly from vendored
Node source. The most significant example is the CJS loader itself
(`internal/modules/cjs/loader.js`), which was originally shimmed as a ~130 LOC
stub but is now used unmodified at its full 2000+ LOC.

#### Embedding Pipeline

All ~155 internal modules are compiled to Hermes bytecode at build time:

1. Reads `embedded-modules.txt` (manifest of module identifiers)
2. Resolves each ID to a file path (checking shims first)
3. Wraps in a CJS wrapper function:
   `(function(exports, require, module, process, internalBinding, primordials) { ... })`
4. Compiles to Hermes bytecode via `hermesc`
5. Converts to C byte arrays and links into the binary

### Binding Registry (`lib/binding-registry/`)

A simple name-to-initializer map. During bootstrap, `hermes-node.cpp` registers
~35 bindings:

```cpp
registry.registerBinding("fs", initFsBinding);
registry.registerBinding("tcp_wrap", initTcpWrapBinding);
// ... etc
```

When JavaScript calls `internalBinding('fs')`, the registry invokes
`initFsBinding(env, exports)` on first access and caches the result. This is the
bridge between Node's JS layer and our native code.

### Primordials (`libjs/primordials.js`)

Nearly every Node `lib/*.js` file destructures from a `primordials` object:

```js
const { ArrayPrototypeSlice, SafeMap, StringPrototypeSlice } = primordials;
```

Node creates this at bootstrap by copying every built-in prototype method into a
flat namespace and creating tamper-proof `Safe*` variants. We use a **thin
shim** (Option B from the design alternatives): the primordials object
re-exports standard builtins without safety wrappers. `SafeMap` is `Map`,
`ArrayPrototypeSlice` is the uncurried version of `Array.prototype.slice`, etc.

The shim algorithmically enumerates prototypes using `Reflect.ownKeys` +
`getOwnPropertyDescriptor` to create correctly-named uncurried copies. It also
polyfills several features missing from Hermes:

- `FinalizationRegistry` (no-op: `register`/`unregister` do nothing)
- `Error.captureStackTrace` / `Error.stackTraceLimit` (V8-isms used throughout
  Node)
- `Symbol.dispose` / `Symbol.asyncDispose`
- `Array.prototype.toSorted()`

This means user code that modifies built-in prototypes (e.g.,
`Array.prototype.push = ...`) could in theory break internal modules. Full
primordials would prevent this, but the added complexity was not justified at
this stage. The uncurried method copies captured at bootstrap time do provide
partial protection -- they remain bound to the original functions even if
prototypes are later modified.

### Event Loop (`lib/event-loop/`)

The `UvEventLoop` class adapts libuv's `uv_loop_t` for Hermes's Node-API event
loop interface. It provides:

- `init()` / `run()` / `close()` lifecycle
- `post_work()` / `cancel_work()` for async operations
- `post_task()` for scheduling JS callbacks from native code

**Tick draining** is critical. Two libuv handles drain microtasks and
`process.nextTick` callbacks on each loop iteration:

- `uv_prepare_t` fires **before** the poll phase. This ensures ticks scheduled
  during the timers or pending-callbacks phase (e.g., from `uv_shutdown`
  completion) are processed before poll blocks.
- `uv_check_t` fires **after** the poll phase. This drains ticks scheduled by
  I/O callbacks.

Both handles are unref'd so they don't prevent the loop from exiting.

Without the prepare handle, `process.nextTick` work scheduled from native
callbacks could stall for the duration of the poll timeout -- potentially
minutes. Node solves this with `InternalCallbackScope`/`MakeCallback` wrappers
on every native callback; the two-handle approach is a simpler approximation.

**Cleanup ordering** is strict and failure to follow it causes use-after-free:

1. Close stdio stream native handles
2. `caresWrapShutdown()` (close DNS channel handles)
3. `closeTimersHandles()` / `closeFsEventWrapHandles()`
4. Stop and close tick-drain handles
5. `eventLoop.close()` (force-closes remaining handles via `uv_walk()`)
6. `clearHandleWrapEventLoop()` (null the loop pointer for GC finalizers)
7. `hermes_napi_destroy_env()`

### Native Bindings (`lib/bindings/`)

~22,200 lines of C++ across 36 source files. Each binding is an `init` function
that populates an exports object with functions, constructors, and shared typed
arrays.

#### Handle and Stream Wraps

The most architecturally significant bindings are the handle/stream base
classes, which provide reusable lifecycle management for all libuv handle types:

**`HandleWrapBase`** (`handle_wrap_base.h/.cpp`): Base class for any
`uv_handle_t` wrapper. Manages:

- **Ref counting**: `ref()` / `unref()` / `hasRef()` control whether the handle
  keeps the event loop alive
- **Close lifecycle**: `close()` → `uv_close()` → callback deletes the C++
  object
- **GC safety**: Uses `napi_wrap` with a prevent-GC reference. The ref prevents
  the JS object from being garbage-collected while the native handle is alive.
  On `doClose()`, the wrap is removed first, transferring ownership from the GC
  finalizer to the `uv_close` callback.

**`LibuvStreamBase`** (`libuv_stream_base.h/.cpp`): Extends HandleWrapBase with
stream I/O:

- `readStart()` / `readStop()` wrapping `uv_read_start` / `uv_read_stop`
- `writeBuffer()` / `writeUtf8String()` / `writeLatin1String()` /
  `writeUcs2String()` / `writev()` wrapping `uv_write`
- `shutdown()` wrapping `uv_shutdown`
- `bytesRead` / `bytesWritten` counter properties
- Shared `streamBaseState` Int32Array for communicating read status to JS

**Recipe for new stream wraps**: Inherit `LibuvStreamBase`, call `uv_*_init()`
in the constructor, call `initStream(env, jsObj, stream)`, call
`addStreamMethods(env, prototype)` on the class prototype.

Concrete wraps: `TCPWrap`, `PipeWrap`, `TTYWrap` (streams), `UDPWrap` (handle
only, not a stream), `ProcessWrap` (handle only).

#### File System (`node_file.cpp`, `node_file_dir.cpp`)

The largest binding (~3000 LOC). Wraps the `uv_fs_*` family for 38 filesystem
operations. Key design choices:

- **Shared stats buffer**: A `Float64Array(36)` is shared between native and JS.
  Stat results are written into it by C++ and read by JS, avoiding per-field
  `napi_set_named_property` calls. Two entries (18 fields each) for `stat` and
  `lstat`. **Exception**: Promise-mode stats allocate fresh arrays to avoid race
  conditions with concurrent callback-mode operations.

- **Sync vs async**: Both paths go through the same binding functions. For sync,
  the libuv callback parameter is `NULL`. For async, a heap-allocated
  `FSReqWrap` holds the `uv_fs_t`, a `napi_ref` to the JS callback object, and
  any buffers that must outlive the operation. On completion, `fsAfterAsync`
  unwraps the JS object and calls `reqObj.oncomplete(status)`.

#### HTTP Parser (`node_http_parser.cpp`)

Wraps llhttp for HTTP/1.x parsing. The parser fires indexed callbacks
(`parser[kOnMessageBegin]`, `parser[kOnHeadersComplete]`, `parser[kOnBody]`,
`parser[kOnMessageComplete]`). Headers accumulate in a 32-pair buffer, flushed
via `kOnHeaders`.

Data flows through JavaScript: `socket.ondata` → `parser.execute(buffer)` →
callbacks. The `consume()`/`unconsume()` methods are state flags only -- there
is no C++-level stream interception.

#### DNS (`node_cares_wrap.cpp`)

Two resolution paths:

- `uv_getaddrinfo` for `dns.lookup()` (what `net.connect()` uses). Hostname IDNA
  conversion via Ada's `ada::idna::to_ascii()`.
- c-ares for `dns.resolve*()` functions (A, AAAA, MX, NS, TXT, SRV, CNAME, PTR,
  NAPTR, SOA, CAA, reverse).

`ChannelWrap` wraps `ares_channel_t` with per-socket `uv_poll_t` handles and a
`uv_timer_t` for timeout processing. A static set tracks all live instances for
clean shutdown. The prevent-GC reference pattern prevents the GC from collecting
a ChannelWrap while its embedded libuv handles are still open.

#### Child Process (`node_process_wrap.cpp`, `node_spawn_sync.cpp`)

Async spawn uses `ProcessWrap` inheriting `HandleWrapBase`. `uv_spawn` both
initializes the handle and starts the process. Critically, `init()` is called
unconditionally after `uv_spawn` -- even on failure -- because `uv_spawn` always
registers the handle with the loop's internal queue.

Sync spawn (`SyncProcessRunner`) creates a temporary `uv_loop_t`, spawns the
child, runs the loop until exit, and collects output into linked-list buffers.
Buffer wrapping (`Buffer.from(uint8array)`) happens in JavaScript, not native
code, to avoid ASAN issues with re-entering the JS interpreter from NAPI
callbacks.

### Shims (`libjs/shims/`)

When a Node internal module cannot load under Hermes (usually because it depends
on an unimplemented binding), a shim provides the subset of exports that
consuming code actually needs. Notable shims:

| Shim                             | Why needed                                                               | What it provides                                                     |
| -------------------------------- | ------------------------------------------------------------------------ | -------------------------------------------------------------------- |
| `internal/abort_controller.js`   | Original needs `internalBinding('performance')` + `event_target`         | Minimal AbortController/AbortSignal using EventEmitter               |
| `internal/url.js`                | Original (1700 LOC) has deep dependency chain                            | Self-contained ~860 LOC URL/URLSearchParams backed by Ada C++ parser |
| `domain.js`                      | Original needs `async_hooks` → `internalBinding('async_context_frame')`  | Minimal Domain class extending EventEmitter for REPL error isolation |
| `cluster.js`                     | Breaks chain: `net` → `cluster` → `child_process` → `dgram` → `udp_wrap` | `isPrimary: true` (standalone CLI is always the primary)             |
| `internal/options.js`            | Original needs `internalBinding('options')`                              | Static defaults for ~90 CLI options                                  |
| `internal/bootstrap/realm.js`    | Original needs full bootstrap infrastructure                             | `BuiltinModule` class with 41 public module names, `compileForPublicLoader()` |
| `internal/perf/observe.js`       | Original needs `internalBinding('performance')`                          | No-op `hasObserver`/`startPerf`/`stopPerf`                           |
| `crypto.js`                      | Original loads ~15 internal crypto modules needing full OpenSSL binding   | `createHash`/`createHmac` backed by picohash (MD5/SHA1/SHA224/SHA256) |

Note: Node's real `internal/modules/cjs/loader.js` (2000+ LOC) loads
successfully and is used directly -- no shim needed. The `module_wrap` binding
provides the constants and stubs that its ESM-related dependencies require at
load time.

### Process Object (`lib/process/`, inline JS in `hermes-node.cpp`)

The `NodeProcess` C++ class provides static properties (`pid`, `ppid`,
`platform`, `arch`, `version`, `argv`, `execPath`) and methods (`cwd()`,
`chdir()`, `hrtime()`, `memoryUsage()`, `cpuUsage()`, `exit()`, `umask()`,
`uptime()`).

`process.env` is a Proxy-like object: property access calls `getenv()`,
assignment calls `setenv()`, `delete` calls `unsetenv()`, and enumeration
returns all environment variables.

Event emitter methods (`on`, `off`, `once`, `emit`, `emitWarning`,
`prependListener`, `removeAllListeners`, etc.) are added via an inline
JavaScript snippet during bootstrap, before the full `events` module loads. This
avoids a circular dependency -- `events.js` itself uses
`process.on('warning', ...)`.

---

## Bootstrap Sequence

The `hermes-node` binary (`tools/hermes-node/hermes-node.cpp`) orchestrates a
carefully ordered startup:

1. **Hermes runtime** with microtask queue, async generators, and ES6 block
   scoping enabled
2. **libuv event loop** initialization
3. **NAPI environment** creation, bridging Hermes and libuv
4. **Minimal console** (C++ `fprintf`-based `log`/`warn`/`error`/`info`) so
   early bootstrap errors are visible
5. **Native binding registration** (~35 bindings) plus host callbacks for event
   loops, microtask draining, and async break
6. **Primordials** execution (polyfills, uncurried builtins, `Safe*` variants)
7. **`internalBinding()` function** creation
8. **`process` global** creation (C++ properties + inline JS event emitter)
9. **Module loader** initialization with primordials and `internalBinding`
   injected
10. **`process.nextTick`** via `internal/process/task_queues` →
    `setupTaskQueue()` → `nextTick` + `runNextTicks`
11. **Timer globals** (`setTimeout`, `setInterval`, `setImmediate`) via
    `internal/timers` → `getTimerCallbacks()` → `setupTimers()`
12. **`globalThis.Buffer`** from the `buffer` module
13. **`globalThis.URL`** / **`globalThis.URLSearchParams`** from `internal/url`
14. **`debuglog`** initialization (`NODE_DEBUG` env var)
15. **CJS loader initialization** (`initializeCJS()` → `Module.builtinModules`,
    global paths, `.ts` extension handler, `Module.runMain`)
16. **Stdio streams** (`setup-stdio.js` installs lazy getters for
    `process.stdin`/`stdout`/`stderr`)
17. **Real console** (Node's `console` module with `util.inspect` formatting,
    `console.table`, etc.)
18. **Tick drain handles** (prepare + check) started on the event loop
19. **User script** via `__loadUserScript(path)` → `Module._load()`, or **REPL**
    via `require('internal/repl').createInternalRepl()`
20. **Event loop** runs until all handles/requests complete
21. **`process.emit('exit')`** before cleanup
22. **Cleanup** in strict reverse order (see Event Loop section)

---

## Hermes Engine Limitations

These are limitations of the Hermes JavaScript engine that affect what Node.js
features work:

| Limitation                                                  | Impact                                                            | Mitigation                                                        |
| ----------------------------------------------------------- | ----------------------------------------------------------------- | ----------------------------------------------------------------- |
| No `FinalizationRegistry`                                   | Leak prevention in event_target, abort_controller                 | No-op polyfill                                                    |
| No `AbortSignal`/`AbortController` globals                  | Stream pipeline abort, fetch API                                  | EventEmitter-based shim                                           |
| No `Atomics`                                                | SharedArrayBuffer operations                                      | Not needed for current scope                                      |
| Async generator prototype chain is flat                     | `instanceof` checks for async generators                          | Accepted; primordials provide `AsyncIteratorPrototype` separately |
| `let`/`const` don't persist across `napi_run_script` calls  | REPL variables declared with `let`/`const` are lost between lines | Known limitation; use `var` in REPL                               |
| Warns about undeclared globals in strict-mode IIFEs         | Noisy warnings during bootstrap                                   | Use `var X = globalThis.X` pattern                                |
| `Error.captureStackTrace` / `Error.stackTraceLimit` missing | V8-isms used pervasively in Node's error system                   | Polyfilled in primordials                                         |
| `triggerTimeoutAsyncBreak()` raises uncatchable error       | SIGINT interruption of `vm.runInThisContext` would crash REPL     | Intercept at NAPI boundary, rethrow as catchable error            |

---

## Known Issues and Limitations

### No Real VM Sandboxing

`vm.runInNewContext()` and `vm.createContext()` do not create isolated contexts.
All code executes in the main global context. `createContext()` merely marks the
sandbox object with a symbol so `isContext()` returns true. This is acceptable
for the REPL (which uses `useGlobal: true`) but means `vm`-based sandboxing is
not actually sandboxed.

### Limited Crypto (No TLS/HTTPS)

The `crypto` module provides hashing only: `createHash` and `createHmac` backed
by picohash (MD5, SHA1, SHA224, SHA256). Full OpenSSL integration is not
implemented. `https`, `tls`, ciphers, key generation, and random bytes are not
available. HTTP works over plaintext only.

### No Worker Threads

`worker_threads` requires multiple Hermes runtime instances in separate threads,
structured clone serialization for `postMessage`, and `SharedArrayBuffer`
support. This is architecturally divergent from Node's V8-based implementation
and is deferred.

### No ESM

Only CommonJS module loading is supported. `import` statements, `import()`
expressions, and `package.json` `"type": "module"` are not handled.

### Async Hooks Stubbed

`async_hooks` and `AsyncLocalStorage` are no-ops. The `AsyncWrap` constants and
provider types are exported for compatibility, but tracking functions do
nothing. This means APM tools and diagnostics that depend on async hooks will
not work.

### `napi_create_buffer_copy` Returns Uint8Array

Hermes's NAPI implementation returns a plain `Uint8Array` from
`napi_create_buffer_copy`, not a Node.js `Buffer`. Code that calls
`buf.toString('utf8')` on the result gets comma-separated byte values instead of
decoded text. The workaround is to wrap with `Buffer.from()` wherever Buffer
methods are needed.

### Latin1 String Write Limitation

`LibuvStreamBase::writeLatin1String` extracts the string as UTF-8 (there is no
`napi_get_value_string_latin1` in NAPI). This is acceptable for networking text
but does not preserve exact byte values for characters above 127.

### REPL `let`/`const` Scope

Each REPL input is evaluated as a separate script via `napi_run_script`.
Variables declared with `let` or `const` do not persist between lines. Only
`var` declarations (which attach to the global object) survive across inputs.

---

## Project Layout

```
hermes-node-compat/
  hermes/                       Hermes submodule (n-api branch)
  external/
    libuv/libuv/                libuv 1.51.0
    cares/cares/                c-ares 1.34.6
    llhttp/llhttp/              llhttp 9.3.0
    ada/ada/                    Ada URL parser
    simdutf/simdutf/            SIMD UTF validation
    picohash/picohash/          picohash (MD5/SHA1/SHA224/SHA256/HMAC)
  lib/
    binding-registry/           internalBinding() dispatch
    bindings/                   Native bindings (~21,600 LOC C++)
    embedded-modules/           JS-to-bytecode build pipeline
    event-loop/                 libuv adapter
    module-loader/              Bootstrap require() for internal modules
    process/                    process global object
  libjs/
    primordials.js              Polyfills + uncurried builtins
    loader.js                   Module loader (JS side)
    setup-stdio.js              process.stdin/stdout/stderr streams
    shims/                      Targeted replacements for Node internals
  libjs-node/                   Vendored Node v24.13.0 lib/*.js (unmodified)
  include/hermes/node-compat/   Public C++ headers
  tools/hermes-node/            CLI binary entry point
  test/                         JS tests (LLVM Lit)
  unittests/                    C++ unit tests (GTest)
  examples/                     Example projects (e.g., Babel parser/transform)
```

### Vendored Dependencies Convention

Each vendored library follows the pattern `external/$lib/$lib/` where the outer
directory contains a wrapper `CMakeLists.txt` and `README.md`, and the inner
directory is the unmodified upstream source. This keeps upstream code clearly
separated from our build integration.

---

## How to Add a New Native Binding

1. **Read Node's implementation** in `/home/tmikov/3rd/node/src/node_<name>.cc`.
   Understand the exports, argument conventions, and async patterns.

2. **Create the binding source** in `lib/bindings/node_<name>.cpp` and header in
   `include/hermes/node-compat/bindings/node_<name>.h`. The header declares
   `napi_value initXxxBinding(napi_env env, napi_value exports)`.

3. **Port V8 API calls to Node-API**:
   - `v8::Local<Value>` → `napi_value`
   - `FunctionCallbackInfo<Value>` → `napi_callback_info` (use
     `napi_get_cb_info`)
   - `v8::String::Utf8Value` → `napi_get_value_string_utf8`
   - `obj->Set(key, val)` → `napi_set_named_property`
   - `ObjectTemplate::NewInstance` → `napi_create_object` +
     `napi_define_properties`
   - `FunctionTemplate::New` → `napi_define_class` (for constructor-based wraps)

4. **Register the binding** in `hermes-node.cpp`:
   `registry.registerBinding("name", initXxxBinding)`.

5. **Add to CMakeLists.txt** in `lib/bindings/`.

6. **If a libuv event loop is needed**, add a `setXxxEventLoop(uv_loop_t*)`
   function and call it from `hermes-node.cpp` before binding init.

7. **Check if a shim is needed**: if the Node JS module that uses this binding
   also depends on other unimplemented bindings, you may need a shim for those.

8. **Add the module to `embedded-modules.txt`** if it should be available at
   runtime.

### Handle Wrap Pattern

For bindings that wrap a libuv handle:

```cpp
class MyWrap : public LibuvStreamBase {  // or HandleWrapBase for non-streams
  uv_xxx_t handle_;
public:
  MyWrap() : LibuvStreamBase(sizeof(uv_xxx_t)) {}

  static napi_value construct(napi_env env, napi_callback_info info) {
    // Extract args, create MyWrap, uv_xxx_init(), initStream(), napi_wrap()
  }
};
```

Call `addStreamMethods(env, prototype)` on the class prototype to add
`readStart`, `readStop`, `write*`, `shutdown`, etc.

---

## How to Add a New Shim

1. **Identify what the consuming code actually needs.** Grep for imports of the
   module across `libjs-node/` to find which exports are used. Often only a
   small subset of a complex module's exports are needed.

2. **Create the shim** at `libjs/shims/<module-path>.js` (mirroring the path
   under `libjs-node/`). Export exactly what consumers need, with the simplest
   correct implementation.

3. **Reconfigure CMake** after adding the file. Shim resolution uses `EXISTS` at
   CMake configure time, so a new shim file is not picked up until reconfigure.

4. **Add to `embedded-modules.txt`** if not already listed (usually it already
   is, since the module ID exists).

---

## Testing

Tests are split between C++ unit tests (GTest) and JavaScript integration tests
(LLVM Lit):

- **`check-hermes-node`**: Runs both suites. This is the pre-commit gate.
- **JS tests** (`test/*.js`): Each file has `// RUN:` directives. Two patterns:
  - Our custom tests: `// RUN: %hermes-node %s | %FileCheck %s` checking for
    `// CHECK: PASS`
  - Ported Node tests: `// RUN: TEST_THREAD_ID=$$ %hermes-node %s` checking for
    exit code 0
- **Node test harness**: `test/node-tests/common/` provides `mustCall`,
  `mustSucceed`, `mustNotCall`, and tmpdir utilities.
- **All development uses ASAN** (`cmake-build-asan`). Tests must pass clean
  under AddressSanitizer.
