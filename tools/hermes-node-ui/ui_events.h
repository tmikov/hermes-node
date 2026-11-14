/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_UI_EVENTS_H
#define HERMES_NODE_COMPAT_UI_UI_EVENTS_H

#include <node_api.h>

#include <string>

/// Global scope, for the reason ui_module.h gives.
struct hnui_window;

namespace hermes {
namespace node_compat {

/// The JS-thread half of input: the window object, and delivering queued
/// events to native listeners and to JavaScript.
///
/// Everything here runs on the window's owner thread and reaches its state
/// only through the `hnui_window` it is given -- never through
/// `ui_module.cpp`'s statics, which belong to that file's two threads and
/// are documented there.

/// Build the JavaScript window object for \p w: an instance of Node's
/// `EventEmitter`, obtained through `globalThis.require('events')` (Ruling
/// 53 -- no JavaScript file is added to the embedded module set for it),
/// carrying the eight state properties from `w->state`. Stores a strong
/// reference in `w->jsWindow`; from then on uiDispatchEvents() delivers.
///
/// \return the object, or nullptr with \p error set and no exception left
///   pending.
napi_value uiCreateWindowObject(hnui_window *w, std::string &error);

/// `ui.writeClipboard(text)`, exported by the module. Pushes a
/// `kWriteClipboard` command; throws a `RangeError` for a string that does
/// not fit in the clipboard buffer (`HNUI_CLIPBOARD_MAX_BYTES`) rather than
/// handing it to Sokol, which would take ownership of the selection with an
/// empty buffer (`sokol_app.h:13205-13215`). Defined here with the window's
/// own methods, and registered by `initUiModule`.
napi_value uiWriteClipboard(napi_env env, napi_callback_info info);

/// `ui.readClipboard()`, exported by the module. Returns a Promise and
/// pushes a `kReadClipboard` command with a deadline of
/// `kClipboardReadDefaultTimeoutMs`; the platform thread answers with a
/// `kClipboardReply` event, which uiDispatchEvents() turns into the
/// settlement: kOk resolves with the text, kUnavailable with `''`, and the
/// rest reject with an Error whose `code` is `ERR_CLIPBOARD_TIMEOUT`,
/// `ERR_CLIPBOARD_TOO_LARGE` or `ERR_CLIPBOARD_FAILED`. Throws, as
/// `writeClipboard` does, when no window has been requested: nothing would
/// drain the command, so the Promise would never settle.
napi_value uiReadClipboard(napi_env env, napi_callback_info info);

/// Reject every `readClipboard()` Promise not yet settled, with an Error
/// whose `code` is `ERR_CLIPBOARD_FAILED`, and forget them, so a reply that
/// arrives afterwards settles nothing (it is traced and ignored). The
/// Promise half of "shutdown and setup failure complete outstanding
/// requests with an error"; `ClipboardRequests::failAll` is the
/// synchronous half. **Task 15 calls the two together when CLOSING is
/// entered.** Until then its only caller is the `__failPendingClipboardReads`
/// test hook. JS thread only.
void uiFailPendingClipboardReads(napi_env env);

/// Deliver every queued event, in order, through `w->events`.
///
/// For each event: assign its window snapshot to `w->state` and to the JS
/// object's properties; start a new surface generation if its framebuffer
/// size changed (`uiFollowFramebufferSize`); run the native listeners in
/// registration order,
/// each through its own `HnuiEventConverter`; `emit` it to JavaScript; then
/// run `process._tickCallback()`, so that `nextTick` and promise callbacks
/// queued by one event's listeners run before the next event is delivered.
/// An exception from `emit` or from the tick callback goes through
/// `handleCallbackException`: handled means carry on with the next event,
/// unhandled does not return.
///
/// **Does nothing until `w->jsWindow` exists**, and in particular does not
/// drain: an event that arrived between `init_cb` and the window's Promise
/// resolving stays queued, and is delivered -- updating the mirror -- by
/// the first call after the object is built. ui_module.cpp makes that
/// first call one loop turn after the Promise resolves
/// (`openEventDelivery`), so that listeners attached right after `await
/// createWindow()` receive those events too.
///
/// A call made while one is already delivering returns at once
/// (`EventDispatcher`); that is reachable, since a listener calling
/// `process.exit()` pumps the loop.
void uiDispatchEvents(hnui_window *w);

/// Call `process._tickCallback()`, routing an escaping exception through
/// `handleCallbackException` -- which does not return when nothing handled
/// it. The frame tick calls the same function after each frame callback.
void uiRunTickCallback(napi_env env);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_UI_EVENTS_H
