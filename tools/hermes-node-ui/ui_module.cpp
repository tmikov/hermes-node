/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_module.h"

#include "hnui_window.h"
#include "ui_events.h"
#include "ui_host.h"
#include "ui_internal.h"

#include <backend.h>
#include <test_hooks.h>
#include <ui_trace.h>

#include <hermes/node-compat/ui/hnui_abi.h>

#include <hermes/node-compat/bindings/node_errors.h>
#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/event_dispatcher.h>
#include <hermes/node-compat/compositor/frame_callbacks.h>
#include <hermes/node-compat/compositor/owner_dispatch.h>
#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>
#include <hermes/node-compat/process/node_process.h>

#include <uv.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hermes {
namespace node_compat {

//===========================================================================
// State
//===========================================================================
//
// The JS-thread half of the window. Shared state is declared in
// ui_internal.h, which says which thread may touch each and what orders it.

std::mutex gMutex;
/// Guarded by gMutex. Written by the JS thread in createWindow(), read by
/// the platform thread once it wakes. Its `title` also backs
/// `sapp_desc::window_title`, which Sokol borrows rather than copies, so
/// this object must outlive `sapp_run()` -- which, having static storage, it
/// does.
WindowRequest gRequest;

/// Created by the platform thread before `sapp_run()`, used by both
/// afterwards -- each through the methods `backend.h` says belong to its
/// thread.
CompositorBackend *gBackend = nullptr;

/// Filled by the platform thread inside `init_cb`, read by the JS thread
/// from the posted continuation, which is where the window's mirror starts
/// from. See the ordering note in ui_internal.h.
WindowState gMetrics;
std::string gStartupError;

/// Created and closed on the JS thread, which is also the only thread its
/// posted functions run on. **Posted to from both threads**: post() is the
/// one OwnerDispatch operation callable from anywhere (owner_dispatch.h),
/// and the platform thread uses it for the window's readiness and startup
/// failure, the frame tick, event dispatch and a finished capture.
OwnerDispatch *gDispatch = nullptr;
napi_env gEnv = nullptr;

//===========================================================================
// The compositor
//===========================================================================

/// Created on the JS thread once its context is current, and reached by
/// the platform thread **only after `gPresentReady`**. Both carry their
/// own locks, so what the flag publishes is the pointers, not the state.
SurfaceTable *gTable = nullptr;
TickMachine *gTick = nullptr;

/// Publishes everything above to the platform thread. Release on the
/// store, acquire on the load -- `frame_cb` runs from the moment
/// `sapp_run()` starts, which is long before any of this exists, so
/// "the pointer is set by then" is not something the frame callback may
/// assume.
std::atomic<bool> gPresentReady{false};

//===========================================================================
// Input
//===========================================================================

/// Platform thread pushes, the window's EventDispatcher drains on the JS
/// thread. Carries its own lock. A file-static rather than a member of the
/// window because events can arrive before `hnui_window` exists -- between
/// `init_cb` and the window's Promise resolving -- and must be kept, not
/// dropped (ui_events.h).
InputQueue gInput;

/// JS thread pushes (`uiPushCommand`), the platform thread drains in
/// `onFrame`. Carries its own lock. A file-static for the same reason
/// `gInput` is: it must exist before the window does, so that a command
/// pushed early is kept rather than lost.
CommandQueue gCommands;

/// True while somebody has promised that the queue will be drained: either
/// a dispatch is posted and has not yet started draining, or the platform
/// thread is holding the flag across an injected batch
/// (drainInjectedEvents). **At most one post outstanding**: whoever finds
/// it already true leaves the drain to the holder. A posted dispatch
/// clears it *before* it drains -- so an event pushed after the clear
/// posts again, and one pushed before it is seen by the drain that
/// follows. The batch holder, which is not a post, releases it and then
/// posts if anything is queued, so a post that was skipped while it held
/// the flag is made up for. Without the flag, a burst of mouse moves would
/// queue one posted function per move, each of which would find the queue
/// already empty.
std::atomic<bool> gDispatchPosted{false};

/// The synchronous native clipboard reads (`hnui_read_clipboard_sync`):
/// the JS thread creates a slot here and blocks on it, and the platform
/// thread answers into it directly. Carries its own lock. A file-static
/// for the reason `gCommands` is one. A JavaScript `readClipboard()` does
/// not use it: its answer goes through `gInput` as a `kClipboardReply`.
ClipboardRequests gClipboardReads;

namespace {

/// Guarded by gMutex.
bool gWindowRequested = false;

/// Where the window is in its life (spec: Lifecycle). **JS thread only**,
/// with no atomic: every transition and every read happens there, and the
/// platform thread is never told -- it learns of CLOSING only through what
/// CLOSING does (a table that refuses publication and withholds the tick, a
/// dispatcher that refuses posts). CLOSED has no value: it is the `_exit()`,
/// and no code observes it.
enum class UiLifecycle { kIdle, kRequested, kRunning, kFailed, kClosing };
UiLifecycle gLifecycle = UiLifecycle::kIdle;

/// Set once by main(), before the JS thread exists.
UiHost *gHost = nullptr;

napi_deferred gDeferred = nullptr;
uv_async_t *gKeepAlive = nullptr;

/// Created on the JS thread with gTable above; read off it only by
/// onOwnerThread().
hnui_window *gWindow = nullptr;

/// The current generation and the framebuffer size it was allocated at.
/// **JS thread only.** Written in onSharedContextReady() and again by
/// uiFollowFramebufferSize() whenever an event reports a different
/// framebuffer size, so they change at run time and the platform thread
/// must never read them: it takes a presented surface's size from that
/// surface's own `SurfaceRef::gen`, through
/// `CompositorBackend::generationSize()` (see maybeCapture()).
GenerationId gGeneration{};
int gGenerationWidth = 0;
int gGenerationHeight = 0;

//===========================================================================
// The referenced libuv handle
//===========================================================================

/// Taken **when `createWindow` is called**, not when the window opens
/// (spec: Lifecycle). The gap between the two is a round trip to another
/// thread, and during it the runtime has nothing else keeping it busy: with
/// no referenced handle, `uv_run` would return, the runtime would tear
/// down, and the window would open into a process on its way out.
///
/// Released by uiEnterClosing(), so that once the window is CLOSING the
/// loop that `process.exit()` and the fatal path pump on their way out is
/// not held open by it; and by rejectWith(), so that a script which handles
/// a rejected createWindow and returns lets the loop drain. A window that
/// opened keeps holding it: a script that calls createWindow and then
/// returns keeps running, as a server that listens and returns does, until
/// something ends the process. When the runtime does reach its own end
/// with a window requested -- the entry script threw, or the window failed
/// -- uiBeforeRuntimeCleanup() ends the process with the runtime's status.
bool takeReferencedHandle(uv_loop_t *loop) {
  gKeepAlive = new uv_async_t();
  // The callback never runs: nothing calls uv_async_send on this handle.
  // It exists to be *referenced*, which is what an initialised async handle
  // is by default and what keeps uv_run from returning.
  const int rc = uv_async_init(loop, gKeepAlive, [](uv_async_t *) {});
  if (rc != 0) {
    delete gKeepAlive;
    gKeepAlive = nullptr;
    return false;
  }
  return true;
}

/// The exact opposite of takeReferencedHandle(), for the early returns
/// between taking the handle and publishing the request.
///
/// Without it, a failure on one of those paths throws a JS error AND leaves
/// the loop holding a REFERENCED handle nothing will ever close -- so the
/// script catches the error, returns normally, and the process then waits
/// forever in UvEventLoop::close()'s uv_run(UV_RUN_DEFAULT) instead of
/// exiting. A thrown error that turns into a hang is worse than either
/// outcome by itself, and worse than the error alone in the specific way
/// this codebase cares about: the program is told what went wrong and then
/// never gets to act on it.
///
/// uv_close is asynchronous and its callback runs on a later turn of the
/// loop, so the allocation is handed to that callback rather than freed
/// here -- the same ownership split OwnerDispatch uses for its own handle.
///
/// Also a step of uiEnterClosing(). Untraced here: each caller traces in
/// its own words, and only when this returned true, so a line records a
/// release that happened and says which path made it.
///
/// \return true if a handle was closed.
bool releaseReferencedHandle() {
  if (gKeepAlive == nullptr)
    return false;
  uv_close(reinterpret_cast<uv_handle_t *>(gKeepAlive), [](uv_handle_t *h) {
    delete reinterpret_cast<uv_async_t *>(h);
  });
  gKeepAlive = nullptr;
  return true;
}

//===========================================================================
// JS thread: settling the Promise
//===========================================================================

void rejectWith(const std::string &message) {
  gLifecycle = UiLifecycle::kFailed;
  uiTrace("lifecycle: FAILED");
  // A window that failed to open no longer holds the loop open: a script
  // that handles the rejection and returns must end (spec: Lifecycle).
  // Released before the rejection, so it happens even if the scope below
  // cannot be opened. The dispatcher stays open: the rejection's handlers
  // run on this thread as ordinary continuations, and CLOSING closes it
  // when the process ends.
  if (releaseReferencedHandle())
    uiTrace("lifecycle: event-loop handle released (createWindow rejected)");
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(gEnv, &scope) != napi_ok)
    return;

