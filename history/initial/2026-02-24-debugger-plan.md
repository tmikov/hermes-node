# Debugger Implementation Plan: CDP + CDN Redirect

Implements `--inspect` / `--inspect-brk` support for hermes-node. A second
hermes-node runtime on a dedicated IO thread hosts a JS WebSocket server
(vendored `ws` package) that bridges Chrome DevTools to Hermes's CDP stack.
DevTools frontend served via CDN redirect.

Design doc: `doc/DEBUGGER.md`

---

## Step 1: Enable HERMES_ENABLE_DEBUGGER in CMake

**Dependencies:** none

Add `set(HERMES_ENABLE_DEBUGGER ON ...)` to the top-level `CMakeLists.txt`
before `add_subdirectory(hermes)`. This compiles the CDP sources (CDPAgent,
CDPDebugAPI, AsyncDebuggerAPI, domain agents, message types) into `hermesvm_a`
via `hermesapi_obj`.

**Files:**
- `CMakeLists.txt` (line 9, before `add_subdirectory(hermes)`)

**Completion criteria:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
# All existing tests pass. Build log shows CDP .cpp files compiling.
```

---

## Step 2: Switch runtime creation to makeHermesRuntime()

**Dependencies:** Step 1

Replace `hermes::vm::Runtime::create()` with `facebook::hermes::makeHermesRuntime()`
in `hermes_node_runtime.cpp`. Extract `vm::Runtime*` via `getVMRuntimeUnsafe()`
for NAPI env creation and the two callbacks that need it (drainJobs,
triggerTimeoutAsyncBreak).

**Changes in `hermes_node_runtime.cpp`:**
- Add includes: `<hermes/hermes.h>`, `<jsi/hermes-interfaces.h>`
- Replace lines 307-312:
  ```cpp
  auto hermesRT = facebook::hermes::makeHermesRuntime(rtConfig);
  auto *vmRuntime = static_cast<hermes::vm::Runtime *>(
      facebook::jsi::castInterface<facebook::hermes::IHermes>(hermesRT.get())
          ->getVMRuntimeUnsafe());
  ```
- Replace `hermes_napi_create_env(runtime.get(), ...)` with
  `hermes_napi_create_env(vmRuntime, ...)`
- Replace all `runtime.get()` in RuntimeState setup with `vmRuntime`
- Replace `runtime->drainJobs()` (line ~741) with `vmRuntime->drainJobs()`
- Replace `runtime.reset()` (line ~852) with `hermesRT.reset()`
- Update `TickDrainData` to use `vmRuntime`

**Changes in `lib/runtime/CMakeLists.txt`:**
- Add `${PROJECT_SOURCE_DIR}/hermes/API` to PRIVATE include dirs (for
  `<hermes/hermes.h>` and `<jsi/hermes-interfaces.h>`)

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`
- `lib/runtime/CMakeLists.txt`

**Completion criteria:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
# All existing tests pass. No functional change.
```

---

## Step 3: Add --inspect / --inspect-brk flag parsing

**Dependencies:** none (can be done in parallel with Steps 1-2)

Add `--inspect[=host:port]` and `--inspect-brk[=host:port]` to both the CLI
and the config struct. Default host:port is `127.0.0.1:9229`.

**Changes in `hermes_node_runtime.h` (HermesNodeConfig):**
```cpp
bool inspect = false;
bool inspectBrk = false;
std::string inspectHost = "127.0.0.1";
int inspectPort = 9229;
```

**Changes in `hermes-node.cpp`:**
- Parse `--inspect`, `--inspect=PORT`, `--inspect=HOST:PORT`
- Parse `--inspect-brk`, `--inspect-brk=PORT`, `--inspect-brk=HOST:PORT`
- `--inspect-brk` implies `inspect = true`
- Update `printUsage()`

**Files:**
- `include/hermes/node-compat/runtime/hermes_node_runtime.h`
- `tools/hermes-node/hermes-node.cpp`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "console.log('PASS')"
# Prints PASS, exits 0. Flag is parsed but has no effect yet.
cmake-build-asan/bin/hermes-node --inspect=0.0.0.0:9230 -e ""
# Exits 0.
cmake-build-asan/bin/hermes-node --inspect-brk -e ""
# Exits 0 (--inspect-brk has no effect yet).
```

