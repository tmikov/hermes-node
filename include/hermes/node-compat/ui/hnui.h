/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_HNUI_H
#define HERMES_NODE_COMPAT_UI_HNUI_H

/// The public C API for hermes-node's UI compositor: the entry points an
/// addon calls. The POD types and enums they operate on live in
/// `hnui_abi.h`, which this header includes and which an addon may also
/// include on its own.
///
/// The whole API, in the order a renderer meets it:
///
///   - Identity and compatibility: `hnui_api_version`, `hnui_sokol_commit`
///     and the `HNUI_CHECK_VERSION(env)` macro an addon calls at init.
///   - The window and the frame: `hnui_get_window`, `hnui_frame_target`,
///     `hnui_request_animation_frame`, `hnui_cancel_animation_frame`.
///   - Input and window state: `hnui_add_event_listener`,
///     `hnui_remove_event_listener`, `hnui_event_dispatch_quiescent`,
///     `hnui_cancel_close`, `hnui_get_window_state`.
///   - Platform services: `hnui_set_cursor`, `hnui_show_cursor`,
///     `hnui_lock_pointer`, `hnui_write_clipboard`,
///     `hnui_read_clipboard_sync`, and `hnui_free` for what the last one
///     allocates.
///
/// Each entry point arrived with the mechanism it exposes (Ruling 58), so
/// none of them is a stub: there is nothing declared here that links and
/// does nothing.
///
/// # Version checking
///
/// An addon is compiled against this header and run against a host built
/// from another copy of it. Two things can disagree without any symbol
/// failing to resolve: the ABI (`HNUI_ABI_VERSION`, bumped when a change is
/// one a compiled addon cannot tolerate) and the vendored Sokol, whose
/// structs the `sg_*` calls pass by value and whose layout is not ours to
/// version. `HNUI_CHECK_VERSION(env)` compares both with the host's
/// (`hnui_api_version()`, `hnui_sokol_commit()`) and throws a JavaScript
/// error naming all four values when either differs.
///
/// **Where the commit is written.** `HNUI_SOKOL_COMMIT_PINNED` below is the
/// one place to edit on a Sokol bump. `external/sokol/CMakeLists.txt` reads
/// it from this file, and `test/sokol-pin.js` checks it against the commit
/// `external/sokol/README.md` records. `HNUI_SOKOL_COMMIT_OVERRIDE`, when
/// defined before this header is included, replaces it: that exists so a
/// test can build a deliberately mismatched addon, and nothing else should
/// define it.
///
/// # Threading
///
/// Every entry point here belongs to the **window's owner thread** -- the
/// JS thread that called `createWindow`, which is also the thread that
/// owns the producing `sg_*` instance and issues every drawing call. The
/// platform thread has no business calling any of them. A window handle
/// carries its owner thread so that a wrong-thread call can be refused by
/// name rather than corrupting the other instance's state (spec: Errors,
/// "Misuse of a checked `hnui_*` call from the wrong thread: abort naming
/// the function").
///
/// # Closing
///
/// Every path that emits `'exit'` in a windowed process --
/// `process.exit()`, an uncaught exception, the runtime reaching its own
/// end, and through `process.exit()` a `close` nobody cancelled and
/// `win.close()` -- first puts the window into CLOSING, before any
/// JavaScript `'exit'` listener runs. The window stays on screen, showing
/// its last presented frame, until the process ends. From that moment:
///
///   - `hnui_request_animation_frame` registers nothing and returns 0.
///   - Frame callbacks already registered are dropped without being called,
///     those still to run in a tick under way included. Their `user`
///     pointers are never handed back: nothing is called with them.
///   - `hnui_frame_target` returns false, even inside the frame callback
///     that was running when CLOSING began.
///   - No further event is delivered: native event listeners are no longer
///     called, and neither are JavaScript ones. (The one exception is the
///     event being delivered when CLOSING began, when that happens inside
///     its own delivery and does not end the process at once: for example
///     when `process.exit` has been replaced by something that returns or
///     is not a function, or when a listener catches what `win.close()`
///     threw. That event's remaining listeners still run, so a listener
///     must not assume it is called only while the window is open.)
///   - `hnui_cancel_close` returns false.
///   - `hnui_set_cursor`, `hnui_show_cursor`, `hnui_lock_pointer` and
///     `hnui_write_clipboard` drop the request without telling the caller
///     (traced under `HERMES_NODE_DEBUG_NATIVE=UI`); an argument they would
///     refuse anyway is still refused on stderr first.
///   - `hnui_read_clipboard_sync` returns `HNUI_ERROR` at once, without
///     waiting for \p timeout_ms.
///
/// `hnui_get_window` keeps returning the window, and
/// `hnui_get_window_state` keeps answering with the last state delivered.
///
/// **There is no teardown notification.** Every one of those paths ends in
/// `_exit()` on the JS thread, and a fatal graphics error ends the process
/// with `_exit()` at once, from whichever thread met it: nothing in the
/// window or in the producing `sg_*` instance is torn down, and the N-API
/// environment is never destroyed, so cleanup hooks registered with
/// `napi_add_env_cleanup_hook`, and the finalizers an environment's
/// destruction would run, do not run either. An addon
/// must not rely on being told the process is ending; whatever it holds
/// ends with the process. Code that must run on the way out belongs in a
/// JavaScript `'exit'` listener, from which the API behaves as listed
/// above.
///
/// # Linking
///
/// The host exports these symbols from its own executable
/// (`hermes_node_link_setup()` in `tools/hermes-node/CMakeLists.txt` gives
/// hermes-node-ui the same `-rdynamic` treatment hermes-node has), so a
/// `dlopen`ed addon resolves them at load time. It exports the producing
/// instance's `sg_*` symbols the same way, which is what lets an addon
/// link against `sokol_gfx.h`'s declarations without carrying an
/// implementation of its own.

