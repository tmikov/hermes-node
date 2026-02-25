# Debugger Design -- `--inspect` Support for hermes-node

## Overview

This document describes the design for adding Chrome DevTools Protocol (CDP)
debugging to hermes-node, replicating Node.js's `--inspect` experience. The
implementation enables connecting any browser to hermes-node for interactive
JavaScript debugging (breakpoints, stepping, console, profiling, heap
snapshots).

Key design decisions:

- **Two-runtime architecture**: A second hermes-node runtime runs on a dedicated
  IO thread, hosting a JavaScript-based WebSocket server. This avoids
  implementing WebSocket/HTTP in C++ and reuses our existing `net`, `http`,
  `crypto`, and `events` modules.

- **Vendored `ws` package**: The battle-tested `ws` npm package (pure JS, zero
  dependencies, MIT license) provides the WebSocket implementation. Three small
  patches make it compatible with hermes-node (lazy `https`/`tls`/`zlib`
  imports, `Math.random` shim for `randomBytes`/`randomFillSync`).

- **Self-contained DevTools frontend**: The Chrome DevTools frontend is packaged
  as a zip archive installed alongside the binary. The inspector HTTP server
  reads files directly from the archive at runtime. No CDN or internet
  connection required.

- **Hermes's existing CDP stack**: The Hermes engine already has a complete CDP
  implementation (`CDPAgent`, `CDPDebugAPI`, domain agents for Debugger,
  Runtime, Profiler, HeapProfiler). We wire it up; we do not reimplement CDP.

---

## User Experience

### CLI Flags

```
hermes-node --inspect[=host:port] script.js
hermes-node --inspect-brk[=host:port] script.js
```

| Flag | Behavior |
| ---- | -------- |
| `--inspect` | Start inspector, run script immediately |
| `--inspect-brk` | Start inspector, pause before first user statement |
| `--inspect-wait` | Start inspector, wait for debugger to connect before running (future) |

Default host:port is `127.0.0.1:9229` (same as Node.js).

### Startup Output

```
Debugger listening on ws://127.0.0.1:9229/a1b2c3d4-...
Open DevTools: http://127.0.0.1:9229/devtools/inspector.html?ws=127.0.0.1:9229/a1b2c3d4-...
```

The second URL works in **any browser** (Chrome, Firefox, Safari, Edge) because
we serve the DevTools frontend ourselves. Chrome users can also use
`chrome://inspect` which auto-discovers the process via the `/json` HTTP
endpoint.

### HTTP Discovery Endpoints

The inspector HTTP server (same port as the WebSocket) serves:

| Path | Response |
| ---- | -------- |
| `GET /json` or `/json/list` | JSON array describing the debug target |
| `GET /json/version` | `{"Browser": "hermes-node/0.x.y", "Protocol-Version": "1.1"}` |
| `GET /devtools/*` | Chrome DevTools frontend files (from zip archive) |
| `WS /<uuid>` | WebSocket upgrade for CDP session |

The `/json/list` target entry:

```json
[{
  "description": "hermes-node instance",
  "id": "<uuid>",
  "title": "script.js",
  "type": "node",
  "url": "file:///path/to/script.js",
  "webSocketDebuggerUrl": "ws://127.0.0.1:9229/<uuid>",
  "devtoolsFrontendUrl": "http://127.0.0.1:9229/devtools/inspector.html?ws=127.0.0.1:9229/<uuid>"
}]
```

---

## Architecture