---

## Step 4: Create CDPDebugAPI and CDPAgent with placeholder callbacks

**Dependencies:** Step 2

When `config.inspect` is true, create `CDPDebugAPI` and `CDPAgent` in
`runHermesNode()`. Use placeholder callbacks (log to stderr or no-op). Add
proper cleanup in the shutdown sequence.

**Changes in `hermes_node_runtime.cpp`:**
- Add includes (guarded by `#ifdef HERMES_ENABLE_DEBUGGER`):
  `<hermes/cdp/CDPAgent.h>`, `<hermes/cdp/CDPDebugAPI.h>`,
  `<hermes/RuntimeTaskRunner.h>`
- After creating `hermesRT` and `env`, conditionally create:
  ```cpp
  #ifdef HERMES_ENABLE_DEBUGGER
  std::unique_ptr<facebook::hermes::cdp::CDPDebugAPI> cdpDebugAPI;
  std::unique_ptr<facebook::hermes::cdp::CDPAgent> cdpAgent;
  if (config.inspect) {
    cdpDebugAPI = facebook::hermes::cdp::CDPDebugAPI::create(*hermesRT);
    cdpAgent = facebook::hermes::cdp::CDPAgent::create(
        1, *cdpDebugAPI,
        [](facebook::hermes::debugger::RuntimeTask) { /* placeholder */ },
        [](const std::string &) { /* placeholder */ });
    cdpAgent->enableRuntimeDomain();
  }
  #endif
  ```
- In shutdown, before `hermes_napi_destroy_env`:
  ```cpp
  #ifdef HERMES_ENABLE_DEBUGGER
  cdpAgent.reset();
  cdpDebugAPI.reset();
  #endif
  ```
- Destruction order: cdpAgent -> cdpDebugAPI -> napi_env -> hermesRT

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "console.log('PASS')"
# Prints PASS, exits 0 cleanly (no ASAN errors, no leaks).
cmake --build cmake-build-asan --target check-hermes-node
# All tests pass.
```

---

## Step 5: Add RuntimeTask queue and uv_async_t for CDP processing

**Dependencies:** Step 4

Replace the placeholder `enqueueRuntimeTaskCallback` with a real queue. Add a
`uv_async_t` to the main event loop that drains both inbound CDP commands and
RuntimeTasks. This is the main-thread side of the cross-thread bridge.

**Changes in `hermes_node_runtime.cpp`:**
- Add `<mutex>`, `<queue>`, `<functional>` includes
- Create a struct for the inspector state on the main thread:
  ```cpp
  struct InspectorState {
    std::mutex mutex;
    std::queue<std::string> inboundCommands;       // CDP JSON from inspector
    std::queue<facebook::hermes::debugger::RuntimeTask> runtimeTasks;
    uv_async_t asyncHandle{};
    facebook::hermes::cdp::CDPAgent *agent = nullptr;
    facebook::hermes::HermesRuntime *hermesRT = nullptr;
  };
  ```
- Initialize `uv_async_init()` on the main event loop, unref it
- The async callback drains both queues:
  - `agent->handleCommand(json)` for inbound commands
  - `task(*hermesRT)` for runtime tasks
- Wire `enqueueRuntimeTaskCallback` to push + `uv_async_send`
- Wire a `pushCommand(string)` function (called from inspector thread later)
- Close the `uv_async_t` during shutdown (before cdpAgent.reset())

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "console.log('PASS')"
# Prints PASS, exits 0 cleanly. The uv_async_t is created, unref'd
# (doesn't block exit), and closed cleanly.
cmake --build cmake-build-asan --target check-hermes-node
```

---

## Step 6: Vendor ws package

**Dependencies:** none (can be done in parallel with Steps 1-5)

Vendor `ws` 8.19.0 into `external/ws/` following the project convention.
Apply the three hermes-compat patches. Make it loadable via `require('ws')`.