#include <hermes/node-compat/ui/hnui_abi.h>

#include <node_api.h>

#include <stdio.h>
#include <string.h>

/// The Sokol commit vendored at `external/sokol/sokol`. Edit here, and in
/// `external/sokol/README.md`, when Sokol is bumped; `test/sokol-pin.js`
/// fails if the two disagree. A plain string literal on a line of its own,
/// because `external/sokol/CMakeLists.txt` extracts it with a regex.
#define HNUI_SOKOL_COMMIT_PINNED "c0db757"

#ifdef HNUI_SOKOL_COMMIT_OVERRIDE
#define HNUI_SOKOL_COMMIT HNUI_SOKOL_COMMIT_OVERRIDE
#else
#define HNUI_SOKOL_COMMIT HNUI_SOKOL_COMMIT_PINNED
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// The `HNUI_ABI_VERSION` the running host was built with.
int hnui_api_version(void);

/// The Sokol commit the running host vendors, as a NUL-terminated string
/// owned by the host and valid for the life of the process.
const char *hnui_sokol_commit(void);

/// Check, at addon init, that this addon and the host agree on the ABI
/// version and the Sokol commit; on a mismatch, throw a JavaScript error
/// naming all four values and **return NULL from the calling function**.
///
/// Use it inside the addon's init function, before anything else, where
/// `return NULL` is the right way out -- `NAPI_MODULE_INIT()` returns a
/// `napi_value`, and NULL with a pending exception makes the load fail with
/// that error:
///
///     NAPI_MODULE_INIT() {
///       HNUI_CHECK_VERSION(env);
///       ...
///       return exports;
///     }
///
/// A macro rather than a function so that the comparison uses the values
/// compiled into the ADDON: a function inside the host would compare the
/// host with itself.
#define HNUI_CHECK_VERSION(env)                                             \
  do {                                                                      \
    if (hnui_api_version() != HNUI_ABI_VERSION ||                           \
        strcmp(hnui_sokol_commit(), HNUI_SOKOL_COMMIT) != 0) {              \
      char hnui_version_message_[256];                                      \
      snprintf(                                                             \
          hnui_version_message_,                                            \
          sizeof(hnui_version_message_),                                    \
          "hnui version mismatch: this addon was built for ABI version %d " \
          "with Sokol %s, but the host provides ABI version %d with Sokol " \
          "%s. Rebuild the addon against the host's hnui.h.",               \
          (int)HNUI_ABI_VERSION,                                            \
          HNUI_SOKOL_COMMIT,                                                \
          hnui_api_version(),                                               \
          hnui_sokol_commit());                                             \
      napi_throw_error((env), "ERR_HNUI_VERSION", hnui_version_message_);   \
      return NULL;                                                          \
    }                                                                       \
  } while (0)

