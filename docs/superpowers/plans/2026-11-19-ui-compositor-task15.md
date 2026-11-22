# Task 15: lifecycle and CLOSING, written out (15b-15f)

Expands the paragraph "Task 15 -- Lifecycle and CLOSING" of
`docs/superpowers/plans/2026-09-21-ui-compositor.md`. Task 15a (the split of
`ui_module.cpp` into `ui_module.cpp`, `ui_platform.cpp`, `ui_test_hooks.cpp`
and `ui_internal.h`) discharges obligation (1) and is not planned here. Code
below is named by function and by the file it lives in after that split.

Baseline before 15b: 449 unit tests, 220 JS tests, 4 unsupported.

Commands are the ones Slice C used: build `hermes-node-ui hnui_demo_addon
x11_window_driver`, run named lit tests with
`.superpowers/sdd/2026-09-21-ui-compositor/runlit.sh <absolute path>...`,
and before every commit `./utils/format.sh -f` then
`cmake --build cmake-build-asan --target check-hermes-node`. After any edit
near `ui_module_nowindow.cpp`'s interfaces, also run the hand compile from
the 15a brief (`clang++ -std=c++17 -fsyntax-only ... ui_module_nowindow.cpp`),
since no Linux build compiles that file.

---

## Design

### (a) Where the exit paths are intercepted

No hook exists today between `lib/process`, `lib/bindings`, `lib/runtime`
and an embedder module. `'exit'` is emitted from three places, and all three
are verified against the current code:

| Path | Where `'exit'` is emitted | What follows |
|---|---|---|
| `process.exit()` | `processExit` (`lib/process/node_process.cpp`), the block guarded by `!processExiting && thisArg != nullptr` | `flushPendingWrites(exitLoop)`, `uv_tty_reset_mode()`, `_exit(code)` |
| fatal exception | `process._fatalException` (`libjs/process-events.js`), `process.emit('exit', 1)` inside `if (!process._exiting)` | returns false to `triggerUncaughtException` (`lib/bindings/node_errors.cpp`): report, `fflush`, `uv_tty_reset_mode()`, `_exit(1)`. No loop pump. |
| runtime return | step 15 of `runHermesNode` (`lib/runtime/hermes_node_runtime.cpp`), after `markProcessExiting()` | `flushPendingWrites`, then step 16 cleanup ending in `eventLoop.close()` |

The third is the "abnormal return" path: with a window requested, step 15
is reached only when step 13 set `exitCode` (the entry script threw), because
otherwise the referenced handle keeps step 14's `uv_run` from returning.

The smallest mechanism that covers all three is one process-wide pair of
function pointers in `lib/process`, set by `main()` before the JS thread
exists (the same ordering argument `setUiModuleHost` relies on):

```cpp
// include/hermes/node-compat/process/node_process.h
enum class ExitReason { kExitCall, kFatalException, kRuntimeReturn };

struct ProcessExitHooks {
  /// Called before 'exit' listeners run, on every path that emits 'exit'.
  /// May be called more than once (a nested process.exit()); must be
  /// idempotent. Runs on env's JS thread.
  void (*beforeExitListeners)(napi_env env, ExitReason reason) = nullptr;
  /// Called by runHermesNode after step 15 (listeners ran, status settled,
  /// stdio flushed) and before step 16 tears anything down. May not return.
  void (*beforeRuntimeCleanup)(napi_env env, int exitCode) = nullptr;
};

/// One embedder per process. Call before any runtime starts; not
/// synchronized.
void setProcessExitHooks(const ProcessExitHooks &hooks);
void runBeforeExitListenersHook(napi_env env, ExitReason reason);
void runBeforeRuntimeCleanupHook(napi_env env, int exitCode);
```

Call sites:

1. `processExit`: `runBeforeExitListenersHook(env, ExitReason::kExitCall)`
   after the argument has been validated and the code computed, **before**
   the `!processExiting` block and unconditionally (a detached
   `const e = process.exit; e()` has no `thisArg` and emits nothing, but
   still pumps the loop in `flushPendingWrites`).
2. `process._fatalException`: JavaScript emits `'exit'` there, so the hook
   needs a JS-callable entry. `node_process.cpp` installs a native
   `process._preExit()` (beside `exit`), whose body is
   `runBeforeExitListenersHook(env, ExitReason::kFatalException)`.
   `_fatalException` calls it after `process.exitCode = 1` and before
   `process.emit('exit', 1)`, inside the existing `try`.
3. Step 15 of `runHermesNode`:
   `runBeforeExitListenersHook(env, ExitReason::kRuntimeReturn)` immediately
   after `markProcessExiting()`.
4. Between step 15's closing brace and step 16:
   `runBeforeRuntimeCleanupHook(env, exitCode)`.

Paths that end the process **without** emitting `'exit'` and without pumping
the loop need no CLOSING and are left alone: the `triggerUncaughtException`
binding callback in `node_errors.cpp` (unhandled rejections; measured, an
async function that throws after `await createWindow()` exits 1 through it
and no `'exit'` listener runs -- that is tracker issue `01a1d5c0-d385`, not
this task), `onFatalException` in `hermes_node_runtime.cpp` (`abort()`),
`terminateGraphicsSession` and `uiSokolLog`'s panic.

### (b) The thread handshake

**The JS thread decides everything and the platform thread is not told.**
Evidence for why it cannot be otherwise:

- The worker's GL context is created on Sokol's own X display:
  `createSharedContext` in `lib/compositor-gpu/gl_backend.cpp` takes
  `display_ = glXGetCurrentDisplay()`. `_sapp_linux_run` in
  `external/sokol/sokol/sokol_app.h` ends, once `quit_ordered` is set, with
  `_sapp_call_cleanup()`, `_sapp_glx_destroy_context()`,
  `_sapp_x11_destroy_window()` and `XCloseDisplay()`. So `sapp_run()`
  returning while the JS thread is alive closes the display under a context
  that thread still has current.
- On macOS `sapp_run()` never returns; the quit path is
  `performClose:` -> `applicationWillTerminate:`, which exits 0 from the
  main thread and cannot carry a status.

Therefore:

- `sapp_quit()` and `sapp_request_quit()` are never called.
  `sapp_cancel_quit()` is called by the platform thread, inside `event_cb`
  (`uiHandlePlatformEvent`, `ui_platform.cpp`), for **every**
  `QUIT_REQUESTED`, unconditionally, including during CLOSING. That is
  already the code; this task keeps it and pins it (15e, case LATE).
- `sapp_run()` does not return and `cleanup_cb` (`onCleanup`) does not run,
  on any path. That is what the spec says for the closing path, and this
  design extends it to the abnormal-return path (see (f) 1).
- **Who calls `_exit()`**: always the JS thread. `processExit` for
  `process.exit()`, `win.close()` and an uncancelled `close`;
  `triggerUncaughtException` for the fatal path; and the new
  `uiBeforeRuntimeCleanup` (`ui_module.cpp`) for a runtime that returns
  with a window requested. Each restores the terminal first.
- CLOSING is a JS-thread state (`gLifecycle` in `ui_module.cpp`, no atomic).
  Entering it runs `uiEnterClosing(reason)`, in this order:
  1. `gTable->beginClosing()`: no further `acquire()` or `publish()`, and
     `TickMachine::shouldSendTick(SurfaceTable &)` answers false from then
     on (`anyFreeSlotLocked` checks `closing_` first). A frame whose GPU
     work is in flight is refused at publication and reclaimed by the
     existing code in `onProducerComplete`.
  2. Per-tick state off (`inFrameCallback`, `haveSlot`), so
     `hnui_frame_target` answers false even when `process.exit()` was
     called from inside a frame callback; then
     `FrameCallbackList::releaseAll` (obligation 3).
  3. `ClipboardRequests::failAll` and `uiFailPendingClipboardReads`
     (obligation 2).
  4. A pending test-hook capture is cancelled.
  5. `gDispatch->close()`: the synchronized sender shutdown. It drains what
     was already posted, so every posted function checks the state and
     refuses (see (d)); afterwards `post()` returns false on any thread.
     **`gDispatch` is closed, never deleted or nulled**: the platform
     thread reads the pointer with no synchronization.
  6. `releaseReferencedHandle()`.
  7. `uiRunTickCallback`, under the condition in (f) 5.
- The platform thread keeps presenting the last published surface until
  `_exit()`. The surface generation and the presenting wrappers are not
  torn down; `_exit()` abandons them, as the spec says of outstanding
  Promises and native operations.