**New files:**
```
external/ws/
  README.md                    # version, license, upstream URL, patch description
  ws-hermes.patch              # the three-edit diff
  ws/                          # upstream 8.19.0 with patches applied
    package.json
    index.js
    wrapper.mjs
    lib/
      buffer-util.js
      constants.js
      event-target.js
      extension.js
      limiter.js
      permessage-deflate.js    # patched: lazy zlib
      receiver.js
      sender.js                # patched: randomFillSync shim
      stream.js
      subprotocol.js
      validation.js
      websocket-server.js
      websocket.js             # patched: lazy https/tls, randomBytes shim
```

**Module loader integration:**
- Add `ws` to the built-in module list in
  `libjs/shims/internal/bootstrap/realm.js`
- Add a shim at `libjs/shims/ws.js` that does:
  ```js
  module.exports = require('<path-to-vendored-ws>');
  ```
  Or, simpler: add a loader hook in `libjs/loader.js` that resolves `ws` to the
  vendored path. The exact mechanism depends on the vendored-npm approach from
  TODO.md item 3 -- if that's not done yet, a simple shim works.

**Alternative (if TODO item 3 is done first):** Use whatever vendored-npm
mechanism is implemented.

**Files:**
- `external/ws/` (new directory tree)
- `libjs/shims/internal/bootstrap/realm.js` (add 'ws' to module list)
- Module resolution hook or shim for `ws`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node -e "
  const { WebSocketServer } = require('ws');
  console.log(typeof WebSocketServer);
  console.log('PASS');
"
# Output:
# function
# PASS
```

---

## Step 7: Add inspectorBridgeContext to config and RuntimeState

**Dependencies:** Step 5

Add an opaque `void *inspectorBridgeContext` field to `HermesNodeConfig` and
`RuntimeState`. This carries the cross-thread messaging state into the inspector
runtime. Null for the user runtime (normal operation).

**Changes:**
- `hermes_node_runtime.h`: add `void *inspectorBridgeContext = nullptr;` to
  `HermesNodeConfig`
- `runtime_state.h`: add `void *inspectorBridgeContext = nullptr;` to
  `RuntimeState`
- `hermes_node_runtime.cpp`: copy
  `config.inspectorBridgeContext` to `runtimeState->inspectorBridgeContext`
  (right after allocating RuntimeState)

**Files:**
- `include/hermes/node-compat/runtime/hermes_node_runtime.h`
- `include/hermes/node-compat/runtime/runtime_state.h`
- `lib/runtime/hermes_node_runtime.cpp`

**Completion criteria:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
# All tests pass. No functional change.
```

---

## Step 8: Create inspector_bridge native binding

**Dependencies:** Step 7

A native binding (`inspector_bridge`) that the inspector JS script uses for
cross-thread CDP messaging. When `inspectorBridgeContext` is null (normal
runtime), the binding exports an empty object. When set (inspector runtime),
it exposes:

- `sendToMain(jsonString)`: push CDP command to main thread's inbound queue,
  signal via `uv_async_send`
- `setMessageCallback(fn)`: register a JS function to receive CDP
  responses/events from main thread
- `getConfig()`: returns `{ host, port, scriptName, sessionId }`
- `notifyReady()`: signal main thread that the server is listening

**The `InspectorBridgeContext` struct** (new header
`include/hermes/node-compat/inspector/inspector_bridge.h`):
```cpp
struct InspectorBridgeContext {
  // Main -> Inspector (outbound CDP messages)
  std::mutex outboundMutex;
  std::queue<std::string> outboundQueue;
  uv_async_t *inspectorAsync = nullptr;  // in inspector's loop

  // Inspector -> Main (inbound CDP commands)
  std::mutex *inboundMutex = nullptr;    // points to InspectorState::mutex
  std::queue<std::string> *inboundQueue = nullptr;
  uv_async_t *mainAsync = nullptr;       // in main loop

  // Config
  std::string host;
  int port = 9229;
  std::string scriptName;
  std::string sessionId;

  // Startup synchronization
  std::mutex readyMutex;
  std::condition_variable readyCv;
  bool ready = false;
  int actualPort = 0;                    // port after bind (if 0 was requested)

  // JS callback for delivering outbound messages to inspector JS
  napi_ref messageCallbackRef = nullptr;
  napi_env inspectorEnv = nullptr;       // set during binding init
};
```

