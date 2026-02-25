# Implementation Progress

Tracks progress on `history/initial/2026-02-24-debugger-plan.md`.

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
| Step 1 | Enable HERMES_ENABLE_DEBUGGER in CMake | — | done |  |
| Step 2 | Switch runtime creation to makeHermesRuntime() | 1 | done |  |
| Step 3 | Add --inspect / --inspect-brk flag parsing | — | done |  |
| Step 4 | Create CDPDebugAPI and CDPAgent with placeholder callbacks | 2 | done |  |
| Step 5 | Add RuntimeTask queue and uv_async_t for CDP processing | 4 | done |  |
| Step 6 | Vendor ws package | — | done | Vendored ws 8.19.0 in prior commit |
| Step 7 | Add inspectorBridgeContext to config and RuntimeState | 5 | done |  |
| Step 8 | Create inspector_bridge native binding | 7 | done |  |
| Step 9 | Create inspector JS server script | 6, 8 | done |  |
| Step 10 | Start inspector runtime on a dedicated thread | 5, 8, 9 | done |  |
| Step 11 | Wire end-to-end CDP message flow | 10 | done |  |
| Step 12 | Add /json discovery endpoints | 10 | done | Already implemented in Step 9 |
| Step 13 | Add DevTools CDN redirect | 12 | done | Already implemented in Step 9 |
| Step 14 | Add --inspect-brk (pause at first line) | 11 |  |  |
| Step 15 | Add stderr diagnostic messages | 10 |  |  |
| Step 16 | End-to-end integration test | 11, 12, 13, 14, 15 |  |  |

## Context Notes

### Step 1: Enable HERMES_ENABLE_DEBUGGER in CMake
- **Files**: modified `CMakeLists.txt`.
- **What was done**: Added `set(HERMES_ENABLE_DEBUGGER ON CACHE BOOL "Enable Hermes debugger/CDP" FORCE)` before `add_subdirectory(hermes)`. This causes Hermes to compile with debugger support, including CDP sources (CDPAgent, CDPDebugAPI, AsyncDebuggerAPI, RuntimeTaskRunner, domain agents) into `hermesapi_obj`.
- **Notes for next step**: `HERMES_ENABLE_DEBUGGER` is now defined for all Hermes targets but NOT for our targets. Our code will need `#ifdef HERMES_ENABLE_DEBUGGER` guards when including CDP headers (Step 4). The `add_definitions(-DHERMES_ENABLE_DEBUGGER)` in Hermes is subdirectory-scoped.

### Step 2: Switch runtime creation to makeHermesRuntime()
- **Files**: modified `lib/runtime/hermes_node_runtime.cpp`.
- **What was done**: Replaced `hermes::vm::Runtime::create(rtConfig)` with `facebook::hermes::makeHermesRuntime(rtConfig)`. Extract `vm::Runtime*` via `hermesRT->getVMRuntimeUnsafe()` for NAPI env creation and the two callbacks that need it (drainJobs, triggerTimeoutAsyncBreak). The `hermesRT` unique_ptr (HermesRuntime) is kept alive for the full function scope; `vmRuntime` raw pointer is used everywhere the old `runtime.get()` was used.
- **Decisions**: Used direct `hermesRT->getVMRuntimeUnsafe()` call instead of `castInterface<IHermes>` pattern since `HermesRuntime` directly inherits from `IHermes`.
- **Notes for next step**: `hermesRT` (`unique_ptr<HermesRuntime>`) is the JSI-level runtime needed by `CDPDebugAPI::create(*hermesRT)` in Step 4. `vmRuntime` (`vm::Runtime*`) is the low-level VM pointer used by NAPI and event loop. CMakeLists.txt already had `hermes/API` include path -- no change needed there.

### Step 3: Add --inspect / --inspect-brk flag parsing
- **Files**: modified `include/hermes/node-compat/runtime/hermes_node_runtime.h`, modified `tools/hermes-node/hermes-node.cpp`.
- **What was done**: Added `inspect`, `inspectBrk`, `inspectHost`, `inspectPort` fields to `HermesNodeConfig`. Added `parseInspectHostPort()` helper and CLI parsing for `--inspect[=[host:]port]` and `--inspect-brk[=[host:]port]`. `--inspect-brk` implies `inspect = true`. Updated `printUsage()`.
- **Decisions**: Used `strrchr` for last colon to split host:port (handles IPv4 addresses). Port 0 is valid (OS-assigned). Port range validated 0-65535.
- **Notes for next step**: Flags are parsed and stored in config but have no runtime effect yet. Step 4 will read `config.inspect` to conditionally create CDPDebugAPI/CDPAgent.