### (c) What happens today (measured 2026-11-19, `cmake-build-asan`, under `utils/with-xvfb.py`)

| Scenario | Today |
|---|---|
| Real `WM_DELETE_WINDOW`, nothing ends the script | `close` is emitted, the window stays, the process runs on (`test/ui-real-events.js` CLOSE). With no listener the window cannot be closed. |
| `process.exit(0)` in a `keydown` listener with a frame requested and the tick already posted | exit 0, but stderr reads `EXIT-LISTENER 0` then `FRAME-CALLBACK-RAN` then `[ui] frame tick: 1 callback(s) ran, nothing drawn`: the frame callback runs after `'exit'` listeners, from `flushPendingWrites`. |
| Uncaught throw in a timer, `'exit'` listener sets `win.title` | exit 1, listener runs, and `[ui] command title: readback='in-exit'`: the command issued by the listener is applied. |
| `createWindow(...)` then a synchronous `throw` | error printed, `EXIT-LISTENER 1`, then `[ui] window ready` **after** it (`onSharedContextReady` runs inside `UvEventLoop::close()`'s `uv_run`), then a hang: `timeout` kills it, status 124. |
| `HERMES_NODE_UI_GL_ROUTE=bogus`, rejection handled, script returns | `REJECTED ...` printed, then a hang (124): nothing releases the referenced handle after a rejection. |

### (d) The refused-`post()` edges (obligation 7)

`OwnerDispatch::post()` returns false once `close()` ran. Each call site,
what it does then, and what proves it. All trace lines are `uiTrace` lines
(stderr, prefixed `[ui] `).

| Call site | After the dispatcher is closed | Trace | Proof |
|---|---|---|---|
| `onFrame` (`ui_platform.cpp`), the tick post | Nothing is repaired. The machine may stay in `kTickQueued`; no tick can ever be delivered again, so its state is moot and **no `TickMachine` entry point is added**. In steady state the edge is unreachable, because step 1 above precedes step 5 and a closing table withholds the tick; what remains is a window of a few instructions. | `frame tick: post refused (dispatcher closed)` | GTest `TickMachineTest.NoTickIsSentOnceTheTableIsClosing` (15b). No lit test: not reachable deterministically. |
| `postEventDispatch` (`ui_module.cpp`) | The flag is put back as today; the events stay queued and are never delivered. | `event dispatch: post refused (dispatcher closed)` | `test/ui-exit-paths.js` COMMANDS (15c) and `test/ui-close.js` LATE (15e). |
| `maybeCapture` (`ui_platform.cpp`), both posts | The capture's Promise stays unsettled. The JS side has already cancelled the request (step 4), so `gCapture.pending()` is false and the platform thread stops looking. | `capture: post refused (dispatcher closed)`; and on the JS thread `capture: abandoned (closing)` | COMMANDS asserts the second line. The first is a race window only. |
| `onInit` (`ui_platform.cpp`), the posts of `onSharedContextReady` and `onStartupFailed`; `uiRunPlatformLoop`'s post of `onStartupFailed` | The window's readiness is never delivered; `gPresentReady` stays false and the platform thread presents its clear colour until `_exit()`. | `init_cb: post refused (dispatcher closed)`; `startup failure: post refused (dispatcher closed)` | `test/ui-exit-paths.js` THROW (15d), as one arm of an alternation. |
| `onSharedContextReady` (`ui_module.cpp`), the post of `openEventDelivery` | Unreachable by construction (same thread, state checked on entry); checked and traced anyway. | `event delivery: post refused (dispatcher closed)` | none; stated. |
| The `SurfaceTable` destroy callback built in `onSharedContextReady` | Already traced (`generation N: destruction could not be posted ...`); unchanged. | existing | none new. |

The other half of the same edge: a function **accepted** before `close()` is
run by `close()`'s drain. Each posted function therefore starts with a
state check and refuses:

| Function (`ui_module.cpp` unless noted) | Trace when refused |
|---|---|
| `runFrameTick` | `frame tick: refused (closing)` |
| `runEventDispatch` | `event dispatch: refused (closing)` |
| `openEventDelivery` | `event delivery: not opened (closing)` |
| `onSharedContextReady`, `onStartupFailed` | `window readiness: refused (closing)` |
| `onCaptureFinished` (`ui_test_hooks.cpp`) | `capture: result dropped (closing)` |

`destroyGeneration` is not guarded: freeing a generation's textures on the
owner thread is correct at any time.

### (e) Existing window tests and how each ends

`process.exit()` with a live window stays a supported way to end, and most
tests keep it: their subject is not closing, and together they are the
coverage of "exit with frames, reads and events in flight". Changed:

| Test | Change | Reason |
|---|---|---|
| `test/ui-real-events.js` CLOSE | the `close` listener calls `e.preventDefault()`; still ends through `process.exit()` | It asserts the window survives a real close request, which is now true only when cancelled. It keeps pinning `sapp_cancel_quit()`. |
| `test/ui-real-events.js` DEMO run | unchanged RUN line; `demo.js` loses its `close` listener | The demo now ends by the default action of a real `WM_DELETE_WINDOW` with no listener at all. |
| `test/ui-events.js` CLOSE | the listener calls `e.preventDefault()` | The injected `QUIT_REQUESTED` would otherwise end the test at that point. |
| `test/ui-commands.js` | ends with `win.close()` | `close` is one of the window's commands; this is the `win.close()` ending in an otherwise unrelated test. |
| `test/fixtures/hnui-demo/demo.js` conformance mode (`test/ui-conformance.js`) | ends with `win.close()` | The worked example should end the way a program would. |
| every other window test | ends through `process.exit()` as now | Subject is not closing. Only comments citing the hang, Ruling 59 or the tracker id are corrected (15f). |

New: `test/ui-close.js` (real `WM_DELETE_WINDOW`, `win.close()`),
`test/ui-exit-paths.js` (`process.exit()`, fatal, runtime return),
`test/ui-exit-tty.js` (terminal restore on the new `_exit()` site).

### (f) Where the spec cannot be met as written, or is silent

1. **Abnormal return.** The spec says the pre-cleanup hook "closes the UI
   handle, and tells the main thread to tear the window down and exit with
   the runtime's code". Not done: tearing the window down means `sapp_run()`
   returning, which closes the display under the JS thread's GL context (b),
   and on macOS cannot carry a status. **Choice:** the hook
   (`uiBeforeRuntimeCleanup`) restores the terminal and calls
   `_exit(exitCode)` on the JS thread, exactly as the other two paths end.
   Step 16 never runs in a process that requested a window. Spec sentence to
   replace the last sentence of "Abnormal return of the runtime":
   > So the module registers two lifecycle hooks with the runtime. The first
   > runs before `'exit'` listeners on every path and enters CLOSING. The
   > second runs when `runHermesNode` has emitted `'exit'` and flushed, and
   > before it cleans anything up: it restores the terminal and calls
   > `_exit()` with the runtime's code. The window is never torn down by
   > Sokol: `sapp_run()` returning would close the X display the JS thread's
   > GL context was created on.
2. **The `quit` command** in "Commands (JS -> main ...)" is not implemented
   and is removed from that list; "quitting only when JS asks" under
   "Failure splits in two" becomes "until the process ends". Closing is
   decided and carried out on the JS thread.
3. **A rejected `createWindow` (state FAILED)** left the referenced handle
   in place; the spec does not say who releases it. **Choice:**
   `rejectWith` releases it, so a script that handles the rejection and
   returns ends, through the hook in 1. The input queue still growing after
   a late rejection (tracker `01a1d5c0-d3b1`) is not addressed.
4. **How a listener cancels `close`.** The spec says "cancelable" and no
   more. JavaScript: the `close` payload gains `preventDefault()`, the DOM
   name. Native: `hnui_cancel_close` (below). Sentence to add to "The JS
   API", Events:
   > A `close` listener cancels by calling `event.preventDefault()`
   > synchronously, while the event is being delivered; a later call does
   > nothing. With no listener, or none that cancels, the window closes and
   > the process ends as if `process.exit()` had been called with no
   > argument.
5. **Draining ticks on entering CLOSING** (obligation 2) runs every queued
   `nextTick` and promise callback, which Node's `process.exit()` does not
   do. **Choice:** `uiRunTickCallback` runs only when
   `uiFailPendingClipboardReads` rejected at least one Promise, and never
   when the reason is `fatal` (after an uncaught exception no further
   callback runs in Node, and a throwing rejection handler there would
   replace the error being reported). The rejected Promises are settled
   either way.
6. **"Fails pending synchronous clipboard requests."** A synchronous read
   blocks the only thread that can enter CLOSING, so none can be pending at
   that moment. `failAll` is still called (obligation 2, and it refuses a
   reply in flight); what a program can observe is that
   `hnui_read_clipboard_sync` called from an `'exit'` listener returns
   `HNUI_ERROR` at once instead of waiting out its timeout. The spec's
   sentence gains "of both kinds" and that clause.
7. **What "unavailable" means for a command.** Refused commands are dropped
   and traced, never thrown and never printed: an `'exit'` listener that
   restores the cursor must not have the rest of its body skipped by an
   exception `processExit` would swallow. `requestAnimationFrame` (both
   forms) returns 0 and registers nothing; `ui.readClipboard()` returns a
   Promise already rejected with `ERR_CLIPBOARD_FAILED`;
   `createWindow` throws.
8. **`win.close()`** does not emit `close`: that event means the window
   system asked. It enters CLOSING and calls `process.exit()` (the property,
   with no argument, so `process.exitCode` is honoured). If a program has
   replaced `process.exit` with something that returns, `win.close()`
   returns with the window in CLOSING and nothing recovers it.
9. **CLOSED** is the `_exit()`; no code observes it and it has no value in
   the state enum. The window stays on screen while `'exit'` listeners run.

### The cancel API

```c
/* hnui.h, after hnui_event_dispatch_quiescent */
bool hnui_cancel_close(hnui_window *w);
```

- Returns **true** when called while an `HNUI_EVENT_CLOSE_REQUESTED` event
  is being delivered for `w` (from a native listener, or from native code
  reached from a JavaScript `close` listener); the close is then cancelled.
  Calling it again during the same event is harmless and also returns true.
- Returns **false** at any other time, for a NULL `w`, and once the window
  is CLOSING. A wrong-thread call on a real window aborts naming the
  function, like every checked entry point.
- `bool`, by analogy with `hnui_frame_target` ("false outside a frame
  callback is a legitimate answer").
- **No `HNUI_ABI_VERSION` bump.** `hnui_abi.h` bumps only for "a change an
  already-compiled addon cannot tolerate"; an added export is tolerated, and
  an addon that calls it against an older host fails at `dlopen` with an
  undefined symbol, which is loud. `test/ui-version-check.js`'s
  `hostApiVersion() === 1` stays. `hnui_event` is untouched (Ruling 51).

---

## Review focus

Failure modes most likely to bite that nothing covers today, each assigned:

1. A quit request reaches Sokol uncancelled (one arriving during CLOSING is
   the likely one): `sapp_run()` returns and closes the display under a live
   JS thread. -- 15e, `test/ui-close.js` LATE.
2. `OwnerDispatch::close()` is called from inside a function that
   dispatcher is itself running (an uncancelled `close` is delivered from a
   posted dispatch; `process.exit()` from a frame callback). -- 15b,
   `OwnerDispatchTest.CloseFromInsideAPostedFunctionRunsTheRestAndRefusesLaterPosts`.
3. The exit status is lost on a new path: an uncancelled close exits 0
   whatever `process.exitCode` says, or a thrown entry script exits 0. --
   15e DEFAULT (status 7) and WINCLOSE (9); 15d THROW (1) and RETURN (5).
4. The new `_exit()` site drops output or leaves the terminal raw. -- 15d,
   THROW (eight lines through a pipe) and `test/ui-exit-tty.js`.
5. `process.exit()` from inside a frame callback: a slot is acquired, the
   batch is half run, and JS references are released while their batch is
   on the stack. -- 15b, `test/ui-exit-paths.js` FRAME, under ASAN.

---

## Task 15b: exit hooks, and CLOSING on the `process.exit()` and fatal paths

Discharges obligations (3) and (8).

**As built** (commit `Enter CLOSING before 'exit' listeners on process.exit
and fatal paths`). LISTENER waits one frame before its first KEY_DOWN:
injected straight after the `await`, that key can be delivered by
`openEventDelivery()` while its own posted dispatch is still queued in the
same pass, leaving no dispatch for CLOSING to refuse; the tick is posted
after `openEventDelivery()`, so the wait is ordered, not timed. FRAME's
mutation (a) has to keep **both** `inFrameCallback` and `haveSlot` set,
since `hnui_frame_target` refuses on either. `LISTENER-NOT:
SECOND-KEY-DELIVERED` pins almost nothing (the line was absent before
implementation too); the refusal trace is what pins the dispatch.
`ui_module.h` declares `enum class ExitReason;` opaquely, because
`node_process.h` pulls in `<uv.h>` and the no-window hand compile has no
libuv include path. `rejectWith` sets FAILED here but releases nothing
until 15d.

**Files:**
- Modify: `include/hermes/node-compat/process/node_process.h`,
  `lib/process/node_process.cpp`, `libjs/process-events.js`,
  `tools/hermes-node-ui/main.cpp`, `ui_module.h`, `ui_module.cpp`,
  `ui_internal.h`, `ui_platform.cpp`, `ui_test_hooks.cpp`, `hnui_api.cpp`,
  `hnui_window.h` (comment on `frameCallbacks` only),
  `test/fixtures/hnui-demo/demo_addon.c`, `unittests/CompositorTest.cpp`,
  `unittests/NodeProcessTest.cpp`
- Create: `test/ui-exit-paths.js`

**Interfaces produced:**

```cpp
// node_process.h -- as in Design (a). This task adds call sites 1 and 2.
// process._preExit()  (native, installed beside process.exit)

// ui_module.h
enum class UiCloseReason { kWindowClose, kExitCall, kFatalException,
                           kRuntimeReturn };          // traced as close, exit, fatal, return
/// Enter CLOSING. JS thread only. Idempotent: a second call returns at once.
void uiEnterClosing(UiCloseReason reason);
/// True from uiEnterClosing() on. JS thread only.
bool uiClosing();
/// ProcessExitHooks::beforeExitListeners. Ignores an env that is not the
/// window's (when a window was requested).
void uiBeforeExitListeners(napi_env env, ExitReason reason);

// ui_module.cpp (file-local)
enum class UiLifecycle { kIdle, kRequested, kRunning, kFailed, kClosing };
UiLifecycle gLifecycle;   // JS thread only. kRequested in createWindow,
                          // kRunning where gPresentReady is stored.

// test hooks (ui_test_hooks.cpp), exported under HERMES_NODE_UI_TEST_HOOKS
//   ui.__waitPlatformFrames(n) -> boolean
// Blocks the calling thread until the platform thread has returned from n
// onFrame() calls that BEGAN after this call was made; false after 10 s.
// Works in every lifecycle state: it is a mutex, a condition variable and
// two counters, not a command.
void uiTestHookFrameBegan();   // platform thread, first line of onFrame
void uiTestHookFrameEnded();   // platform thread, on every return of onFrame

// demo_addon.c
//   frameTargetAvailable() -> boolean   (hnui_frame_target's answer, now)
```

`main()` calls `setProcessExitHooks({uiBeforeExitListeners, nullptr})` under
`HERMES_NODE_UI_HAVE_WINDOW`, beside `setUiModuleHost`. The no-window build
sets nothing.

**Trace lines (exact):**

```
lifecycle: CLOSING (reason=<close|exit|fatal|return>)
lifecycle: frame callbacks released=<n>
lifecycle: dispatcher closed
lifecycle: event-loop handle released
frame tick: refused (closing)
event dispatch: refused (closing)
event delivery: not opened (closing)
window readiness: refused (closing)
capture: result dropped (closing)
requestAnimationFrame: refused (closing)
```

**What `uiEnterClosing` does in this task** (steps 3, 4 and 7 of Design (b)
arrive in 15c): set `gLifecycle = kClosing` first; trace the CLOSING line
only if `uiWindowRequested()`; then steps 1, 2, 5, 6, each null-guarded,
because CLOSING can be entered in `kRequested` before `gTable`, `gWindow`
or the surfaces exist. `releaseReferencedHandle` gains the handle trace,
printed only when it actually closes one. In `kIdle` it sets the state and
does nothing else, so that a `createWindow` called from an `'exit'` listener
throws (`createWindow: the process is exiting.`) rather than opening a
window into a process on its way out.

Refusals added here: `runFrameTick`, `runEventDispatch`,
`openEventDelivery`, `onSharedContextReady`, `onStartupFailed`,
`onCaptureFinished` return on `uiClosing()` with their trace line;
`requestAnimationFrame` (`ui_module.cpp`) and
`hnui_request_animation_frame` (`hnui_api.cpp`) return 0 and register
nothing; `hnui_frame_target` returns false (it already does once
`inFrameCallback` is cleared; no new check).

- [x] **Step 1: Write the failing unit tests.**
  - `unittests/NodeProcessTest.cpp`, `ProcessExitHooksTest.HooksReceiveWhatTheCallSitePassed`:
    with no hooks set both `run...` functions return; after
    `setProcessExitHooks`, `runBeforeExitListenersHook(nullptr, ExitReason::kFatalException)`
    reaches the hook with that reason and
    `runBeforeRuntimeCleanupHook(nullptr, 7)` with 7. Reset the hooks at the
    end of the test. *Mutation: pass `kExitCall` regardless.*
  - `unittests/CompositorTest.cpp`,
    `OwnerDispatchTest.CloseFromInsideAPostedFunctionRunsTheRestAndRefusesLaterPosts`
    (review focus 2): on a real `uv_loop_t`, post A, B, C. B calls
    `dispatch.close()` and then `post(D)`. Run the loop. Expect A, B, C ran
    once each in that order, D did not run and its `post` returned false,
    the loop then closes cleanly (`uv_loop_close` returns 0 after one more
    `uv_run`), and ASAN is silent. This may pass as written; if it does,
    make the mutation to prove it pins something. *Mutation: in
    `OwnerDispatch::updateAsyncRef`, delete the `if (closed_) return;` --
    the outer drain then touches a null handle.*
  - `TickMachineTest.NoTickIsSentOnceTheTableIsClosing`: `requestFrame()`,
    `table.beginClosing()`, `shouldSendTick(table)` is false, state stays
    `kIdle`. Expected to pass at once. *Mutation: delete the `closing_`
    check in `SurfaceTable::anyFreeSlotLocked`.*
- [x] **Step 2: Run them.** The first fails to compile (`ProcessExitHooks`
  does not exist). Implement the three functions in `node_process.cpp` and
  rerun; make the three mutations, watch each fail, revert.
- [x] **Step 3: Add `frameTargetAvailable()` to the addon and
  `__waitPlatformFrames` to the test hooks.** `onFrame` has several
  returns: use one scope guard at its top, active only when
  `uiTestHooksEnabled()`.
- [x] **Step 4: Write the failing lit test** `test/ui-exit-paths.js`
  (`REQUIRES: ui, ui-gpu` and `REQUIRES: display`). `process.argv[2]` is
  the case, `process.argv[3]` the addon. One RUN group per case, in this
  shape, with `<C>` the case name in upper case and `<c>` in lower:

  ```
  // RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s <c> %hnui_demo_addon > %t.<c>.out 2> %t.<c>.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=<C>-CODE %s
  // RUN: %FileCheck --check-prefix=<C> %s < %t.<c>.out
  // RUN: %FileCheck --check-prefix=<C>-TRACE --implicit-check-not=AddressSanitizer %s < %t.<c>.err
  ```

  Every case creates a 320x240 window titled `exit paths` and installs
  `process.on('exit', (code) => console.log('EXIT code=' + code + ' rafRefused=' + (ui.requestAnimationFrame(() => {}) === 0)))`
  unless it says otherwise. The `rafRefused` field is the in-process
  evidence that CLOSING preceded the listener; it does not depend on how
  stdout and stderr interleave.

  - **LISTENER** (obligation 8). A `keydown` listener, on its first call:
    `ui.requestAnimationFrame(() => console.log('FRAME-AFTER-EXIT'))`,
    injects a second `KEY_DOWN`, calls `ui.__waitPlatformFrames(1)`, then
    `process.exit(0)`. On a second call it prints `SECOND-KEY-DELIVERED`.
    The script injects the first `KEY_DOWN`. The barrier is what makes the
    case deterministic: the frame that began after it has drained the
    injector (posting a dispatch) and, at its end, posted the tick, so both
    are queued when `process.exit()` runs.

    ```
    // LISTENER: EXIT code=0 rafRefused=true
    // LISTENER-NOT: FRAME-AFTER-EXIT
    // LISTENER-NOT: SECOND-KEY-DELIVERED
    // LISTENER-CODE: EXITCODE 0
    // LISTENER-TRACE: lifecycle: CLOSING (reason=exit)
    // LISTENER-TRACE-DAG: event dispatch: refused (closing)
    // LISTENER-TRACE-DAG: frame tick: refused (closing)
    // LISTENER-TRACE: lifecycle: dispatcher closed
    // LISTENER-TRACE: lifecycle: event-loop handle released
    // LISTENER-TRACE: requestAnimationFrame: refused (closing)
    // LISTENER-TRACE-NOT: callback(s) ran
    ```

    *Mutations: (a) delete the `uiClosing()` return in `runFrameTick` --
    `FRAME-AFTER-EXIT` is printed; (b) delete the
    `runBeforeExitListenersHook` call in `processExit` -- `rafRefused=false`
    and no CLOSING line.*
  - **FRAME** (review focus 5). Registers, in order, a JS callback that
    prints `CB1` and calls `process.exit(4)`, a JS callback that prints
    `CB2-RAN`, and `addon.requestNativeFrame('n1')`. The `'exit'` listener
    prints `EXIT code=<code> target=<addon.frameTargetAvailable()>`.

    ```
    // FRAME: CB1
    // FRAME-NEXT: EXIT code=4 target=false
    // FRAME-NOT: CB2-RAN
    // FRAME-CODE: EXITCODE 4
    // FRAME-TRACE: lifecycle: CLOSING (reason=exit)
    // FRAME-TRACE: lifecycle: frame callbacks released=2
    ```

    *Mutations: (a) do not clear `inFrameCallback` in `uiEnterClosing` --
    `target=true`; (b) skip `releaseAll` -- the `released=2` line is gone.*
  - **FATAL.** `setTimeout(() => { throw new Error('boom in timer'); }, 1)`
    once the window exists.

    ```
    // FATAL: EXIT code=1 rafRefused=true
    // FATAL-CODE: EXITCODE 1
    // FATAL-TRACE: lifecycle: CLOSING (reason=fatal)
    // FATAL-TRACE: Error: boom in timer
    ```

    *Mutation: delete `process._preExit()` from `_fatalException` --
    `rafRefused=false`.*
- [x] **Step 5: Run it.** Expected before implementation: LISTENER prints
  `FRAME-AFTER-EXIT` and `rafRefused=false` (the behaviour measured in
  Design (c)); FRAME fails on `frameTargetAvailable`'s answer and the
  missing trace; FATAL prints `rafRefused=false`.
- [x] **Step 6: Implement** call sites 1 and 2, `process._preExit`, the
  state, `uiEnterClosing`, the six guards and the refusals. Replace the
  "Nothing releases it yet" paragraph above `takeReferencedHandle`, the
  "Known edge, owned by Task 15's teardown" comment in `onFrame`, and the
  "Entries still here at exit are not released" sentence in `hnui_window.h`
  with what is now true. `releaseAll`'s callback is
  `releaseFrameCallback(gWindow, cb)`; count the calls for the trace.
- [x] **Step 7: Run to verify it passes, then
  `test/test-process-exit-event.js`, `test/test-process-exit-code.js`,
  `test/test-uncaught-exception-async.js` and `test/ui-windowless.js`**,
  which run the changed runtime code with no hooks and with hooks and no
  window.
- [x] **Step 8: Make each named mutation, watch its case fail, revert.**
- [x] **Step 9: Format, run `check-hermes-node`, commit**
  (`Enter CLOSING before 'exit' listeners on process.exit and fatal paths`).

---

## Task 15c: what CLOSING fails and refuses

Discharges obligations (2) and (7).

**As built** (two commits: `Capture process._preExit once in the fatal
path`, then `Fail clipboard reads and refuse commands once the window is
closing`). `libjs/process-events.js` captures `process._preExit` at load,
because a program that deleted or replaced the property would otherwise
skip every `'exit'` listener inside a swallowing `try`; two cases in
`test/test-uncaught-exception-async.js` pin it. Two trailing `-NOT` checks
in this plan were blind and were rewritten: LISTENER gained a fourth
FileCheck run, `LISTENER-TICKS` with `--implicit-check-not='callback(s)
ran'`, and COMMANDS' `late title` exclusion became an
`--implicit-check-not`. `hnui_read_clipboard_sync` creates its slot before
the push (the command carries the slot's id) and returns `HNUI_ERROR` at
once when the push is refused. `failAll` and `uiFailPendingClipboardReads`
return counts, traced as `clipboard reads failed sync=<n> promise=<n>`.

**Files:**
- Modify: `include/hermes/node-compat/compositor/channels.h`,
  `lib/compositor/channels.cpp`, `unittests/CompositorTest.cpp`,
  `tools/hermes-node-ui/ui_module.h`, `ui_module.cpp`, `ui_events.h`,
  `ui_events.cpp`, `ui_platform.cpp`, `ui_test_hooks.cpp`, `ui_internal.h`,
  `hnui_api.cpp`, `test/ui-exit-paths.js`, `test/ui-clipboard.js` (the
  FAIL-PENDING comment that calls the hook "a stand-in for Task 15's
  shutdown")

**Interfaces consumed:** `uiEnterClosing`, `uiClosing`,
`__waitPlatformFrames` (15b).

**Interfaces produced:**

```cpp
// channels.h
size_t ClipboardRequests::failAll(ClipboardStatus status);   // was void: how many it completed

// ui_events.h
size_t uiFailPendingClipboardReads(napi_env env);            // was void: how many it rejected

// ui_module.h
/// False, with nothing queued, once the window is CLOSING.
bool uiPushCommand(Command command);                         // was void

// ui_test_hooks.cpp, declared in ui_internal.h. JS thread.
void uiTestHooksAbandonCapture();   // cancels gCapture, forgets gCaptureDeferred
```

**Trace lines (exact):**

```
lifecycle: clipboard reads failed sync=<n> promise=<n>
capture: abandoned (closing)
command refused (closing): <title|fullscreen|icon|cursor|custom-cursor|show-cursor|lock-pointer|read-clipboard|write-clipboard>
frame tick: post refused (dispatcher closed)
event dispatch: post refused (dispatcher closed)
capture: post refused (dispatcher closed)
init_cb: post refused (dispatcher closed)
startup failure: post refused (dispatcher closed)
event delivery: post refused (dispatcher closed)
```

`uiEnterClosing` gains steps 3, 4 and 7 of Design (b). The clipboard line
prints the two **returned** counts. Step 7 is
`if (promiseCount > 0 && reason != UiCloseReason::kFatalException) uiRunTickCallback(gEnv);`
and is the last thing the function does, because it can run program code
and, if that throws with no listener, does not return.

Callers of `uiPushCommand`: the JS window methods and the four `void`
`hnui_*` commands ignore the result; `hnui_read_clipboard_sync` returns
`HNUI_ERROR` without creating a slot or waiting; `uiReadClipboard` rejects
the Promise it just created with `ERR_CLIPBOARD_FAILED` and the message
`readClipboard: the window is closing.` instead of recording it in
`gPendingReads`.

Each `post()` in Design (d)'s first table has its result checked and its
line traced. No other behaviour changes at those sites.

- [x] **Step 1: Extend the unit test**
  `ClipboardRequestTest.FailAllCompletesEveryWaiterAndALaterAnswerIsRefused`
  to expect the count (2 for its two live waiters, 0 on a second call).
  Fail (does not compile), implement, pass.
- [x] **Step 2: Add two cases to `test/ui-exit-paths.js`**, RUN groups in
  the 15b shape.
  - **READS** (obligation 2). The `'exit'` listener pushes `'exit'` onto an
    array, prints `ORDER <array joined by ,>`, then prints
    `SYNC status=<addon.readClipboardSync(5000).status>`. A 1 ms timer
    calls `ui.readClipboard().then(() => order.push('resolved'), (e) => order.push('rejected:' + e.code))`
    and, in the same turn, `process.exit(0)`. The Promise is pending at
    that point by construction: its reply can only be delivered by a
    dispatch, and the JS thread has not returned to the loop.

    ```
    // READS: ORDER rejected:ERR_CLIPBOARD_FAILED,exit
    // READS-NEXT: SYNC status=HNUI_ERROR
    // READS-CODE: EXITCODE 0
    // READS-TRACE: lifecycle: CLOSING (reason=exit)
    // READS-TRACE: clipboard read promise #1: failed by uiFailPendingClipboardReads
    // READS-TRACE: lifecycle: clipboard reads failed sync=0 promise=1
    // READS-TRACE: command refused (closing): read-clipboard
    ```

    *Mutations: (a) drop the `uiRunTickCallback` call -- `ORDER exit`;
    (b) drop the `uiFailPendingClipboardReads` call -- `promise=0` cannot be
    printed without it, and `ORDER exit`; (c) make `uiPushCommand` accept
    while closing -- the sync read is answered by the platform thread and
    reports `HNUI_UNAVAILABLE`.* The `failAll` call has no observable of its
    own beyond the `sync=0` it returns (Design (f) 6); say so in the test's
    header rather than implying more.
  - **COMMANDS** (obligation 7, and "platform commands unavailable to
    `'exit'` listeners"). Before exiting: `win.title = 'early title'`,
    `ui.__waitPlatformFrames(1)`, and a `ui.__captureFrame` request that can
    never match (`publicationId: 999`, `giveUpAfterFrames: 0`), its Promise
    given a no-op `catch`. A 1 ms timer calls `process.exit(0)`. The
    `'exit'` listener sets `win.title = 'late title'`, calls
    `win.setCursor('ibeam')` and `ui.writeClipboard('late')`, injects a
    `KEY_DOWN`, and prints `BARRIER <ui.__waitPlatformFrames(1)>`.

    ```
    // COMMANDS: BARRIER true
    // COMMANDS-CODE: EXITCODE 0
    // COMMANDS-TRACE: command title: readback='early title'
    // COMMANDS-TRACE: lifecycle: CLOSING (reason=exit)
    // COMMANDS-TRACE: capture: abandoned (closing)
    // COMMANDS-TRACE: command refused (closing): title
    // COMMANDS-TRACE: command refused (closing): cursor
    // COMMANDS-TRACE: command refused (closing): write-clipboard
    // COMMANDS-TRACE: event dispatch: post refused (dispatcher closed)
    // COMMANDS-TRACE-NOT: late title
    ```

    The barrier in the listener is what makes the last two lines mean
    something: the frame that began after it drained the command queue (so
    an accepted `late title` would have been read back) and the injector
    (so the event was pushed and its post attempted). *Mutations: (a) let
    `uiPushCommand` accept while closing -- `readback='late title'`
    appears; (b) remove `gDispatch->close()` from `uiEnterClosing` -- the
    post is accepted and the `post refused` line is gone; (c) remove the
    `uiTestHooksAbandonCapture()` call -- its line is gone.*
- [x] **Step 3: Run to verify both fail** (READS: `ORDER exit`, no
  clipboard line; COMMANDS: `readback='late title'` present, measured in
  Design (c)).
- [x] **Step 4: Implement.** Remove the "only reachable once the dispatcher
  is closed (Task 15)" comments at the call sites this step changes, and
  the "Task 15 calls the two together" sentence in `ui_events.h`, replacing
  each with what the code now does.
- [x] **Step 5: Run to verify it passes, plus `test/ui-clipboard.js`,
  `test/ui-commands.js`, `test/ui-abi-sizes.js` and `test/ui-conformance.js`**
  (every caller of `uiPushCommand`).
- [x] **Step 6: Make each named mutation, watch it fail, revert.**
- [x] **Step 7: Format, run `check-hermes-node`, commit**
  (`Fail clipboard reads and refuse commands once the window is closing`).

---

## Task 15d: a runtime that returns, and a rejected createWindow

The pre-cleanup lifecycle hook.

**As built** (commit `End a windowed process when its runtime returns`).
THROW's trace check is split in two, because the alternation as planned was
satisfied by either arm with `gDispatch->close()` removed: an accepted
post is refused inside `close()`'s drain, before `lifecycle: dispatcher
closed`, and a refused post after it. THROW-TRACE now forbids `window
readiness: refused (closing)` after `dispatcher closed`, and THROW-ARM, a
separate run, keeps the alternation. `test/ui-exit-tty.js` runs the program
under `timeout --foreground` inside the pty session (`run-on-pty.py`'s child
calls `setsid()`, so an outer `timeout` would not reach it; the inner bound
is 25 s since 15e). `ui_host.h` had no "Task 15" comment; its `run()` doc
was rewritten instead. `rejectWith` traces its release as
`event-loop handle released (createWindow rejected)`. The windowless
mutation as named stays green, because `uiBeforeRuntimeCleanup`'s two early
returns overlap there; removing both fails it.

**Files:**
- Modify: `lib/runtime/hermes_node_runtime.cpp`,
  `tools/hermes-node-ui/main.cpp`, `ui_module.h`, `ui_module.cpp`,
  `ui_host.cpp` and `ui_host.h` (comments only: the two that say orderly
  shutdown "is Task 15"), `ui_platform.cpp` (the comment after `sapp_run`),
  `test/ui-exit-paths.js`, `test/ui-route-refusal.js`,
  `test/ui-windowless.js`
- Create: `test/ui-exit-tty.js`

**Interfaces consumed:** `ProcessExitHooks`, `uiEnterClosing`,
`__waitPlatformFrames`.

**Interfaces produced:**

```cpp
// ui_module.h
/// ProcessExitHooks::beforeRuntimeCleanup. Returns when no window was ever
/// requested (the windowless path ends through UiHost::run as before) or
/// when env is not the window's. Otherwise does not return:
/// std::fflush(nullptr), uv_tty_reset_mode(), _exit(exitCode).
void uiBeforeRuntimeCleanup(napi_env env, int exitCode);
```

**Trace lines (exact):**

```
lifecycle: FAILED
lifecycle: runtime returned, exiting with status <n>
```

`runHermesNode` gains call sites 3 and 4 of Design (a). `main()` passes
both hooks. `rejectWith` (`ui_module.cpp`) sets `gLifecycle = kFailed`,
traces `lifecycle: FAILED` and calls `releaseReferencedHandle()`; it does
not close the dispatcher. This is the fourth `_exit()` site in the tree
that restores the terminal first, and its comment says why it is safe to
skip step 16 (the reasons `processExit` gives) and why it must not return
(Design (b), first bullet list).

- [x] **Step 1: Add case THROW to `test/ui-exit-paths.js`.** No `await`:
  the script installs an `'exit'` listener that prints
  `EXIT-LINE <i> code=<code>` for `i` in 0..7 and then
  `BARRIER <ui.__waitPlatformFrames(1)>`, calls `ui.createWindow(...)`, and
  throws `new Error('boom after createWindow')` in the same turn. Its
  stdout goes through a pipe, because the flush is part of the claim:

  ```
  // RUN: timeout 120 %with-xvfb sh -c '{ HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s throw %hnui_demo_addon 2> %t.throw.err; echo "EXITCODE $?" > %t.throw.code; } | cat > %t.throw.out'
  // RUN: %FileCheck --check-prefix=THROW-CODE %s < %t.throw.code
  // RUN: %FileCheck --check-prefix=THROW %s < %t.throw.out
  // RUN: %FileCheck --check-prefix=THROW-TRACE --implicit-check-not='window ready' %s < %t.throw.err

  // THROW: EXIT-LINE 0 code=1
  // THROW: EXIT-LINE 7 code=1
  // THROW-NEXT: BARRIER true
  // THROW-CODE: EXITCODE 1
  // THROW-TRACE: Error: boom after createWindow
  // THROW-TRACE: lifecycle: CLOSING (reason=return)
  // THROW-TRACE: {{(init_cb: post refused \(dispatcher closed\)|window readiness: refused \(closing\))}}
  // THROW-TRACE: lifecycle: runtime returned, exiting with status 1
  ```

  The alternation is exact, not loose: the barrier in the listener returns
  only after a platform frame, `init_cb` precedes the first frame, and its
  post either was refused or had been accepted and was drained by
  `close()`. *Mutations: (a) delete the `runBeforeRuntimeCleanupHook` call
  -- the run ends at `timeout` with 124; (b) delete call site 3 -- no
  `reason=return` line; (c) move call site 4 above step 15's
  `flushPendingWrites` -- lines are missing from the piped stdout (if all
  eight still arrive, raise the count until the mutation fails, and keep
  that count); (d) remove `gDispatch->close()` from `uiEnterClosing` --
  neither arm of the alternation is printed.*
- [x] **Step 2: Add a third RUN group to `test/ui-route-refusal.js`**,
  `HERMES_NODE_UI_GL_ROUTE=bogus` with an extra argument `return`: the
  rejection handler prints `REJECTED ...`, sets `process.exitCode = 5` and
  returns instead of calling `process.exit(3)`. Needs no display.

  ```
  // RUN: timeout 60 env HERMES_NODE_UI_GL_ROUTE=bogus HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s return > %t.ret.out 2> %t.ret.err; echo "EXITCODE $?" | %FileCheck --check-prefix=RETCODE %s
  // RUN: %FileCheck --check-prefix=RET %s < %t.ret.out
  // RUN: %FileCheck --check-prefix=RETERR %s < %t.ret.err
  // RET: REJECTED {{.*}}unknown value for HERMES_NODE_UI_GL_ROUTE: 'bogus'
  // RETCODE: EXITCODE 5
  // RETERR: lifecycle: FAILED
  // RETERR: lifecycle: event-loop handle released
  // RETERR: lifecycle: runtime returned, exiting with status 5
  ```

  *Mutation: remove `releaseReferencedHandle()` from `rejectWith` -- 124,
  which is today's behaviour.*
- [x] **Step 3: Create `test/ui-exit-tty.js`** (`REQUIRES: ui, ui-gpu`; no
  display). Modelled on `test/test-tty-restore.js`'s `natural` case:

  ```
  // RUN: python3 %S/fixtures/tty/run-on-pty.py env HERMES_NODE_UI_GL_ROUTE=bogus %hermes-node-ui %s | %FileCheck %s
  // CHECK: exit status: 0
  // CHECK: RESULT: RESTORED
  ```

  The script calls `process.stdin.setRawMode(true)`, then
  `require('hermes-node:ui').createWindow().catch(() => {})`, and returns
  (unref stdin the way `test-tty-restore.js` does, so the loop drains).
  *Mutation: delete `uv_tty_reset_mode()` from `uiBeforeRuntimeCleanup` --
  `RESULT:` no longer says `RESTORED`.* If the test passes with the call
  deleted, the terminal is being restored by something else on this path;
  find out what before keeping the test.
- [x] **Step 4: Add a second RUN group to `test/ui-windowless.js`** with
  `HERMES_NODE_DEBUG_NATIVE=UI` and stderr captured: same script, same exit
  code as its first run, and `--implicit-check-not='lifecycle:'` over
  stderr. A script that never asks for a window must still return through
  `UiHost::run()`. *Mutation: remove the `uiWindowRequested()` test from
  `uiBeforeRuntimeCleanup` -- `lifecycle: runtime returned` is printed.*
- [x] **Step 5: Run all four; expect** THROW 124 with `window ready` in the
  trace, RET 124, the tty test `RESULT:` anything (it hangs first: the
  script never ends), windowless passing.
- [x] **Step 6: Implement.** Rewrite the three comments listed under Files
  so none says the shutdown is still to come.
- [x] **Step 7: Run to verify, make each mutation, revert.**
- [x] **Step 8: Format, run `check-hermes-node`, commit**
  (`End a windowed process when its runtime returns`).

---

## Task 15e: the cancelable close, `win.close()` and `hnui_cancel_close`

Discharges obligations (5) and (6), and the code half of (4). Closes
tracker issue `01a0d88a-6518` in this commit.

**As built** (commit `Close the window on an uncancelled close, and add
win.close()`). LATE waits one frame first, for LISTENER's reason. WINCLOSE's
mutation ("`win.close()` only enters CLOSING") does not end at `timeout`:
CLOSING released the referenced handle, the loop drains and the
runtime-return hook exits 9, so the planned trailing `-NOT` lines became
`--implicit-check-not`s and `close: ending the process through
process.exit()` was added to the trace. Two cases were added, REPLACED and
RFRAME, for a `process.exit` replaced by a function that returns; they
needed two refusals the plan does not name, `uiDispatchEvents` refusing
every event once CLOSING and `runFrameTick` reclaiming its slot instead of
submitting when a callback entered CLOSING. `uiCloseWindow` throws a
`TypeError` when `process.exit` is not a function, and an uncancelled
close's default action is skipped when the window is already CLOSING.
Design (e)'s test changes were made here, including the two `win.close()`
endings planned as 15f Step 1.

**Files:**
- Modify: `include/hermes/node-compat/ui/hnui.h`,
  `tools/hermes-node-ui/hnui_api.cpp`, `hnui_window.h`, `ui_module.h`,
  `ui_module.cpp`, `ui_events.cpp`, `ui_platform.cpp`
  (`uiHandlePlatformEvent`: comment, trace text unchanged, quit counter),
  `ui_test_hooks.cpp`, `ui_internal.h`,
  `test/fixtures/hnui-demo/demo_addon.c`, `demo_conformance.c`, `demo.js`,
  `test/ui-conformance.js`, `test/ui-events.js`, `test/ui-real-events.js`,
  `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`,
  `dz/issues/01a0d88a-6518-*.md` (through `dz close`)
- Create: `test/ui-close.js`

**Interfaces consumed:** `uiEnterClosing`, `uiClosing`,
`__waitPlatformFrames`.

**Interfaces produced:**

```c
/* hnui.h -- see "The cancel API" in Design for the contract. */
bool hnui_cancel_close(hnui_window *w);
```

```cpp
// hnui_window.h, owner thread only
bool inCloseEvent = false;     // true from before the first native listener
                               // of a kCloseRequested until emitToJs returns
bool closeCancelled = false;   // reset when inCloseEvent is set

// ui_module.h
/// The default action of a close: uiEnterClosing(kWindowClose), then call
/// the `exit` property of `process` with no argument. Does not return when
/// that exits. JS thread only. \return false with a JS exception pending
/// if the call threw.
bool uiCloseWindow(hnui_window *w);

// JS: the `close` payload is { preventDefault() }.  win.close()
// test hooks: ui.__waitPlatformQuitRequests(total) -> boolean
//   Blocks until the platform thread has executed `total` sapp_cancel_quit()
//   calls since startup; false after 10 s. Counted in uiHandlePlatformEvent,
//   after the call.
// demo_addon.c: armCloseCancel(n)   the event-log listener calls
//                                   hnui_cancel_close for the next n close
//                                   events and logs `native:cancel_close=<0|1>`
//               cancelCloseNow() -> boolean   hnui_cancel_close, called now
```

**Trace lines (exact):**

```
quit requested: cancelled, forwarded as close        (existing, unchanged)
close: cancelled
close: not cancelled
close: preventDefault outside a close event; ignored
close: ending the process through process.exit()
close: process.exit returned; the window stays in CLOSING
```

**`deliver` (`ui_events.cpp`) for `kCloseRequested`:** set `inCloseEvent`
and clear `closeCancelled` before the native listeners; clear
`inCloseEvent` when `emitToJs` returns; trace one of the two `close:`
lines; run `uiRunTickCallback` as for any event; close the handle scope;
then, if not cancelled, `uiCloseWindow(w)`, routing a false return through
`handleCallbackException`. A `close` listener that throws and is handled by
an `uncaughtException` listener has not cancelled.

`preventDefault` is one native function created with the `hnui_window` as
its data; `win.close()` is defined in `defineWindowCommands`. Both check
`uiClosing()` first and return undefined. The native entry point is
`requireOwnerThread`-checked like its neighbours.

- [x] **Step 1: Extend the addon** with `armCloseCancel` and
  `cancelCloseNow`, and `demo_conformance.c` plus `demo.js`'s conformance
  mode with one observation, placed after the
  `hnui_event_dispatch_quiescent next frame callback` line: call
  `hnui_cancel_close` outside any event, inject `QUIT_REQUESTED`, call it
  from the native listener, print
  `hnui_cancel_close outside=0 inside=1`. Add that line as a `CHECK-NEXT`
  in `test/ui-conformance.js`. (Injection is right here: this is the entry
  point's own bookkeeping, after the converter.)
- [x] **Step 2: Write the failing lit test** `test/ui-close.js`
  (`REQUIRES: ui, ui-gpu`, `REQUIRES: display`). `process.argv` is
  `<case> <addon> <driver>`. Close requests are real: the script calls
  `child_process.spawnSync(driver, ['--close', TITLE])`, the driver's
  one-shot mode, which waits for the window by title and sends
  `WM_PROTOCOLS`/`WM_DELETE_WINDOW`. `TITLE` is `'hnui close ' + process.pid`.
  RUN groups in the 15b shape, with `%x11_window_driver` appended; DEFAULT
  uses the piped form of 15d's THROW. Every trace check adds
  `--implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb'`.

  - **DEFAULT.** `process.exitCode = 7`; an `'exit'` listener prints
    `EXIT-LINE <i> code=<code>` for 0..7; `win.on('close', () => console.log('CLOSE-EVENT'))`;
    then one close request, and nothing else ends the script.

    ```
    // DEFAULT: CLOSE-EVENT
    // DEFAULT-NEXT: EXIT-LINE 0 code=7
    // DEFAULT: EXIT-LINE 7 code=7
    // DEFAULT-CODE: EXITCODE 7
    // DEFAULT-TRACE: quit requested: cancelled, forwarded as close
    // DEFAULT-TRACE: close: not cancelled
    // DEFAULT-TRACE: lifecycle: CLOSING (reason=close)
    // DEFAULT-TRACE: close: ending the process through process.exit()
    ```

    *Mutations: (a) delete the `uiCloseWindow` call in `deliver` -- 124,
    today's behaviour; (b) pass an explicit 0 to `process.exit` -- status
    0.*
  - **CANCEL.** The `close` listener, first call: `e.preventDefault()`,
    print `CANCELLED`, keep `e`, and request a frame whose callback prints
    `FRAME-AFTER-CANCEL`, calls the kept `e.preventDefault()` (outside any
    event), and sends a second close request. Second call: print
    `SECOND-CLOSE`. The `'exit'` listener prints `EXIT code=<code>`.

    ```
    // CANCEL: CANCELLED
    // CANCEL-NEXT: FRAME-AFTER-CANCEL
    // CANCEL-NEXT: SECOND-CLOSE
    // CANCEL-NEXT: EXIT code=0
    // CANCEL-CODE: EXITCODE 0
    // CANCEL-TRACE: close: cancelled
    // CANCEL-TRACE: close: preventDefault outside a close event; ignored
    // CANCEL-TRACE: close: not cancelled
    // CANCEL-TRACE: lifecycle: CLOSING (reason=close)
    ```

    *Mutations: (a) make `preventDefault` do nothing -- the process ends at
    the first request and `FRAME-AFTER-CANCEL` never prints; (b) do not
    reset `closeCancelled` per event -- the second request is cancelled
    too and the run ends at `timeout`.*
  - **NATIVE** (obligation 6). `addon.startEventLog()`,
    `addon.armCloseCancel(1)`, print `OUTSIDE <addon.cancelCloseNow()>`.
    The JS `close` listener prints `JS-CLOSE` each time and, the first
    time, requests a frame that prints `LOG <takeEventLog() filtered to lines containing cancel_close>`
    and sends the second request.

    ```
    // NATIVE: OUTSIDE false
    // NATIVE-NEXT: JS-CLOSE
    // NATIVE-NEXT: LOG native:cancel_close=1
    // NATIVE-NEXT: JS-CLOSE
    // NATIVE-NEXT: EXIT code=0
    // NATIVE-CODE: EXITCODE 0
    // NATIVE-TRACE: close: cancelled
    // NATIVE-TRACE: close: not cancelled
    ```

    *Mutation: have `hnui_cancel_close` return true without setting
    `closeCancelled` -- the process ends at the first request; the second
    `JS-CLOSE` and the `LOG` line are missing.*
  - **WINCLOSE.** `process.exitCode = 9`; a `close` listener prints
    `CLOSE-EVENT-EMITTED`; a 1 ms timer calls `win.close()` and then prints
    `RETURNED`. No driver.

    ```
    // WINCLOSE: EXIT code=9
    // WINCLOSE-NOT: CLOSE-EVENT-EMITTED
    // WINCLOSE-NOT: RETURNED
    // WINCLOSE-CODE: EXITCODE 9
    // WINCLOSE-TRACE: lifecycle: CLOSING (reason=close)
    // WINCLOSE-TRACE-NOT: quit requested
    ```

    *Mutation: have `win.close()` only enter CLOSING -- `RETURNED` prints
    and the run ends at `timeout`.*
  - **LATE** (review focus 1). One uncancelled close request. The `'exit'`
    listener sends a second one with `spawnSync`, then prints
    `QUITS <ui.__waitPlatformQuitRequests(2)> FRAMES <ui.__waitPlatformFrames(1)>`.

    ```
    // LATE: QUITS true FRAMES true
    // LATE-CODE: EXITCODE 0
    // LATE-TRACE: quit requested: cancelled, forwarded as close
    // LATE-TRACE: lifecycle: CLOSING (reason=close)
    // LATE-TRACE: quit requested: cancelled, forwarded as close
    // LATE-TRACE: event dispatch: post refused (dispatcher closed)
    ```

    The frame barrier after the quit barrier is the liveness check: a
    platform loop that left `sapp_run()` runs no further frame.
    *Mutation: cancel only the first quit request (a static flag in
    `uiHandlePlatformEvent`) -- `sapp_run returned` appears, which the
    implicit check forbids, and `FRAMES false`.*
- [x] **Step 3: Run; expect** DEFAULT, CANCEL, NATIVE and LATE to end at
  `timeout` (the window stays open today) or fail to load (`preventDefault`
  and the addon functions do not exist), WINCLOSE to throw
  `win.close is not a function`.
- [x] **Step 4: Implement** the entry point, the two window fields,
  `deliver`'s close handling, `uiCloseWindow`, `preventDefault`,
  `win.close()` and the quit counter. Rewrite the Ruling 54 comment in
  `uiHandlePlatformEvent` and its paragraph in `ui_module.h`: the quit is
  cancelled always, and why (Design (b)).
- [x] **Step 5: Update the tests whose premise changed** (Design (e)):
  `test/ui-events.js` CLOSE and `test/ui-real-events.js` CLOSE call
  `e.preventDefault()` and say why in their headers;
  `test/fixtures/hnui-demo/demo.js` drops its `close` listener and the
  comment above it, and `ui-real-events.js`'s DEMO mutation note becomes
  "delete the `uiCloseWindow` call in `deliver`". Run both, and
  `test/ui-conformance.js`.
- [x] **Step 6: Run `test/ui-close.js` to verify it passes; make each
  mutation; revert.** Then run it 20 times in a row under
  `for i in $(seq 20)`: LATE and CANCEL each depend on a second process
  reaching the X server, and a case that fails once in twenty is a race to
  find now, not a flake to list later.
- [x] **Step 7: Correct the spec.** In "Input and platform services":
  rewrite **Closing** (always cancelled; `preventDefault` /
  `hnui_cancel_close`; an uncancelled close enters CLOSING and ends the
  process) and remove `quit` from the Commands list. In "The JS API": add
  the sentence from Design (f) 4 and say what `win.close()` does (f 8). In
  "The native C API": add the prototype and a bullet stating its contract.
  In "The reference addon": add `hnui_cancel_close` to the conformance
  list. In "Lifecycle": the replacement from (f) 1, "until the process
  ends" for "quitting only when JS asks", the FAILED sentence from (f) 3,
  CLOSING failing reads "of both kinds" with the clause from (f) 6, the
  refusal rules from (f) 7, and (f) 9.
- [x] **Step 8: Close the issue** before committing, with the tracker run
  as CLAUDE.md says (build the bundled ditz2 once if `dist/` is missing):
  `node examples/ditz2/ditz2/dist/cli/main.js close 01a0d88a` with a
  comment naming `test/ui-close.js` DEFAULT as what pins it. Stage `dz/`
  with the fix.
- [x] **Step 9: Format, run `check-hermes-node`, commit**
  (`Close the window on an uncancelled close, and add win.close()`).

---

## Task 15f: revisit the window tests, and the documents

Discharges the rest of obligation (4).

**As built** (two commits: `Revisit the window tests now that a window can
close`, then `Record how a window closes` for the documents and the tracker). Step 1's endings already existed (15e); what
was missing was a check that can fail on the mutation "`win.close()` enters
CLOSING and returns", which exits 0 through the runtime-return hook and
passes CODE. Both tests' trace runs now require `lifecycle: CLOSING
(reason=close)` and `close: ending the process through process.exit()` and
carry `--implicit-check-not='runtime returned'`; the mutation "does
nothing" ends at `timeout` as planned. Beyond the files listed, stale
comments were corrected in `test/ui-present.js`, `test/ui-create-window.js`,
`lib/compositor-gpu/gl_present.cpp`, and the "every `_exit()` path restores
the terminal" comments in `sokol_log.h`, `ui_host.cpp`, `hnui_api.cpp` and
`ui_module.cpp`, since one path does not: the unhandled-rejection `_exit(1)`
in `lib/bindings/node_errors.cpp`, filed as tracker `01a1f36f-ce35`.

**Files:**
- Modify: `test/ui-commands.js`, `test/fixtures/hnui-demo/demo.js`
  (conformance mode's ending), `test/ui-conformance.js` (header only), and
  the comments in every file that cites the hang, Ruling 59 or
  `01a0d88a-6518` -- today `test/ui-clipboard.js`, `ui-commands.js`,
  `ui-conformance.js`, `ui-demo-visual.js`, `ui-early-events.js`,
  `ui-event-exceptions.js`, `ui-events.js`, `ui-raf.js`,
  `ui-real-events.js`, `ui-resize-generation.js`, `ui-version-check.js`,
  `test/fixtures/hnui-demo/demo.js`, `demo_visual.c` (find them with
  `grep -rln '01a0d88a\|Ruling 59' test tools lib include`);
  `CLAUDE.md`, `docs/superpowers/plans/2026-09-21-ui-compositor.md`,
  `docs/superpowers/plans/progress-ui-compositor.md`

- [x] **Step 1: End two tests by `win.close()`.** `test/ui-commands.js`
  calls `win.close()` where it calls `process.exit(0)` on its marker, and
  gains `// TRACE: lifecycle: CLOSING (reason=close)` after its last
  command check; `demo.js`'s conformance mode ends with `win.close()` after
  `DONE`. Both keep `EXITCODE 0`. *Mutation, for both at once: make
  `win.close()` return without exiting -- each run ends at its `timeout`.*
  Run both, make the mutation, revert.
- [x] **Step 2: Correct the comments.** Every window test still ends
  through `process.exit()` or `win.close()` and still keeps `timeout <n>`
  (lit has no per-test timeout, tracker `01a0d797-e326`); what is no longer
  true is the stated reason "closing the window hangs". Replace each such
  sentence; do not touch RUN lines. Confirm with the grep above that
  nothing under `test/`, `tools/`, `lib/` or `include/` still says so.
- [x] **Step 3: Update `CLAUDE.md`'s UI Compositor section.** Slices A-C
  done and Task 15 of D done; the bullet "Every test that opens a window
  ends through `process.exit()` ..." becomes a description of how a window
  closes now (the three paths, CLOSING before `'exit'` listeners, the quit
  always cancelled and why, `_exit()` always on the JS thread, `cleanup_cb`
  never runs); add `__waitPlatformFrames` and `__waitPlatformQuitRequests`
  to what the test hooks enable, with the rule that a test needing "the
  platform thread got that far" waits on one of them rather than on time;
  in "Bootstrap Sequence", one sentence that `runHermesNode` and
  `processExit` call `ProcessExitHooks`, and where.
- [x] **Step 4: Update the plan and the progress file.** In the plan,
  replace the Task 15 paragraph with the five tasks ticked, each of the
  eight obligations followed by the task and test that discharged it, and
  the rulings made here (Design (f) 1-9 and the cancel API) numbered after
  the last existing ruling. Amend Task 16's paragraph: the macOS backend
  inherits "the quit is always cancelled, the JS thread `_exit`s", and must
  check that `sapp_cancel_quit()` in `windowShouldClose:` behaves as it
  does on X11. Amend Task 17's: the lifecycle assertions it was waiting for
  exist. In the progress file, the status table, a "What Task 15 produced"
  paragraph, and the test totals from this task's final suite run.
- [x] **Step 5: Format, run `check-hermes-node`** (the totals must be at
  least 449 + the unit tests added in 15b-15c, and 220 + 3 new JS files,
  with 4 unsupported), **commit**
  (`Revisit the window tests and record how a window closes`).

---

## What this plan does not do

- No teardown of GPU resources, the window or the `napi_env`'s UI
  references: every path ends in `_exit()`.
- No fix for an unhandled rejection skipping `'exit'` listeners (tracker
  `01a1d5c0-d385`), nor for the input queue after a late rejection
  (`01a1d5c0-d3b1`).
- Nothing on macOS beyond keeping `ui_module_nowindow.cpp` compiling: the
  hooks are installed only under `HERMES_NODE_UI_HAVE_WINDOW`.
