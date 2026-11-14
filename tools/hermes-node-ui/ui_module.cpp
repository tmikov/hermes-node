/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_module.h"

#include "hnui_window.h"
#include "ui_host.h"

#include <backend.h>
#include <test_hooks.h>
#include <ui_trace.h>

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

/// Filled by the platform thread inside `init_cb`, read by the JS thread
/// from the posted continuation. See the ordering note above.
struct WindowMetrics {
  int width = 0;
  int height = 0;
  int framebufferWidth = 0;
  int framebufferHeight = 0;
  double dpiScale = 1.0;
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

/// Platform thread only, between init_cb and the post.
WindowMetrics gMetrics;
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

/// The one generation this build ever allocates, and its size.
///
/// A resize would start a second (spec: Generations) and turn these into a
/// per-generation lookup; nothing here resizes, because the event that
/// would trigger it is Task 11's and the resize assertions are Task 17's.
/// Written by the JS thread before `gPresentReady`, read by the platform
/// thread after it.
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

/// JS-side requestAnimationFrame callbacks, owner thread only.
struct JsFrameCallback {
  int id = 0;
  napi_ref fn = nullptr;
};
std::vector<JsFrameCallback> gJsCallbacks;
int gNextJsCallbackId = 1;

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

/// Monotonic milliseconds, shared by every callback in one tick.
///
/// **Not yet `performance.now()`'s origin**, which spec:583 requires ("All
/// callbacks in a tick receive the same timestamp: monotonic milliseconds
/// sharing performance.now()'s origin"). `uv_hrtime()` is monotonic and
/// shared across the tick, which is the half that matters for an
/// animation; agreeing with the JS clock's origin is Task 13's, with the
/// rest of the rAF semantics.
double nowMs() {
  return static_cast<double>(uv_hrtime()) / 1e6;
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
    if (!gTick->publishFrame(*gTable, ref))
      gTable->reclaim(ref);
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

/// Run one tick's JavaScript callbacks. The list was swapped out by the
/// caller, so a callback that registers another one lands in the next
/// tick's list rather than extending this one.
void runJsFrameCallbacks(
    std::vector<JsFrameCallback> &callbacks,
    double timestamp) {
  if (callbacks.empty() || gEnv == nullptr)
    return;

  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(gEnv, &scope) != napi_ok)
    return;

  napi_value undefined = nullptr;
  napi_value argument = nullptr;
  napi_get_undefined(gEnv, &undefined);
  napi_create_double(gEnv, timestamp, &argument);

  for (JsFrameCallback &entry : callbacks) {
    napi_value fn = nullptr;
    if (napi_get_reference_value(gEnv, entry.fn, &fn) == napi_ok &&
        fn != nullptr) {
      napi_value result = nullptr;
      if (napi_call_function(gEnv, undefined, fn, 1, &argument, &result) !=
          napi_ok) {
        // **Reported, not swallowed -- but not yet routed to
        // `process._fatalException` either.** That path is what decides
        // whether a listener took the error or the process exits 1
        // (CLAUDE.md, "Uncaught Exceptions From Async Callbacks"), and
        // wiring a frame callback into it is Task 13's, which owns the
        // handled-versus-fatal split for rAF. Printing is the honest
        // interim: this codebase's standing complaint is against
        // swallow-and-continue, and a visible message is not that.
        napi_value error = nullptr;
        if (napi_get_and_clear_last_exception(gEnv, &error) == napi_ok) {
          napi_value message = nullptr;
          char buffer[512] = {0};
          size_t length = 0;
          if (napi_coerce_to_string(gEnv, error, &message) == napi_ok) {
            napi_get_value_string_utf8(
                gEnv, message, buffer, sizeof(buffer), &length);
          }
          std::fprintf(
              stderr,
              "hermes-node-ui: an exception escaped a requestAnimationFrame "
              "callback: %s\n",
              buffer);
          std::fflush(stderr);
        }
      }
    }
    napi_delete_reference(gEnv, entry.fn);
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

  // Both lists are swapped out BEFORE anything runs: a callback
  // registered from inside a callback belongs to the next tick.
  std::vector<hnui_window::NativeFrameCallback> native;
  native.swap(gWindow->nativeCallbacks);
  std::vector<JsFrameCallback> js;
  js.swap(gJsCallbacks);

  const double timestamp = nowMs();
  // **Native callbacks before JS ones, which is not yet what spec:757
  // requires**: "Native frame callbacks share the tick and timestamp with
  // JS ones and run in registration order among them." They share the
  // tick and the timestamp here; the single registration-ordered batch
  // mixing both is Task 13's, which owns rAF ordering along with
  // microtask checkpoints and cancellation. Recorded rather than left to
  // be discovered, because two lists that each preserve their own order
  // look correct until an addon and a script both register in one frame.
  for (hnui_window::NativeFrameCallback &entry : native)
    entry.cb(gWindow, entry.user, timestamp);
  runJsFrameCallbacks(js, timestamp);

  gWindow->inFrameCallback = false;
  gWindow->haveSlot = false;
  // Nothing can have drawn without a slot -- hnui_frame_target refuses
  // without one -- but the conjunction is what keeps that a fact rather
  // than an assumption: a tick reported as having drawn with no slot to
  // publish would leave the machine in kGpuPending for good.
  const bool drew = gWindow->drew && slot.has_value();
  gTick->endCallbacksDrew(drew);

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
  observed.targetWidth = gGenerationWidth;
  observed.targetHeight = gGenerationHeight;
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

  napi_value window = nullptr;
  if (napi_create_object(gEnv, &window) != napi_ok) {
    napi_close_handle_scope(gEnv, scope);
    rejectWith("could not create the window object");
    return;
  }

  setNumber(window, "width", gMetrics.width);
  setNumber(window, "height", gMetrics.height);
  setNumber(window, "framebufferWidth", gMetrics.framebufferWidth);
  setNumber(window, "framebufferHeight", gMetrics.framebufferHeight);
  setNumber(window, "dpiScale", gMetrics.dpiScale);
  // Static for now: the event path that keeps these current is Task 11's,
  // and reporting a value that never changes is better than omitting the
  // property, which would make `win.minimized` read `undefined` rather than
  // false.
  setBool(window, "focused", true);
  setBool(window, "fullscreen", gRequest.fullscreen);
  setBool(window, "minimized", false);

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
  gTable = new SurfaceTable([](GenerationId gen) {
    const bool posted = gDispatch != nullptr &&
        gDispatch->post([gen]() { gBackend->destroyGenerationSurfaces(gen); });
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

  // The other half of completion: the producer's arrives through the
  // callback submitAndTrackCompletion() takes, the consumer's through
  // this sink, because blit() has nowhere to carry one.
  gBackend->setConsumerCompletion(onConsumerComplete, nullptr);

  // Surfaces are sized to the FRAMEBUFFER in pixels, not to the window in
  // logical units (spec: Surfaces, "sized to the framebuffer in pixels").
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
}

void onStartupFailed() {
  uiTrace("window failed: %s", gStartupError.c_str());
  rejectWith(gStartupError);
}

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

  // On Linux sokol_app maps window size to framebuffer size 1:1
  // (sokol_app.h, _sapp_linux_run), so sapp_width() is both. Divided by the
  // DPI scale anyway rather than assumed equal: the logical/physical split
  // is real on every other platform this interface will meet, and a
  // framebuffer size that silently doubles as a logical one is the kind of
  // thing that reads correctly right up until it does not.
  const float dpiScale = sapp_dpi_scale();
  gMetrics.framebufferWidth = sapp_width();
  gMetrics.framebufferHeight = sapp_height();
  gMetrics.dpiScale = dpiScale;
  gMetrics.width = dpiScale > 0.0f ? static_cast<int>(sapp_widthf() / dpiScale)
                                   : sapp_width();
  gMetrics.height = dpiScale > 0.0f
      ? static_cast<int>(sapp_heightf() / dpiScale)
      : sapp_height();

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
  // Input is Task 11. Until then events are dropped rather than queued:
  // queueing them would fill the InputQueue with events nothing drains,
  // which is a leak dressed as progress.
  (void)event;
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
/// a global once a window exists.
///
/// Returns an id `cancelAnimationFrame` will take, which is Task 13's
/// along with the rest of the rAF semantics (microtask checkpoints, the
/// single registration-ordered batch, cancellation from inside a
/// microtask). What exists here is the half this task's gate needs: a
/// callback, run once, on the next tick.
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
  if (gTick == nullptr) {
    napi_throw_error(
        env,
        nullptr,
        "requestAnimationFrame: no window exists yet. Await createWindow() "
        "before requesting a frame -- frames are paced by the window's "
        "presentation, so there is nothing to pace against until then.");
    return nullptr;
  }

  // A strong reference: the callback must survive until the tick runs it,
  // and nothing else on the JS side is holding it by then. Released in
  // runJsFrameCallbacks(), whether or not the call itself succeeded.
  napi_ref ref = nullptr;
  if (napi_create_reference(env, argv[0], 1, &ref) != napi_ok) {
    napi_throw_error(
        env, nullptr, "requestAnimationFrame: the callback could not be held.");
    return nullptr;
  }

  const int id = gNextJsCallbackId++;
  gJsCallbacks.push_back(JsFrameCallback{id, ref});
  // Requests coalesce in the machine, so n callbacks in one frame still
  // mean one tick.
  gTick->requestFrame();

  napi_value result = nullptr;
  napi_create_int32(env, id, &result);
  return result;
}

void installFrameGlobals() {
  napi_value global = nullptr;
  if (napi_get_global(gEnv, &global) != napi_ok)
    return;
  napi_value fn = nullptr;
  if (napi_create_function(
          gEnv,
          "requestAnimationFrame",
          NAPI_AUTO_LENGTH,
          requestAnimationFrame,
          nullptr,
          &fn) != napi_ok) {
    return;
  }
  napi_set_named_property(gEnv, global, "requestAnimationFrame", fn);
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
          env, exports, "requestAnimationFrame", requestAnimationFrame)) {
    return nullptr;
  }
  // The capture hook is exported ONLY when the hooks are enabled, so that
  // a production run cannot be made capturable by a script that merely
  // knows the name. `uiTestHooksEnabled()` is read once per process, so
  // this is decided at module load and cannot change under the program.
  if (uiTestHooksEnabled()) {
    if (!exportFunction(env, exports, "__captureFrame", captureFrame))
      return nullptr;
  }
  return exports;
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
