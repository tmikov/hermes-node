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
| Step 8 | Create inspector_bridge native binding | 7 |  |  |
| Step 9 | Create inspector JS server script | 6, 8 |  |  |
| Step 10 | Start inspector runtime on a dedicated thread | 5, 8, 9 |  |  |
| Step 11 | Wire end-to-end CDP message flow | 10 |  |  |
| Step 12 | Add /json discovery endpoints | 10 |  |  |
| Step 13 | Add DevTools CDN redirect | 12 |  |  |
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