/// The window this `napi_env` owns, or NULL if it has none.
///
/// NULL is the ordinary answer for a runtime that never called
/// `createWindow`, or called it and was rejected -- not an error, and an
/// addon that can render headless may carry on. It is also what an addon
/// loaded into plain hermes-node would get, except that such an addon does
/// not load at all: the symbol is not there to resolve.
///
/// The handle is owned by the host and lives as long as the window does.
/// The caller must not free it.
hnui_window *hnui_get_window(napi_env env);

/// The render target for the frame currently being drawn: both view
/// handles, the target's pixel size, its color format, its sample count
/// and its generation (see `hnui_target`).
///
/// **Calling this is what marks the frame as drawn.** A tick in which no
/// callback asks for a target rendered nothing, so its slot is reclaimed
/// and the tick machine returns to idle immediately rather than waiting
/// for a publication that will never come (spec: Frames, "A tick in which
/// nothing drew ... returns to IDLE immediately"). That is why this is a
/// single call that both answers and records, rather than a query plus a
/// separate "I drew" flag an addon could forget to set.
///
/// \return false outside a frame callback -- which is a legitimate
///   question with a legitimate answer, not a misuse -- false if
///   \p out carries a `size` below `HNUI_TARGET_MIN_SIZE`, and false for a
///   NULL \p w, which is the same ordinary answer `hnui_get_window()` gives
///   and must not be a fatal one: an addon told it has no window is
///   entitled to hand that NULL straight back. A wrong-thread call on a
///   real window aborts instead.
///
/// The views are **borrowed for the duration of this frame callback
/// only**. They must not be retained, destroyed, or used after it returns:
/// the slot they name goes back into rotation, and a later frame's
/// callback will be drawing into it.
///
/// **`sokol_gfx` state that lasts one frame must be produced and consumed
/// inside one frame callback.** That covers data appended with
/// `sg_append_buffer` and `sokol_gl` commands recorded but not yet drawn.
/// The host, not the addon, ends each `sokol_gfx` frame, and it may end one
/// between two callbacks: on Metal it commits empty frames after a resize,
/// to release the retired surfaces, and `sokol_gl` rewinds on every commit.
/// Commands recorded in one callback and drawn in a later one may therefore
/// be gone, and so may appended data.
bool hnui_frame_target(hnui_window *w, hnui_target *out);

/// Register \p cb to run on the next frame tick, with \p user as its
/// opaque argument. Native callbacks share the tick and the timestamp with
/// the JS-side `requestAnimationFrame` callbacks and run in registration
/// order among them.
///
/// One tick, one call: a callback registered from inside a callback runs
/// on the next tick, exactly as `requestAnimationFrame` does in a browser.
///
/// \return an id for `hnui_cancel_animation_frame`, from the same id space
///   as `requestAnimationFrame`'s; ids start at 1 and are never reused.
///   Returns 0 -- "no id" -- for a NULL \p w or a NULL \p cb, for the
///   reason `hnui_frame_target` returns false for the first of those, and
///   once the window is closing (see "Closing" above), when \p cb is not
///   registered.
int hnui_request_animation_frame(hnui_window *w, hnui_frame_cb cb, void *user);