  napi_value error = nullptr;
  napi_value messageValue = nullptr;
  if (napi_create_string_utf8(
          gEnv, message.c_str(), NAPI_AUTO_LENGTH, &messageValue) == napi_ok &&
      napi_create_error(gEnv, nullptr, messageValue, &error) == napi_ok) {
    napi_reject_deferred(gEnv, gDeferred, error);
  }
  gDeferred = nullptr;
  napi_close_handle_scope(gEnv, scope);
}

//===========================================================================
// The frame tick, and what completes it
//===========================================================================

/// Defined with the rest of the module surface, below; declared here
/// because `onSharedContextReady()` installs the globals as soon as the
/// window is ready and is itself defined above that point.
void installFrameGlobals();

/// The `uv_hrtime()` reading that `process.uptime()` calls zero, so that a
/// frame timestamp and `process.uptime() * 1000` are one clock (Ruling 56:
/// this runtime has no `performance.now()` whose origin the spec could
/// share, and `process.uptime()` is the monotonic clock with a defined
/// origin that it does have). Computed once, when the window becomes
/// ready, by computeFrameOrigin(). JS thread only.
uint64_t gFrameOriginNs = 0;

/// Ask `process.uptime()` and subtract it from `uv_hrtime()` now -- the
/// same function a script calls, so the two cannot disagree about where
/// zero is.
bool computeFrameOrigin(std::string &error) {
  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value uptime = nullptr;
  napi_value seconds = nullptr;
  napi_valuetype type = napi_undefined;
  double value = 0;
  if (napi_get_global(gEnv, &global) != napi_ok ||
      napi_get_named_property(gEnv, global, "process", &process) != napi_ok ||
      napi_get_named_property(gEnv, process, "uptime", &uptime) != napi_ok ||
      napi_typeof(gEnv, uptime, &type) != napi_ok || type != napi_function ||
      napi_call_function(gEnv, process, uptime, 0, nullptr, &seconds) !=
          napi_ok ||
      napi_get_value_double(gEnv, seconds, &value) != napi_ok) {
    bool pending = false;
    napi_value ignored = nullptr;
    if (napi_is_exception_pending(gEnv, &pending) == napi_ok && pending)
      napi_get_and_clear_last_exception(gEnv, &ignored);
    error =
        "could not read process.uptime(), which frame timestamps are "
        "measured against";
    return false;
  }
  gFrameOriginNs = uv_hrtime() - static_cast<uint64_t>(value * 1e9);
  return true;
}

/// Milliseconds on `process.uptime()`'s clock. Read once per tick, before
/// the first callback, and handed to every callback of that tick.
double frameTimestampMs() {
  return static_cast<double>(uv_hrtime() - gFrameOriginNs) / 1e6;
}

} // namespace

/// End the graphics session, loudly.
///
/// Reached when a completion could not be **established** --
/// `GL_WAIT_FAILED` -- which is terminal by design: "the slot is
/// quarantined, never returned to FREE, and the graphics session is
/// terminated with an error rather than risking a write into a texture
/// still being read" (spec: Errors). Quarantine is the *absence* of an
/// action rather than an action: the caller does not balance the slot's
/// outstanding GPU work, so it can never reach FREE, and nothing after
/// this line runs anyway.
///
/// `_exit()` rather than a return, for the reason every other `_exit()`
/// path in this tree gives (CLAUDE.md, Bootstrap Sequence): a live Hermes
/// runtime on another thread cannot be unwound through normal C++
/// teardown. And like every one of those paths, it restores the terminal
/// first.
[[noreturn]] void terminateGraphicsSession(const std::string &reason) {
  std::fprintf(
      stderr,
      "hermes-node-ui: the graphics session has ended: %s\n"
      "The surface involved is quarantined rather than reused: reusing it "
      "would mean writing into a texture the GPU may still be reading.\n",
      reason.c_str());
  std::fflush(stderr);
  uv_tty_reset_mode();
  _exit(1);
}

