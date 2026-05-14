# Plan: Refactor Runtime Bootstrap into Callable API

## Context

The hermes-node bootstrap is a 1000-line monolithic function (`runBootstrap`) in `hermes-node.cpp`. All binding state is stored in ~12 file-scope statics (loop pointers, callback fn+data pairs, `napi_ref`s), making it impossible to run two runtimes in the same process. This refactoring extracts a clean, callable API so that starting a runtime is a simple call with a config struct, invocable from any thread. This is a prerequisite for the debugger (which will spin up a second runtime on an inspector thread to run a JS-based WebSocket server).

## Design

### Public API

```cpp
// include/hermes/node-compat/runtime/hermes_node_runtime.h

struct HermesNodeConfig {
    std::string scriptPath;              // empty = no script
    std::string evalCode;                // JS to eval after bootstrap (before event loop)
    std::vector<std::string> argv;       // process.argv
    std::string nodeVersion;             // override process.version
    bool enableRepl = false;             // start REPL when no script
};

/// Run a complete hermes-node instance. Blocks until the event loop exits.
/// Thread-safe: can be called from any thread, each call is fully independent.
/// Returns the exit code.
int runHermesNode(const HermesNodeConfig &config);
```

### Per-Instance State (`RuntimeState`)

Replace all file-scope statics with a single struct stored via `napi_set_instance_data`:

```cpp
// include/hermes/node-compat/runtime/runtime_state.h

struct RuntimeState {
    // Shared event loop (replaces 6+ separate s_*Loop globals)
    uv_loop_t *loop = nullptr;

    // Microtask drain (replaces s_drainMicrotasksFn/Data in node_task_queue.cpp)
    void (*drainMicrotasksFn)(void *) = nullptr;
    void *drainMicrotasksData = nullptr;

    // Async break (replaces s_triggerAsyncBreak/Data in node_contextify.cpp)
    void (*triggerAsyncBreakFn)(void *) = nullptr;
    void *triggerAsyncBreakData = nullptr;

    // Stream base shared state (replaces s_streamBaseState in libuv_stream_base.cpp)
    int32_t *streamBaseState = nullptr;

    // Per-binding state needed for cleanup
    // (timersState pointer set during timers binding init, used for closeTimersHandles)
    void *timersState = nullptr;

    // c-ares channels set (replaces s_channels in node_cares_wrap.cpp)
    std::unordered_set<void *> caresChannels;

    // Constructor refs (per-env, replaces file-scope napi_ref globals)
    napi_ref tcpCtorRef = nullptr;      // node_tcp_wrap.cpp
    napi_ref pipeCtorRef = nullptr;     // node_pipe_wrap.cpp
    napi_ref hashCtorRef = nullptr;     // node_crypto.cpp
    napi_ref contextifySymbolRef = nullptr; // node_contextify.cpp
};

/// Get the per-env RuntimeState. Returns nullptr if not set.
inline RuntimeState *getRuntimeState(napi_env env) {
    void *data = nullptr;
    napi_get_instance_data(env, &data);
    return static_cast<RuntimeState *>(data);
}
```

## Implementation

### Step 1: Create `RuntimeState` and `getRuntimeState()`

New files:
- `include/hermes/node-compat/runtime/runtime_state.h` -- the struct + inline getter
- `lib/runtime/CMakeLists.txt` -- tiny library (header-only or with a .cpp for the cleanup functions)

The RuntimeState struct is set via `napi_set_instance_data` right after creating `napi_env`, before any binding registration. A destructor/cleanup function frees it on env destruction.

### Step 2: Convert binding globals to per-instance state

Each binding file gets a mechanical transformation: replace `s_xxxLoop` / `s_xxx` with `getRuntimeState(env)->field`.

**Files to modify** (12 files, all in `lib/bindings/`):

| File | Globals removed | Replacement |
|------|----------------|-------------|
| `handle_wrap_base.cpp` | `s_handleWrapLoop` | `getRuntimeState(env)->loop` |
| `node_timers.cpp` | `s_loop`, `s_timersState` | `state->loop`, `state->timersState` |
| `node_file.cpp` | `s_fsLoop` | `state->loop` |
| `node_file_dir.cpp` | `s_fsDirLoop` | `state->loop` |
| `node_fs_event_wrap.cpp` | `s_fsEventLoop` | `state->loop` |
| `node_cares_wrap.cpp` | `s_caresLoop`, `s_channels` | `state->loop`, `state->caresChannels` |
| `node_contextify.cpp` | `s_triggerAsyncBreak*`, `s_contextPrivateSymbolRef` | `state->triggerAsyncBreakFn/Data`, `state->contextifySymbolRef` |
| `libuv_stream_base.cpp` | `s_streamBaseState` | `state->streamBaseState` |
| `node_tcp_wrap.cpp` | `s_tcpCtorRef` | `state->tcpCtorRef` |
| `node_pipe_wrap.cpp` | `s_pipeCtorRef` | `state->pipeCtorRef` |
| `node_crypto.cpp` | `hashConstructorRef` | `state->hashCtorRef` |
| `node_task_queue.cpp` | `s_drainMicrotasksFn/Data` | `state->drainMicrotasksFn/Data` |

**Pattern for each binding**: All NAPI callbacks already receive `napi_env env`. For libuv callbacks, the `env` is available via the wrap object (e.g. `HandleWrapBase::env_`, `TimersState::env`).

**Remove all `set*EventLoop()` / `set*()` / `clear*()` free functions**. The caller configures `RuntimeState` directly before binding init.