```
+------------------------------------------------------------------+
|  Any Browser                                                      |
|  (DevTools frontend served by inspector runtime)                  |
+------------------------------------------------------------------+
        |
        |  WebSocket (default port 9229)
        |  JSON CDP messages
        v
+------------------------------------------------------------------+
|  Inspector Runtime (IO thread)                                    |
|    - Separate HermesRuntime + napi_env + UvEventLoop              |
|    - Runs JS WebSocket/HTTP server (vendored ws + http modules)   |
|    - Serves /json endpoints                                       |
|    - Serves DevTools frontend from zip archive                    |
+------------------------------------------------------------------+
        |
        |  uv_async_t cross-thread signaling + message queues
        |
        v
+------------------------------------------------------------------+
|  Main Runtime (main thread)                                       |
|    - HermesRuntime created via makeHermesRuntime()                 |
|    - CDPDebugAPI + CDPAgent                                       |
|    - User's JavaScript                                            |
|    - Event loop (uv_run)                                          |
+------------------------------------------------------------------+
        |
        v
+------------------------------------------------------------------+
|  Hermes CDP Stack (hermes/API/hermes/cdp/)                        |
|    CDPAgent        - thread-safe command dispatch                  |
|    CDPDebugAPI     - per-runtime debug state                      |
|    Domain agents   - Debugger, Runtime, Profiler, HeapProfiler    |
+------------------------------------------------------------------+
        |
        v
+------------------------------------------------------------------+
|  vm::Debugger      - bytecode-level breakpoints, stepping         |
+------------------------------------------------------------------+
```

### Why Two Runtimes

A single runtime cannot serve a WebSocket while also being debugged (paused at a
breakpoint means the event loop is blocked). Node.js solves this with a
dedicated C++ IO thread running hand-rolled HTTP/WebSocket code (~1500 lines of
C++). We solve it with a second hermes-node runtime on a dedicated thread.

Benefits:

- **No C++ WebSocket/HTTP code**. The inspector's networking is JavaScript using
  our existing `net`, `http`, `events`, and `stream` modules plus the vendored
  `ws` package. This is code we already maintain and test.

- **`runHermesNode()` is already thread-safe**. Commit 69fc0e4 eliminated
  per-binding file-scope statics and extracted a callable API with per-env
  `RuntimeState`. Spinning up a second runtime is just a function call.

- **Dogfooding**. The inspector runtime exercises our own networking stack,
  catching bugs we would otherwise miss.

### Threading Model

```
Main Thread                        IO Thread (Inspector)
-----------                        ---------------------
runHermesNode(userConfig)          runHermesNode(inspectorConfig)
  |                                  |
  |  HermesRuntime (user code)       |  HermesRuntime (WS server)
  |  CDPDebugAPI + CDPAgent          |  ws.WebSocketServer on port 9229
  |  uv_run(UV_RUN_DEFAULT)         |  uv_run(UV_RUN_DEFAULT)
  |                                  |
  |  <-- uv_async_t signaling -->    |
  |                                  |
```

Cross-thread communication uses two `uv_async_t` handles (one in each event
loop) and mutex-protected message queues:

1. **IO -> Main** (CDP command from DevTools): Inspector JS receives a WebSocket
   message, calls a native binding function that pushes the JSON string onto the
   inbound queue and calls `uv_async_send` on the main loop's async handle. The
   main loop's async callback calls `cdpAgent->handleCommand(json)`.

2. **Main -> IO** (CDP response/event to DevTools): `CDPAgent`'s outbound
   message callback pushes the JSON string onto the outbound queue and calls
   `uv_async_send` on the inspector loop's async handle. The inspector loop's
   async callback delivers the message to JS, which sends it over the WebSocket.

3. **Runtime tasks**: `CDPAgent` may enqueue `RuntimeTask` functions (e.g., for
   expression evaluation during a breakpoint pause). These are delivered via the
   same inbound queue mechanism and executed as `task(*hermesRT)` on the main
   thread.

---

## Component Details

### HermesRuntime Switchover

The main runtime must be created via `makeHermesRuntime()` (JSI) instead of
`vm::Runtime::create()` (internal API), because `CDPDebugAPI::create()` requires
a `HermesRuntime&` reference.

```cpp
#include <hermes/hermes.h>
#include <jsi/hermes-interfaces.h>

// Create HermesRuntime (owns vm::Runtime internally)
auto hermesRT = facebook::hermes::makeHermesRuntime(vmConfig);

// Extract vm::Runtime* for NAPI (canonical pattern from ConsoleHost.cpp)
auto *vmRuntime = static_cast<hermes::vm::Runtime *>(
    facebook::jsi::castInterface<facebook::hermes::IHermes>(hermesRT.get())
        ->getVMRuntimeUnsafe());

// Both NAPI and CDP share the same underlying vm::Runtime
napi_env env = hermes_napi_create_env(vmRuntime, eventLoop.getHost());
auto cdpDebugAPI = facebook::hermes::cdp::CDPDebugAPI::create(*hermesRT);
```