namespace {

/// The producer's GPU work for \p ref has finished -- or could not be
/// shown to have finished. **May run on any thread**: the platform thread
/// in polling mode, the JS thread in the blocking fallback.
void onProducerComplete(void *user, SurfaceRef ref, bool ok) {
  (void)user;
  auto action = [ref, ok]() {
    if (!ok) {
      gTick->publicationDropped();
      terminateGraphicsSession(
          "the producer's GPU work could not be confirmed complete "
          "(GL_WAIT_FAILED)");
    }

    // **`TickMachine::publishFrame()`, never `SurfaceTable::publish()`.**
    // The machine leaves kGpuPending only inside publishFrame() and
    // publicationDropped(); a handler that published straight through the
    // table would put the frame on screen and park the machine in
    // kGpuPending forever, so every tick after the first would be
    // withheld and the animation would stop after one frame.
    //
    // Publish BEFORE endGpuWork, so the completion transfers its
    // reference to READY ownership rather than dropping it and hoping
    // (spec: Generations -- "a completion handler transfers its reference
    // to READY ownership before releasing its own"). On a refusal the
    // slot is reclaimed, which leaves it RETIRING while this work is
    // outstanding, and the endGpuWork below is what finally frees it.
    if (!gTick->publishFrame(*gTable, ref)) {
      // Traced where it is APPLIED: a frame of a generation a resize
      // retired while its GPU work was in flight lands here, and
      // test/ui-resize-generation.js asserts that it did.
      uiTrace(
          "generation %u slot %u: publication refused, slot reclaimed",
          ref.gen.value,
          static_cast<unsigned>(ref.slot));
      gTable->reclaim(ref);
    }
    gTable->endGpuWork(ref);
  };

  // The hook is consulted here rather than inside the gate: an unheld
  // gate would run this inline anyway, but a production run should not
  // pay even a mutex for a mechanism nothing has enabled.
  if (uiTestHooksEnabled())
    gGate.completeProducer(action);
  else
    action();
}

/// A blit against \p ref has completed on the GPU, so the read reference
/// the frame callback took before submitting it can go. May run on any
/// thread.
void onConsumerComplete(void *user, SurfaceRef ref, bool ok) {
  (void)user;
  auto action = [ref, ok]() {
    if (!ok) {
      terminateGraphicsSession(
          "a blit's completion could not be confirmed (GL_WAIT_FAILED)");
    }
    gTable->releaseRead(ref);
  };
  if (uiTestHooksEnabled())
    gGate.completeConsumer(action);
  else
    action();
}

/// Run one JavaScript frame callback, which next() has already taken out
/// of the list.
///
/// The reference is released as soon as the function is in hand, BEFORE
/// the call: the handle scope keeps the function alive for the call, and
/// on the fatal path below nothing would come back to release it after.
///
/// An exception goes through `handleCallbackException`: handled -- a
/// listener took it -- and this returns, so the batch carries on; unhandled
/// and it does not return, because `process._fatalException` has emitted
/// `'exit'` and the process is going away (spec: Frames, "Exceptions follow
/// the runtime's existing rule").
void runJsFrameCallback(const FrameCallback &cb, double timestamp) {
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(gEnv, &scope) != napi_ok) {
    releaseFrameCallback(gWindow, cb);
    return;
  }

  napi_value fn = nullptr;
  const bool haveFn = napi_get_reference_value(
                          gEnv, static_cast<napi_ref>(cb.fn), &fn) == napi_ok &&
      fn != nullptr;
  releaseFrameCallback(gWindow, cb);

  if (haveFn) {
    napi_value undefined = nullptr;
    napi_value argument = nullptr;
    napi_value result = nullptr;
    napi_get_undefined(gEnv, &undefined);
    napi_create_double(gEnv, timestamp, &argument);
    if (napi_call_function(gEnv, undefined, fn, 1, &argument, &result) !=
        napi_ok) {
      handleCallbackException(gEnv);
    }
  }

  napi_close_handle_scope(gEnv, scope);
}

} // namespace

/// One frame tick, on the JS thread, posted by the platform thread when
/// `TickMachine::shouldSendTick()` said so.
void runFrameTick() {
  // A tick posted before CLOSING began and run by the dispatcher's closing
  // drain. Nothing about the machine is repaired: no tick can be delivered
  // again, so whichever state it is left in is moot.
  if (uiClosing()) {
    uiTrace("frame tick: refused (closing)");
    return;
  }
  if (gTick == nullptr || gTable == nullptr || gWindow == nullptr)
    return;

  gTick->beginCallbacks();

  // The slot is acquired for the whole tick rather than on demand, so
  // that every callback in one tick draws into the same surface -- which
  // is what makes "several renderers may draw one frame in turn; the
  // first clears and the rest load" (spec: The native C API) mean
  // anything.
  std::optional<SurfaceRef> slot = gTable->acquire();
  gWindow->haveSlot = slot.has_value();
  if (slot)
    gWindow->slot = *slot;
  gWindow->drew = false;
  gWindow->inFrameCallback = true;

  // One timestamp for the whole tick, read before the first callback.
  const double timestamp = frameTimestampMs();

  // The batch is everything registered BEFORE this point, native and
  // JavaScript together in registration order (Ruling 57). A callback
  // registered from inside a callback -- or from a nextTick or promise
  // callback at one of the checkpoints below -- joins the next batch; one
  // cancelled before next() reaches it does not run.
  FrameCallbackList &callbacks = gWindow->frameCallbacks;
  callbacks.beginBatch();
  int ran = 0;
  FrameCallback cb;
  while (callbacks.next(&cb)) {
    ++ran;
    if (cb.kind == FrameCallback::Kind::kNative) {
      // A native callback holds nothing of ours to release, and cannot
      // throw in the C++ sense -- but it can leave a JavaScript exception
      // PENDING through N-API (a napi_throw_*, or a napi_call_function
      // into JavaScript that threw, left uncleared). The checkpoint below
      // then routes it: uiRunTickCallback's own N-API calls refuse with
      // napi_pending_exception, which sends it to handleCallbackException
      // -- so it reaches `uncaughtException`, or exits 1, exactly as a
      // JavaScript callback's throw would. (From reading the code; no test
      // leaves one pending.)
      reinterpret_cast<hnui_frame_cb>(cb.fn)(gWindow, cb.user, timestamp);
    } else {
      runJsFrameCallback(cb, timestamp);
    }
    // The checkpoint after every callback, as a browser runs one: nextTick
    // and promise callbacks queued by this callback run before the next
    // callback does. An exception escaping it is routed exactly as a
    // callback's own is, by the same function event delivery uses.
    uiRunTickCallback(gEnv);
  }

  // A callback closed the window -- win.close() under a process.exit that
  // returned, since a real one does not come back here. uiEnterClosing()
  // has already cleared the per-tick state and released the rest of the
  // batch, and the table is closing, so nothing is submitted: a publication
  // it would refuse is not worth a GPU submission. The slot is put back;
  // the machine is left where it is, since no tick can be delivered again.
  if (uiClosing()) {
    if (slot)
      gTable->reclaim(*slot);
    uiTrace("frame tick: ended by CLOSING; slot reclaimed, nothing submitted");
    return;
  }

  // Whatever happened inside the batch, a handled exception included,
  // everything from here on runs: the frame an earlier callback drew is
  // still submitted, and a tick that drew nothing still goes back to idle.
  gWindow->inFrameCallback = false;
  gWindow->haveSlot = false;
  // Nothing can have drawn without a slot -- hnui_frame_target refuses
  // without one -- but the conjunction is what keeps that a fact rather
  // than an assumption: a tick reported as having drawn with no slot to
  // publish would leave the machine in kGpuPending for good.
  const bool drew = gWindow->drew && slot.has_value();
  gTick->endCallbacksDrew(drew);
  // Traced where the batch RAN, so a test can see a tick that ran nothing
  // -- every callback cancelled -- and still came back to idle.
  uiTrace(
      "frame tick: %d callback(s) ran, %s",
      ran,
      drew ? "drew" : "nothing drawn");

  if (!slot)
    return;
  if (!drew) {
    // A tick that rendered nothing. The slot goes back immediately and
    // the machine is already IDLE, so a rAF chain that only mutates state
    // never stalls waiting for a publication.
    gTable->reclaim(*slot);
    return;
  }

  // The reference is taken BEFORE submission, so that the generation
  // cannot be destroyed while a command buffer is still writing into it.
  gTable->beginGpuWork(*slot);
  gBackend->submitAndTrackCompletion(*slot, onProducerComplete, nullptr);
}

