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
#include <cstdlib>
#include <cstring>
#include <string>

/// The public C API, implemented against the host's private view of a
/// window (`hnui_window.h`).
///
/// Every entry point arrived with the mechanism it exposes, because an
/// entry point with no caller is an entry point nobody has checked is
/// usable (Ruling 58); Task 14a added the last of them.

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

/// A refused argument to one of the `void` command entry points. Loud, one
/// line, naming the function and the value: those entry points return
/// nothing by the spec's signature, so stderr is the only channel there is,
/// and a silent drop would leave a caller wondering why nothing happened.
void refuse(const char *function, const std::string &what) {
  std::fprintf(
      stderr, "hermes-node-ui: %s: refusing %s.\n", function, what.c_str());
  uiTrace("%s: refused %s", function, what.c_str());
}

} // namespace

extern "C" {

int hnui_api_version(void) {
  return HNUI_ABI_VERSION;
}

const char *hnui_sokol_commit(void) {
  return HNUI_SOKOL_COMMIT;
}

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
  if (!hnui_size_ok(providedSize, HNUI_TARGET_MIN_SIZE)) {
    uiTrace(
        "hnui_frame_target: size %u is below the minimum %u; refused.",
        static_cast<unsigned>(providedSize),
        static_cast<unsigned>(HNUI_TARGET_MIN_SIZE));
    return false;
  }

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
  // formality. test/ui-abi-sizes.js (SIZES) is the behavioural test that a
  // poisoned tail survives.
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
  // Once the window is CLOSING no tick will ever run what this registers
  // (uiEnterClosing() released the list), so nothing is registered.
  if (hermes::node_compat::uiClosing()) {
    uiTrace("hnui_request_animation_frame: refused (closing)");
    return 0;
  }

  // The function pointer travels as the list's opaque `fn`; the list
  // knows nothing about hnui types (frame_callbacks.h).
  const int id = w->frameCallbacks.add(
      hermes::node_compat::FrameCallback::Kind::kNative,
      reinterpret_cast<void *>(cb),
      user);
  // The request itself. The tick machine coalesces -- n requests before a
  // tick still mean one tick -- so registering several callbacks for one
  // frame costs one flag, not n.
  w->tick->requestFrame();
  uiTrace("hnui_request_animation_frame: id %d", id);
  return id;
}

void hnui_cancel_animation_frame(hnui_window *w, int id) {
  if (!haveWindow(w, "hnui_cancel_animation_frame"))
    return;
  requireOwnerThread(w, "hnui_cancel_animation_frame");

  // An id the list does not hold -- never issued, already run, already
  // cancelled, or the callback running right now -- is a silent no-op by
  // contract (hnui.h). One id space, so this may remove a JavaScript
  // callback, whose reference is released here like any other route out.
  hermes::node_compat::FrameCallback removed;
  if (w->frameCallbacks.cancel(id, &removed))
    releaseFrameCallback(w, removed);
}

int hnui_add_event_listener(hnui_window *w, hnui_event_cb cb, void *user) {
  if (!haveWindow(w, "hnui_add_event_listener"))
    return 0;
  requireOwnerThread(w, "hnui_add_event_listener");
  if (cb == nullptr)
    return 0;

  const int id = w->nextListenerId++;
  w->eventListeners.push_back(hnui_window::NativeEventListener{id, cb, user});
  uiTrace("hnui_add_event_listener: id %d", id);
  return id;
}

void hnui_remove_event_listener(hnui_window *w, int id) {
  if (!haveWindow(w, "hnui_remove_event_listener"))
    return;
  requireOwnerThread(w, "hnui_remove_event_listener");

  // Erased from the live list at once, and that IS the deferral: deliver()
  // walks a COPY of the list taken at the start of each event, so a
  // listener removed during event N is still in event N's copy and runs for
  // it, while event N+1 -- including the next one of the same batch --
  // copies a list that no longer has it. What the caller must wait for
  // before freeing `user` is hnui_event_dispatch_quiescent(), since that
  // copy can still hold the entry. An unknown id matches nothing.
  auto &listeners = w->eventListeners;
  for (auto it = listeners.begin(); it != listeners.end(); ++it) {
    if (it->id == id) {
      listeners.erase(it);
      uiTrace("hnui_remove_event_listener: id %d removed", id);
      return;
    }
  }
  uiTrace("hnui_remove_event_listener: id %d is not registered", id);
}

bool hnui_event_dispatch_quiescent(hnui_window *w) {
  // No window means no listeners, so nothing can be on the stack.
  if (w == nullptr)
    return true;
  requireOwnerThread(w, "hnui_event_dispatch_quiescent");
  return w->events == nullptr || !w->events->dispatching();
}

void hnui_set_cursor(hnui_window *w, hnui_cursor c) {
  if (!haveWindow(w, "hnui_set_cursor"))
    return;
  requireOwnerThread(w, "hnui_set_cursor");
  // The range is checked HERE, before the value goes anywhere: an enum
  // argument can hold any int from a C caller, and the platform thread
  // maps it through a table.
  const int value = static_cast<int>(c);
  if (value < static_cast<int>(HNUI_CURSOR_DEFAULT) ||
      value > static_cast<int>(HNUI_CURSOR_NOT_ALLOWED)) {
    refuse("hnui_set_cursor", "cursor value " + std::to_string(value));
    return;
  }
  hermes::node_compat::uiPushCommand(
      hermes::node_compat::Command::setCursor(value));
}