### Step 4: Create CDPDebugAPI and CDPAgent with placeholder callbacks
- **Files**: modified `lib/runtime/hermes_node_runtime.cpp`, modified `lib/runtime/CMakeLists.txt`.
- **What was done**: When `config.inspect` is true, create `CDPDebugAPI` (from `HermesRuntime&`) and `CDPAgent` (with no-op placeholder callbacks for `enqueueRuntimeTask` and `messageCallback`). Call `enableRuntimeDomain()` on the agent. In shutdown, destroy `cdpAgent` then `cdpDebugAPI` before `hermes_napi_destroy_env`. All code guarded by `#ifdef HERMES_ENABLE_DEBUGGER`. Added `target_compile_definitions(hermesNodeRuntime PRIVATE HERMES_ENABLE_DEBUGGER)` to CMakeLists.txt since the Hermes `add_definitions` is subdirectory-scoped.
- **Decisions**: Placed CDP creation after RuntimeState setup but before handle scope open. Destruction order: cdpAgent -> cdpDebugAPI -> napi_env -> hermesRT (reverse creation order).
- **Notes for next step**: `cdpAgent` and `cdpDebugAPI` are local variables in `runHermesNode()`. Step 5 will replace the placeholder `enqueueRuntimeTask` callback with a real queue + `uv_async_t`. The `hermesRT` pointer is available for `RuntimeTask` execution (`task(*hermesRT)`).

### Step 5: Add RuntimeTask queue and uv_async_t for CDP processing
- **Files**: modified `lib/runtime/hermes_node_runtime.cpp`.
- **What was done**: Added `InspectorState` struct (mutex-protected queues for inbound CDP commands and RuntimeTasks, `uv_async_t`, pointers to CDPAgent and HermesRuntime, `std::atomic<bool> asyncActive` flag). Added `onInspectorAsync` callback that drains both queues. Added `pushInspectorCommand` helper (unused now, for Step 8+). Replaced placeholder `enqueueRuntimeTask` callback with real push-to-queue + `uv_async_send`. Added shutdown: close async handle alongside check/prepare handles, then drain remaining RuntimeTasks after `cdpAgent.reset()` (CDPAgent destructor may enqueue tasks).
- **Decisions**: Used `std::atomic<bool> asyncActive` to guard `uv_async_send` calls after the handle is closed (CDPAgent destructor calls `enqueueRuntimeTask` during destruction). Queues swapped under lock then drained outside lock to minimize hold time. `pushInspectorCommand` marked `[[maybe_unused]]` to suppress warning until Step 8.
- **Notes for next step**: `InspectorState` is stack-allocated in `runHermesNode()`. Step 7 adds `InspectorBridgeContext` to config/RuntimeState. Step 8 creates the inspector_bridge binding that calls `pushInspectorCommand` (or accesses queues directly via `InspectorBridgeContext` pointers). The `messageCallback` (outbound CDP) is still a placeholder -- wired in Step 11.

### Step 7: Add inspectorBridgeContext to config and RuntimeState
- **Files**: modified `hermes_node_runtime.h`, `runtime_state.h`, `hermes_node_runtime.cpp`.
- **What was done**: Added `void *inspectorBridgeContext = nullptr` to `HermesNodeConfig` (opaque pointer for cross-thread CDP messaging, null for user runtime, set for inspector runtime) and to `RuntimeState` (so bindings can access it via `getRuntimeState(env)`). Copied config value to runtimeState in `runHermesNode()` alongside the other state initialization.
- **Notes for next step**: Step 8 will define the actual `InspectorBridgeContext` struct and create the `inspector_bridge` native binding that retrieves this pointer from `RuntimeState` to communicate with the main thread.