namespace {

//===========================================================================
// Event dispatch
//===========================================================================

/// Whether events may be delivered yet. **JS thread only.** Opened by
/// openEventDelivery(), which onSharedContextReady() posts; until then a
/// dispatch leaves everything queued.
bool gEventDeliveryOpen = false;

/// JS thread, posted by postEventDispatch(). Everything after the flag is
/// ui_events.cpp's, reached through the window alone.
void runEventDispatch() {
  // Cleared BEFORE draining, never after: see gDispatchPosted. Cleared
  // while delivery is still closed too, so an event pushed meanwhile posts
  // again rather than finding the flag stuck; that dispatch then also
  // returns here, and openEventDelivery() drains what they left.
  gDispatchPosted.store(false);
  // Posted before CLOSING and run by the dispatcher's closing drain: what
  // is queued stays queued and is never delivered.
  if (uiClosing()) {
    uiTrace("event dispatch: refused (closing)");
    return;
  }
  if (!gEventDeliveryOpen)
    return;
  uiDispatchEvents(gWindow);
}

/// JS thread, posted by onSharedContextReady() once the window's Promise
/// is resolved. Delivers everything that arrived since the window was
/// created, in order, and from then on every dispatch delivers.
///
/// **Why a post, and why that is enough.** The Promise's continuation --
/// where a script attaches its listeners, right after `await
/// createWindow()` -- runs when the runtime drains microtasks, which it
/// does in the loop's check and prepare phases, not between two functions
/// of one OwnerDispatch drain pass. A dispatch the platform thread posted
/// after init_cb lands in the SAME pass as onSharedContextReady whenever
/// the JS thread was slow to wake, so it would run before the
/// continuation and emit into an object with no listeners. A function
/// posted from inside a pass runs in a later pass (owner_dispatch.h,
/// "Ordering"), hence on a later loop turn, after the check phase has run
/// the continuation. So listeners attached synchronously after the
/// `await` see every event from the window's creation.
void openEventDelivery() {
  if (uiClosing()) {
    uiTrace("event delivery: not opened (closing)");
    return;
  }
  gEventDeliveryOpen = true;
  uiDispatchEvents(gWindow);
}

} // namespace

/// Any thread: the platform thread after pushing an event, the JS thread
/// once the window object exists.
void postEventDispatch() {
  if (gDispatchPosted.exchange(true))
    return;
  // post() refuses once uiEnterClosing() has closed the dispatcher. The
  // events stay queued and are never delivered; the flag is put back, as it
  // always was, so that it is never left claiming a post that did not
  // happen.
  if (gDispatch == nullptr) {
    gDispatchPosted.store(false);
    return;
  }
  if (!gDispatch->post(runEventDispatch)) {
    gDispatchPosted.store(false);
    uiTrace("event dispatch: post refused (dispatcher closed)");
  }
}

namespace {

/// Whether the calling thread is the one owning the producing context --
/// `hnui_window::ownerThread`, the JS thread. Any thread.
///
/// Reading `gWindow` from the platform thread is safe for the one caller
/// that does, the destroy callback: a generation can only die once it is
/// no longer current, which takes a second one, and every generation after
/// the first is started from an event delivered after `gPresentReady`
/// published the window.
bool onOwnerThread() {
  return gWindow != nullptr &&
      std::this_thread::get_id() == gWindow->ownerThread;
}

/// Free \p gen's surfaces. **Owner thread**, reached only through the
/// OwnerDispatch post in the SurfaceTable destroy callback -- and the trace
/// says so by checking rather than by assuming, printed after the fact so
/// it records a destruction that actually ran.
void destroyGeneration(GenerationId gen) {
  gBackend->destroyGenerationSurfaces(gen);
  uiTrace(
      "generation %u: destroyed (owner=%d)",
      gen.value,
      onOwnerThread() ? 1 : 0);
}

} // namespace