void hnui_show_cursor(hnui_window *w, bool show) {
  if (!haveWindow(w, "hnui_show_cursor"))
    return;
  requireOwnerThread(w, "hnui_show_cursor");
  hermes::node_compat::uiPushCommand(
      hermes::node_compat::Command::showCursor(show));
}

void hnui_lock_pointer(hnui_window *w, bool lock) {
  if (!haveWindow(w, "hnui_lock_pointer"))
    return;
  requireOwnerThread(w, "hnui_lock_pointer");
  hermes::node_compat::uiPushCommand(
      hermes::node_compat::Command::lockPointer(lock));
}

void hnui_write_clipboard(hnui_window *w, const char *utf8) {
  if (!haveWindow(w, "hnui_write_clipboard"))
    return;
  requireOwnerThread(w, "hnui_write_clipboard");
  if (utf8 == nullptr) {
    refuse("hnui_write_clipboard", "a NULL string");
    return;
  }
  // The bound ui.writeClipboard enforces: Sokol's buffer holds
  // HNUI_CLIPBOARD_MAX_BYTES including the terminator, and a longer string
  // makes it take the selection with an empty buffer.
  const size_t length = std::strlen(utf8);
  if (length >= HNUI_CLIPBOARD_MAX_BYTES) {
    refuse(
        "hnui_write_clipboard",
        std::to_string(length) + " bytes (the clipboard holds at most " +
            std::to_string(HNUI_CLIPBOARD_MAX_BYTES - 1) + ")");
    return;
  }
  hermes::node_compat::uiPushCommand(
      hermes::node_compat::Command::writeClipboard(std::string(utf8, length)));
}

void hnui_get_window_state(hnui_window *w, hnui_window_state *out) {
  if (!haveWindow(w, "hnui_get_window_state"))
    return;
  requireOwnerThread(w, "hnui_get_window_state");
  if (out == nullptr)
    return;

  // The size-prefix rule, exactly as hnui_frame_target applies it: read
  // `size` first, refuse a value below the minimum without writing a byte,
  // and never write more than the caller said it has.
  const uint32_t providedSize = out->size;
  if (!hnui_size_ok(providedSize, HNUI_WINDOW_STATE_MIN_SIZE)) {
    uiTrace(
        "hnui_get_window_state: size %u is below the minimum %u; refused.",
        static_cast<unsigned>(providedSize),
        static_cast<unsigned>(HNUI_WINDOW_STATE_MIN_SIZE));
    return;
  }

  const hermes::node_compat::WindowState &state = w->state;
  hnui_window_state filled = {};
  filled.size = providedSize;
  filled.width = state.width;
  filled.height = state.height;
  filled.framebuffer_width = state.framebufferWidth;
  filled.framebuffer_height = state.framebufferHeight;
  filled.dpi_scale = state.dpiScale;
  filled.focused = state.focused;
  filled.fullscreen = state.fullscreen;
  filled.minimized = state.minimized;

  const size_t writable =
      std::min(static_cast<size_t>(providedSize), sizeof(hnui_window_state));
  std::memcpy(out, &filled, writable);
}

int hnui_read_clipboard_sync(hnui_window *w, int timeout_ms, char **out) {
  using hermes::node_compat::ClipboardResult;
  using hermes::node_compat::ClipboardStatus;
  using hermes::node_compat::Command;

  // NULL before anything else: "every other status leaves *out NULL",
  // including the refusals.
  if (out != nullptr)
    *out = nullptr;
  if (!haveWindow(w, "hnui_read_clipboard_sync"))
    return HNUI_ERROR;
  requireOwnerThread(w, "hnui_read_clipboard_sync");
  if (out == nullptr)
    return HNUI_INVALID_ARGUMENT;

  const int timeoutMs = timeout_ms > 0 ? timeout_ms : 0;
  auto slot = hermes::node_compat::uiClipboardReads().create();
  const uint64_t deadline = hermes::node_compat::clipboardClockNs() +
      static_cast<uint64_t>(timeoutMs) * 1000000u;
  // Refused once the window is CLOSING (an 'exit' listener's read, say):
  // nothing will answer, so this fails at once rather than waiting out the
  // timeout. The slot was created only to name the command; it is released
  // on return, and the registry reaps its expired entry.
  if (!hermes::node_compat::uiPushCommand(Command::readClipboard(
          slot->id(), /*replyThroughSlot=*/true, deadline))) {
    return HNUI_ERROR;
  }

  // Blocks this (the JS) thread. The platform thread answers into the slot
  // directly, never through anything this thread would have to drain, and
  // never waits on this thread -- so the wait ends by an answer or by the
  // deadline, whatever the JS thread had queued.
  ClipboardResult result = slot->wait(timeoutMs);
  // The slot is released on return; a reply still in flight finds it gone
  // and is discarded (ClipboardRequests::answer).

  switch (result.status) {
    case ClipboardStatus::kOk: {
      char *copy = static_cast<char *>(std::malloc(result.text.size() + 1));
      if (copy == nullptr)
        return HNUI_ERROR;
      std::memcpy(copy, result.text.data(), result.text.size());
      copy[result.text.size()] = '\0';
      *out = copy;
      return HNUI_OK;
    }
    case ClipboardStatus::kUnavailable:
      return HNUI_UNAVAILABLE;
    case ClipboardStatus::kTimeout:
      return HNUI_TIMEOUT;
    case ClipboardStatus::kTooLarge:
      return HNUI_TOO_LARGE;
    case ClipboardStatus::kError:
      return HNUI_ERROR;
  }
  return HNUI_ERROR;
}

void hnui_free(void *p) {
  // free(NULL) is a no-op, which is the NULL contract hnui.h states.
  std::free(p);
}

} // extern "C"