The `HERMES_ENABLE_DEBUGGER` CMake flag must be ON for the CDP sources to
compile. Set in our top-level `CMakeLists.txt`:

```cmake
set(HERMES_ENABLE_DEBUGGER ON CACHE BOOL "Enable Hermes debugger/CDP" FORCE)
```

### CDP Agent Wiring

`CDPAgent` is thread-safe by design. Its constructor takes:

- `executionContextID` (int32): identifies the JS context (use 1)
- `cdpDebugAPI` (CDPDebugAPI&): per-runtime debug state
- `enqueueRuntimeTaskCallback`: delivers tasks needing exclusive runtime access
- `messageCallback`: delivers outbound CDP messages (called from any thread)

```cpp
auto cdpAgent = facebook::hermes::cdp::CDPAgent::create(
    /*executionContextID=*/1,
    *cdpDebugAPI,
    /*enqueueRuntimeTask=*/[&](RuntimeTask task) {
      pushToInboundQueue(std::move(task));
      uv_async_send(&mainAsyncHandle);
    },
    /*messageCallback=*/[&](const std::string &msg) {
      pushToOutboundQueue(msg);
      uv_async_send(&inspectorAsyncHandle);
    });
```

Both callbacks may be called from arbitrary threads.

### Inspector Native Binding

The inspector runtime needs a small native binding (`inspector_bridge`) to
communicate with the main thread. This binding provides:

- `sendToMain(jsonString)`: Push a CDP command to the inbound queue, signal the
  main loop. Called by JS when a WebSocket message arrives.
- `onMessageFromMain(callback)`: Register a JS callback that receives CDP
  responses/events. Called by the outbound async handler.
- `getInspectorConfig()`: Returns `{ host, port, scriptName, sessionId }` so
  the JS server knows what to serve in `/json/list`.

The binding is registered only on the inspector runtime, not the user runtime.

### WebSocket Server (Vendored `ws`)

The `ws` package (v8.19.0, MIT license) is vendored into the project with three
patches:

| File | Change | Reason |
| ---- | ------ | ------ |
| `websocket.js` | Lazy-try `require('https')` and `require('tls')` | Not implemented; only needed for `wss://` |
| `websocket.js` | `Math.random` shim for `randomBytes` | `crypto.randomBytes` not implemented |
| `sender.js` | `Math.random` shim for `randomFillSync` | `crypto.randomFillSync` not implemented |
| `permessage-deflate.js` | Lazy-try `require('zlib')` | Not implemented; only needed for compression |

These patches are minimal and maintainable. The `Math.random` shims are adequate
for WebSocket key generation and frame masking in a localhost debugging context
(not security-sensitive). When `crypto.randomBytes` is implemented, the shims
become unnecessary.

The inspector JS script uses `ws.WebSocketServer` with an existing
`http.createServer()` instance (for shared HTTP + WebSocket on the same port).

### Inspector JS Script

The inspector runtime runs a bundled JS script (embedded as bytecode, like all
internal modules). Pseudocode:

```js
const http = require('http');
const { WebSocketServer } = require('ws');
const { sendToMain, onMessageFromMain, getInspectorConfig } =
    internalBinding('inspector_bridge');

const config = getInspectorConfig();
const server = http.createServer(handleHttpRequest);
const wss = new WebSocketServer({ server });

wss.on('connection', (ws) => {
  // Forward WebSocket messages to main runtime
  ws.on('message', (data) => sendToMain(data.toString()));

  // Forward CDP responses/events back to WebSocket
  onMessageFromMain((msg) => ws.send(msg));

  ws.on('close', () => { /* notify main runtime */ });
});

server.listen(config.port, config.host);

function handleHttpRequest(req, res) {
  if (req.url === '/json' || req.url === '/json/list') {
    // Return target JSON with webSocketDebuggerUrl, devtoolsFrontendUrl
  } else if (req.url === '/json/version') {
    // Return browser/protocol version
  } else if (req.url.startsWith('/devtools/')) {
    // Serve DevTools frontend file from zip archive
  }
}
```