/// Runs on the JS thread, posted from `init_cb`. This is stage 2b: the
/// worker binds the shared context the platform thread created, which only
/// the thread that will use it can do.
void onSharedContextReady() {
  // Posted by init_cb and run by the dispatcher's closing drain: the
  // window's readiness is never delivered, gPresentReady stays false, and
  // the platform thread presents its clear colour until _exit().
  if (uiClosing()) {
    uiTrace("window readiness: refused (closing)");
    return;
  }
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(gEnv, &scope) != napi_ok)
    return;

  std::string error;
  if (!gBackend->makeCurrentOnWorker(error)) {
    napi_close_handle_scope(gEnv, scope);
    rejectWith(error);
    return;
  }

  // RULING 16, carried from Task 3 Step 5 and landing here: this is the
  // first production SurfaceTable in the tree, so this is where the
  // destroy callback gets its real implementation.
  //
  // **`dispatch.post(...)`, never an inline free.** A generation's last
  // reference is dropped by whichever thread ran the completion that
  // dropped it -- the platform thread for a consumer read, a driver's
  // thread in principle -- and destroying a generation means deleting GL
  // textures and Sokol resources, which may only happen on the thread
  // that owns the producing context. SurfaceTable already guarantees the
  // half it can (the callback runs with its lock released, never inline
  // from within a transition); getting onto the right thread is this
  // callback's job.
  //
  // A refused post is reported **under tracing only**, and that is a
  // deliberate acceptance rather than an oversight. post() returns false
  // exactly once the dispatcher has been closed -- that is, at shutdown
  // -- so the textures it could not free belong to a process that is
  // going away, and the leak is moot. A stderr line on every close would
  // be noise on the common path. The cost of the choice, stated plainly:
  // in a default run (no HERMES_NODE_DEBUG_NATIVE=UI) a dropped
  // destruction is silent.
  //
  // Both traces name the thread, because "on the owner thread" is otherwise
  // unobservable: the first says which thread dropped the last reference,
  // the second which thread the destruction then ran on.
  // test/ui-resize-generation.js asserts the pair -- owner=0 then owner=1
  // -- and the first half is what makes the second mean anything, since a
  // reference dropped on the owner thread would read owner=1 posted or not.
  gTable = new SurfaceTable([](GenerationId gen) {
    uiTrace(
        "generation %u: last reference dropped (owner=%d)",
        gen.value,
        onOwnerThread() ? 1 : 0);
    const bool posted = gDispatch != nullptr &&
        gDispatch->post([gen]() { destroyGeneration(gen); });
    if (!posted) {
      uiTrace(
          "generation %u: destruction could not be posted to the owner "
          "thread; its GPU resources are leaked",
          gen.value);
    }
  });
  gTick = new TickMachine();

  // Built in a local and published to gWindow only once every step below
  // has succeeded: a rejected createWindow leaves no window behind, which
  // is what hnui_get_window()'s contract says (hnui.h, ui_module.h).
  hnui_window *w = new hnui_window();
  const auto abandon = [&](const std::string &why) {
    if (w->jsWindow != nullptr)
      napi_delete_reference(gEnv, w->jsWindow);
    delete w;
    napi_close_handle_scope(gEnv, scope);
    rejectWith(why);
  };
  w->ownerThread = std::this_thread::get_id();
  w->table = gTable;
  w->tick = gTick;
  w->backend = gBackend;
  w->env = gEnv;
  // The mirror starts from init_cb's snapshot; every event after that
  // carries its own (Ruling 52).
  w->state = gMetrics;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    w->title = gRequest.title;
  }
  w->events = std::make_unique<EventDispatcher>(gInput);

  // The other half of completion: the producer's arrives through the
  // callback submitAndTrackCompletion() takes, the consumer's through
  // this sink, because blit() has nowhere to carry one.
  gBackend->setConsumerCompletion(onConsumerComplete, nullptr);

  // Surfaces are sized to the FRAMEBUFFER in pixels, not to the window in
  // logical units (spec: Surfaces, "sized to the framebuffer in pixels").
  // Generation 1; every later one is started by uiFollowFramebufferSize().
  gGenerationWidth = gMetrics.framebufferWidth;
  gGenerationHeight = gMetrics.framebufferHeight;
  gGeneration = gTable->newGeneration(gGenerationWidth, gGenerationHeight);
  std::string surfaceError;
  if (!gBackend->createGenerationSurfaces(
          gGeneration, gGenerationWidth, gGenerationHeight, surfaceError)) {
    abandon(surfaceError);
    return;
  }

  // The window object: an EventEmitter, kept current from w->state
  // by ui_events.cpp. Built before the store below, so that a failure
  // here, like the surface failure above, rejects with nothing yet
  // published to the platform thread.
  std::string objectError;
  napi_value window = uiCreateWindowObject(w, objectError);
  if (window == nullptr) {
    abandon(objectError);
    return;
  }

  // Before the store below: once it is made, the platform thread may send
  // the first tick, and that tick reads the origin.
  std::string originError;
  if (!computeFrameOrigin(originError)) {
    abandon(originError);
    return;
  }

  gWindow = w;

  // Everything above is published to the platform thread by this store,
  // which its frame callback acquires. Nothing before it may be read
  // there, and nothing after it may be written here.
  gPresentReady.store(true, std::memory_order_release);
  gLifecycle = UiLifecycle::kRunning;

  // "the module exports requestAnimationFrame / cancelAnimationFrame and,
  // once a window exists, installs them as globals, because libraries
  // test `typeof requestAnimationFrame`" (spec: The JS API). Once a
  // window exists is exactly here.
  installFrameGlobals();

  uiTrace(
      "window ready: %dx%d logical, %dx%d framebuffer, dpi %.2f",
      gMetrics.width,
      gMetrics.height,
      gMetrics.framebufferWidth,
      gMetrics.framebufferHeight,
      gMetrics.dpiScale);

  napi_resolve_deferred(gEnv, gDeferred, window);
  gDeferred = nullptr;
  napi_close_handle_scope(gEnv, scope);

  // Events that arrived before the object existed are still queued, and
  // every dispatch posted for them -- including one the platform thread
  // posted after init_cb that runs later in this same drain pass -- leaves
  // them there until delivery opens. Opened from a post, so that the
  // script's `await createWindow()` continuation has run first: see
  // openEventDelivery().
  //
  // A refusal is unreachable by construction: this function runs on the
  // thread that closes the dispatcher, and returned above if it was
  // CLOSING. Checked and traced anyway, so that it would be seen.
  if (gDispatch != nullptr && !gDispatch->post(openEventDelivery))
    uiTrace("event delivery: post refused (dispatcher closed)");
}

void onStartupFailed() {
  if (uiClosing()) {
    uiTrace("window readiness: refused (closing)");
    return;
  }
  uiTrace("window failed: %s", gStartupError.c_str());
  rejectWith(gStartupError);
}

void uiFollowFramebufferSize(const WindowState &state) {
  if (gTable == nullptr || gBackend == nullptr)
    return;
  // A zero or negative size is not a size to allocate at -- a minimized
  // window may report one -- and the surfaces already allocated stay
  // usable: the presenter stretches whatever it has.
  if (state.framebufferWidth <= 0 || state.framebufferHeight <= 0)
    return;
  if (state.framebufferWidth == gGenerationWidth &&
      state.framebufferHeight == gGenerationHeight) {
    return;
  }

  // newGeneration() makes the new generation current and drops the table's
  // reference to the old one, in one transition. Nothing here frees the old
  // generation: it lives on for as long as its presenting surface, its
  // reads, its wrappers or a frame in flight own it, and SurfaceTable hands
  // it to the destroy callback when the last of them lets go. A frame of
  // the old generation still in flight is refused at publication and
  // reclaimed (onProducerComplete).
  const GenerationId previous = gGeneration;
  const int previousWidth = gGenerationWidth;
  const int previousHeight = gGenerationHeight;
  const GenerationId next =
      gTable->newGeneration(state.framebufferWidth, state.framebufferHeight);
  std::string error;
  if (!gBackend->createGenerationSurfaces(
          next, state.framebufferWidth, state.framebufferHeight, error)) {
    // Terminal. At startup the same failure rejects createWindow's
    // Promise; here there is nobody to reject, and the table's current
    // generation now has no surfaces, so no frame could ever be drawn
    // again -- an animation that silently stopped is the worse outcome.
    terminateGraphicsSession(
        "the surfaces for a resized framebuffer (" +
        std::to_string(state.framebufferWidth) + "x" +
        std::to_string(state.framebufferHeight) +
        ") could not be allocated: " + error);
  }
  gGeneration = next;
  gGenerationWidth = state.framebufferWidth;
  gGenerationHeight = state.framebufferHeight;
  uiTrace(
      "generation %u: started at %dx%d, replacing generation %u at %dx%d",
      next.value,
      gGenerationWidth,
      gGenerationHeight,
      previous.value,
      previousWidth,
      previousHeight);
}

//===========================================================================
// JS thread: the module surface
//===========================================================================

/// Reads an integer property, leaving \p out alone when absent. Absent and
/// zero are different answers -- `{width: 0}` is a caller asking for
/// something impossible, while no `width` at all is a caller taking the
/// default -- so this reports which it saw.
void readInt(napi_env env, napi_value options, const char *name, int *out) {
  napi_value value = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, options, name, &value) != napi_ok)
    return;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_number)
    return;
  int32_t parsed = 0;
  if (napi_get_value_int32(env, value, &parsed) == napi_ok)
    *out = parsed;
}

void readBool(napi_env env, napi_value options, const char *name, bool *out) {
  napi_value value = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, options, name, &value) != napi_ok)
    return;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_boolean)
    return;
  bool parsed = false;
  if (napi_get_value_bool(env, value, &parsed) == napi_ok)
    *out = parsed;
}

void readDouble(
    napi_env env,
    napi_value options,
    const char *name,
    double *out) {
  napi_value value = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, options, name, &value) != napi_ok)
    return;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_number)
    return;
  double parsed = 0;
  if (napi_get_value_double(env, value, &parsed) == napi_ok)
    *out = parsed;
}

