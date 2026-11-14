/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_module.h"

#include "event_injector.h"
#include "hnui_window.h"
#include "sapp_event_convert.h"
#include "ui_events.h"
#include "ui_host.h"
#ifdef HERMES_NODE_UI_HAVE_X11
#include "x11_clipboard.h"
#include "x11_readback.h"
#endif

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

#include <sokol_app.h>

#include <uv.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hermes {
namespace node_compat {

namespace {

//===========================================================================
// State
//===========================================================================
//
// Two threads share this file. The split below is deliberate and is the
// thing to preserve when editing: the fields under `gMutex` are the only
// ones either thread may touch at a time the other might, and everything
// else is owned outright by one thread.
//
// What makes the rest safe without a lock is the handshake, not luck. The
// platform thread publishes its results BEFORE posting to the JS thread
// through OwnerDispatch, and OwnerDispatch posts through `uv_async_send`
// under its own mutex; the JS thread reads them only from inside the posted
// function. That pairing is the happens-before edge. A field read outside
// that pairing has no ordering at all, so do not add one.

struct WindowRequest {
  int width = 1280;
  int height = 800;
  std::string title = "hermes-node";
  bool highDpi = false;
  bool fullscreen = false;
};

std::mutex gMutex;
/// Guarded by gMutex.
bool gWindowRequested = false;
/// Guarded by gMutex. Written by the JS thread in createWindow(), read by
/// the platform thread once it wakes. Its `title` also backs
/// `sapp_desc::window_title`, which Sokol borrows rather than copies, so
/// this object must outlive `sapp_run()` -- which, being a file-static, it
/// does.
WindowRequest gRequest;

/// Set once by main(), before the JS thread exists.
UiHost *gHost = nullptr;

/// Created by the platform thread before `sapp_run()`, used by both
/// afterwards -- each through the methods `backend.h` says belong to its
/// thread.
CompositorBackend *gBackend = nullptr;

/// Filled by the platform thread inside `init_cb`, read by the JS thread
/// from the posted continuation, which is where the window's mirror starts
/// from. See the ordering note above.
WindowState gMetrics;
std::string gStartupError;
bool gStartupFailed = false;

/// JS thread only.
OwnerDispatch *gDispatch = nullptr;
napi_env gEnv = nullptr;
napi_deferred gDeferred = nullptr;
uv_async_t *gKeepAlive = nullptr;

//===========================================================================
// The compositor
//===========================================================================

/// Created on the JS thread once its context is current, and reached by
/// the platform thread **only after `gPresentReady`**. Both carry their
/// own locks, so what the flag publishes is the pointers, not the state.
SurfaceTable *gTable = nullptr;
TickMachine *gTick = nullptr;
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

/// Publishes everything above to the platform thread. Release on the
/// store, acquire on the load -- `frame_cb` runs from the moment
/// `sapp_run()` starts, which is long before any of this exists, so
/// "the pointer is set by then" is not something the frame callback may
/// assume.
std::atomic<bool> gPresentReady{false};

/// Platform thread only: what is on screen, and which generations this
/// thread holds wrappers for.
SurfaceRef gPresenting{};
bool gHavePresenting = false;
std::vector<GenerationId> gWrappedGenerations;

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

/// The platform thread's tracked window state: the `window` snapshot of
/// the last event it converted, refreshed from `sapp_*` queries before each
/// real event (the converter handles focus and minimize, not size).
/// Platform thread only.
WindowState gTracked;

/// Whether the tick machine has been told the tracked minimized state. An
/// ICONIFIED that arrives before `gPresentReady` has no machine to tell, so
/// the first frame after readiness catches it up. Platform thread only.
bool gTickMinimizedSynced = false;

/// Test hook: platform events injected by `__injectEvent`, taken by
/// `onFrame`. Carries its own lock.
PlatformEventInjector gInjector;

/// The synchronous native clipboard reads (`hnui_read_clipboard_sync`):
/// the JS thread creates a slot here and blocks on it, and the platform
/// thread answers into it directly. Carries its own lock. A file-static
/// for the reason `gCommands` is one. A JavaScript `readClipboard()` does
/// not use it: its answer goes through `gInput` as a `kClipboardReply`.
ClipboardRequests gClipboardReads;

/// The most the platform thread will wait for another client's selection
/// reply, whatever deadline the request carries. Every frame and every
/// event waits behind it, and a native caller may pass a deadline of a
/// minute; that caller gets kTimeout after this long instead.
constexpr int kClipboardReadCapMs = 2000;

//===========================================================================
// Test hooks (lib/compositor-gpu/test_hooks.h)
//===========================================================================
//
// Compiled always, enabled only by HERMES_NODE_UI_TEST_HOOKS, and every
// call site below checks `uiTestHooksEnabled()` first -- nothing in
// test_hooks.h enforces that, so it is a convention this file has to keep
// rather than one it inherits.

CompletionGate gGate;
FrameCapture gCapture;

/// What the platform thread owes the JS thread once a capture resolves.
/// Guarded by gCaptureMutex, which also guards the request's own
/// parameters: the JS thread writes them, the platform thread reads them.
struct CaptureState {
  std::string path;
  int giveUpAfterFrames = 0;
  int framesObserved = 0;
  bool finished = false;
  bool matched = false;
  uint64_t publicationId = 0;
  int width = 0;
  int height = 0;
};
std::mutex gCaptureMutex;
CaptureState gCaptureState;
/// JS thread only.
napi_deferred gCaptureDeferred = nullptr;

//===========================================================================
// The referenced libuv handle
//===========================================================================

/// Taken **when `createWindow` is called**, not when the window opens
/// (spec: Lifecycle). The gap between the two is a round trip to another
/// thread, and during it the runtime has nothing else keeping it busy: with
/// no referenced handle, `uv_run` would return, the runtime would tear
/// down, and the window would open into a process on its way out.
///
/// Nothing releases it yet. The lifecycle hook that closes it before
/// runtime cleanup is Task 15's, and until it exists a script that calls
/// createWindow and then simply returns will wait forever inside
/// `UvEventLoop::close()`'s `uv_run(UV_RUN_DEFAULT)`. `process.exit()` is
/// the path that works today, and is also the one the spec describes as how
/// a UI process ends normally. test/ui-create-window.js says the same at
/// its call site.
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
void releaseReferencedHandle() {
  if (gKeepAlive == nullptr)
    return;
  uv_close(reinterpret_cast<uv_handle_t *>(gKeepAlive), [](uv_handle_t *h) {
    delete reinterpret_cast<uv_async_t *>(h);
  });
  gKeepAlive = nullptr;
}

//===========================================================================
// JS thread: settling the Promise
//===========================================================================

void rejectWith(const std::string &message) {
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

/// Adds \p name = \p value to \p object. Errors are dropped on purpose:
/// every call here sets a plain number or boolean on a freshly created
/// object, so the only way one fails is an env that is already throwing,
/// and in that state the caller's own next NAPI call reports it.
void setNumber(napi_value object, const char *name, double value) {
  napi_value number = nullptr;
  if (napi_create_double(gEnv, value, &number) == napi_ok)
    napi_set_named_property(gEnv, object, name, number);
}

void setBool(napi_value object, const char *name, bool value) {
  napi_value boolean = nullptr;
  if (napi_get_boolean(gEnv, value, &boolean) == napi_ok)
    napi_set_named_property(gEnv, object, name, boolean);
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

/// One frame tick, on the JS thread, posted by the platform thread when
/// `TickMachine::shouldSendTick()` said so.
void runFrameTick() {
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
      // A native callback holds nothing of ours to release, and is C:
      // there is no exception for it to throw.
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

//===========================================================================
// Event dispatch
//===========================================================================

/// JS thread, posted by postEventDispatch(). Everything after the flag is
/// ui_events.cpp's, reached through the window alone.
void runEventDispatch() {
  // Cleared BEFORE draining, never after: see gDispatchPosted.
  gDispatchPosted.store(false);
  uiDispatchEvents(gWindow);
}

/// Any thread: the platform thread after pushing an event, the JS thread
/// once the window object exists.
void postEventDispatch() {
  if (gDispatchPosted.exchange(true))
    return;
  // post() refuses only once the dispatcher is closed, which is Task 15's
  // teardown. The events stay queued, and the flag is put back so that a
  // later event tries again instead of finding it stuck at "posted".
  if (gDispatch == nullptr || !gDispatch->post(runEventDispatch))
    gDispatchPosted.store(false);
}

//===========================================================================
// Frame capture (test hook)
//===========================================================================

/// Hand the finished capture back to the JavaScript that asked for it.
/// JS thread, posted by the platform thread.
void onCaptureFinished() {
  if (gCaptureDeferred == nullptr || gEnv == nullptr)
    return;

  CaptureState state;
  {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    state = gCaptureState;
  }

  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(gEnv, &scope) != napi_ok)
    return;

  napi_value result = nullptr;
  if (napi_create_object(gEnv, &result) == napi_ok) {
    setBool(result, "matched", state.matched);
    setNumber(result, "framesObserved", state.framesObserved);
    setNumber(
        result, "publicationId", static_cast<double>(state.publicationId));
    setNumber(result, "width", state.width);
    setNumber(result, "height", state.height);
    napi_value path = nullptr;
    if (napi_create_string_utf8(
            gEnv, state.path.c_str(), NAPI_AUTO_LENGTH, &path) == napi_ok) {
      napi_set_named_property(gEnv, result, "path", path);
    }
    napi_resolve_deferred(gEnv, gCaptureDeferred, result);
  }
  gCaptureDeferred = nullptr;
  napi_close_handle_scope(gEnv, scope);
}

/// Write a captured frame as a binary PPM (P6).
///
/// A deliberately dull format: a five-byte header and three bytes per
/// pixel, which a test can parse in a few lines of JavaScript with no
/// decoder. The alpha channel is dropped because the comparison is over
/// colour and the swapchain's alpha is not something the pattern controls.
bool writeCapturePpm(
    const std::string &path,
    int width,
    int height,
    const std::vector<uint8_t> &rgba) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;
  std::fprintf(file, "P6\n%d %d\n255\n", width, height);
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  const size_t written = std::fwrite(rgb.data(), 1, rgb.size(), file);
  std::fclose(file);
  return written == rgb.size();
}

/// Report what this frame actually presented, and take a capture if it is
/// the frame somebody asked for. Platform thread, once per presented
/// frame, immediately after the blit.
///
/// **Matching, not polling** (`test_hooks.h`): the presenter says what it
/// drew and finds out in the same call whether that was the frame
/// requested. Every field must agree -- and the one that does the work
/// here is `publicationId`, because a re-blit of the still-presenting
/// surface reports every other field identically.
void maybeCapture() {
  if (!uiTestHooksEnabled() || !gCapture.pending())
    return;

  CaptureRequest observed;
  observed.generation = gPresenting.gen;
  observed.publicationId = gTick->presentingPublicationId();
  // The presented surface's OWN size, keyed by its generation -- never
  // gGenerationWidth/Height, which the JS thread rewrites on a resize and
  // which would then describe a generation other than the one on screen
  // (and be a data race besides). The generation is alive here: this
  // thread holds its wrappers.
  int targetWidth = 0;
  int targetHeight = 0;
  if (gBackend->generationSize(gPresenting.gen, &targetWidth, &targetHeight)) {
    observed.targetWidth = targetWidth;
    observed.targetHeight = targetHeight;
  }
  observed.framebufferWidth = sapp_width();
  observed.framebufferHeight = sapp_height();
  observed.dpiScale = gMetrics.dpiScale;
  // Task 11's input path is what makes this ever non-zero. Stated rather
  // than omitted: a test that asks for a non-zero serial today would
  // never match, and that is the correct answer rather than a bug.
  observed.inputSerial = 0;

  std::string path;
  int giveUpAfterFrames = 0;
  int framesObserved = 0;
  {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    if (gCaptureState.finished)
      return;
    path = gCaptureState.path;
    giveUpAfterFrames = gCaptureState.giveUpAfterFrames;
    framesObserved = ++gCaptureState.framesObserved;
  }

  if (gCapture.tryMatch(observed)) {
    const int width = observed.framebufferWidth;
    const int height = observed.framebufferHeight;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    const bool read =
        gBackend->readPresentedPixels(width, height, pixels.data());
    const bool wrote = read && writeCapturePpm(path, width, height, pixels);
    {
      std::lock_guard<std::mutex> lock(gCaptureMutex);
      gCaptureState.finished = true;
      gCaptureState.matched = wrote;
      gCaptureState.publicationId = observed.publicationId;
      gCaptureState.width = width;
      gCaptureState.height = height;
    }
    uiTrace(
        "capture: %s generation %u publication %llu (%dx%d)",
        wrote ? "written" : "FAILED",
        observed.generation.value,
        static_cast<unsigned long long>(observed.publicationId),
        width,
        height);
    // The dropped bool, same shape as the tick post in onFrame() and with
    // the same owner: a refused post here leaves __captureFrame()'s
    // Promise unsettled, bounded only by the caller's own timeout. Only
    // reachable once the dispatcher is closed (Task 15).
    if (gDispatch != nullptr)
      gDispatch->post(onCaptureFinished);
    return;
  }

  // Not this frame. A budget in FRAMES, not milliseconds: the suite runs
  // 16-way parallel and the plan forbids timing-based assertions, so a
  // test that needs to prove a request was NOT satisfied counts presented
  // frames instead of waiting a while.
  if (giveUpAfterFrames > 0 && framesObserved >= giveUpAfterFrames) {
    gCapture.cancel();
    {
      std::lock_guard<std::mutex> lock(gCaptureMutex);
      gCaptureState.finished = true;
      gCaptureState.matched = false;
    }
    uiTrace(
        "capture: no match after %d presented frame(s); publication %llu "
        "was presenting",
        framesObserved,
        static_cast<unsigned long long>(observed.publicationId));
    // As above: a refused post leaves the Promise unsettled (Task 15).
    if (gDispatch != nullptr)
      gDispatch->post(onCaptureFinished);
  }
}

/// Destroy the presenting instance's wrappers for every generation that
/// has stopped needing them.
///
/// **On a trigger, not by waiting** (spec: Generations). Waiting for the
/// reference count to reach zero would deadlock, since the wrappers are
/// themselves counted owners; `wrappersReleasable()` is the trigger, and
/// it is asked only about generations this thread actually holds wrappers
/// for.
void releaseObsoleteWrappers() {
  for (size_t i = 0; i < gWrappedGenerations.size();) {
    const GenerationId gen = gWrappedGenerations[i];
    if (gTable->wrappersReleasable(gen)) {
      gBackend->releaseWrappers(gen);
      gTable->releaseWrappers(gen);
      gWrappedGenerations.erase(
          gWrappedGenerations.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
}

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

/// Runs on the JS thread, posted from `init_cb`. This is stage 2b: the
/// worker binds the shared context the platform thread created, which only
/// the thread that will use it can do.
void onSharedContextReady() {
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

  gWindow = new hnui_window();
  gWindow->ownerThread = std::this_thread::get_id();
  gWindow->table = gTable;
  gWindow->tick = gTick;
  gWindow->backend = gBackend;
  gWindow->env = gEnv;
  // The mirror starts from init_cb's snapshot; every event after that
  // carries its own (Ruling 52).
  gWindow->state = gMetrics;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gWindow->title = gRequest.title;
  }
  gWindow->events = std::make_unique<EventDispatcher>(gInput);

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
    napi_close_handle_scope(gEnv, scope);
    rejectWith(surfaceError);
    return;
  }

  // The window object: an EventEmitter, kept current from gWindow->state
  // by ui_events.cpp. Built before the store below, so that a failure
  // here, like the surface failure above, rejects with nothing yet
  // published to the platform thread.
  std::string objectError;
  napi_value window = uiCreateWindowObject(gWindow, objectError);
  if (window == nullptr) {
    napi_close_handle_scope(gEnv, scope);
    rejectWith(objectError);
    return;
  }

  // Before the store below: once it is made, the platform thread may send
  // the first tick, and that tick reads the origin.
  std::string originError;
  if (!computeFrameOrigin(originError)) {
    napi_close_handle_scope(gEnv, scope);
    rejectWith(originError);
    return;
  }

  // Everything above is published to the platform thread by this store,
  // which its frame callback acquires. Nothing before it may be read
  // there, and nothing after it may be written here.
  gPresentReady.store(true, std::memory_order_release);

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
  // any post made for them has already run and found nothing to deliver
  // to. Posted rather than run here, so that the script's `await
  // createWindow()` continuation -- where its listeners get attached --
  // usually runs first. Not always: a dispatch the platform thread posted
  // after init_cb and before the JS thread woke for this function is taken
  // by the same OwnerDispatch drain pass and runs right after it, before
  // the continuation. Such an event still updates the mirror; it is
  // emitted before the script's listeners exist.
  postEventDispatch();
}

void onStartupFailed() {
  uiTrace("window failed: %s", gStartupError.c_str());
  rejectWith(gStartupError);
}

} // namespace

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

namespace {

//===========================================================================
// Platform thread: Sokol callbacks
//===========================================================================

/// Sokol's logger, and the fatal half of the spec's "Failure splits in
/// two".
///
/// Window and context creation failing is fatal **by contract**: Sokol
/// routes it to `_SAPP_PANIC` from inside `sapp_run()`, before `init_cb`
/// ever runs, and there is no recovery path. Returning from a logger does
/// not make Sokol stop -- it carries on into the state the panic was about
/// -- so this terminates instead.
void sokolLog(
    const char *tag,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    void *userData) {
  (void)userData;

  static const char *const kLevels[] = {"panic", "error", "warning", "info"};
  const char *level = logLevel < 4 ? kLevels[logLevel] : "?";

  std::fprintf(
      stderr,
      "[%s:%s] item %u%s%s (%s:%u)\n",
      tag ? tag : "sokol",
      level,
      logItemId,
      message ? ": " : "",
      message ? message : "",
      filename ? filename : "sokol",
      lineNr);
  std::fflush(stderr);

  if (logLevel != 0)
    return;

  std::fprintf(
      stderr,
      "hermes-node-ui: the windowing system could not be initialised. This "
      "is fatal: sokol_app panics from inside sapp_run(), before any "
      "callback of ours runs, and offers no way to recover.\n");
  std::fflush(stderr);

  // Every _exit() path in this tree restores the terminal, and this is the
  // fourth. See CLAUDE.md's Bootstrap Sequence section: raw mode is a
  // libuv global, and leaving it set hands the user back a shell that no
  // longer echoes. Nothing here flushes the JS thread's queued stdio the
  // way processExit() does -- that thread is still live and owns the loop,
  // so pumping it from here would be a data race rather than a fix.
  uv_tty_reset_mode();
  _exit(1);
}

/// Bring the tracked state's size, DPI scale and fullscreen flag up to date
/// from Sokol. The converter handles what an event itself changes --
/// focus, minimize -- and copies the rest from its source, so a RESIZED
/// that was not preceded by this would report the old size.
///
/// On Linux sokol_app maps window size to framebuffer size 1:1
/// (sokol_app.h, _sapp_linux_run), so sapp_width() is both. Divided by the
/// DPI scale anyway rather than assumed equal: the logical/physical split
/// is real on every other platform this interface will meet, and a
/// framebuffer size that silently doubles as a logical one is the kind of
/// thing that reads correctly right up until it does not.
void refreshTrackedState() {
  const float dpiScale = sapp_dpi_scale();
  gTracked.framebufferWidth = sapp_width();
  gTracked.framebufferHeight = sapp_height();
  gTracked.dpiScale = dpiScale;
  gTracked.width = dpiScale > 0.0f ? static_cast<int>(sapp_widthf() / dpiScale)
                                   : sapp_width();
  gTracked.height = dpiScale > 0.0f
      ? static_cast<int>(sapp_heightf() / dpiScale)
      : sapp_height();
  gTracked.fullscreen = sapp_is_fullscreen();
}

const char *clipboardStatusName(ClipboardStatus status) {
  switch (status) {
    case ClipboardStatus::kOk:
      return "ok";
    case ClipboardStatus::kUnavailable:
      return "unavailable";
    case ClipboardStatus::kTimeout:
      return "timeout";
    case ClipboardStatus::kTooLarge:
      return "too-large";
    case ClipboardStatus::kError:
      return "error";
  }
  return "error";
}

/// Read the clipboard now, waiting at most \p timeoutMs for another
/// client's reply. Platform thread only.
///
/// On X11 this is our own read (Ruling 55, `x11_clipboard.h`), never
/// `sapp_get_clipboard_string()` alone, whose failures return its previous
/// buffer. Elsewhere -- macOS, with Task 16 -- Sokol's getter reads the
/// pasteboard directly and has no stale-buffer path.
ClipboardResult readClipboardNow(int timeoutMs) {
#ifdef HERMES_NODE_UI_HAVE_X11
  return x11ReadClipboard(
      const_cast<void *>(sapp_x11_get_display()),
      reinterpret_cast<unsigned long>(sapp_x11_get_window()),
      timeoutMs,
      HNUI_CLIPBOARD_MAX_BYTES - 1);
#else
  (void)timeoutMs;
  const char *text = sapp_get_clipboard_string();
  if (text == nullptr)
    return ClipboardResult{ClipboardStatus::kError, std::string()};
  return ClipboardResult{ClipboardStatus::kOk, text};
#endif
}

/// A PlatformSource over the real platform, as of now.
PlatformSource realPlatformSource() {
  PlatformSource source;
  source.window = gTracked;
  // A paste carries the text read when it happened, through the same read
  // `readClipboard()` uses. A read that did not succeed means the paste is
  // not forwarded (the converter refuses it) -- an empty-but-successful
  // read still is, since pasting nothing is a real paste.
  source.readClipboard = [](std::string &out) {
    ClipboardResult result = readClipboardNow(kClipboardReadDefaultTimeoutMs);
    if (result.status != ClipboardStatus::kOk) {
      uiTrace(
          "paste: clipboard read %s; not forwarded",
          clipboardStatusName(result.status));
      return false;
    }
    out = std::move(result.text);
    return true;
  };
  source.droppedPaths = [] {
    std::vector<std::string> paths;
    const int count = sapp_get_num_dropped_files();
    for (int i = 0; i < count; ++i)
      paths.emplace_back(sapp_get_dropped_file_path(i));
    return paths;
  };
  return source;
}

/// Hand every injected event to uiHandlePlatformEvent(), with the injected
/// clipboard, paths and window substituted where present.
///
/// **One post for the whole batch.** The flag is held for the duration
/// (taken only once there is a first event, so an empty frame never holds
/// it), so no dispatch is posted until every event of the batch is queued,
/// and a batch that a test injected together is delivered by one dispatch
/// -- which is what lets test/ui-events.js tell "promise callbacks ran
/// between two events" apart from "they ran after the dispatch". Two
/// cases still split a batch, and neither loses anything: a dispatch
/// already outstanding when the batch began may run mid-batch (the events
/// after it then post for themselves, the ordinary path), and one already
/// running on the JS thread drains events as they land.
void drainInjectedEvents() {
  InjectedPlatformEvent injected;
  if (!gInjector.takeNext(&injected))
    return;
  const bool batched = !gDispatchPosted.exchange(true);

  do {
    refreshTrackedState();
    PlatformSource source = realPlatformSource();
    if (injected.hasClipboard) {
      source.readClipboard = [&injected](std::string &out) {
        out = injected.clipboard;
        return true;
      };
    }
    // Always substituted: Sokol's drop buffer holds nothing for an event
    // Sokol did not produce, and sapp_get_dropped_file_path() asserts on
    // an index it did not hand out.
    source.droppedPaths = [&injected] { return injected.paths; };
    if (injected.hasWindow)
      source.window = injected.window;
    uiHandlePlatformEvent(injected.event, source);
  } while (gInjector.takeNext(&injected));

  if (!batched)
    return;
  // Released, then the queue is checked -- not "did this batch push
  // anything". While the flag was held, a postEventDispatch() from the JS
  // thread (the one onSharedContextReady makes for events that predate the
  // window object) returned without posting, on the strength of this
  // holder; checking the queue is what keeps that promise. A batch whose
  // every event the converter refused is covered by the same check.
  gDispatchPosted.store(false);
  if (!gInput.empty())
    postEventDispatch();
}

//===========================================================================
// Platform commands
//===========================================================================

/// The one place an `hnui_cursor` becomes a Sokol cursor. Explicit rather
/// than a cast: the two enumerations are named alike and happen to line up
/// today, and nothing but this table keeps them honest when either moves.
sapp_mouse_cursor sappCursorFor(int cursor) {
  switch (static_cast<hnui_cursor>(cursor)) {
    case HNUI_CURSOR_DEFAULT:
      return SAPP_MOUSECURSOR_DEFAULT;
    case HNUI_CURSOR_ARROW:
      return SAPP_MOUSECURSOR_ARROW;
    case HNUI_CURSOR_IBEAM:
      return SAPP_MOUSECURSOR_IBEAM;
    case HNUI_CURSOR_CROSSHAIR:
      return SAPP_MOUSECURSOR_CROSSHAIR;
    case HNUI_CURSOR_POINTING_HAND:
      return SAPP_MOUSECURSOR_POINTING_HAND;
    case HNUI_CURSOR_RESIZE_EW:
      return SAPP_MOUSECURSOR_RESIZE_EW;
    case HNUI_CURSOR_RESIZE_NS:
      return SAPP_MOUSECURSOR_RESIZE_NS;
    case HNUI_CURSOR_RESIZE_NWSE:
      return SAPP_MOUSECURSOR_RESIZE_NWSE;
    case HNUI_CURSOR_RESIZE_NESW:
      return SAPP_MOUSECURSOR_RESIZE_NESW;
    case HNUI_CURSOR_RESIZE_ALL:
      return SAPP_MOUSECURSOR_RESIZE_ALL;
    case HNUI_CURSOR_NOT_ALLOWED:
      return SAPP_MOUSECURSOR_NOT_ALLOWED;
    case HNUI_CURSOR_FORCE_I32:
      break;
  }
  return SAPP_MOUSECURSOR_DEFAULT;
}

/// A `Command::readClipboard`. Platform thread.
///
/// The read is bounded by the request's remaining deadline and by
/// kClipboardReadCapMs. **Nothing here waits on the JS thread**: a sync
/// read's answer is written into its slot from this thread, because the JS
/// thread is blocked on that slot and could never drain a queued reply.
void applyReadClipboard(const Command &command) {
  const uint64_t id = command.requestId();
  const bool viaSlot = command.replyThroughSlot();
  const char *path = viaSlot ? "sync" : "promise";

  ClipboardResult result;
  const uint64_t now = clipboardClockNs();
  if (now >= command.deadlineNs()) {
    // Its waiter has already given up (or is about to): reading would block
    // this thread for an answer nobody takes. A Promise still has to
    // settle, so it is told kTimeout; a sync waiter needs nothing.
    uiTrace(
        "clipboard read %s #%llu: deadline passed; skipped",
        path,
        static_cast<unsigned long long>(id));
    if (viaSlot)
      return;
    result.status = ClipboardStatus::kTimeout;
  } else {
    const uint64_t remainingNs = command.deadlineNs() - now;
    // Rounded up: a deadline 0.4 ms away is a short wait, not none.
    const uint64_t remainingMs = (remainingNs + 999999) / 1000000;
    const int timeoutMs = static_cast<int>(std::min<uint64_t>(
        remainingMs, static_cast<uint64_t>(kClipboardReadCapMs)));
    result = readClipboardNow(timeoutMs);
    uiTrace(
        "clipboard read %s #%llu: status=%s bytes=%zu",
        path,
        static_cast<unsigned long long>(id),
        clipboardStatusName(result.status),
        result.text.size());
  }

  if (viaSlot) {
    if (!gClipboardReads.answer(id, std::move(result))) {
      uiTrace(
          "clipboard read sync #%llu: waiter gone; reply discarded",
          static_cast<unsigned long long>(id));
    }
    return;
  }

  UiEvent reply;
  reply.type = UiEventType::kClipboardReply;
  reply.requestId = id;
  reply.clipboardStatus = result.status;
  reply.text = std::move(result.text);
  gInput.push(reply);
  postEventDispatch();
}

/// Apply one command with the matching sapp_* call. Platform thread only.
/// **Each trace prints the value read back afterwards**, never the
/// request, so a trace cannot pass with its call removed.
void applyCommand(const Command &command) {
  switch (command.kind()) {
    case Command::Kind::kSetTitle: {
      sapp_set_window_title(command.title().c_str());
      // The read-backs below exist for the trace and run only when it is on:
      // each is a synchronous X round trip.
      if (!uiTracingEnabled())
        break;
#ifdef HERMES_NODE_UI_HAVE_X11
      std::string name;
      if (uiX11FetchWindowName(name))
        uiTrace("command title: readback='%s'", name.c_str());
      else
        uiTrace("command title: no readback available");
#else
      uiTrace(
          "command title: applied '%s' (unverified: no read-back on this "
          "platform)",
          command.title().c_str());
#endif
      break;
    }
    case Command::Kind::kSetFullscreen:
      // sapp_toggle_fullscreen() toggles; compare first.
      if (sapp_is_fullscreen() != command.flag())
        sapp_toggle_fullscreen();
      uiTrace("command fullscreen: is_fullscreen=%d", sapp_is_fullscreen());
      break;
    case Command::Kind::kSetIcon: {
      sapp_icon_desc icon = {};
      icon.images[0].width = command.width();
      icon.images[0].height = command.height();
      icon.images[0].pixels.ptr = command.cursorRgba().data();
      icon.images[0].pixels.size = command.cursorRgba().size();
      sapp_set_icon(&icon);
      if (!uiTracingEnabled())
        break;
#ifdef HERMES_NODE_UI_HAVE_X11
      int w = 0, h = 0;
      uint32_t first = 0;
      if (uiX11FetchIconHead(w, h, first))
        uiTrace(
            "command icon: _NET_WM_ICON %dx%d first_pixel=0x%08x", w, h, first);
      else
        uiTrace("command icon: no _NET_WM_ICON readback");
#else
      uiTrace(
          "command icon: applied (unverified: no read-back on this platform)");
#endif
      break;
    }
    case Command::Kind::kSetCursor: {
      sapp_set_mouse_cursor(sappCursorFor(command.cursor()));
      uiTrace(
          "command cursor %d: sapp_mouse_cursor=%d",
          command.cursor(),
          static_cast<int>(sapp_get_mouse_cursor()));
      break;
    }
    case Command::Kind::kSetCursorImage: {
      sapp_image_desc image = {};
      image.width = command.width();
      image.height = command.height();
      image.cursor_hotspot_x = command.hotspotX();
      image.cursor_hotspot_y = command.hotspotY();
      image.pixels.ptr = command.cursorRgba().data();
      image.pixels.size = command.cursorRgba().size();
      // sapp_bind_mouse_cursor_image returns the cursor it was given whether
      // or not binding worked, so its result is not traced: only the
      // selection is read back, and the bind itself is unverified.
      sapp_bind_mouse_cursor_image(SAPP_MOUSECURSOR_CUSTOM_0, &image);
      sapp_set_mouse_cursor(SAPP_MOUSECURSOR_CUSTOM_0);
      uiTrace(
          "command custom cursor: sapp_mouse_cursor=%d",
          static_cast<int>(sapp_get_mouse_cursor()));
      break;
    }
    case Command::Kind::kShowCursor:
      sapp_show_mouse(command.flag());
      uiTrace("command show cursor: sapp_mouse_shown=%d", sapp_mouse_shown());
      break;
    case Command::Kind::kLockPointer:
      sapp_lock_mouse(command.flag());
      uiTrace(
          "command lock pointer: sapp_mouse_locked=%d", sapp_mouse_locked());
      break;
    case Command::Kind::kReadClipboard:
      applyReadClipboard(command);
      break;
    case Command::Kind::kWriteClipboard: {
      sapp_set_clipboard_string(command.text().c_str());
      if (!uiTracingEnabled())
        break;
#ifdef HERMES_NODE_UI_HAVE_X11
      // Ownership, asked of the server. sapp_get_clipboard_string() is not
      // used: when we are not the owner it runs a selection conversion that
      // stalls this thread and overwrites Sokol's buffer.
      uiTrace(
          "command clipboard: owns_selection=%d bytes=%zu",
          uiX11OwnsClipboard() ? 1 : 0,
          command.text().size());
#else
      uiTrace(
          "command clipboard: written %zu bytes (unverified: no read-back on "
          "this platform)",
          command.text().size());
#endif
      break;
    }
  }
}

void drainCommands() {
  std::vector<Command> commands;
  gCommands.drain(commands);
  for (const Command &command : commands)
    applyCommand(command);
}

void onInit() {
  std::string error;
  if (!gBackend->createSharedContext(error)) {
    // The recoverable half. The main loop keeps running -- this returns
    // into it -- so that the rejection reaches JS rather than racing a
    // process teardown.
    gStartupError = error;
    gStartupFailed = true;
    if (gDispatch != nullptr)
      gDispatch->post(onStartupFailed);
    return;
  }

  refreshTrackedState();
  gMetrics = gTracked;

  uiTrace("init_cb: shared context ready");
  if (gDispatch != nullptr)
    gDispatch->post(onSharedContextReady);
}

/// The presenting half, once per platform frame: take a newly published
/// surface if there is one, blit it or the previous one, release completed
/// reads, and send the frame tick if one is due (spec: Architecture,
/// `frame_cb`). Draining the command queue joins this list in Task 11.
void onFrame() {
  if (gBackend == nullptr)
    return;

  // **Before the readiness check, and unconditionally.** This is where the
  // stage-2 completion probe is answered, and the worker is blocked
  // waiting for that answer before the window's Promise can resolve -- so
  // a frame callback that skipped it while "not ready yet" would deadlock
  // the very startup it is part of.
  gBackend->pollCompletions();

  // Commands from JavaScript, before the injected events: a test that
  // issues commands and then injects a marker relies on the marker never
  // overtaking them. Before the readiness check, like the completions, so
  // a command pushed early is applied by the first frame rather than left
  // behind.
  drainCommands();

  // Injected events, immediately after the completions. Before the
  // readiness check like them, so that an event injected early is queued
  // rather than left behind -- the JS side delivers nothing until its
  // window object exists anyway.
  if (uiTestHooksEnabled())
    drainInjectedEvents();

  // Exactly one acquisition per frame, whichever of the three presenting
  // calls below follows it.
  if (!gBackend->acquireFrameSwapchain()) {
    // No usable swapchain: present nothing, and release nothing, because
    // nothing was submitted. GL never reports this; macOS does, for an
    // obscured window.
    gBackend->presentInvalid();
    return;
  }

  if (!gPresentReady.load(std::memory_order_acquire)) {
    // The window exists from the moment sapp_run() starts, and the JS
    // thread has not finished binding its context yet. Something still
    // has to go on screen: _sapp_linux_frame swaps unconditionally after
    // this returns, so skipping the clear would present undefined
    // back-buffer contents rather than nothing.
    gBackend->presentClear();
    return;
  }

  // An ICONIFIED converted before readiness had no machine to tell.
  if (!gTickMinimizedSynced) {
    gTick->setMinimized(gTracked.minimized);
    gTickMinimizedSynced = true;
  }

  // The composed gate is used below for the tick; the take is composed in
  // the same way, and for the same reason -- takeFrame() moves the
  // machine's state under the table's own lock, so the presenter cannot
  // take a frame the machine has not finished publishing.
  std::optional<SurfaceRef> taken = gTick->takeFrame(*gTable);
  if (taken) {
    std::string error;
    if (!gBackend->wrapForPresenting(taken->gen, error))
      terminateGraphicsSession(error);
    if (std::find(
            gWrappedGenerations.begin(),
            gWrappedGenerations.end(),
            taken->gen) == gWrappedGenerations.end()) {
      // One wrapper registration per generation, matching the one set of
      // wrappers the backend created. Each is a counted owner, which is
      // what keeps the generation alive while this thread can still draw
      // from it.
      gTable->addWrapper(taken->gen);
      gWrappedGenerations.push_back(taken->gen);
    }
    gPresenting = *taken;
    gHavePresenting = true;
  }

  if (!gHavePresenting) {
    // Nothing has ever been published. Attachment contents are initially
    // undefined (sokol_gfx.h:3714), so the presenter samples nothing and
    // shows a clear colour instead.
    gBackend->presentClear();
  } else {
    // **Every blit takes a read reference before submission**, including
    // a re-present of an unchanged surface, and releases it when that
    // blit completes on the GPU (spec: Slot states, Blit). The release is
    // onConsumerComplete's job.
    gTable->addRead(gPresenting);
    gBackend->blit(gPresenting);
    maybeCapture();
  }

  releaseObsoleteWrappers();

  // **The composed overload**, not anyFreeSlot() followed by
  // shouldSendTick(bool): the free-slot reading and the tick transition
  // happen in one critical section, so the answer cannot go stale between
  // them. `shouldSendTick(&table)` would bind to the bool overload
  // through a pointer conversion and silently mean "a slot is always
  // free"; TickMachine deletes that overload so it cannot compile.
  //
  // **Known edge, owned by Task 15's teardown.** shouldSendTick() CONSUMES
  // the request: a true return has already cleared `pending_` and moved
  // the machine to kTickQueued. So if gDispatch is null, or post() returns
  // false -- which happens once the dispatcher is closed -- nothing ever
  // calls beginCallbacks(), the machine stays in kTickQueued, and every
  // later shouldSendTickLocked() refuses on `state_ != kIdle`. The
  // animation is then dead with nothing printed. It is unreachable until
  // something closes the dispatcher, which only the lifecycle hook will.
  //
  // Putting the tick back is declined on semantics, not on absence: it IS
  // expressible with the methods that exist -- requestFrame() then
  // beginCallbacks() then endCallbacksDrew(false) returns the machine to
  // kIdle with the request pending, and beginCallbacks()'s assert holds --
  // but it would mean calling two methods that mean "the callbacks began"
  // and "the callbacks ended" when no callback ran. Whoever writes the
  // teardown should add a real entry point if one is needed, rather than
  // conclude from this comment that the class is missing something.
  // Recorded here so that edge is found rather than rediscovered as a hang.
  if (gTick->shouldSendTick(*gTable) && gDispatch != nullptr)
    gDispatch->post(runFrameTick);
}

void onEvent(const sapp_event *event) {
  refreshTrackedState();
  uiHandlePlatformEvent(*event, realPlatformSource());
}

void onCleanup() {
  uiTrace("cleanup_cb");
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

/// `requestAnimationFrame(cb)` -- exported by the module and installed as
/// a global once a window exists. Returns the id `cancelAnimationFrame`
/// takes, from the id space native callbacks share.
napi_value requestAnimationFrame(napi_env env, napi_callback_info info) {
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

/// `__captureFrame(options)` -- the JavaScript end of the frame-capture
/// test hook (`test_hooks.h`).
///
/// **The caller supplies every agreement field**, and that is the point
/// rather than an inconvenience: the capture is taken only when the
/// generation, the publication id, the target size, the framebuffer size,
/// the DPI scale and the input serial all match what the presenter
/// actually drew, so a test that wants a capture has to state which frame
/// it means. A hook that worked out the fields for itself would agree
/// with the presenter by construction and would test nothing.
///
/// `giveUpAfterFrames` bounds the wait **in presented frames, not in
/// milliseconds**: the suite runs 16-way parallel and the plan forbids
/// timing-based assertions, and the negative case this hook exists for
/// ("a re-blit must not satisfy a request for the next publication") can
/// only be established by letting some number of re-blits go past.
///
/// \return a Promise for
///   `{matched, framesObserved, publicationId, width, height, path}`.
napi_value captureFrame(napi_env env, napi_callback_info info) {
  if (!uiTestHooksEnabled()) {
    napi_throw_error(
        env,
        nullptr,
        "__captureFrame is only available when HERMES_NODE_UI_TEST_HOOKS is "
        "set.");
    return nullptr;
  }
  if (gTick == nullptr || gDispatch == nullptr) {
    napi_throw_error(env, nullptr, "__captureFrame: no window exists yet.");
    return nullptr;
  }
  if (gCaptureDeferred != nullptr) {
    napi_throw_error(
        env,
        nullptr,
        "__captureFrame: a capture is already outstanding. The hook holds "
        "at most one request, so a second would silently replace the first.");
    return nullptr;
  }

  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_valuetype type = napi_undefined;
  if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok ||
      type != napi_object) {
    napi_throw_type_error(
        env, nullptr, "__captureFrame: an options object is required.");
    return nullptr;
  }

  // Every field defaults to zero rather than to "whatever the presenter
  // has". An omitted field therefore means "must be zero", which is a
  // request that simply does not match -- the strict reading, and the one
  // that cannot accidentally capture the wrong frame.
  CaptureRequest want;
  int generation = 0;
  double publicationId = 0;
  int targetWidth = 0;
  int targetHeight = 0;
  int framebufferWidth = 0;
  int framebufferHeight = 0;
  double dpiScale = 0;
  double inputSerial = 0;
  int giveUpAfterFrames = 0;
  std::string path;

  readInt(env, argv[0], "generation", &generation);
  readDouble(env, argv[0], "publicationId", &publicationId);
  readInt(env, argv[0], "targetWidth", &targetWidth);
  readInt(env, argv[0], "targetHeight", &targetHeight);
  readInt(env, argv[0], "framebufferWidth", &framebufferWidth);
  readInt(env, argv[0], "framebufferHeight", &framebufferHeight);
  readDouble(env, argv[0], "dpiScale", &dpiScale);
  readDouble(env, argv[0], "inputSerial", &inputSerial);
  readInt(env, argv[0], "giveUpAfterFrames", &giveUpAfterFrames);
  readString(env, argv[0], "path", &path);

  want.generation = GenerationId{static_cast<uint32_t>(generation)};
  want.publicationId = static_cast<uint64_t>(publicationId);
  want.targetWidth = targetWidth;
  want.targetHeight = targetHeight;
  want.framebufferWidth = framebufferWidth;
  want.framebufferHeight = framebufferHeight;
  want.dpiScale = dpiScale;
  want.inputSerial = static_cast<uint64_t>(inputSerial);

  {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    // A fresh state, so that a previous capture's "finished" flag cannot
    // make this one return before it has seen a single frame.
    gCaptureState = CaptureState();
    gCaptureState.path = path;
    gCaptureState.giveUpAfterFrames = giveUpAfterFrames;
  }
  gCapture.request(want);

  napi_value promise = nullptr;
  if (napi_create_promise(env, &gCaptureDeferred, &promise) != napi_ok) {
    gCapture.cancel();
    gCaptureDeferred = nullptr;
    napi_throw_error(
        env, nullptr, "__captureFrame: could not create a Promise.");
    return nullptr;
  }
  return promise;
}

/// `__holdProducerCompletions()` / `__releaseProducerCompletions()` -- the
/// JavaScript end of `CompletionGate`'s producer half (`test_hooks.h`).
/// Exported only under HERMES_NODE_UI_TEST_HOOKS.
///
/// What it buys is a frame that is **deterministically still in flight**
/// when something else happens: hold, draw, do the other thing, release.
/// Without it "the GPU had not finished yet" is a race the test can only
/// hope to win -- test/ui-resize-generation.js needs a frame of a retired
/// generation to complete *after* the resize, so that its publication is
/// refused and its slot reclaimed, and that is the path it pins.
///
/// Release runs the queued completions **on the calling thread**, which is
/// the JS thread, rather than the one that would have carried them. That
/// is within onProducerComplete()'s contract, which already runs on either.
napi_value holdProducerCompletions(napi_env env, napi_callback_info info) {
  (void)info;
  gGate.holdProducer();
  uiTrace("test hook: producer completions held");
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value releaseProducerCompletions(napi_env env, napi_callback_info info) {
  (void)info;
  uiTrace("test hook: producer completions released");
  gGate.releaseProducer();
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

/// `__failPendingClipboardReads()`: what Task 15's shutdown will do to the
/// Promise reads, reachable now so the mechanism has a caller and a test.
napi_value failPendingClipboardReads(napi_env env, napi_callback_info info) {
  (void)info;
  uiFailPendingClipboardReads(env);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

/// The `SAPP_EVENTTYPE_` name without its prefix, or false.
bool sappEventTypeFromName(const std::string &name, sapp_event_type *out) {
  static const struct {
    const char *name;
    sapp_event_type type;
  } kTypes[] = {
      {"KEY_DOWN", SAPP_EVENTTYPE_KEY_DOWN},
      {"KEY_UP", SAPP_EVENTTYPE_KEY_UP},
      {"CHAR", SAPP_EVENTTYPE_CHAR},
      {"MOUSE_DOWN", SAPP_EVENTTYPE_MOUSE_DOWN},
      {"MOUSE_UP", SAPP_EVENTTYPE_MOUSE_UP},
      {"MOUSE_SCROLL", SAPP_EVENTTYPE_MOUSE_SCROLL},
      {"MOUSE_MOVE", SAPP_EVENTTYPE_MOUSE_MOVE},
      {"MOUSE_ENTER", SAPP_EVENTTYPE_MOUSE_ENTER},
      {"MOUSE_LEAVE", SAPP_EVENTTYPE_MOUSE_LEAVE},
      {"TOUCHES_BEGAN", SAPP_EVENTTYPE_TOUCHES_BEGAN},
      {"TOUCHES_MOVED", SAPP_EVENTTYPE_TOUCHES_MOVED},
      {"TOUCHES_ENDED", SAPP_EVENTTYPE_TOUCHES_ENDED},
      {"TOUCHES_CANCELLED", SAPP_EVENTTYPE_TOUCHES_CANCELLED},
      {"RESIZED", SAPP_EVENTTYPE_RESIZED},
      {"ICONIFIED", SAPP_EVENTTYPE_ICONIFIED},
      {"RESTORED", SAPP_EVENTTYPE_RESTORED},
      {"FOCUSED", SAPP_EVENTTYPE_FOCUSED},
      {"UNFOCUSED", SAPP_EVENTTYPE_UNFOCUSED},
      {"SUSPENDED", SAPP_EVENTTYPE_SUSPENDED},
      {"RESUMED", SAPP_EVENTTYPE_RESUMED},
      {"QUIT_REQUESTED", SAPP_EVENTTYPE_QUIT_REQUESTED},
      {"CLIPBOARD_PASTED", SAPP_EVENTTYPE_CLIPBOARD_PASTED},
      {"FILES_DROPPED", SAPP_EVENTTYPE_FILES_DROPPED},
  };
  for (const auto &entry : kTypes) {
    if (name == entry.name) {
      *out = entry.type;
      return true;
    }
  }
  return false;
}

/// Reads an array of strings, leaving \p out alone when absent. An
/// element that is not a string is skipped.
void readStringArray(
    napi_env env,
    napi_value options,
    const char *name,
    std::vector<std::string> *out) {
  napi_value value = nullptr;
  bool isArray = false;
  if (napi_get_named_property(env, options, name, &value) != napi_ok ||
      napi_is_array(env, value, &isArray) != napi_ok || !isArray) {
    return;
  }
  uint32_t length = 0;
  if (napi_get_array_length(env, value, &length) != napi_ok)
    return;
  out->clear();
  for (uint32_t i = 0; i < length; ++i) {
    napi_value element = nullptr;
    napi_valuetype type = napi_undefined;
    if (napi_get_element(env, value, i, &element) != napi_ok ||
        napi_typeof(env, element, &type) != napi_ok || type != napi_string) {
      continue;
    }
    size_t size = 0;
    if (napi_get_value_string_utf8(env, element, nullptr, 0, &size) !=
        napi_ok) {
      continue;
    }
    std::string text(size, '\0');
    if (napi_get_value_string_utf8(env, element, &text[0], size + 1, &size) ==
        napi_ok) {
      text.resize(size);
      out->push_back(std::move(text));
    }
  }
}

/// Whether \p options has a property \p name that is not undefined.
bool hasProperty(napi_env env, napi_value options, const char *name) {
  napi_value value = nullptr;
  napi_valuetype type = napi_undefined;
  return napi_get_named_property(env, options, name, &value) == napi_ok &&
      napi_typeof(env, value, &type) == napi_ok && type != napi_undefined;
}

/// `__injectEvent(desc)` -- the JavaScript end of event injection
/// (`event_injector.h`). Exported only under HERMES_NODE_UI_TEST_HOOKS.
///
/// **A platform event, not a UiEvent**: `desc` describes a `sapp_event`
/// -- `type` is the `SAPP_EVENTTYPE_` name without the prefix, `button`
/// uses Sokol's numbering, `modifiers` Sokol's bits -- and the platform
/// thread converts it on its next frame exactly as it converts a real one.
/// `clipboard`, `paths` and `window` stand in for what the converter would
/// otherwise read from the platform. Every other field defaults to zero.
napi_value injectEvent(napi_env env, napi_callback_info info) {
  if (!uiTestHooksEnabled()) {
    napi_throw_error(
        env,
        nullptr,
        "__injectEvent is only available when HERMES_NODE_UI_TEST_HOOKS is "
        "set.");
    return nullptr;
  }

  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_valuetype argType = napi_undefined;
  if (argc < 1 || napi_typeof(env, argv[0], &argType) != napi_ok ||
      argType != napi_object) {
    napi_throw_type_error(
        env, nullptr, "__injectEvent: an event description is required.");
    return nullptr;
  }
  napi_value desc = argv[0];

  std::string typeName;
  readString(env, desc, "type", &typeName);
  InjectedPlatformEvent injected;
  if (!sappEventTypeFromName(typeName, &injected.event.type)) {
    const std::string message = "__injectEvent: unknown event type '" +
        typeName + "'; expected a SAPP_EVENTTYPE_ name without the prefix.";
    napi_throw_type_error(env, nullptr, message.c_str());
    return nullptr;
  }

  sapp_event &e = injected.event;
  int keyCode = 0;
  int button = 0;
  double modifiers = 0;
  double charCode = 0;
  double mouseX = 0, mouseY = 0, mouseDx = 0, mouseDy = 0;
  double scrollX = 0, scrollY = 0;
  bool repeat = false;
  readInt(env, desc, "keyCode", &keyCode);
  readBool(env, desc, "repeat", &repeat);
  readDouble(env, desc, "modifiers", &modifiers);
  readDouble(env, desc, "mouseX", &mouseX);
  readDouble(env, desc, "mouseY", &mouseY);
  readDouble(env, desc, "mouseDx", &mouseDx);
  readDouble(env, desc, "mouseDy", &mouseDy);
  readDouble(env, desc, "scrollX", &scrollX);
  readDouble(env, desc, "scrollY", &scrollY);
  readInt(env, desc, "button", &button);
  readDouble(env, desc, "charCode", &charCode);
  e.key_code = static_cast<sapp_keycode>(keyCode);
  e.key_repeat = repeat;
  e.modifiers = static_cast<uint32_t>(modifiers);
  e.mouse_x = static_cast<float>(mouseX);
  e.mouse_y = static_cast<float>(mouseY);
  e.mouse_dx = static_cast<float>(mouseDx);
  e.mouse_dy = static_cast<float>(mouseDy);
  e.scroll_x = static_cast<float>(scrollX);
  e.scroll_y = static_cast<float>(scrollY);
  e.mouse_button = static_cast<sapp_mousebutton>(button);
  e.char_code = static_cast<uint32_t>(charCode);

  if (hasProperty(env, desc, "clipboard")) {
    injected.hasClipboard = true;
    readString(env, desc, "clipboard", &injected.clipboard);
  }
  readStringArray(env, desc, "paths", &injected.paths);

  napi_value window = nullptr;
  napi_valuetype windowType = napi_undefined;
  if (napi_get_named_property(env, desc, "window", &window) == napi_ok &&
      napi_typeof(env, window, &windowType) == napi_ok &&
      windowType == napi_object) {
    injected.hasWindow = true;
    WindowState &state = injected.window;
    readInt(env, window, "width", &state.width);
    readInt(env, window, "height", &state.height);
    readInt(env, window, "framebufferWidth", &state.framebufferWidth);
    readInt(env, window, "framebufferHeight", &state.framebufferHeight);
    readDouble(env, window, "dpiScale", &state.dpiScale);
    readBool(env, window, "focused", &state.focused);
    readBool(env, window, "fullscreen", &state.fullscreen);
    readBool(env, window, "minimized", &state.minimized);
  }

  gInjector.inject(std::move(injected));

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value createWindow(napi_env env, napi_callback_info info) {
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
    releaseReferencedHandle();
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
  }
  return exports;
}

void uiHandlePlatformEvent(
    const sapp_event &event,
    const PlatformSource &source) {
  // Ruling 54: the close button emits `close` and the window stays open.
  // Cancelled BEFORE anything else, because Sokol acts on the flag as soon
  // as this callback returns; what an uncancelled close does is Task 15's.
  if (event.type == SAPP_EVENTTYPE_QUIT_REQUESTED) {
    sapp_cancel_quit();
    uiTrace("quit requested: cancelled, forwarded as close");
  }

  UiEvent converted;
  if (!convertSappEvent(event, source, &converted))
    return;
  gTracked = converted.window;

  // A minimized window receives no ticks (spec: Frames). Told here, on the
  // platform thread, before the event is even queued -- so by the time a
  // `minimize` listener requests a frame, the machine already withholds
  // it. Before readiness there is no machine; onFrame catches it up.
  if ((converted.type == UiEventType::kMinimize ||
       converted.type == UiEventType::kRestore) &&
      gPresentReady.load(std::memory_order_acquire)) {
    gTick->setMinimized(converted.window.minimized);
    gTickMinimizedSynced = true;
  }

  gInput.push(converted);
  postEventDispatch();
}

bool uiWindowRequested() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gWindowRequested;
}

void uiPushCommand(Command command) {
  gCommands.push(std::move(command));
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

int uiRunPlatformLoop() {
  gBackend = createGlBackend();
  if (gBackend == nullptr) {
    std::fprintf(
        stderr,
        "hermes-node-ui: no GPU backend is available for this build.\n");
    return 1;
  }

  // Stage 1, and it must happen here rather than in init_cb: Sokol ranks
  // its framebuffer config and creates its context inside sapp_run()
  // (sokol_app.h:14272-14274), long before init_cb is first called from the
  // frame path. A route decided afterwards could not change either.
  std::string error;
  if (!gBackend->selectContextRoute(error)) {
    std::fprintf(stderr, "hermes-node-ui: %s\n", error.c_str());
    std::fflush(stderr);
    // Posted as well as printed, so the program's own `catch` runs rather
    // than the Promise simply never settling. There is no Sokol loop yet to
    // keep alive for delivery -- sapp_run() has not been called -- so what
    // guarantees the JS thread gets to observe it is that UiHost joins that
    // thread after this returns.
    gStartupError = error;
    gStartupFailed = true;
    if (gDispatch != nullptr)
      gDispatch->post(onStartupFailed);
    return 1;
  }

  WindowRequest request;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    request = gRequest;
  }

  // What an event converted before init_cb reports, until init_cb's
  // refresh replaces the size. A new window is taken to have focus, which
  // is also what the JS object said before Task 11b kept it current.
  gTracked.focused = true;
  gTracked.fullscreen = request.fullscreen;

  sapp_desc desc = {};
  desc.init_cb = onInit;
  desc.frame_cb = onFrame;
  desc.event_cb = onEvent;
  desc.cleanup_cb = onCleanup;
  desc.width = request.width;
  desc.height = request.height;
  // Borrowed, not copied, by sokol_app -- which is why gRequest is a
  // file-static and this reads from it rather than from the local copy
  // above.
  desc.window_title = gRequest.title.c_str();
  desc.high_dpi = request.highDpi;
  desc.fullscreen = request.fullscreen;
  desc.logger.func = sokolLog;
  // 4.3 core, matching what GlBackend requires of the context it shares
  // with. Sokol already defaults to 4.3 on Linux; stated explicitly so the
  // two cannot drift apart silently, since a lower context would run until
  // the first texture view and then assert inside Sokol.
  desc.gl.major_version = 4;
  desc.gl.minor_version = 3;
  // Drops are delivered with their paths (`hnui_event::paths`), bounded by
  // the ABI's own limits so that what Sokol keeps and what the ABI
  // promises cannot disagree.
  desc.enable_dragndrop = true;
  desc.max_dropped_files = HNUI_DROP_MAX_FILES;
  desc.max_dropped_file_path_length = HNUI_DROP_MAX_PATH_BYTES;
  // Needed for `ui.writeClipboard`, and for answering other clients that
  // ask for our selection. It also makes Sokol emit CLIPBOARD_PASTED, on
  // X11 right after the KEY_DOWN of a Ctrl+V with no other modifier
  // (`_sapp_x11_key_event`, sokol_app.h:13562-13570); the event's text then
  // comes from readClipboardNow(), which blocks this thread inside event_cb
  // for up to kClipboardReadDefaultTimeoutMs when the owner is slow.
  desc.enable_clipboard = true;
  desc.clipboard_size = HNUI_CLIPBOARD_MAX_BYTES;

  uiTrace("sapp_run: %dx%d", desc.width, desc.height);
  sapp_run(&desc);

  // Reached on Linux when the window closes; never on macOS ([NSApp run]).
  // The JS thread is still running at this point, and bringing it down in
  // an orderly way -- CLOSING, the pre-cleanup lifecycle hook, the exit
  // code -- is Task 15. Until then UiHost joins it, and in practice the
  // program ends through process.exit() before this line is reached.
  uiTrace("sapp_run returned");
  return 0;
}

} // namespace node_compat
} // namespace hermes