**Files:**
- `include/hermes/node-compat/inspector/inspector_bridge.h` (new)
- `lib/inspector/inspector_bridge.cpp` (new)
- `lib/inspector/CMakeLists.txt` (new)
- `CMakeLists.txt` (add `add_subdirectory(lib/inspector)`)
- `lib/runtime/hermes_node_runtime.cpp` (register binding)

**Completion criteria:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
# Builds. All existing tests pass.
# The binding is registered but does nothing for normal runtimes.
```

---

## Step 9: Create inspector JS server script

**Dependencies:** Steps 6, 8

Write the JavaScript script that runs on the inspector runtime. It creates an
HTTP server, a WebSocket server (using `ws`), and wires them to the
`inspector_bridge` binding.

**New file: `libjs/inspector-server.js`**

The script:
1. `require('ws')` for WebSocketServer
2. `require('http')` for HTTP server
3. `internalBinding('inspector_bridge')` for cross-thread messaging
4. Creates `http.createServer()` handling:
   - `GET /json` and `/json/list`: target discovery JSON
   - `GET /json/version`: version info
   - `GET /devtools/inspector.html*`: CDN redirect HTML
5. Creates `WebSocketServer({ server, path: '/' + sessionId })`
6. On WS connect: forward messages via `sendToMain()`
7. On outbound from main: forward via `ws.send()`
8. Calls `notifyReady()` after `server.listen()` completes
9. Only accepts one session at a time (single-client)

**Add to embedded modules:**
- Add `inspector-server` to `lib/embedded-modules/embedded-modules.txt`

**Files:**
- `libjs/inspector-server.js` (new)
- `lib/embedded-modules/embedded-modules.txt` (add entry)

**Completion criteria:**
- File exists and is syntactically valid
- Embedded module builds into bytecode
```bash
cmake --build cmake-build-asan --target hermesNodeEmbeddedModules
# No compile errors.
```

---

## Step 10: Start inspector runtime on a dedicated thread

**Dependencies:** Steps 5, 8, 9

Add the code in `runHermesNode()` that, when `config.inspect` is true:
1. Generates a session UUID (simple counter or random hex)
2. Allocates and populates `InspectorBridgeContext`
3. Starts a `std::thread` (or `uv_thread_t`) that calls `runHermesNode()` with
   an inspector config:
   ```cpp
   HermesNodeConfig inspectorConfig;
   inspectorConfig.evalCode = "require('inspector-server');";
   inspectorConfig.inspectorBridgeContext = bridgeCtx;
   inspectorConfig.argv = {"hermes-node-inspector"};
   ```
4. Waits for `bridgeCtx->readyCv` (inspector signals it's listening)
5. Prints to stderr:
   ```
   Debugger listening on ws://HOST:PORT/UUID
   ```

**Shutdown:**
After the main event loop exits and before cdpAgent cleanup:
1. Signal inspector runtime to stop (e.g., `uv_async_send` a shutdown flag,
   or close the bridge async handle so the inspector's event loop exits)
2. Join the inspector thread

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "console.log('PASS')"
# Output (stderr): Debugger listening on ws://127.0.0.1:9229/<uuid>
# Output (stdout): PASS
# Exits cleanly (no ASAN errors).
```

---

## Step 11: Wire end-to-end CDP message flow

**Dependencies:** Step 10

Connect all the pieces so CDP messages flow:
DevTools -> WebSocket -> inspector JS -> inspector_bridge.sendToMain() ->
main thread inbound queue -> uv_async -> cdpAgent->handleCommand() ->
outbound callback -> bridge outbound queue -> inspector uv_async ->
inspector_bridge.messageCallback -> inspector JS -> ws.send() -> DevTools

**Changes:**
- Wire `CDPAgent`'s outbound callback to push to
  `bridgeCtx->outboundQueue` + `uv_async_send(bridgeCtx->inspectorAsync)`