**Headers to modify**: Remove the `set*`/`clear*`/`close*` declarations from the binding headers:
- `include/hermes/node-compat/bindings/handle_wrap_base.h`
- `include/hermes/node-compat/bindings/node_timers.h`
- `include/hermes/node-compat/bindings/node_file.h`
- `include/hermes/node-compat/bindings/node_file_dir.h`
- `include/hermes/node-compat/bindings/node_fs_event_wrap.h`
- `include/hermes/node-compat/bindings/node_cares_wrap.h`
- `include/hermes/node-compat/bindings/node_contextify.h`
- `include/hermes/node-compat/bindings/node_task_queue.h`
- `include/hermes/node-compat/bindings/node_stream_wrap.h` (for `setStreamBaseState`)

**Cleanup functions** (`closeTimersHandles`, `caresWrapShutdown`, etc.) become methods on `RuntimeState` or free functions taking `napi_env`:

```cpp
// In runtime_state.h or a .cpp:
void runtimeStateCloseTimers(RuntimeState *state);
void runtimeStateCareShutdown(RuntimeState *state);
```

**Known limitation**: The SIGINT watchdog in `node_contextify.cpp` uses `sigaction` which is process-global. Two runtimes cannot both install SIGINT handlers. For now, this is documented but not fixed (the inspector runtime won't use contextify's SIGINT watchdog).

### Step 3: Extract `runHermesNode()` function

Move the entire bootstrap sequence from `hermes-node.cpp` into a new library:
- `lib/runtime/hermes_node_runtime.cpp` -- implements `runHermesNode()`
- `include/hermes/node-compat/runtime/hermes_node_runtime.h` -- public API

The function does exactly what `runBootstrap()` does today:
1. Create `vm::Runtime` + `UvEventLoop` + `napi_env`
2. Allocate `RuntimeState`, configure it, store via `napi_set_instance_data`
3. Register bindings, run primordials, create process, init module loader
4. Set up timers, globals, debuglog, stdio, console
5. Execute script / eval code / start REPL
6. Run event loop
7. Emit 'exit', cleanup, return exit code

**Helper functions** (`printAndClearException`, `installConsole`, `onFatalException`, `drainTicksImpl`, etc.) move into the .cpp as `static` functions or into a detail namespace.

The binding registration block (lines 375-412 of current hermes-node.cpp) moves unchanged -- all bindings are always registered.

### Step 4: Simplify `hermes-node.cpp`

The main binary becomes just argument parsing + `runHermesNode()`:

```cpp
int main(int argc, char **argv) {
    HermesNodeConfig config;
    // parse --node-version, --inspect, etc. into config
    // set config.argv from remaining args
    // set config.scriptPath or config.enableRepl
    return runHermesNode(config);
}
```

~50 lines instead of ~1050.

### Step 5: CMakeLists.txt changes

New `lib/runtime/CMakeLists.txt`:
```cmake
add_hermes_library(hermesNodeRuntime STATIC
    hermes_node_runtime.cpp
)
target_link_libraries(hermesNodeRuntime
    PUBLIC hermesNapiCompile hermesNapi hermesvm_a uv_a
    PUBLIC hermesNodeBindingRegistry hermesNodeBindings
    PUBLIC hermesNodeModuleLoader hermesNodeProcess
    PUBLIC hermesNodeEmbeddedModules hermesNodeEventLoop
)
```

`tools/hermes-node/CMakeLists.txt`: link `hermesNodeRuntime` instead of all the individual libs.

Root `CMakeLists.txt`: add `add_subdirectory(lib/runtime)`.

## Verification

1. `cmake --build cmake-build-asan --target check-hermes-node` -- all existing tests pass
2. No file-scope statics remaining in `lib/bindings/` (grep for `^static.*s_`)
3. Manual test: `hermes-node script.js` and `hermes-node` (REPL) work as before

## File Map

### New files
```
include/hermes/node-compat/runtime/runtime_state.h
include/hermes/node-compat/runtime/hermes_node_runtime.h
lib/runtime/CMakeLists.txt
lib/runtime/hermes_node_runtime.cpp
```

### Modified files (bindings -- global state removal)
```
lib/bindings/handle_wrap_base.cpp
lib/bindings/node_timers.cpp
lib/bindings/node_file.cpp
lib/bindings/node_file_dir.cpp
lib/bindings/node_fs_event_wrap.cpp
lib/bindings/node_cares_wrap.cpp
lib/bindings/node_contextify.cpp
lib/bindings/libuv_stream_base.cpp
lib/bindings/node_tcp_wrap.cpp
lib/bindings/node_pipe_wrap.cpp
lib/bindings/node_crypto.cpp
lib/bindings/node_task_queue.cpp
```

### Modified headers (remove set*/clear* declarations)
```
include/hermes/node-compat/bindings/handle_wrap_base.h
include/hermes/node-compat/bindings/node_timers.h
include/hermes/node-compat/bindings/node_file.h
include/hermes/node-compat/bindings/node_file_dir.h
include/hermes/node-compat/bindings/node_fs_event_wrap.h
include/hermes/node-compat/bindings/node_cares_wrap.h
include/hermes/node-compat/bindings/node_contextify.h
include/hermes/node-compat/bindings/node_task_queue.h
include/hermes/node-compat/bindings/node_stream_wrap.h
```

### Modified build files
```
CMakeLists.txt                         -- add_subdirectory(lib/runtime)
lib/bindings/CMakeLists.txt            -- add dependency on runtime_state header
tools/hermes-node/CMakeLists.txt       -- link hermesNodeRuntime
tools/hermes-node/hermes-node.cpp      -- shrink to arg parsing + runHermesNode()
```