void readString(
    napi_env env,
    napi_value options,
    const char *name,
    std::string *out) {
  napi_value value = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, options, name, &value) != napi_ok)
    return;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string)
    return;
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok)
    return;
  std::string parsed(length, '\0');
  if (napi_get_value_string_utf8(env, value, &parsed[0], length + 1, &length) ==
      napi_ok) {
    parsed.resize(length);
    *out = std::move(parsed);
  }
}

namespace {

/// `requestAnimationFrame(cb)` -- exported by the module and installed as
/// a global once a window exists. Returns the id `cancelAnimationFrame`
/// takes, from the id space native callbacks share.
napi_value requestAnimationFrame(napi_env env, napi_callback_info info) {
  // Refused rather than thrown, even for a bad argument: an 'exit' listener
  // that schedules a frame must not have the rest of its body skipped by an
  // exception nobody will report.
  if (uiClosing()) {
    uiTrace("requestAnimationFrame: refused (closing)");
    napi_value zero = nullptr;
    napi_create_int32(env, 0, &zero);
    return zero;
  }
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_valuetype type = napi_undefined;
  if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok ||
      type != napi_function) {
    napi_throw_type_error(
        env,
        nullptr,
        "requestAnimationFrame: a callback function is required.");
    return nullptr;
  }
  if (gWindow == nullptr || gTick == nullptr) {
    napi_throw_error(
        env,
        nullptr,
        "requestAnimationFrame: no window exists yet. Await createWindow() "
        "before requesting a frame -- frames are paced by the window's "
        "presentation, so there is nothing to pace against until then.");
    return nullptr;
  }

  // A strong reference: the callback must survive until the tick runs it,
  // and nothing else on the JS side is holding it by then. Released by
  // whichever route takes it out of the list (hnui_window.h,
  // frameCallbacks).
  napi_ref ref = nullptr;
  if (napi_create_reference(env, argv[0], 1, &ref) != napi_ok) {
    napi_throw_error(
        env, nullptr, "requestAnimationFrame: the callback could not be held.");
    return nullptr;
  }

  const int id =
      gWindow->frameCallbacks.add(FrameCallback::Kind::kJs, ref, nullptr);
  // Requests coalesce in the machine, so n callbacks in one frame still
  // mean one tick.
  gTick->requestFrame();

  napi_value result = nullptr;
  napi_create_int32(env, id, &result);
  return result;
}

/// `cancelAnimationFrame(id)` -- exported and installed beside
/// requestAnimationFrame. Like the browser's, it never throws: an id that
/// names nothing waiting to run (never issued, already run, already
/// cancelled, the callback running right now) does nothing, and so does a
/// non-number or a call with no window. The id may be a native callback's.
napi_value cancelAnimationFrame(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_valuetype type = napi_undefined;
  int32_t id = 0;
  if (gWindow == nullptr || argc < 1 ||
      napi_typeof(env, argv[0], &type) != napi_ok || type != napi_number ||
      napi_get_value_int32(env, argv[0], &id) != napi_ok) {
    return nullptr;
  }
  FrameCallback removed;
  if (gWindow->frameCallbacks.cancel(id, &removed))
    releaseFrameCallback(gWindow, removed);
  return nullptr;
}

void installFrameGlobals() {
  napi_value global = nullptr;
  if (napi_get_global(gEnv, &global) != napi_ok)
    return;
  const struct {
    const char *name;
    napi_callback callback;
  } globals[] = {
      {"requestAnimationFrame", requestAnimationFrame},
      {"cancelAnimationFrame", cancelAnimationFrame},
  };
  for (const auto &entry : globals) {
    napi_value fn = nullptr;
    if (napi_create_function(
            gEnv, entry.name, NAPI_AUTO_LENGTH, entry.callback, nullptr, &fn) ==
        napi_ok) {
      napi_set_named_property(gEnv, global, entry.name, fn);
    }
  }
}

napi_value createWindow(napi_env env, napi_callback_info info) {
  // From an 'exit' listener, or after any path that entered CLOSING: a
  // window opened now would open into a process on its way out.
  if (uiClosing()) {
    napi_throw_error(env, nullptr, "createWindow: the process is exiting.");
    return nullptr;
  }

  // One window per process, and a second request THROWS rather than
  // returning the first window's handle (spec: Lifecycle; sokol_app
  // supports no more, sokol#437). Synchronously, not as a rejected
  // Promise: this is a misuse of the API rather than a failure of the
  // request, and it is knowable without doing anything.
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gWindowRequested) {
      napi_throw_error(
          env,
          nullptr,
          "a window already exists in this process. hermes-node-ui supports "
          "one window per process (sokol_app does not do multi-window), and "
          "a window that has been closed cannot be reopened.");
      return nullptr;
    }
  }

  if (gHost == nullptr) {
    napi_throw_error(
        env,
        nullptr,
        "hermes-node:ui has no host. This module only works inside "
        "hermes-node-ui.");
    return nullptr;
  }

  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  WindowRequest request;
  if (argc >= 1) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, argv[0], &type) == napi_ok && type == napi_object) {
      readInt(env, argv[0], "width", &request.width);
      readInt(env, argv[0], "height", &request.height);
      readBool(env, argv[0], "highDpi", &request.highDpi);
      readBool(env, argv[0], "fullscreen", &request.fullscreen);
      readString(env, argv[0], "title", &request.title);
    }
  }

  if (request.width <= 0 || request.height <= 0) {
    napi_throw_error(
        env, nullptr, "createWindow: width and height must be positive.");
    return nullptr;
  }

  uv_loop_t *loop = nullptr;
  if (napi_get_uv_event_loop(env, &loop) != napi_ok || loop == nullptr) {
    napi_throw_error(
        env,
        nullptr,
        "createWindow: this runtime exposes no libuv event loop, so the "
        "window could not be attached to it.");
    return nullptr;
  }

  // Before the request is published, so that a failure here leaves nothing
  // for the platform thread to act on.
  if (!takeReferencedHandle(loop)) {
    napi_throw_error(
        env,
        nullptr,
        "createWindow: could not attach a handle to the event loop.");
    return nullptr;
  }

  gEnv = env;
  gDispatch = new OwnerDispatch(loop);

  napi_value promise = nullptr;
  if (napi_create_promise(env, &gDeferred, &promise) != napi_ok) {
    // Give back everything taken since takeReferencedHandle(), and do it
    // before throwing. The handle is referenced, so leaving it behind
    // converts this caught JS error into a process that never exits.
    //
    // gWindowRequested is still false at this point, which is what makes a
    // retry after this failure sound rather than a second leak: the next
    // createWindow() call allocates a fresh handle and dispatcher because
    // these two were actually released, not merely abandoned.
    //
    // ~OwnerDispatch closes on this thread, which is its owner thread --
    // the requirement its own header states -- since createWindow runs on
    // the JS thread that owns the loop.
    if (releaseReferencedHandle())
      uiTrace("createWindow: event-loop handle released (no Promise)");
    delete gDispatch;
    gDispatch = nullptr;
    gEnv = nullptr;
    napi_throw_error(env, nullptr, "createWindow: could not create a Promise.");
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(gMutex);
    gRequest = std::move(request);
    gWindowRequested = true;
  }
  gLifecycle = UiLifecycle::kRequested;

  uiTrace(
      "createWindow: %dx%d '%s'",
      gRequest.width,
      gRequest.height,
      gRequest.title.c_str());

  // Wakes the platform thread's condition-variable wait, whose predicate is
  // "a window was requested OR the runtime finished" -- the shape Task 6
  // wrote precisely so this call could be added without touching run().
  gHost->requestWindow();
  return promise;
}

} // namespace