- In inspector runtime's event loop, the `inspectorAsync` callback drains
  `outboundQueue` and calls the JS `messageCallback`
- Wire `InspectorState`'s `pushCommand` (from Step 5) to be called by
  `inspector_bridge::sendToMain()`

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`
- `lib/inspector/inspector_bridge.cpp`

**Completion criteria:**
```bash
# Start hermes-node with --inspect, connect with a WebSocket client
# (e.g., wscat or a simple Node.js script), send Runtime.enable, verify
# a response comes back.
cmake-build-asan/bin/hermes-node --inspect -e "setTimeout(() => {}, 30000)" &
sleep 1
# From another terminal:
echo '{"id":1,"method":"Runtime.enable"}' | wscat -c ws://127.0.0.1:9229/<uuid>
# Should receive a response with "id":1 and a Runtime.executionContextCreated
# notification.
kill %1
```

---

## Step 12: Add /json discovery endpoints

**Dependencies:** Step 10

The inspector JS script's HTTP handler serves the standard Node.js discovery
endpoints that `chrome://inspect` and other tools use.

**In `libjs/inspector-server.js`, the HTTP handler:**
- `GET /json` or `/json/list`:
  ```json
  [{
    "description": "hermes-node instance",
    "id": "<uuid>",
    "title": "<scriptName>",
    "type": "node",
    "url": "file://<scriptPath>",
    "webSocketDebuggerUrl": "ws://HOST:PORT/<uuid>",
    "devtoolsFrontendUrl": "http://HOST:PORT/devtools/inspector.html?ws=HOST:PORT/<uuid>"
  }]
  ```
- `GET /json/version`:
  ```json
  {"Browser": "hermes-node/0.1.0", "Protocol-Version": "1.1"}
  ```

**Files:**
- `libjs/inspector-server.js`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "setTimeout(() => {}, 30000)" &
sleep 1
curl -s http://127.0.0.1:9229/json/list | python3 -m json.tool
# Valid JSON array with webSocketDebuggerUrl field.
curl -s http://127.0.0.1:9229/json/version | python3 -m json.tool
# Valid JSON with Browser and Protocol-Version.
kill %1
```

---

## Step 13: Add DevTools CDN redirect

**Dependencies:** Step 12

Serve a small HTML page at `/devtools/inspector.html` that redirects to the
Google-hosted DevTools frontend CDN, preserving the `ws=` query parameter.

**In `libjs/inspector-server.js`:**
- `GET /devtools/inspector.html*`:
  ```html
  <!DOCTYPE html><html><head><script>
  var ws = location.search.match(/[?&]ws=([^&]+)/);
  location = 'https://chrome-devtools-frontend.appspot.com/serve_file/' +
    '@60127beb442528082b3f6eff7392267e145262c3/js_app.html' +
    '?ws=' + (ws ? decodeURIComponent(ws[1]) : location.host);
  </script></head><body>Opening DevTools...</body></html>
  ```

**Files:**
- `libjs/inspector-server.js`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "setTimeout(() => {}, 30000)" &
sleep 1
curl -s http://127.0.0.1:9229/devtools/inspector.html?ws=127.0.0.1:9229/test
# HTML response containing chrome-devtools-frontend.appspot.com redirect.
kill %1
```

---

## Step 14: Add --inspect-brk (pause at first line)

**Dependencies:** Step 11

When `--inspect-brk` is specified, the main runtime pauses before executing
the first statement of user code. The debugger must be connected and send
`Debugger.resume` to continue.