/// Cancel the frame callback \p id, so that it does not run: one waiting
/// for the next tick, or one later in the tick now running.
///
/// \p id may be either kind's: native and JavaScript frame callbacks share
/// one id space, so this cancels a `requestAnimationFrame` callback too,
/// and JavaScript's `cancelAnimationFrame` cancels a native one.
///
/// **An unknown id is a silent no-op, by contract** -- one never issued,
/// one that already ran, one already cancelled, and the id of the callback
/// that is running right now. That is what `cancelAnimationFrame` does in
/// a browser, and it lets a renderer cancel on teardown without tracking
/// whether its callback has fired. A NULL \p w does nothing either; a
/// wrong-thread call on a real window aborts.
void hnui_cancel_animation_frame(hnui_window *w, int id);

/// Register \p cb to receive every input and window event, with \p user
/// as its opaque argument.
///
/// Listeners run on the owner thread, in registration order, before the
/// event is emitted to JavaScript -- and after the window state the event
/// carries has been applied, so `hnui_get_window_state` called from inside
/// \p cb already reports it. The `hnui_event` and everything it points at
/// is borrowed for the duration of the call only.
///
/// A listener registered from inside a listener first sees the next
/// event.
///
/// \return an id, or 0 -- "no id" -- for a NULL \p w or a NULL \p cb, for
///   the reason `hnui_frame_target` returns false for the first of those.
int hnui_add_event_listener(hnui_window *w, hnui_event_cb cb, void *user);

/// Remove the listener \p id. **Removal is deferred**, unlike a frame
/// callback's cancellation: a listener removed while an event is being
/// delivered still receives THAT event, and no later one -- including the
/// next event of the same batch. So a listener may remove itself, or
/// another, from inside a listener.
///
/// What this means for the listener's `user` data: it may be freed only
/// when `hnui_event_dispatch_quiescent` says no listener is on the stack,
/// because a removed listener may still be about to run for the event in
/// flight. An unknown id -- never issued, or already removed -- is a silent
/// no-op, and removing twice is not an error. A NULL \p w does nothing; a
/// wrong-thread call on a real window aborts.
void hnui_remove_event_listener(hnui_window *w, int id);

/// True when the window is not delivering events, so the `user` data of a
/// listener already removed may be freed. **False for the whole dispatch
/// pass**, not only inside a native listener: a pass delivers every queued
/// event in turn -- native listeners, then the JavaScript `emit`, then the
/// nextTick and promise callbacks that queued, per event -- and a removed
/// listener may still be in the copy of the list the pass is walking. So it
/// is false inside a JavaScript listener or a tick callback reached from
/// one, too; true inside a frame callback. True for a NULL \p w, which has
/// no listeners. A wrong-thread call on a real window aborts.
bool hnui_event_dispatch_quiescent(hnui_window *w);

/// Cancel the close the window system is asking for, from inside the
/// delivery of an `HNUI_EVENT_CLOSE_REQUESTED` event.
///
/// The window is never closed by the window system: the host cancels every
/// close request it receives and forwards it as that event. Once the
/// event's native listeners and JavaScript `close` listeners have all run,
/// a close that nobody cancelled closes the window, and the process ends as
/// if `process.exit()` had been called with no argument -- so
/// `process.exitCode` is the status. This is the native half of
/// cancelling; the JavaScript half is the `close` payload's
/// `preventDefault()`, and either one is enough.
///
/// \return true when called while a close request is being delivered for
///   \p w -- from a native listener, or from native code reached from a
///   JavaScript `close` listener -- and the close is then cancelled. A
///   second call during the same event is harmless and returns true too.
///   False at any other time (a later call cancels nothing: the decision
///   is made when delivery ends), for a NULL \p w, and once the window is
///   closing. A wrong-thread call on a real window aborts.
bool hnui_cancel_close(hnui_window *w);