//===========================================================================
// Entry points
//===========================================================================

void setUiModuleHost(UiHost *host) {
  gHost = host;
}

namespace {

bool exportFunction(
    napi_env env,
    napi_value exports,
    const char *name,
    napi_callback callback) {
  napi_value fn = nullptr;
  if (napi_create_function(
          env, name, NAPI_AUTO_LENGTH, callback, nullptr, &fn) != napi_ok) {
    return false;
  }
  return napi_set_named_property(env, exports, name, fn) == napi_ok;
}

} // namespace

napi_value initUiModule(napi_env env, napi_value exports) {
  if (!exportFunction(env, exports, "createWindow", createWindow))
    return nullptr;
  if (!exportFunction(
          env, exports, "requestAnimationFrame", requestAnimationFrame) ||
      !exportFunction(
          env, exports, "cancelAnimationFrame", cancelAnimationFrame)) {
    return nullptr;
  }
  if (!exportFunction(env, exports, "writeClipboard", uiWriteClipboard))
    return nullptr;
  if (!exportFunction(env, exports, "readClipboard", uiReadClipboard))
    return nullptr;
  // The capture and injection hooks are exported ONLY when the hooks are
  // enabled, so that a production run cannot be made capturable, or fed
  // synthetic input, by a script that merely knows the name.
  // `uiTestHooksEnabled()` is read once per process, so this is decided at
  // module load and cannot change under the program.
  if (uiTestHooksEnabled()) {
    if (!exportFunction(env, exports, "__captureFrame", captureFrame))
      return nullptr;
    if (!exportFunction(env, exports, "__injectEvent", injectEvent))
      return nullptr;
    if (!exportFunction(
            env,
            exports,
            "__holdProducerCompletions",
            holdProducerCompletions) ||
        !exportFunction(
            env,
            exports,
            "__releaseProducerCompletions",
            releaseProducerCompletions)) {
      return nullptr;
    }
    if (!exportFunction(
            env,
            exports,
            "__failPendingClipboardReads",
            failPendingClipboardReads)) {
      return nullptr;
    }
    if (!exportFunction(
            env, exports, "__waitPlatformFrames", waitPlatformFrames)) {
      return nullptr;
    }
    if (!exportFunction(
            env,
            exports,
            "__waitPlatformQuitRequests",
            waitPlatformQuitRequests)) {
      return nullptr;
    }
  }
  return exports;
}

namespace {

const char *closeReasonName(UiCloseReason reason) {
  switch (reason) {
    case UiCloseReason::kWindowClose:
      return "close";
    case UiCloseReason::kExitCall:
      return "exit";
    case UiCloseReason::kFatalException:
      return "fatal";
    case UiCloseReason::kRuntimeReturn:
      return "return";
  }
  return "exit";
}

} // namespace

bool uiClosing() {
  return gLifecycle == UiLifecycle::kClosing;
}

void uiEnterClosing(UiCloseReason reason) {
  if (gLifecycle == UiLifecycle::kClosing)
    return;
  // First, so that everything below -- and everything any function the
  // dispatcher's drain runs -- already sees CLOSING.
  gLifecycle = UiLifecycle::kClosing;
  if (!uiWindowRequested())
    return;
  uiTrace("lifecycle: CLOSING (reason=%s)", closeReasonName(reason));

  // Each step is null-guarded: CLOSING can be entered while the window is
  // still only requested, before the table, the window or its surfaces
  // exist.

  // No further acquire() or publish(), and the platform thread's tick
  // gate answers no from now on. A frame whose GPU work is in flight is
  // refused at publication and reclaimed by onProducerComplete().
  if (gTable != nullptr)
    gTable->beginClosing();

  // The per-tick state off first, so hnui_frame_target answers false
  // even when this was reached from inside a frame callback -- then every
  // registration still in the list, the rest of a running batch included,
  // is released rather than run. The batch's own loop in runFrameTick()
  // finds the list empty if control ever returns to it.
  if (gWindow != nullptr) {
    gWindow->inFrameCallback = false;
    gWindow->haveSlot = false;
    int released = 0;
    gWindow->frameCallbacks.releaseAll([&](const FrameCallback &cb) {
      releaseFrameCallback(gWindow, cb);
      ++released;
    });
    uiTrace("lifecycle: frame callbacks released=%d", released);
  }

  // Outstanding clipboard reads of both kinds complete with an error (spec:
  // Lifecycle). A synchronous read blocks the only thread that can get
  // here, so none can be waiting now, and every slot such a read created
  // has already been released by the read that created it -- so a reply
  // still in flight is refused whether or not failAll() runs. It runs
  // anyway, as the spec's step and so that the count it returns (0) is
  // traced; a slot created after this point is refused at its push, since
  // uiPushCommand() refuses once CLOSING is set. The Promise reads are
  // rejected now and their handlers run at the end of this function, below.
  const size_t syncFailed = gClipboardReads.failAll(ClipboardStatus::kError);
  const size_t promiseFailed = uiFailPendingClipboardReads(gEnv);
  uiTrace(
      "lifecycle: clipboard reads failed sync=%zu promise=%zu",
      syncFailed,
      promiseFailed);

  // A capture requested through the test hook is abandoned: its result
  // could only be delivered by a post, which is about to be refused.
  uiTestHooksAbandonCapture();

  // The synchronized sender shutdown. It drains what was already
  // posted -- every function that can be posted checks uiClosing() and
  // refuses -- and post() returns false on any thread afterwards. Closed,
  // never deleted or nulled: the platform thread reads the pointer with no
  // synchronization. Safe from inside a function this dispatcher is itself
  // running (an event dispatch, a frame tick): the pass already extracted
  // runs to its end, and nothing after close() runs
  // (OwnerDispatchTest.CloseFromInsideAPostedFunctionRunsTheRestAndRefusesLaterPosts).
  if (gDispatch != nullptr) {
    gDispatch->close();
    uiTrace("lifecycle: dispatcher closed");
  }

  // The loop is no longer held open on the window's behalf.
  if (releaseReferencedHandle())
    uiTrace("lifecycle: event-loop handle released");

  // Last, because it runs program code -- the rejection handlers of the
  // reads failed above -- and, if that throws with nothing to catch it,
  // does not return. Only when a Promise was actually rejected, so a
  // process.exit() with nothing outstanding runs no queued nextTick or
  // promise callback, which Node's does not either; and never on the fatal
  // path, where Node runs no further callback after an uncaught exception
  // and a throwing handler would replace the error being reported.
  if (promiseFailed > 0 && reason != UiCloseReason::kFatalException)
    uiRunTickCallback(gEnv);
}