### Step 8: Create inspector_bridge native binding
- **Files**: created `include/hermes/node-compat/inspector/inspector_bridge.h`, created `lib/inspector/inspector_bridge.cpp`, created `lib/inspector/CMakeLists.txt`, modified `CMakeLists.txt`, modified `lib/runtime/CMakeLists.txt`, modified `lib/runtime/hermes_node_runtime.cpp`.
- **What was done**: Defined `InspectorBridgeContext` struct with fields for cross-thread CDP messaging (inbound/outbound queues, mutexes, async handles, config, startup synchronization, JS callback ref). Created `inspector_bridge` native binding with 4 functions: `sendToMain(json)` pushes CDP commands to main thread's inbound queue + signals via `uv_async_send`; `setMessageCallback(fn)` stores persistent ref to JS callback for outbound CDP messages; `getConfig()` returns `{host, port, scriptName, sessionId}`; `notifyReady(actualPort)` signals main thread via condition variable that the inspector server is listening. When `inspectorBridgeContext` is null (normal runtime), binding returns empty object. Created `hermesNodeInspector` static library, linked from `hermesNodeRuntime`. Registered as `"inspector_bridge"` binding.
- **Decisions**: The bridge context holds raw pointers to the main thread's `InspectorState` members (mutex, queue, async handle, asyncActive flag) rather than a pointer to the whole `InspectorState`, keeping the inspector side decoupled. The `mainAsyncActive` pointer (to `std::atomic<bool>`) guards `uv_async_send` calls after shutdown.
- **Notes for next step**: Step 9 creates the inspector JS server script that calls `internalBinding('inspector_bridge')` to use these functions. Step 10 allocates `InspectorBridgeContext`, populates its pointers from `InspectorState`, and passes it via `config.inspectorBridgeContext` when starting the inspector runtime thread. Step 11 wires the outbound `uv_async_t` on the inspector loop to drain `outboundQueue` and invoke the JS `messageCallback`.

### Step 9: Create inspector JS server script
- **Files**: created `libjs/shims/inspector-server.js`, modified `lib/embedded-modules/embedded-modules.txt`.
- **What was done**: Created the inspector server script that runs on the inspector runtime's IO thread. The script: (1) gets bridge functions via `internalBinding('inspector_bridge')`, (2) creates an HTTP server handling `/json`, `/json/list`, `/json/version`, and `/devtools/*` (CDN redirect) endpoints, (3) creates a `WebSocketServer` (from vendored `ws`) on path `/<sessionId>`, (4) forwards WS messages to main thread via `bridge.sendToMain()`, (5) receives outbound CDP messages via `bridge.setMessageCallback()` and sends them over WS, (6) enforces single-client sessions, (7) calls `bridge.notifyReady(port)` after listen completes. Added `inspector-server` to `embedded-modules.txt`.
- **Decisions**: Placed file at `libjs/shims/inspector-server.js` (not `libjs/inspector-server.js`) because the embedded-modules CMake resolution for CJS modules checks `libjs/shims/` first, then `libjs-node/`. Bootstrap modules (prefixed with `@bootstrap`) use `libjs/` directly. Included all HTTP endpoints (/json, /json/version, /devtools CDN redirect) in the initial script since Steps 12 and 13 will refine them. Used `config.actualPort` (set after listen callback) for /json response URLs so port-0 auto-assign works.
- **Notes for next step**: Step 10 creates the inspector thread, allocates `InspectorBridgeContext`, populates it from `InspectorState`, and starts a second `runHermesNode()` with `evalCode = "require('inspector-server');"`. The `config.actualPort` field needs to be part of `getConfig()` return value (currently not returned by the binding -- the binding returns `port` from the context, which is the requested port; `actualPort` is set by `notifyReady()`). The script stores it locally after `server.listen()`.

### Step 10: Start inspector runtime on a dedicated thread
- **Files**: modified `include/hermes/node-compat/inspector/inspector_bridge.h`, modified `lib/inspector/inspector_bridge.cpp`, modified `lib/runtime/hermes_node_runtime.cpp`, modified `libjs/shims/inspector-server.js`.
- **What was done**: When `config.inspect` is true, generates a UUID session ID (via `uv_random`), allocates `InspectorBridgeContext`, populates it with pointers from `InspectorState`, launches a `std::thread` running `runHermesNode()` with `evalCode = "require('inspector-server');"`, waits for `readyCv` signal from the inspector, and prints `Debugger listening on ws://HOST:PORT/UUID` to stderr. Added graceful shutdown: main thread sends `uv_async_send` to a shutdown handle on the inspector loop, which invokes a JS callback that closes the HTTP/WS servers so the event loop exits naturally. Thread is joined during main runtime cleanup. Mutex-protected `canSendShutdown` flag prevents use-after-close races. Inspector bridge shutdown flag clearing (`canSendShutdown = false`) added before `eventLoop.close()` for the inspector runtime.
- **Decisions**: Used JS callback for shutdown instead of `uv_stop()` because `UvEventLoop::close()` uses `uv_run(UV_RUN_DEFAULT)` internally which blocks on active handles. The JS callback closes the HTTP/WS servers letting the loop exit naturally. Session ID uses `uv_random()` (sync mode) with `uv_hrtime()` + PID fallback. `InspectorBridgeContext` heap-allocated, deleted after CDPAgent/CDPDebugAPI destruction.
- **Notes for next step**: Step 11 wires the CDPAgent's outbound message callback to push to `bridgeCtx->outboundQueue` + `uv_async_send(bridgeCtx->inspectorAsync)`, and initializes `inspectorAsync` on the inspector's event loop to drain the queue and invoke the JS `messageCallback`. The outbound callback is currently a placeholder no-op.