### DevTools Frontend

The Chrome DevTools frontend is a web application (HTML, JS, CSS) that
implements the debugger UI. It communicates with the debuggee via CDP over
WebSocket.

#### Packaging

The frontend is packaged as a **single zip archive** installed alongside the
hermes-node binary:

```
bin/
  hermes-node
share/hermes-node/
  devtools-frontend.zip       (~10-15 MB compressed)
```

At build time, a CMake script downloads the DevTools frontend for a specific
Chrome version (pinned to a known-compatible commit hash), packages it into a
zip, and installs it. The version is pinned to match the CDP protocol version
that Hermes implements.

#### Serving

The inspector HTTP server reads files directly from the zip archive on demand.
A minimal zip reader (central directory parsing + inflate for individual entries)
is needed. Options:

1. **miniz** (~1200 lines of C, public domain): Single-file zip/deflate library.
   Well-suited for reading individual entries from a zip without extracting the
   whole archive.

2. **Extract to temp directory**: On inspector startup, extract the zip to a temp
   directory and serve files from disk. Simpler but creates temp files.

Option 1 is preferred (no temp files, no cleanup needed).

When the browser navigates to
`http://host:port/devtools/inspector.html?ws=host:port/uuid`, the HTTP handler
strips the `/devtools/` prefix, looks up the remaining path in the zip archive,
and serves the file with the appropriate Content-Type header.

#### Build Integration

```cmake
option(HERMES_NODE_DEVTOOLS "Download and package DevTools frontend" ON)

if(HERMES_NODE_DEVTOOLS)
  set(DEVTOOLS_VERSION "60127beb442528082b3f6eff7392267e145262c3")
  # Download from Chrome CDN or npm at configure time
  # Package into devtools-frontend.zip
  # Install alongside the binary
endif()
```