**Implementation:**
1. After the inspector is listening (Step 10's readyCv wait), if `inspectBrk`:
   - Call `cdpAgent->enableDebuggerDomain()` (enables break-on-first-statement)
   - Wait for a WebSocket client to connect (bridge signals via another
     condvar or a flag + `uv_async`)
   - OR: just enable the debugger domain and let the runtime pause when it hits
     the first user-code statement. The `AsyncDebuggerAPI` will block the main
     thread in `didPause()`, processing interrupt callbacks (which include our
     CDP commands via `uv_async_t`).
2. The key insight: when paused at a breakpoint, `AsyncDebuggerAPI::didPause()`
   runs a blocking loop that processes interrupts. `CDPAgent` uses
   `triggerInterrupt_TS()` to deliver work during pause. Our `uv_async_t`
   callback is NOT in this loop. So we need the `enqueueRuntimeTaskCallback`
   to also use `triggerInterrupt_TS()` (via `RuntimeTaskRunner`) in addition to
   the `uv_async_t`.

**Important subtlety:** During a debugger pause, the main event loop is NOT
running (`uv_run` is blocked). CDP commands arrive from the inspector thread
and must be processed. The `RuntimeTaskRunner` pattern from hcdp handles this:
it enqueues tasks both via the `AsyncDebuggerAPI` interrupt mechanism AND via
the integrator callback. We need to use `RuntimeTaskRunner` or replicate its
dual-path approach.

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`

**Completion criteria:**
```bash
# test-inspect-brk.js: console.log('SHOULD NOT SEE THIS YET');
cmake-build-asan/bin/hermes-node --inspect-brk test-inspect-brk.js &
sleep 2
# "Debugger listening on ws://..." printed to stderr
# "SHOULD NOT SEE THIS YET" has NOT been printed yet (runtime is paused)
# Connect and send Debugger.resume:
node -e "
  const ws = new (require('ws'))('ws://127.0.0.1:9229/<uuid>');
  ws.on('open', () => {
    ws.send(JSON.stringify({id:1, method:'Debugger.resume'}));
    setTimeout(() => { ws.close(); process.exit(); }, 1000);
  });
"
# NOW "SHOULD NOT SEE THIS YET" should print
kill %1
```

---

## Step 15: Add stderr diagnostic messages

**Dependencies:** Step 10

Print Node.js-compatible diagnostic messages to stderr.

**Messages:**
- On inspector start:
  ```
  Debugger listening on ws://HOST:PORT/UUID
  For help, see: https://nodejs.org/en/docs/inspector
  ```
- Optionally, also print the DevTools URL:
  ```
  Open DevTools: http://HOST:PORT/devtools/inspector.html?ws=HOST:PORT/UUID
  ```

**Files:**
- `lib/runtime/hermes_node_runtime.cpp`

**Completion criteria:**
```bash
cmake-build-asan/bin/hermes-node --inspect -e "" 2>&1 | head -3
# Debugger listening on ws://127.0.0.1:9229/...
# For help, see: https://nodejs.org/en/docs/inspector
```

---

## Step 16: End-to-end integration test

**Dependencies:** Steps 11-15

Add a Lit test that validates the full debugging flow.

**New file: `test/test-inspect.js`**
```
// RUN: %hermes-node --inspect=0 %s 2>%t.stderr
// Verify exit code 0 and that the debug server was announced.
// RUN: grep -q "Debugger listening" %t.stderr
```

The test uses `--inspect=0` (OS-assigned port) to avoid port conflicts in
parallel test runs. The script exits immediately; we just verify the inspector
started and the process exited cleanly.

**Files:**
- `test/test-inspect.js` (new)

**Completion criteria:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
# New test passes along with all existing tests.
```

---

## Dependency Graph

```
Step 1 ──> Step 2 ──> Step 4 ──> Step 5 ──> Step 10 ──> Step 11 ──> Step 14
                                    |           |           |
Step 3 (parallel) ─────────────────>|           |           |
                                                |           |
Step 6 (parallel) ────> Step 9 ────>|           |           |
                          |                     |           |
Step 7 ──> Step 8 ───────>─────────>|           |           |
                                                |           |
                                    Step 12 <───+           |
                                       |                    |
                                    Step 13                 |
                                                            |
                                    Step 15 <──────────────>|
                                                            |
                                    Step 16 <───────────────+
```

Parallelizable: Steps 1+3+6 can all start simultaneously. Steps 7-8 depend
only on Step 5 for the InspectorState design. Step 9 depends on 6 and 8.
Steps 12-13 only need the HTTP handler in the JS script (from Step 9).