### Step 11: Wire end-to-end CDP message flow
- **Files**: modified `include/hermes/node-compat/inspector/inspector_bridge.h`, modified `lib/inspector/inspector_bridge.cpp`, modified `lib/runtime/hermes_node_runtime.cpp`.
- **What was done**: Wired the complete CDP message path: DevTools -> WS -> inspector JS -> sendToMain -> main inbound queue -> uv_async -> cdpAgent->handleCommand -> outbound callback -> bridgeCtx->outboundQueue -> inspector uv_async -> onOutboundAsync -> JS messageCallback -> ws.send -> DevTools. Three changes: (1) Changed `inspectorAsync` from pointer to embedded `uv_async_t` in `InspectorBridgeContext`, added `inspectorAsyncActive` atomic guard. (2) Added `onOutboundAsync` callback in inspector_bridge.cpp that drains the outbound queue and invokes the JS messageCallback; initialized `inspectorAsync` in `initInspectorBridgeBinding`. (3) Reordered `hermes_node_runtime.cpp` to allocate `bridgeCtx` before `CDPAgent::create()` so the outbound messageCallback lambda can capture `bridgeCtx`. Removed redundant `pushInspectorCommand` function.
- **Decisions**: Moved `bridgeCtx` allocation before CDPAgent creation (no circular dependency — bridgeCtx only needs config fields and InspectorState pointers). Used `std::atomic<bool> inspectorAsyncActive` to guard `uv_async_send` from CDPAgent's message callback, matching the existing pattern for `mainAsyncActive`. Set `inspectorAsyncActive = false` in inspector runtime cleanup alongside `canSendShutdown = false`.
- **Notes for next step**: The full message path is now live. Step 14 (--inspect-brk) needs to handle the case where the runtime is paused at a breakpoint and the main event loop is not running — CDP commands must still be processed via `triggerInterrupt_TS()` / `RuntimeTaskRunner` dual-path approach.

### Step 12: Add /json discovery endpoints
- **Files**: no changes needed (already implemented in `libjs/shims/inspector-server.js` from Step 9).
- **What was done**: Verified that the `/json`, `/json/list`, and `/json/version` HTTP endpoints work correctly. Step 9 included the full implementation of these endpoints. Tested with `--inspect=0` (OS-assigned port) and confirmed: (1) `/json/list` returns valid JSON array with description, id, title, type, url, webSocketDebuggerUrl, devtoolsFrontendUrl; (2) `/json` returns identical response; (3) `/json/version` returns `{"Browser":"hermes-node/0.1.0","Protocol-Version":"1.1"}`; (4) title and url fields populate correctly from scriptPath (title=path, url=`file://`+path) or fall back to "hermes-node"/empty for `-e` mode.
- **Notes for next step**: Step 13 (DevTools CDN redirect) is also already implemented in the inspector-server.js HTTP handler (lines 63-81). The `/devtools/*` path serves a redirect HTML page pointing to chrome-devtools-frontend.appspot.com.

### Step 13: Add DevTools CDN redirect
- **Files**: no changes needed (already implemented in `libjs/shims/inspector-server.js` from Step 9, lines 63-81).
- **What was done**: Verified the `/devtools/*` CDN redirect endpoint works correctly. The implementation serves an HTML page with a `<script>` that sets `location` to `chrome-devtools-frontend.appspot.com/serve_file/@60127beb442528082b3f6eff7392267e145262c3/js_app.html?ws=...`. Tested with `--inspect=0`: (1) With `?ws=` param: redirect uses the provided ws value; (2) Without `?ws=` param: defaults to the actual session URL (host:port/sessionId). All 133 tests pass.

