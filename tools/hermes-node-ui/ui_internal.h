/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "event_injector.h"

#include <backend.h>
#include <test_hooks.h>

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/owner_dispatch.h>
#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>

#include <node_api.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

/// Private to the window's three source files, and not a public header:
/// `ui_module.cpp` (the JS thread: the `hermes-node:ui` module surface),
/// `ui_platform.cpp` (the platform thread: the `sokol_app` callbacks and the
/// commands they apply) and `ui_test_hooks.cpp` (the hooks exported under
/// `HERMES_NODE_UI_TEST_HOOKS`). Everything declared here is, by being here,
/// touched from more than one of them; everything else stays file-local.
/// Each declaration says which thread(s) may touch it and what orders or
/// guards that.

namespace hermes {
namespace node_compat {

//===========================================================================
// State
//===========================================================================
//
// Two threads share these files. The split below is deliberate and is the
// thing to preserve when editing: a field is either guarded by `gMutex`,
// or an atomic, or an object carrying its own lock, or owned outright by
// one thread, and each declaration below says which. Nothing else may be
// touched by one thread at a time the other might touch it.
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

// --- Defined in ui_module.cpp ----------------------------------------------

/// Both threads lock it: guards gRequest here, and ui_module.cpp's own
/// gWindowRequested.
extern std::mutex gMutex;
/// Under gMutex: written once by createWindow() (JS) before requestWindow();
/// copied by uiRunPlatformLoop() (platform) and onSharedContextReady() (JS).
/// Read unlocked only after that write: createWindow()'s trace, and the
/// platform thread's borrowed `desc.window_title`.
extern WindowRequest gRequest;
/// Written once by uiRunPlatformLoop() (platform) before sapp_run(); both
/// threads use it afterwards, the JS thread first from a function posted
/// after that write (OwnerDispatch orders it).
extern CompositorBackend *gBackend;
/// Written by onInit() (platform) before it posts onSharedContextReady(),
/// which reads it on the JS thread (ordered by the post); maybeCapture()
/// also reads it on the platform thread.
extern WindowState gMetrics;
/// Written on the platform thread (onInit(), uiRunPlatformLoop()) before it
/// posts onStartupFailed(), which reads it on the JS thread (ordered by the
/// post).
extern std::string gStartupError;
/// Created by createWindow() (JS) before requestWindow(), which the platform
/// thread's wake is ordered after (UiHost's mutex); post() is then called
/// from both threads under OwnerDispatch's own lock. Closed by
/// uiEnterClosing() (JS) and never deleted or nulled after the request is
/// published, because the platform thread reads the pointer unsynchronized;
/// a closed dispatcher refuses every post().
extern OwnerDispatch *gDispatch;
/// JS thread only (ui_module.cpp, ui_test_hooks.cpp). Written by
/// createWindow().
extern napi_env gEnv;
/// Created by onSharedContextReady() (JS); the platform thread touches it
/// only after an acquire of gPresentReady. Carries its own lock.
extern SurfaceTable *gTable;
/// Created by onSharedContextReady() (JS); the platform thread touches it
/// only after an acquire of gPresentReady. Carries its own lock.
extern TickMachine *gTick;
/// Atomic: stored true (release) by onSharedContextReady() on the JS thread,
/// loaded (acquire) on the platform thread before it reads gTable/gTick.
extern std::atomic<bool> gPresentReady;
/// Any thread, own lock: the platform thread pushes, the JS thread's
/// EventDispatcher drains.
extern InputQueue gInput;
/// Any thread, own lock: the JS thread pushes (uiPushCommand), the platform
/// thread drains (drainCommands).
extern CommandQueue gCommands;
/// Atomic, both threads: set by postEventDispatch() and drainInjectedEvents()
/// (platform), cleared by runEventDispatch() (JS) and drainInjectedEvents().
extern std::atomic<bool> gDispatchPosted;
/// Any thread, own lock: the JS thread creates slots and blocks on them, and
/// fails them all in uiEnterClosing(); the platform thread answers them in
/// applyReadClipboard().
extern ClipboardRequests gClipboardReads;

// --- Defined in ui_test_hooks.cpp ------------------------------------------

/// Any thread, own lock: held and released by the hooks on the JS thread;
/// completions pass through it on whichever thread runs them.
extern CompletionGate gGate;
/// Any thread, own lock: captureFrame() (JS) requests and cancels,
/// uiTestHooksAbandonCapture() (JS) cancels, maybeCapture() (platform)
/// matches and cancels.
extern FrameCapture gCapture;
/// Any thread, own lock: injectEvent() (JS) pushes, drainInjectedEvents()
/// (platform) takes.
extern PlatformEventInjector gInjector;
/// Any thread; guards gCaptureState.
extern std::mutex gCaptureMutex;
/// Under gCaptureMutex: reset by captureFrame() and copied by
/// onCaptureFinished() on the JS thread, updated by maybeCapture() on the
/// platform thread.
extern CaptureState gCaptureState;

// --- Functions defined in ui_module.cpp ------------------------------------

/// Any thread (both completions, onFrame(), uiFollowFramebufferSize());
/// touches no shared state, and does not return.
[[noreturn]] void terminateGraphicsSession(const std::string &reason);
/// JS thread; posted by onFrame() on the platform thread.
void runFrameTick();
/// Any thread by its state (gDispatchPosted is atomic, post() locks); its
/// callers today are all on the platform thread.
void postEventDispatch();
/// JS thread; posted by onInit() on the platform thread.
void onSharedContextReady();
/// JS thread; posted by onInit() or uiRunPlatformLoop() on the platform
/// thread.
void onStartupFailed();
/// JS thread (NAPI): option readers for createWindow() and the test hooks.
void readInt(napi_env env, napi_value options, const char *name, int *out);
/// JS thread (NAPI). See readInt().
void readBool(napi_env env, napi_value options, const char *name, bool *out);
/// JS thread (NAPI). See readInt().
void readDouble(
    napi_env env,
    napi_value options,
    const char *name,
    double *out);
/// JS thread (NAPI). See readInt().
void readString(
    napi_env env,
    napi_value options,
    const char *name,
    std::string *out);

// --- Functions defined in ui_test_hooks.cpp --------------------------------

/// JS thread; posted by maybeCapture() on the platform thread.
void onCaptureFinished();
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
napi_value captureFrame(napi_env env, napi_callback_info info);
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
napi_value holdProducerCompletions(napi_env env, napi_callback_info info);
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
napi_value releaseProducerCompletions(napi_env env, napi_callback_info info);
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
napi_value failPendingClipboardReads(napi_env env, napi_callback_info info);
/// JS thread, called by uiEnterClosing(). Cancels a pending capture request
/// (gCapture, so the platform thread stops matching) and forgets its
/// Promise, which then never settles. Traces `capture: abandoned (closing)`
/// only when there was a Promise to abandon. Does nothing with the hooks
/// off.
void uiTestHooksAbandonCapture();
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
napi_value injectEvent(napi_env env, napi_callback_info info);
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
/// Blocks the JS thread on gFrameCountMutex, which only the two functions
/// below also take.
napi_value waitPlatformFrames(napi_env env, napi_callback_info info);
/// Platform thread, first line of onFrame(), only when the hooks are on.
/// Takes gFrameCountMutex (file-local to ui_test_hooks.cpp).
void uiTestHookFrameBegan();
/// Platform thread, on every return of onFrame(), only when the hooks are
/// on. Takes gFrameCountMutex and wakes waitPlatformFrames().
void uiTestHookFrameEnded();
/// JS thread (NAPI); exported by initUiModule() only when the hooks are on.
/// Blocks the JS thread on gQuitCountMutex, which only the function below
/// also takes.
napi_value waitPlatformQuitRequests(napi_env env, napi_callback_info info);
/// Platform thread, in uiHandlePlatformEvent() right after its
/// sapp_cancel_quit() call, only when the hooks are on. Takes
/// gQuitCountMutex (file-local to ui_test_hooks.cpp) and wakes
/// waitPlatformQuitRequests().
void uiTestHookQuitCancelled();

} // namespace node_compat
} // namespace hermes