bool uiCloseWindow(hnui_window *w) {
  (void)w; // one window per process; named for the callers' symmetry
  uiEnterClosing(UiCloseReason::kWindowClose);

  // Closing the window ends the process (spec: Lifecycle), through the
  // runtime's own process.exit() rather than an _exit() of ours: that is
  // the path which emits 'exit', honours process.exitCode, flushes stdio
  // and restores the terminal, and a program that has replaced it has
  // asked for its replacement to run. The property's CURRENT value, looked
  // up now, for the reason uiRunTickCallback() looks up _tickCallback.
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(gEnv, &scope) != napi_ok)
    return true;
  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value exitFn = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_global(gEnv, &global) != napi_ok ||
      napi_get_named_property(gEnv, global, "process", &process) != napi_ok ||
      napi_get_named_property(gEnv, process, "exit", &exitFn) != napi_ok ||
      napi_typeof(gEnv, exitFn, &type) != napi_ok || type != napi_function) {
    bool pending = false;
    if (napi_is_exception_pending(gEnv, &pending) != napi_ok || !pending) {
      napi_throw_type_error(
          gEnv,
          nullptr,
          "closing the window: process.exit is not a function, so the "
          "process could not be ended.");
    }
    napi_close_handle_scope(gEnv, scope);
    return false;
  }

  uiTrace("close: ending the process through process.exit()");
  napi_value result = nullptr;
  const bool called =
      napi_call_function(gEnv, process, exitFn, 0, nullptr, &result) == napi_ok;
  napi_close_handle_scope(gEnv, scope);
  if (!called)
    return false;

  // Reached only when process.exit has been replaced by something that
  // returns. Nothing undoes CLOSING: the dispatcher is closed, the frame
  // callbacks released and the loop handle given back, and every posted
  // function and every event still queued is refused from here on. The
  // process ends when the program ends it some other way, or when its loop
  // runs dry (uiBeforeRuntimeCleanup()).
  uiTrace("close: process.exit returned; the window stays in CLOSING");
  return true;
}

void uiBeforeExitListeners(napi_env env, ExitReason reason) {
  // Another runtime's exit is not this window's. With no window requested
  // there is no env to compare against, and the state is recorded anyway
  // (see uiEnterClosing()).
  if (uiWindowRequested() && env != gEnv)
    return;
  switch (reason) {
    case ExitReason::kExitCall:
      uiEnterClosing(UiCloseReason::kExitCall);
      return;
    case ExitReason::kFatalException:
      uiEnterClosing(UiCloseReason::kFatalException);
      return;
    case ExitReason::kRuntimeReturn:
      uiEnterClosing(UiCloseReason::kRuntimeReturn);
      return;
  }
}

void uiBeforeRuntimeCleanup(napi_env env, int exitCode) {
  // A script that never asked for a window has nothing here to protect, and
  // ends exactly as plain hermes-node does: step 16, then UiHost::run()
  // joins the JS thread and returns its status.
  //
  // The two checks overlap in that case -- gEnv is still null, so the
  // second would return as well -- and the order is not interchangeable.
  // uiWindowRequested() reads under gMutex; gEnv is a plain variable the
  // window's JS thread writes in createWindow() BEFORE it publishes the
  // request under that same mutex. So a true answer here guarantees that
  // gEnv has been published to this thread, and only then is it read. That
  // matters when a second runtime exists, the inspector's: its JS thread
  // reaches this hook too, and reading gEnv first would be an
  // unsynchronized read of a variable another thread may be writing.
  if (!uiWindowRequested())
    return;
  if (env != gEnv)
    return;

  // Ending here rather than letting step 16 run is not a shortcut. The
  // window cannot be torn down: sapp_run() returning would close the X
  // display this thread's GL context was created on, while this thread
  // still has it current, and on macOS sapp_run() never returns and its
  // quit path exits 0 from the main thread, which cannot carry a status.
  // So the window stays until the process ends, and the process ends here,
  // from the JS thread, as process.exit() and the fatal path end it.
  //
  // Skipping step 16 is safe for the reasons processExit() gives for its
  // own _exit(): the status is already settled, the 'exit' listeners have
  // run, and step 15 flushed what they wrote; what step 16 would add is
  // tearing down a runtime the OS is about to reclaim, which under ASAN is
  // also thousands of leak reports for the live Hermes heap. And it must
  // not return once a window may exist: the main thread is then inside
  // sapp_run(), which returns on no path, so after step 16 the JS thread
  // would finish with nothing left to end the process -- a window on screen
  // with no runtime behind it, whose platform thread is still reaching into
  // what this thread created. (When startup failed before sapp_run(), the
  // main thread is in UiHost::run()'s join instead, and returning would
  // work; ending the same way on every windowed path is simpler than
  // telling the two apart, and gives up nothing.)
  uiTrace("lifecycle: runtime returned, exiting with status %d", exitCode);
  std::fflush(nullptr);
  // Every _exit() path in this tree restores the terminal first (CLAUDE.md,
  // Bootstrap Sequence). Step 16 is what restores it on plain hermes-node's
  // natural path -- closing the stdio handles reaches uv__tty_close -- and
  // this path skips step 16, so nothing else would.
  uv_tty_reset_mode();
  _exit(exitCode);
}

bool uiWindowRequested() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gWindowRequested;
}

namespace {

/// The name a refused command is traced under.
const char *commandName(Command::Kind kind) {
  switch (kind) {
    case Command::Kind::kSetTitle:
      return "title";
    case Command::Kind::kSetFullscreen:
      return "fullscreen";
    case Command::Kind::kSetIcon:
      return "icon";
    case Command::Kind::kSetCursor:
      return "cursor";
    case Command::Kind::kSetCursorImage:
      return "custom-cursor";
    case Command::Kind::kShowCursor:
      return "show-cursor";
    case Command::Kind::kLockPointer:
      return "lock-pointer";
    case Command::Kind::kReadClipboard:
      return "read-clipboard";
    case Command::Kind::kWriteClipboard:
      return "write-clipboard";
  }
  return "unknown";
}

} // namespace

bool uiPushCommand(Command command) {
  // Dropped and traced, never thrown or printed (spec: Lifecycle, "platform
  // commands are unavailable" to 'exit' listeners): a listener that restores
  // the cursor must not have the rest of its body skipped by an exception
  // that processExit would swallow.
  if (uiClosing()) {
    uiTrace("command refused (closing): %s", commandName(command.kind()));
    return false;
  }
  gCommands.push(std::move(command));
  return true;
}

ClipboardRequests &uiClipboardReads() {
  return gClipboardReads;
}

hnui_window *uiWindowForEnv(napi_env env) {
  // One window per process, so this is an identity check rather than a
  // lookup: the window belongs to the runtime that asked for it, and any
  // other env has none. Several runtimes can exist in a hermes-node
  // process and only one can own a window (spec: The native C API), which
  // is exactly the case this refuses rather than answering wrongly.
  if (gEnv == nullptr || env != gEnv)
    return nullptr;
  return gWindow;
}

} // namespace node_compat
} // namespace hermes
