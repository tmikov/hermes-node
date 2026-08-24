# Runtime Bootstrap Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extract the hermes-node bootstrap into a callable `runHermesNode(config)` API, eliminating all file-scope statics so multiple runtimes can coexist in one process.

**Architecture:** Replace ~12 file-scope statics across 12 binding files with a single `RuntimeState` struct stored per-env via `napi_set_instance_data`. Move the 1000-line `runBootstrap()` into a reusable `lib/runtime/` library. `hermes-node.cpp` becomes ~50 lines of argument parsing.

**Tech Stack:** C++ (Clang), CMake/Ninja, NAPI, libuv, Hermes VM

---

### Task 1: Create RuntimeState header

**Files:**
- Create: `include/hermes/node-compat/runtime/runtime_state.h`

**Step 1: Create the RuntimeState header**

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <node_api.h>
#include <uv.h>

#include <unordered_set>

namespace hermes {
namespace node_compat {

/// Per-runtime-instance state. Replaces all file-scope statics in bindings.
/// Stored via napi_set_instance_data, retrieved via getRuntimeState(env).
/// Allocated by the bootstrap, freed by napi_set_instance_data finalizer.
struct RuntimeState {
  // --- Shared event loop (replaces 6 separate s_*Loop globals) ---
  uv_loop_t *loop = nullptr;

  // --- Microtask drain (replaces s_drainMicrotasksFn/Data) ---
  void (*drainMicrotasksFn)(void *) = nullptr;
  void *drainMicrotasksData = nullptr;

  // --- Async break for contextify SIGINT (replaces s_triggerAsyncBreak/Data) ---
  void (*triggerAsyncBreakFn)(void *) = nullptr;
  void *triggerAsyncBreakData = nullptr;

  // --- Stream base shared typed array (replaces s_streamBaseState) ---
  int32_t *streamBaseState = nullptr;

  // --- Timers state pointer for cleanup (replaces s_timersState) ---
  // Typed as void* to avoid including timers internals. Cast in timers code.
  void *timersState = nullptr;

  // --- c-ares channels set for shutdown (replaces s_channels) ---
  // Typed as void* to avoid including ChannelWrap. Cast in cares code.
  std::unordered_set<void *> caresChannels;

  // --- Constructor refs (per-env, replaces file-scope napi_ref globals) ---
  napi_ref tcpCtorRef = nullptr;
  napi_ref pipeCtorRef = nullptr;
  napi_ref hashCtorRef = nullptr;
  napi_ref contextifySymbolRef = nullptr;
};

/// Retrieve the per-env RuntimeState. Returns nullptr if not set.
inline RuntimeState *getRuntimeState(napi_env env) {
  void *data = nullptr;
  napi_get_instance_data(env, &data);
  return static_cast<RuntimeState *>(data);
}

} // namespace node_compat
} // namespace hermes
```

**Step 2: Build to check header compiles**

Run: `cmake --build cmake-build-asan --target hermesNodeBindings 2>&1 | tail -5`
Expected: Build succeeds (header not yet included anywhere, just checking for syntax).

**Step 3: Commit**

```
git add include/hermes/node-compat/runtime/runtime_state.h
git commit -m "Add RuntimeState header for per-instance binding state"
```

---

### Task 2: Wire RuntimeState into hermes-node.cpp

Set the instance data right after creating napi_env, before any binding registration. This is a non-breaking change -- bindings still read their own globals, but RuntimeState is available for the incremental migration.

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Add include and allocate/store RuntimeState**

After line 53 (`#include <hermes/node-compat/process/node_process.h>`), add:
```cpp
#include <hermes/node-compat/runtime/runtime_state.h>
```

In `runBootstrap()`, after `napi_env env = hermes_napi_create_env(...)` (line 345) and before the handle scope, add RuntimeState setup:

```cpp
  napi_env env = hermes_napi_create_env(runtime.get(), eventLoop.getHost());

  // Allocate and install per-instance state for bindings.
  auto *runtimeState = new RuntimeState();
  runtimeState->loop = eventLoop.getLoop();
  runtimeState->drainMicrotasksFn = [](void *data) {
    static_cast<hermes::vm::Runtime *>(data)->drainJobs();
  };
  runtimeState->drainMicrotasksData = runtime.get();
  runtimeState->triggerAsyncBreakFn = [](void *data) {
    static_cast<hermes::vm::Runtime *>(data)->triggerTimeoutAsyncBreak();
  };
  runtimeState->triggerAsyncBreakData = runtime.get();
  napi_set_instance_data(
      env,
      runtimeState,
      [](napi_env, void *data, void *) {
        delete static_cast<RuntimeState *>(data);
      },
      nullptr);
```

