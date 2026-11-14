/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_TOOLS_HNUI_WINDOW_H
#define HERMES_NODE_COMPAT_TOOLS_HNUI_WINDOW_H

#include <backend.h>

#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>
#include <hermes/node-compat/ui/hnui.h>

#include <thread>
#include <vector>

/// What an `hnui_window *` actually points at.
///
/// `hnui_abi.h` forward-declares `hnui_window` as an opaque tag and says
/// nothing else about it, which is the whole point: an addon holds the
/// pointer and never dereferences it. This header is the host's private
/// view of the same object, shared between the two files that need it --
/// `ui_module.cpp`, which owns the instance and drives the frame, and
/// `hnui_api.cpp`, which implements the C entry points against it.
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
/// called `createWindow` -- except the three pointers, which are set
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
  // Native frame callbacks (`hnui_request_animation_frame`)
  //=========================================================================

  struct NativeFrameCallback {
    int id = 0;
    hnui_frame_cb cb = nullptr;
    void *user = nullptr;
  };

  /// Registered for the **next** tick. The frame loop swaps this list out
  /// before running it, so a callback that registers another one lands in
  /// a fresh list and runs a tick later -- the browser rule, and the one
  /// spec:757 states for native callbacks too.
  std::vector<NativeFrameCallback> nativeCallbacks;
  int nextCallbackId = 1;
};

#endif // HERMES_NODE_COMPAT_TOOLS_HNUI_WINDOW_H