The DevTools frontend is an optional build component. When disabled, the
inspector still works -- just without the self-hosted frontend (users can
connect via `chrome://inspect` in Chrome, which uses Chrome's bundled frontend).

#### Finding the Archive at Runtime

The binary locates the zip archive relative to its own path:

```cpp
// Get binary path via uv_exepath()
// Look for ../share/hermes-node/devtools-frontend.zip (Unix)
// Look for devtools-frontend.zip in same directory (fallback)
```

If the archive is not found, the inspector prints a warning but still starts
(WebSocket debugging works, just no self-hosted frontend). The `/devtools/*`
HTTP handler returns 404.

---

## Startup Sequence

When `--inspect` or `--inspect-brk` is specified:

1. **Parse CLI flags**: Extract host, port, inspect mode.

2. **Create main HermesRuntime** via `makeHermesRuntime()`. Extract
   `vm::Runtime*` for NAPI.

3. **Create CDPDebugAPI** from `HermesRuntime&`.

4. **Create CDPAgent** with enqueue and outbound callbacks wired to
   cross-thread message queues.

5. **Set up main-loop `uv_async_t`** (unref'd) for receiving messages from the
   inspector thread.

6. **Start inspector thread**: Calls `runHermesNode()` with a config that runs
   the inspector JS script. The script:
   - Creates `http.Server` + `ws.WebSocketServer`
   - Binds to `host:port`
   - Begins accepting connections

7. **Print debugger URL** to stderr.

8. **If `--inspect-brk`**: Set `shouldPauseOnScriptLoad(true)` on the debugger.
   Optionally block until a client connects (pump main event loop with
   `uv_run(UV_RUN_ONCE)` until `inspector->hasActiveSession()`).

9. **Run bootstrap** and user script normally. The inspector runs concurrently
   on its own thread.

### `--inspect-brk` Behavior

The inspector runtime boots independently on the IO thread. It can be fully
listening before the main runtime executes any user code. The sequence:

1. Inspector thread starts, `ws.WebSocketServer` binds to port
2. Inspector signals main thread that it is ready (via `uv_async_t`)
3. Main thread enables `shouldPauseOnScriptLoad`
4. Main thread loads user script; Hermes pauses at first statement
5. `AsyncDebuggerAPI` blocks the runtime thread in `didPause()`
6. Main loop continues processing `uv_async_t` callbacks (CDP commands) while
   paused, because `didPause()` runs interrupt callbacks
7. DevTools sends `Debugger.resume` -> CDPAgent -> domain agent -> unpause

This works because `CDPAgent::handleCommand()` delivers work via
`triggerInterrupt_TS()` (when JS is running/paused) or via the integrator
callback (when JS is idle). Both paths function correctly during a debugger
pause.

---

## Shutdown Sequence

1. User script completes (or `process.exit()` called)
2. Main event loop exits
3. `process.emit('exit')`
4. **Stop inspector**: Signal the inspector runtime to shut down (close the WS
   server, stop the event loop). Wait for the IO thread to join.
5. **Destroy CDPAgent** (thread-safe, can be done from main thread)
6. **Close main-loop inspector `uv_async_t`**
7. **Destroy CDPDebugAPI** (must happen before HermesRuntime is destroyed)
8. Continue normal cleanup: close stdio, close timers, close event loop,
   destroy napi_env, destroy HermesRuntime

---

## Vendored `ws` Integration

The `ws` package is vendored into the project following the established
convention:

```
external/ws/
  README.md              Version, license, upstream URL
  ws-hermes.patch        The three-patch diff (documented, reproducible)
  ws/                    Upstream source with patches applied
    lib/
      buffer-util.js
      constants.js
      event-target.js
      extension.js
      limiter.js
      permessage-deflate.js    (patched: lazy zlib)
      receiver.js
      sender.js                (patched: randomFillSync shim)
      stream.js
      subprotocol.js
      validation.js
      websocket-server.js
      websocket.js             (patched: lazy https/tls, randomBytes shim)
    index.js
    package.json
```

The vendored `ws` is made available as `require('ws')` via the module loader
(added to the built-in module list in `internal/bootstrap/realm.js`). This also
makes `ws` available to user code as a built-in module -- a useful side effect,
since WebSocket is one of the most commonly needed Node.js packages.

---

## New Files

```
lib/inspector/
  CMakeLists.txt                    Build integration
  inspector_bridge.cpp              Native binding for cross-thread messaging
  inspector_bridge.h                Header

libjs/inspector/
  inspector-server.js               JS WebSocket/HTTP server script

external/ws/
  README.md                         Upstream info
  ws-hermes.patch                   Patch file
  ws/                               Vendored ws 8.19.0 (patched)

external/miniz/                     Zip reader (if option 1 for frontend serving)
  README.md
  miniz/
    miniz.h / miniz.c
  CMakeLists.txt
```

### Modified Files

```
CMakeLists.txt                                HERMES_ENABLE_DEBUGGER, add_subdirectory
lib/runtime/hermes_node_runtime.cpp           makeHermesRuntime(), CDP wiring, inspector lifecycle
tools/hermes-node/hermes-node.cpp             --inspect flag parsing
libjs/shims/internal/bootstrap/realm.js       Add 'ws' to built-in module list
lib/embedded-modules/embedded-modules.txt     Add ws modules + inspector script
```

---

## Future Work

- **`crypto.randomBytes` / `randomFillSync`**: Proper implementation using
  `uv_random()` (libuv's cross-platform CSPRNG wrapper). Eliminates the
  `Math.random` shims in vendored `ws`.

- **`--inspect-wait`**: Like `--inspect-brk` but does not pause at first line;
  just waits for a debugger to connect before running.

- **SIGUSR1**: Dynamically enable inspection on a running process (Unix only).

- **`zlib` module**: Would enable `permessage-deflate` WebSocket compression
  and unblock other npm packages.

- **`https` / `tls` modules**: Would enable secure WebSocket (`wss://`)
  connections and unblock HTTPS.

- **Multiple clients**: `CDPDebugAPI` supports multiple concurrent `CDPAgent`
  instances. The inspector JS script could accept multiple WebSocket connections.

- **Source maps**: Integration with Hermes's source map support for debugging
  compiled/bundled code.