Keep all the existing `set*EventLoop()` calls in place for now (they'll be removed one-by-one as each binding is converted).

**Step 2: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass (no behavior change yet).

**Step 3: Commit**

```
git add tools/hermes-node/hermes-node.cpp
git commit -m "Install RuntimeState as napi instance data during bootstrap"
```

---

### Task 3: Convert handle_wrap_base.cpp

Replace `s_handleWrapLoop` with `getRuntimeState(env)->loop`.

**Files:**
- Modify: `lib/bindings/handle_wrap_base.cpp`
- Modify: `include/hermes/node-compat/bindings/handle_wrap_base.h`

**Step 1: Modify handle_wrap_base.cpp**

Add include at top:
```cpp
#include <hermes/node-compat/runtime/runtime_state.h>
```

Remove the three globals and their functions (lines 21-33):
```cpp
// DELETE: static uv_loop_t *s_handleWrapLoop = nullptr;
// DELETE: void setHandleWrapEventLoop(uv_loop_t *loop) { ... }
// DELETE: uv_loop_t *getHandleWrapEventLoop() { ... }
// DELETE: void clearHandleWrapEventLoop() { ... }
```

Replace `getHandleWrapEventLoop()` usage. The only read site is in `pointerCb` (line ~156). `pointerCb` receives `napi_env` as its first parameter, so replace:
```cpp
// OLD:
if (!uv_is_closing(wrap->handle_) && s_handleWrapLoop) {
// NEW:
auto *rtState = getRuntimeState(env);
if (!uv_is_closing(wrap->handle_) && rtState && rtState->loop) {
```

Note: `pointerCb` signature is `void pointerCb(napi_env env, void *data, void *)` -- env IS available.

Also, `HandleWrapBase::init()` calls `getHandleWrapEventLoop()` -- grep for this and replace with `getRuntimeState(env)->loop`.

Actually, `init()` doesn't use the loop global directly. The subclasses (TCPWrap, PipeWrap, etc.) call `uv_tcp_init(getHandleWrapEventLoop(), ...)` in their own code. `getHandleWrapEventLoop()` is the public getter, used by subclasses. So we need to keep a way for subclasses to get the loop. They all have `env` available. Replace the public function:

In `handle_wrap_base.h`, change the declaration:
```cpp
// OLD:
void setHandleWrapEventLoop(uv_loop_t *loop);
uv_loop_t *getHandleWrapEventLoop();
void clearHandleWrapEventLoop();
// NEW: (remove all three, callers use getRuntimeState(env)->loop instead)
```

**Step 2: Find and update all callers of getHandleWrapEventLoop()**

Run: `grep -rn 'getHandleWrapEventLoop\|setHandleWrapEventLoop\|clearHandleWrapEventLoop' lib/ include/ tools/`

For each call site in the bindings (tcp_wrap, pipe_wrap, tty_wrap, udp_wrap, process_wrap, fs_event_wrap), replace `getHandleWrapEventLoop()` with `getRuntimeState(env)->loop`. All these sites have `env` available.

In `hermes-node.cpp`, remove the calls:
```cpp
// DELETE: setHandleWrapEventLoop(eventLoop.getLoop());
// DELETE: clearHandleWrapEventLoop();
```
(These are now handled by RuntimeState setup in Task 2.)

**Step 3: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 4: Commit**

```
git add lib/bindings/handle_wrap_base.cpp include/hermes/node-compat/bindings/handle_wrap_base.h tools/hermes-node/hermes-node.cpp
git add <any other binding files that called getHandleWrapEventLoop>
git commit -m "Replace s_handleWrapLoop global with RuntimeState lookup"
```

---

### Task 4: Convert simple loop globals (node_file, node_file_dir, node_fs_event_wrap)

These three files each have a single `s_*Loop` global that's passed to libuv functions. Each async operation already has `napi_env env` available via the request wrap struct.

**Files:**
- Modify: `lib/bindings/node_file.cpp`
- Modify: `lib/bindings/node_file_dir.cpp`
- Modify: `lib/bindings/node_fs_event_wrap.cpp`
- Modify: `include/hermes/node-compat/bindings/node_file.h`
- Modify: `include/hermes/node-compat/bindings/node_file_dir.h`
- Modify: `include/hermes/node-compat/bindings/node_fs_event_wrap.h`
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Convert node_file.cpp**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

Remove `s_fsLoop` global and `setFsEventLoop()` setter (lines 30-34).

All 37 read sites follow the pattern `uv_fs_xxx(s_fsLoop, ...)` inside async branches. The async code creates a `FSReqWrap` which has `env`. Each async operation function receives `napi_env env` as its first NAPI callback parameter.

Replace each `s_fsLoop` with `getRuntimeState(env)->loop`. Since there are ~37 sites, use search-and-replace within the async branches. Every `uv_fs_*(s_fsLoop,` becomes `uv_fs_*(getRuntimeState(env)->loop,`.

Also replace the `uv_fs_poll_init` call at line 3152:
```cpp
// OLD: uv_fs_poll_init(s_fsLoop, &wrap->handle);
// NEW: uv_fs_poll_init(getRuntimeState(env)->loop, &wrap->handle);
```

Remove `setFsEventLoop` declaration from `node_file.h`.

**Step 2: Convert node_file_dir.cpp**

Same pattern. Remove `s_fsDirLoop` and `setFsDirEventLoop()`. Replace 6 read sites with `getRuntimeState(env)->loop`. All sites have `env` from the NAPI callback parameter.

Remove `setFsDirEventLoop` declaration from `node_file_dir.h`.

**Step 3: Convert node_fs_event_wrap.cpp**

Remove `s_fsEventLoop` and `setFsEventWrapEventLoop()`. One read site at line 128 inside `fsEventStart` (NAPI callback, has `env`). Also remove `closeFsEventWrapHandles()` (it's a no-op).

Remove declarations from `node_fs_event_wrap.h`.

**Step 4: Remove the set* calls from hermes-node.cpp**

```cpp
// DELETE: setFsEventLoop(eventLoop.getLoop());
// DELETE: setFsDirEventLoop(eventLoop.getLoop());
// DELETE: setFsEventWrapEventLoop(eventLoop.getLoop());
// DELETE: closeFsEventWrapHandles();
```

**Step 5: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 6: Commit**

```
git add lib/bindings/node_file.cpp lib/bindings/node_file_dir.cpp lib/bindings/node_fs_event_wrap.cpp
git add include/hermes/node-compat/bindings/node_file.h include/hermes/node-compat/bindings/node_file_dir.h include/hermes/node-compat/bindings/node_fs_event_wrap.h
git add tools/hermes-node/hermes-node.cpp
git commit -m "Replace fs/dir/event loop globals with RuntimeState lookup"
```

---

### Task 5: Convert node_task_queue.cpp

Replace `s_drainMicrotasksFn`/`s_drainMicrotasksData` globals.

**Files:**
- Modify: `lib/bindings/node_task_queue.cpp`
- Modify: `include/hermes/node-compat/bindings/node_task_queue.h`
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Modify node_task_queue.cpp**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

Remove globals and setter (lines 21-27):
```cpp
// DELETE: static DrainMicrotasksFn s_drainMicrotasksFn = nullptr;
// DELETE: static void *s_drainMicrotasksData = nullptr;
// DELETE: void setTaskQueueDrainMicrotasks(...) { ... }
```

In `runMicrotasks` (lines 52-59), `env` is the first parameter:
```cpp
// OLD:
if (s_drainMicrotasksFn)
  s_drainMicrotasksFn(s_drainMicrotasksData);
// NEW:
auto *rtState = getRuntimeState(env);
if (rtState && rtState->drainMicrotasksFn)
  rtState->drainMicrotasksFn(rtState->drainMicrotasksData);
```

**Step 2: Update header and hermes-node.cpp**

Remove `DrainMicrotasksFn` typedef and `setTaskQueueDrainMicrotasks` declaration from `node_task_queue.h`.

In hermes-node.cpp, remove:
```cpp
// DELETE: setTaskQueueDrainMicrotasks([](void *data) { ... }, runtime.get());
```
(Already configured in RuntimeState setup from Task 2.)

**Step 3: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 4: Commit**

```
git add lib/bindings/node_task_queue.cpp include/hermes/node-compat/bindings/node_task_queue.h tools/hermes-node/hermes-node.cpp
git commit -m "Replace task queue drain globals with RuntimeState lookup"
```

---

### Task 6: Convert libuv_stream_base.cpp

Replace `s_streamBaseState`.

**Files:**
- Modify: `lib/bindings/libuv_stream_base.cpp`
- Modify: `include/hermes/node-compat/bindings/libuv_stream_base.h` (or wherever `setStreamBaseState` is declared)

**Step 1: Modify libuv_stream_base.cpp**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

Remove global and setter (lines 31-35):
```cpp
// DELETE: static int32_t *s_streamBaseState = nullptr;
// DELETE: void LibuvStreamBase::setStreamBaseState(int32_t *state) { ... }
```

All 18 read sites are writes into the array, guarded by `if (s_streamBaseState)`. In each case, `this` is a `LibuvStreamBase` which has `env_` from `HandleWrapBase`. Replace:
```cpp
// OLD:
if (s_streamBaseState) {
  s_streamBaseState[kReadBytesOrError] = ...;
// NEW:
auto *sbs = getRuntimeState(env_)->streamBaseState;
if (sbs) {
  sbs[kReadBytesOrError] = ...;
```

The `setStreamBaseState` call site is in `node_stream_wrap.cpp` (initStreamWrapBinding). Change it to:
```cpp
// OLD: LibuvStreamBase::setStreamBaseState(statePtr);
// NEW: getRuntimeState(env)->streamBaseState = statePtr;
```

Remove the `setStreamBaseState` declaration from the header.

**Step 2: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 3: Commit**

```
git add lib/bindings/libuv_stream_base.cpp lib/bindings/node_stream_wrap.cpp
git add include/hermes/node-compat/bindings/libuv_stream_base.h
git commit -m "Replace stream base state global with RuntimeState lookup"
```

---

### Task 7: Convert node_timers.cpp

Replace `s_loop` and `s_timersState`.

**Files:**
- Modify: `lib/bindings/node_timers.cpp`
- Modify: `include/hermes/node-compat/bindings/node_timers.h`
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Modify node_timers.cpp**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

Remove `s_loop`, `setTimersEventLoop()`, `s_timersState` (lines 25-61 area).

**For `s_loop` reads**: The libuv callbacks (`onTimerFired`, `onCheckImmediate`) access the loop via `handle->data` which points to `TimersState`. `TimersState` has `env`. So:
```cpp
// In onTimerFired (line 97):
// OLD: uv_update_time(s_loop);
// NEW: uv_update_time(getRuntimeState(state->env)->loop);
```

In `initTimersBinding` (line 416 area), `env` is a parameter:
```cpp
// OLD: uv_timer_init(s_loop, &state->timerHandle);
// NEW: uv_loop_t *loop = getRuntimeState(env)->loop;
//      uv_timer_init(loop, &state->timerHandle);
```

**For `s_timersState`**: In `initTimersBinding`, replace `s_timersState = state` with `getRuntimeState(env)->timersState = state`. In `cleanupState`, replace the check and clear. In `closeTimersHandles`, it needs env -- change to `closeTimersHandles(napi_env env)`.

**Step 2: Update closeTimersHandles signature**

In `node_timers.h`, change:
```cpp
// OLD: void closeTimersHandles();
// NEW: void closeTimersHandles(napi_env env);
```

In `node_timers.cpp`, implement:
```cpp
void closeTimersHandles(napi_env env) {
  auto *rtState = getRuntimeState(env);
  if (!rtState || !rtState->timersState)
    return;
  auto *state = static_cast<TimersState *>(rtState->timersState);
  // ... existing close logic ...
}
```

In `hermes-node.cpp`, update the call:
```cpp
// OLD: closeTimersHandles();
// NEW: closeTimersHandles(env);
```

Also remove: `setTimersEventLoop(eventLoop.getLoop());`

**Step 3: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 4: Commit**

```
git add lib/bindings/node_timers.cpp include/hermes/node-compat/bindings/node_timers.h tools/hermes-node/hermes-node.cpp
git commit -m "Replace timers globals with RuntimeState lookup"
```

---

### Task 8: Convert node_cares_wrap.cpp

Replace `s_caresLoop` and `s_channels`.

**Files:**
- Modify: `lib/bindings/node_cares_wrap.cpp`
- Modify: `include/hermes/node-compat/bindings/node_cares_wrap.h`
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Modify node_cares_wrap.cpp**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

Remove `s_caresLoop`, `setCaresWrapEventLoop()`, and `s_channels`.

**For `s_caresLoop`**: 4 read sites. All have env available (NAPI callbacks or ChannelWrap methods with `env_` member). Replace with `getRuntimeState(env)->loop`.

In `ChannelWrap::setup()` and `sockStateCb()`, ChannelWrap stores `env_` as a member. Replace:
```cpp
// OLD: uv_timer_init(s_caresLoop, &timer_);
// NEW: uv_timer_init(getRuntimeState(env_)->loop, &timer_);
```

For `sockStateCb` (static method), `void *data` arg is the ChannelWrap pointer:
```cpp
auto *channel = static_cast<ChannelWrap *>(data);
// OLD: uv_poll_init_socket(s_caresLoop, &task->poll_watcher, sock);
// NEW: uv_poll_init_socket(getRuntimeState(channel->env_)->loop, &task->poll_watcher, sock);
```
(If `env_` is private, use a getter or make `sockStateCb` a friend/member.)

**For `s_channels`**: Replace `s_channels.insert(this)` / `s_channels.erase(this)` in constructor/destructor with:
```cpp
getRuntimeState(env_)->caresChannels.insert(this);
getRuntimeState(env_)->caresChannels.erase(this);
```

**For `caresWrapShutdown()`**: Change to `caresWrapShutdown(napi_env env)`:
```cpp
void caresWrapShutdown(napi_env env) {
  auto *rtState = getRuntimeState(env);
  if (!rtState) return;
  auto channels = rtState->caresChannels; // copy to avoid invalidation
  for (auto *ptr : channels)
    static_cast<ChannelWrap *>(ptr)->closeChannel();
}
```

**Step 2: Update header and hermes-node.cpp**

In `node_cares_wrap.h`:
```cpp
// OLD: void setCaresWrapEventLoop(uv_loop_t *loop);
// OLD: void caresWrapShutdown();
// NEW: void caresWrapShutdown(napi_env env);
```

In `hermes-node.cpp`:
```cpp
// DELETE: setCaresWrapEventLoop(eventLoop.getLoop());
// OLD: caresWrapShutdown();
// NEW: caresWrapShutdown(env);
```

**Step 3: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 4: Commit**

```
git add lib/bindings/node_cares_wrap.cpp include/hermes/node-compat/bindings/node_cares_wrap.h tools/hermes-node/hermes-node.cpp
git commit -m "Replace cares_wrap globals with RuntimeState lookup"
```

---

### Task 9: Convert node_contextify.cpp

Replace `s_triggerAsyncBreak`/`s_triggerAsyncBreakData` and `s_contextPrivateSymbolRef`.
Keep `s_sigintWatching`/`s_sigintReceived`/`s_previousSigaction` as process-global statics (POSIX signal handling is inherently process-wide).

**Files:**
- Modify: `lib/bindings/node_contextify.cpp`
- Modify: `include/hermes/node-compat/bindings/node_contextify.h`
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Convert contextifySymbolRef**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

In `getContextPrivateSymbol()` (around line 268), env is a parameter:
```cpp
// OLD: if (s_contextPrivateSymbolRef) {
//        napi_get_reference_value(env, s_contextPrivateSymbolRef, &sym);
// NEW: auto *rtState = getRuntimeState(env);
//      if (rtState->contextifySymbolRef) {
//        napi_get_reference_value(env, rtState->contextifySymbolRef, &sym);
```

And the SET site (line 299):
```cpp
// OLD: napi_create_reference(env, sym, 1, &s_contextPrivateSymbolRef);
// NEW: napi_create_reference(env, sym, 1, &getRuntimeState(env)->contextifySymbolRef);
```

Remove the `static napi_ref s_contextPrivateSymbolRef` global.

**Step 2: Handle triggerAsyncBreak in signal handler**

The `sigintHandler` is a POSIX signal handler -- it has NO access to env or RuntimeState. The `s_triggerAsyncBreak`/`s_triggerAsyncBreakData` must remain process-global for the signal handler.

Keep these two as file-scope statics, but populate them from RuntimeState during `startSigintWatchdogCb`:

```cpp
// Keep as file-scope statics (signal handler needs them):
static void (*s_triggerAsyncBreak)(void *) = nullptr;
static void *s_triggerAsyncBreakData = nullptr;
```

Remove `setContextifyAsyncBreak()`. Instead, in `startSigintWatchdogCb` (which has env), copy from RuntimeState:
```cpp
auto *rtState = getRuntimeState(env);
s_triggerAsyncBreak = rtState->triggerAsyncBreakFn;
s_triggerAsyncBreakData = rtState->triggerAsyncBreakData;
```

Remove `setContextifyAsyncBreak` from `node_contextify.h`.

In `hermes-node.cpp`, remove:
```cpp
// DELETE: setContextifyAsyncBreak([](void *data) { ... }, runtime.get());
```
(Already configured in RuntimeState from Task 2.)

**Step 3: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 4: Commit**

```
git add lib/bindings/node_contextify.cpp include/hermes/node-compat/bindings/node_contextify.h tools/hermes-node/hermes-node.cpp
git commit -m "Replace contextify globals with RuntimeState lookup

Keep SIGINT atomics as process-global statics (inherent to POSIX signals)."
```

---

### Task 10: Convert constructor ref globals (tcp_wrap, pipe_wrap, crypto)

These three files each have a file-scope `napi_ref` to a JS constructor, set during binding init, read from libuv callbacks.

**Files:**
- Modify: `lib/bindings/node_tcp_wrap.cpp`
- Modify: `lib/bindings/node_pipe_wrap.cpp`
- Modify: `lib/bindings/node_crypto.cpp`

**Step 1: Convert node_tcp_wrap.cpp**

Add include: `#include <hermes/node-compat/runtime/runtime_state.h>`

Remove `static napi_ref s_tcpCtorRef = nullptr;` (line 22).

In `initTcpWrapBinding` (line 772), replace:
```cpp
// OLD: napi_create_reference(env, tcpCtor, 1, &s_tcpCtorRef);
// NEW: napi_create_reference(env, tcpCtor, 1, &getRuntimeState(env)->tcpCtorRef);
```

In `OnConnection` (line 245), env is available via `wrap->env()`:
```cpp
// OLD: napi_get_reference_value(env, s_tcpCtorRef, &tcpCtor);
// NEW: napi_get_reference_value(env, getRuntimeState(env)->tcpCtorRef, &tcpCtor);
```

**Step 2: Convert node_pipe_wrap.cpp**

Same pattern. Remove `s_pipeCtorRef` (line 22).

In `initPipeWrapBinding` (line 519):
```cpp
// OLD: napi_create_reference(env, pipeCtor, 1, &s_pipeCtorRef);
// NEW: napi_create_reference(env, pipeCtor, 1, &getRuntimeState(env)->pipeCtorRef);
```

In `OnConnection` (line 189):
```cpp
// OLD: napi_get_reference_value(env, s_pipeCtorRef, &pipeCtor);
// NEW: napi_get_reference_value(env, getRuntimeState(env)->pipeCtorRef, &pipeCtor);
```

**Step 3: Convert node_crypto.cpp**

Remove `static napi_ref hashConstructorRef = nullptr;` (line 74).

In `initCryptoBinding` (line 374):
```cpp
// OLD: napi_create_reference(env, hashCtor, 1, &hashConstructorRef);
// NEW: napi_create_reference(env, hashCtor, 1, &getRuntimeState(env)->hashCtorRef);
```

In `hashCopy` (line 178):
```cpp
// OLD: napi_get_reference_value(env, hashConstructorRef, &hashCtorVal);
// NEW: napi_get_reference_value(env, getRuntimeState(env)->hashCtorRef, &hashCtorVal);
```

**Step 4: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 5: Commit**

```
git add lib/bindings/node_tcp_wrap.cpp lib/bindings/node_pipe_wrap.cpp lib/bindings/node_crypto.cpp
git commit -m "Replace constructor ref globals with RuntimeState lookup"
```

---

### Task 11: Verify no globals remain

**Step 1: Grep for remaining file-scope statics in bindings**

Run: `grep -rn '^static.*s_' lib/bindings/ | grep -v 'sigint\|s_previous'`

Expected: No matches (only the SIGINT-related statics in node_contextify.cpp remain, which are intentionally process-global).

Also check: `grep -rn 'setHandleWrapEventLoop\|setFsEventLoop\|setFsDirEventLoop\|setFsEventWrapEventLoop\|setTimersEventLoop\|setCaresWrapEventLoop\|setTaskQueueDrainMicrotasks\|setContextifyAsyncBreak\|setStreamBaseState\|clearHandleWrapEventLoop' lib/ include/ tools/`

Expected: No matches (all removed).

**Step 2: Format and test**

Run: `./utils/format.sh -f && cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 3: Commit if format changed anything**

```
git add -u
git commit -m "Format after global state removal"
```

---

### Task 12: Create lib/runtime library and extract runHermesNode()

Move the bootstrap from hermes-node.cpp into a reusable library.

**Files:**
- Create: `include/hermes/node-compat/runtime/hermes_node_runtime.h`
- Create: `lib/runtime/CMakeLists.txt`
- Create: `lib/runtime/hermes_node_runtime.cpp`
- Modify: `CMakeLists.txt` (root)
- Modify: `tools/hermes-node/CMakeLists.txt`
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Create public header**

File: `include/hermes/node-compat/runtime/hermes_node_runtime.h`

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Configuration for a hermes-node runtime instance.
struct HermesNodeConfig {
  /// Script file to execute. Empty = no script file.
  std::string scriptPath;

  /// Inline JS code to eval after bootstrap, before event loop.
  /// Useful for programmatic use (e.g. inspector runtime).
  std::string evalCode;

  /// process.argv values. First element should be the binary name.
  std::vector<std::string> argv;

  /// Override process.version. Empty = use default.
  std::string nodeVersion;

  /// Start the REPL when no scriptPath and no evalCode are provided.
  bool enableRepl = false;
};

/// Run a complete hermes-node instance. Blocks until the event loop exits.
/// Thread-safe: can be called from any thread; each invocation is fully
/// independent (own runtime, event loop, bindings state).
/// Returns the process exit code.
int runHermesNode(const HermesNodeConfig &config);

} // namespace node_compat
} // namespace hermes
```

**Step 2: Create CMakeLists.txt**

File: `lib/runtime/CMakeLists.txt`

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeRuntime STATIC
  hermes_node_runtime.cpp
)

target_include_directories(hermesNodeRuntime
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
)

target_link_libraries(hermesNodeRuntime
  PUBLIC
    hermesNodeEmbeddedModules
    hermesNodeEventLoop
    hermesNodeBindingRegistry
    hermesNodeBindings
    hermesNodeModuleLoader
    hermesNodeProcess
    hermesNapiCompile
    hermesvm_a
)
```

**Step 3: Create hermes_node_runtime.cpp**

Move the entire contents of `runBootstrap()` plus its helper functions (`printAndClearException`, `installConsole`, `onFatalException`, `drainTicksImpl`, `onCheckDrainTicks`, `onPrepareDrainTicks`, and `TickDrainData`) from `hermes-node.cpp` into this file.

The function signature changes from:
```cpp
static int runBootstrap(int argc, char **argv, int scriptArgIndex, const Config &cfg)
```
to:
```cpp
int runHermesNode(const HermesNodeConfig &config)
```

Key adaptations inside the function:
- Replace `argv[0]` with `config.argv.empty() ? "hermes-node" : config.argv[0].c_str()`
- Replace `cfg.nodeVersion` with `config.nodeVersion.empty() ? nullptr : config.nodeVersion.c_str()`
- Replace `scriptPath` with `config.scriptPath.empty() ? nullptr : config.scriptPath.c_str()`
- Replace the argv loop with config.argv
- Replace the REPL check with `config.enableRepl`
- Add evalCode support: after bootstrap, before event loop, if `!config.evalCode.empty()`, eval it via `napi_run_script`
- All includes from hermes-node.cpp for bindings, loader, process, etc. move here
- The `using namespace hermes::node_compat;` stays

The RuntimeState setup (from Task 2) is already in this code. The set*EventLoop calls are already removed (Tasks 3-10).

**Step 4: Add to root CMakeLists.txt**

In root `CMakeLists.txt`, add `add_subdirectory(lib/runtime)` after `add_subdirectory(lib/bindings)` (runtime depends on bindings).

**Step 5: Update tools/hermes-node/CMakeLists.txt**

Replace all individual library links with just `hermesNodeRuntime`:

```cmake
add_hermes_tool(hermes-node
  hermes-node.cpp
  LINK_LIBS
    hermesNodeRuntime
)
```

Keep the whole-archive linking for `hermesNapi` and the `-rdynamic` flag.

**Step 6: Build and test**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 7: Commit**

```
git add include/hermes/node-compat/runtime/hermes_node_runtime.h
git add lib/runtime/CMakeLists.txt lib/runtime/hermes_node_runtime.cpp
git add CMakeLists.txt tools/hermes-node/CMakeLists.txt tools/hermes-node/hermes-node.cpp
git commit -m "Extract runHermesNode() into lib/runtime library"
```

---

### Task 13: Simplify hermes-node.cpp

After Task 12, hermes-node.cpp should contain only argument parsing and a call to `runHermesNode()`.

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp`

**Step 1: Rewrite hermes-node.cpp**

The file should now be approximately:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

#include <cstdio>
#include <cstring>

using namespace hermes::node_compat;

static void printUsage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [options] [script.js] [-- script-args...]\n"
      "\n"
      "Options:\n"
      "  --node-version <version>  Override process.version (e.g. v24.13.0)\n"
      "  -e, --eval <code>         Evaluate inline JavaScript\n"
      "  -h, --help                Show this help\n",
      argv0);
}

