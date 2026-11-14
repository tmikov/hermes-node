/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/ui/hnui.h>

#include "hnui_window.h"
#include "ui_module.h"

#include <ui_trace.h>

#include <uv.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

/// The public C API, implemented against the host's private view of a
/// window (`hnui_window.h`).
///
/// **The minimal subset** -- `hnui_get_window`, `hnui_frame_target`,
/// `hnui_request_animation_frame` -- for the reason `hnui.h` states: these
/// three are what the reference addon needs to draw, and an entry point
/// with no caller is an entry point nobody has checked is usable. The
/// remaining thirteen arrive with Task 14, alongside the conformance mode
/// that calls each of them once.

namespace {

using hermes::node_compat::uiTrace;

/// A checked entry point called from the wrong thread **aborts, naming
/// itself** (spec: Errors -- "Misuse of a checked `hnui_*` call from the
/// wrong thread: abort naming the function").
///
/// Not a false return, and the distinction is the point. Returning false
/// would let a program carry on having called `sg_*` from a thread that
/// does not own that instance's context, which corrupts the *other*
/// instance's state rather than failing -- the failure mode the whole
/// two-instance design exists to prevent, and one that surfaces later, as
/// a rendering fault with no connection to its cause. Addons are trusted
/// code; the contract they are trusted to keep is stated, and this is what
/// enforces the half that can be enforced cheaply.
[[noreturn]] void abortWrongThread(const char *function) {
  std::fprintf(
      stderr,
      "hermes-node-ui: %s was called from a thread that does not own the "
      "window.\nEvery hnui_* entry point belongs to the thread that called "
      "createWindow -- the one that owns the producing sokol_gfx instance "
      "and issues every drawing call. Calling from another thread would "
      "reach that instance's state from off its context; see hnui.h, "
      "\"Threading\".\n",
      function);
  std::fflush(stderr);
  // Every _exit() path in this tree restores the terminal first; see
  // CLAUDE.md's Bootstrap Sequence section. abort() would skip that, and
  // this path is reachable from a program that had set raw mode.
  uv_tty_reset_mode();
  std::abort();
}

/// A null handle is an ORDINARY input, and "no" is the whole of the answer.
///
/// `hnui_get_window()` documents NULL as the ordinary answer for a runtime
/// that never called `createWindow` -- "an addon that can render headless
/// may carry on" -- so an addon that carries on is, by that same contract,
/// entitled to hand the NULL back in here. Folding null into
/// abortWrongThread() made the documented headless path kill the process,
/// with a diagnostic that blamed the caller's THREAD for a handle that was
/// simply not there. Nothing reached it only because demo_addon.c guards
/// every call site with `w != NULL`, which is why the reference addon now
/// has one call that does not (probeNullWindow).
///
/// Not a silent no-op either: the return value IS the report -- false, and
/// the id 0, both of which these entry points already use for "no" -- and
/// the trace names the function for anyone running with
/// HERMES_NODE_DEBUG_NATIVE=UI.
bool haveWindow(hnui_window *w, const char *function) {
  if (w != nullptr)
    return true;
  uiTrace("%s: called with a null window handle; answering no.", function);
  return false;
}

/// The wrong-thread check proper. \p w must be non-null: every caller runs
/// haveWindow() first, which is what keeps "there is no window" and "this is
/// the wrong thread for the window there is" two different answers.
void requireOwnerThread(hnui_window *w, const char *function) {
  if (w->ownerThread != std::this_thread::get_id())
    abortWrongThread(function);
}

} // namespace

extern "C" {

hnui_window *hnui_get_window(napi_env env) {
  // No thread check: this is the call that *finds* the window, so there is
  // no handle to read an owner thread from yet. It answers for the env it
  // is given, and an env belongs to its own runtime thread already.
  return hermes::node_compat::uiWindowForEnv(env);
}

bool hnui_frame_target(hnui_window *w, hnui_target *out) {
  if (!haveWindow(w, "hnui_frame_target"))
    return false;
  requireOwnerThread(w, "hnui_frame_target");
  if (out == nullptr)
    return false;

  // The size-prefix rule (hnui_abi.h): a `size` below the minimum is
  // REJECTED rather than read, because it cannot be distinguished from a
  // struct nobody filled in. Read before anything else is written, so a
  // rejected call leaves the caller's buffer untouched.
  const uint32_t providedSize = out->size;
  if (!hnui_size_ok(providedSize, HNUI_TARGET_MIN_SIZE))
    return false;

  // Outside a frame callback this is a legitimate question with a
  // legitimate answer (spec: "returns false outside a frame callback; only
  // a wrong-thread call aborts"). Same for a tick whose acquire found no
  // free slot -- the tick machine withholds ticks for exactly that reason,
  // so this is a race rather than a normal state, but it is still a "no".
  if (!w->inFrameCallback || !w->haveSlot)
    return false;

  hnui_target filled = {};
  filled.size = providedSize;
  if (!w->backend->frameTarget(w->slot, &filled))
    return false;
  // frameTarget() fills a struct of OUR size; it must not be trusted to
  // have preserved the caller's.
  filled.size = providedSize;

  // "The implementation writes at most `size` bytes and at least the
  // minimum." An addon compiled against an older, smaller version of this
  // header passes a smaller `size`, and the bytes past it belong to
  // whatever that addon put after the struct. Copying sizeof(hnui_target)
  // regardless would write over them -- which is the exact failure the
  // size prefix exists to prevent, so the clamp is the mechanism and not a
  // formality. (Task 14 owns the behavioural test that a poisoned tail
  // survives; this is the writer it needs.)
  const size_t writable =
      std::min(static_cast<size_t>(providedSize), sizeof(hnui_target));
  std::memcpy(out, &filled, writable);

  // Asking for the target is what marks the frame as drawn -- see hnui.h.
  // Set after the target was actually produced: a call that could not be
  // answered did not draw, and a tick that publishes a surface nothing
  // rendered into would present undefined contents.
  w->drew = true;
  return true;
}

int hnui_request_animation_frame(hnui_window *w, hnui_frame_cb cb, void *user) {
  if (!haveWindow(w, "hnui_request_animation_frame"))
    return 0;
  requireOwnerThread(w, "hnui_request_animation_frame");
  if (cb == nullptr)
    return 0;

  const int id = w->nextCallbackId++;
  w->nativeCallbacks.push_back(hnui_window::NativeFrameCallback{id, cb, user});
  // The request itself. The tick machine coalesces -- n requests before a
  // tick still mean one tick -- so registering several callbacks for one
  // frame costs one flag, not n.
  w->tick->requestFrame();
  uiTrace("hnui_request_animation_frame: id %d", id);
  return id;
}

} // extern "C"