/// Fill \p out with the window's current state, as of the last event
/// delivered on the owner thread.
///
/// The size-prefix rule applies: the caller sets `out->size`, the host
/// writes at most that many bytes, and a `size` below
/// `HNUI_WINDOW_STATE_MIN_SIZE` is refused without writing anything. A
/// NULL \p w or \p out writes nothing either.
void hnui_get_window_state(hnui_window *w, hnui_window_state *out);

/// Set the mouse cursor. A value outside `HNUI_CURSOR_DEFAULT ..
/// HNUI_CURSOR_NOT_ALLOWED` is refused: one line on stderr naming the
/// function and the value, and nothing is sent. The four commands here
/// return nothing by design, so stderr is the only channel and silence
/// would be a dropped request nobody could find. Applied by the platform
/// thread on its next frame.
void hnui_set_cursor(hnui_window *w, hnui_cursor c);

/// Show or hide the mouse cursor over the window.
void hnui_show_cursor(hnui_window *w, bool show);

/// Lock the pointer to the window (relative motion), or release it.
void hnui_lock_pointer(hnui_window *w, bool lock);

/// Write \p utf8 to the clipboard. Refused, with a line on stderr naming
/// the function, for a NULL string or one longer than
/// `HNUI_CLIPBOARD_MAX_BYTES - 1` bytes -- the bound `ui.writeClipboard`
/// throws a `RangeError` at.
void hnui_write_clipboard(hnui_window *w, const char *utf8);

/// Read the clipboard as UTF-8, synchronously, waiting at most
/// \p timeout_ms milliseconds -- what ImGui's `GetClipboardTextFn` needs,
/// since it must return a value.
///
/// The read is performed by the platform thread on its next frame, and the
/// answer is written straight into a per-request slot this call waits on;
/// nothing on the owner thread runs while it waits. **It can time out**,
/// and does rather than hang: the platform thread can itself be blocked
/// (in presentation, or in another client's slow selection transfer). A
/// reply that arrives after the deadline is discarded. A \p timeout_ms of
/// zero or less always returns `HNUI_TIMEOUT`.
///
/// **Two limits \p timeout_ms does not override**, both from the X11
/// reader. The platform thread waits at most 2000 ms for another client to
/// answer, whatever \p timeout_ms says, because it is the thread that
/// presents and handles input, and a slow owner must not freeze the
/// window; a longer \p timeout_ms then gets `HNUI_TIMEOUT` at about two
/// seconds. And an owner that does not echo the request's timestamp in its
/// reply -- which ICCCM 2.2 requires, and which is how a reply is told
/// apart from a late one to an earlier read -- has its reply discarded, and
/// the read is reported as `HNUI_TIMEOUT`.
///
/// \return `HNUI_OK` with `*out` set to a NUL-terminated, `malloc`ed copy
///   of the text, to be freed with `hnui_free`. Every other status leaves
///   `*out` NULL -- never text from an earlier read:
///   - `HNUI_UNAVAILABLE`: nothing owns the clipboard, or it holds no text.
///   - `HNUI_TIMEOUT`: no answer before the deadline.
///   - `HNUI_TOO_LARGE`: over `HNUI_CLIPBOARD_MAX_BYTES - 1` bytes, or
///     offered incrementally.
///   - `HNUI_ERROR`: anything else, including a NULL \p w (the ordinary
///     answer for a runtime with no window, as for every entry point here)
///     or a failed allocation.
///   - `HNUI_INVALID_ARGUMENT`: \p out is NULL.
///   A wrong-thread call on a real window aborts.
int hnui_read_clipboard_sync(hnui_window *w, int timeout_ms, char **out);

/// Free memory an `hnui_*` call allocated for the caller
/// (`hnui_read_clipboard_sync`'s `*out`). Accepts NULL. Any thread.
void hnui_free(void *p);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // HERMES_NODE_COMPAT_UI_HNUI_H