int main(int argc, char **argv) {
  HermesNodeConfig config;
  int scriptArgIndex = argc;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (std::strcmp(argv[i], "--node-version") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --node-version requires a value\n");
        return 1;
      }
      config.nodeVersion = argv[++i];
    } else if (std::strcmp(argv[i], "-e") == 0 ||
               std::strcmp(argv[i], "--eval") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s requires a value\n", argv[i]);
        return 1;
      }
      config.evalCode = argv[++i];
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return 1;
    } else {
      scriptArgIndex = i;
      break;
    }
  }

  // Build process.argv: [binary, script-or-eval-args...].
  config.argv.push_back(argv[0]);
  for (int i = scriptArgIndex; i < argc; ++i)
    config.argv.push_back(argv[i]);

  if (scriptArgIndex < argc) {
    config.scriptPath = argv[scriptArgIndex];
  } else if (config.evalCode.empty()) {
    config.enableRepl = true;
  }

  return runHermesNode(config);
}
```

**Step 2: Verify no stale includes or dead code remain**

The file should have NO binding includes, no `hermes/VM/Runtime.h`, no `napi/hermes_napi.h`. Just the runtime header and standard library headers.

**Step 3: Build and test**

Run: `./utils/format.sh -f && cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 4: Commit**

```
git add tools/hermes-node/hermes-node.cpp
git commit -m "Simplify hermes-node.cpp to argument parsing + runHermesNode()"
```

---

### Task 14: Final verification

**Step 1: Run full test suite**

Run: `cmake --build cmake-build-asan --target check-hermes-node`
Expected: All tests pass.

**Step 2: Verify no stale globals**

Run: `grep -rn '^static.*s_' lib/bindings/ | grep -v 'sigint\|s_previous'`
Expected: No output.

**Step 3: Manual smoke tests**

Run: `cmake-build-asan/bin/hermes-node -e "console.log('hello'); setTimeout(() => console.log('world'), 100)"`
Expected: prints "hello" then "world".

Run: `cmake-build-asan/bin/hermes-node` (REPL)
Expected: Shows `> ` prompt, can type `1+1` and get `2`.

Run: `echo "const fs = require('fs'); console.log(fs.readdirSync('.').length > 0 ? 'PASS' : 'FAIL')" > /tmp/test_rt.js && cmake-build-asan/bin/hermes-node /tmp/test_rt.js`
Expected: prints "PASS".

**Step 4: Verify hermes-node.cpp is small**

Run: `wc -l tools/hermes-node/hermes-node.cpp`
Expected: ~60-80 lines.

Run: `wc -l lib/runtime/hermes_node_runtime.cpp`
Expected: ~900-1000 lines (the bootstrap logic moved here).
