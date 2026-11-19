/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_TOOLS_HNUI_WINDOW_H
#define HERMES_NODE_COMPAT_TOOLS_HNUI_WINDOW_H

#include <backend.h>

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/event_dispatcher.h>
#include <hermes/node-compat/compositor/frame_callbacks.h>
#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>
#include <hermes/node-compat/ui/hnui.h>

#include <node_api.h>

#include <memory>
#include <thread>
#include <vector>

/// What an `hnui_window *` actually points at.
///
/// `hnui_abi.h` forward-declares `hnui_window` as an opaque tag and says
/// nothing else about it, which is the whole point: an addon holds the
/// pointer and never dereferences it. This header is the host's private
/// view of the same object, shared between the two files that need it --
/// `ui_module.cpp`, which owns the instance and drives the frame,
/// `ui_events.cpp`, which delivers input to listeners, and `hnui_api.cpp`,
/// which implements the C entry points against it.
///
/// **Defined at global scope, deliberately.** `hnui_abi.h`'s
/// `typedef struct hnui_window hnui_window;` declares `::hnui_window`, so
/// a definition inside `hermes::node_compat` would be a *different* type
/// that happens to share a name, and every conversion between the two
/// would be a reinterpret_cast nobody wrote down.
///
/// # Threading
///
/// Everything here belongs to the **owner thread** -- the JS thread that
/// called `createWindow` -- except the three compositor pointers, which are set
/// before the platform thread can reach them and never change afterwards.
/// `SurfaceTable` and `TickMachine` carry their own locks and are
/// deliberately *not* guarded by anything here; the per-tick fields below
/// are touched only by the owner thread and need no lock at all, which is
/// what `ownerThread` exists to let the entry points check rather than
/// assume.
struct hnui_window {
  /// The thread that owns this window: the one that called
  /// `createWindow`, runs every frame callback, and issues every `sg_*`
  /// call. A checked entry point called from any other thread aborts
  /// naming itself (spec: Errors).
  std::thread::id ownerThread;

  /// Owned by `ui_module.cpp` and outliving this object; never null once
  /// the window is ready.
  hermes::node_compat::SurfaceTable *table = nullptr;
  hermes::node_compat::TickMachine *tick = nullptr;
  hermes::node_compat::CompositorBackend *backend = nullptr;

  //=========================================================================
  // Per-tick state. Owner thread only, valid only inside a frame tick.
  //=========================================================================

  /// Whether a frame tick's callbacks are running right now.
  /// `hnui_frame_target` returns false outside one -- a legitimate
  /// question with a legitimate answer, not a misuse.
  bool inFrameCallback = false;

  /// The slot this tick acquired, valid while `inFrameCallback`.
  hermes::node_compat::SurfaceRef slot{};
  bool haveSlot = false;

  /// Whether anything asked for the frame's target. A tick that drew
  /// nothing returns to idle immediately instead of waiting for a
  /// publication that will never come (spec: Frames), and this is the flag
  /// that decides which of the two happened.
  bool drew = false;

  //=========================================================================
  // Frame callbacks: `hnui_request_animation_frame` and
  // `requestAnimationFrame`, in one list. Owner thread only.
  //=========================================================================

  /// Native and JavaScript callbacks in one registration-ordered list with
  /// one id space (Ruling 57). A JavaScript entry's `fn` is a strong
  /// `napi_ref`, released through `releaseFrameCallback` by every route
  /// out of the list: the tick, when it takes the entry to run it; either
  /// cancel entry point, when it removes one; and `uiEnterClosing`, which
  /// hands whatever is still here -- the rest of a running batch included
  /// -- to `FrameCallbackList::releaseAll` while the env is alive, before
  /// any `'exit'` listener runs. Nothing is added afterwards: both request
  /// entry points refuse once the window is CLOSING.
  hermes::node_compat::FrameCallbackList frameCallbacks;

  //=========================================================================
  // Events (`ui_events.cpp`). Owner thread only.
  //=========================================================================
  //
  // Everything event delivery needs is reachable from here, so that
  // `ui_events.cpp` reaches nothing through the statics of `ui_module.cpp`
  // and `ui_platform.cpp`.

  /// The runtime that owns this window.
  napi_env env = nullptr;

  /// The JavaScript window object -- an `EventEmitter` -- once
  /// `createWindow`'s Promise is about to resolve with it; null before.
  /// Events that arrive before it exists stay queued (see `ui_events.h`).
  /// A strong reference: the window lives as long as the process does.
  napi_ref jsWindow = nullptr;

  /// The JS thread's mirror of the window's state: the `window` snapshot of
  /// the last event delivered (Ruling 52), assigned before any listener for
  /// that event runs. What `hnui_get_window_state` reports, and what the
  /// JS object's eight state properties are kept equal to.
  hermes::node_compat::WindowState state;

  struct NativeEventListener {
    int id = 0;
    hnui_event_cb cb = nullptr;
    void *user = nullptr;
  };

  /// Whether a close request (`kCloseRequested`) is being delivered: true
  /// from before its first native listener runs until the JavaScript
  /// `emit` returns. What `hnui_cancel_close` and the `close` payload's
  /// `preventDefault()` check, so that a call at any other time cancels
  /// nothing.
  bool inCloseEvent = false;

  /// Whether a listener of the close request being delivered cancelled it.
  /// Reset when `inCloseEvent` is set, so one event's cancellation never
  /// carries over to the next.
  bool closeCancelled = false;

  /// `hnui_add_event_listener` registrations, in registration order.
  std::vector<NativeEventListener> eventListeners;
  int nextListenerId = 1;

  /// What `win.title` reads back: the last title set, or the one the window
  /// was created with. A JavaScript-side value, deliberately -- the
  /// platform thread owns the real title and the JS thread must not ask it.
  std::string title;

  /// Drains the process's `InputQueue` (`ui_module.cpp` owns the queue;
  /// the platform thread pushes into it).
  std::unique_ptr<hermes::node_compat::EventDispatcher> events;
};

/// Release what a frame callback that has left \p w's list holds: the
/// JavaScript function's reference, for a JavaScript entry. A native entry
/// holds nothing of ours -- its `user` pointer is the addon's.
inline void releaseFrameCallback(
    hnui_window *w,
    const hermes::node_compat::FrameCallback &cb) {
  if (cb.kind == hermes::node_compat::FrameCallback::Kind::kJs)
    napi_delete_reference(w->env, static_cast<napi_ref>(cb.fn));
}

#endif // HERMES_NODE_COMPAT_TOOLS_HNUI_WINDOW_H
