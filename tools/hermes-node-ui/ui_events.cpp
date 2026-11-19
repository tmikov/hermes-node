/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_events.h"

#include "hnui_window.h"
#include "sapp_event_convert.h"
#include "ui_module.h"

#include <ui_trace.h>

#include <hermes/node-compat/bindings/node_errors.h>
#include <hermes/node-compat/compositor/hnui_event_convert.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

namespace {

// Property setters. Errors are dropped on purpose, as in ui_test_hooks.cpp's
// own: each sets a plain value on an object this file holds, so the only
// way one fails is an env that is already throwing, and the caller's next
// call -- emit, or the tick callback -- is where that gets reported.

void setNumber(napi_env env, napi_value object, const char *name, double v) {
  napi_value value = nullptr;
  if (napi_create_double(env, v, &value) == napi_ok)
    napi_set_named_property(env, object, name, value);
}

void setBool(napi_env env, napi_value object, const char *name, bool v) {
  napi_value value = nullptr;
  if (napi_get_boolean(env, v, &value) == napi_ok)
    napi_set_named_property(env, object, name, value);
}

void setString(
    napi_env env,
    napi_value object,
    const char *name,
    const std::string &v) {
  napi_value value = nullptr;
  if (napi_create_string_utf8(env, v.data(), v.size(), &value) == napi_ok)
    napi_set_named_property(env, object, name, value);
}

/// The eight state properties, from \p state. Called when the object is
/// built and again for every event, before any listener for it runs.
void applyState(napi_env env, napi_value window, const WindowState &state) {
  setNumber(env, window, "width", state.width);
  setNumber(env, window, "height", state.height);
  setNumber(env, window, "framebufferWidth", state.framebufferWidth);
  setNumber(env, window, "framebufferHeight", state.framebufferHeight);
  setNumber(env, window, "dpiScale", state.dpiScale);
  setBool(env, window, "focused", state.focused);
  setBool(env, window, "fullscreen", state.fullscreen);
  setBool(env, window, "minimized", state.minimized);
}

/// The name an event is emitted under, or nullptr for one JavaScript is
/// not told about. A switch with every arm named, so a UiEventType added
/// later is a -Wswitch warning here rather than an event silently dropped.
const char *jsEventName(UiEventType type) {
  switch (type) {
    case UiEventType::kKeyDown:
      return "keydown";
    case UiEventType::kKeyUp:
      return "keyup";
    case UiEventType::kTextInput:
      return "char";
    case UiEventType::kMouseDown:
      return "mousedown";
    case UiEventType::kMouseUp:
      return "mouseup";
    case UiEventType::kMouseMove:
      return "mousemove";
    case UiEventType::kMouseWheel:
      return "wheel";
    case UiEventType::kMouseEnter:
      return "mouseenter";
    case UiEventType::kMouseLeave:
      return "mouseleave";
    case UiEventType::kResize:
      return "resize";
    case UiEventType::kFocus:
      return "focus";
    case UiEventType::kBlur:
      return "blur";
    case UiEventType::kMinimize:
      return "minimize";
    case UiEventType::kRestore:
      return "restore";
    case UiEventType::kCloseRequested:
      return "close";
    case UiEventType::kPaste:
      return "paste";
    case UiEventType::kFileDrop:
      return "drop";
    case UiEventType::kInvalid:
    case UiEventType::kClipboardReply:
      return nullptr;
  }
  return nullptr;
}

void setModifiers(napi_env env, napi_value payload, int modifiers) {
  setBool(env, payload, "shiftKey", (modifiers & HNUI_MOD_SHIFT) != 0);
  setBool(env, payload, "ctrlKey", (modifiers & HNUI_MOD_CTRL) != 0);
  setBool(env, payload, "altKey", (modifiers & HNUI_MOD_ALT) != 0);
  setBool(env, payload, "metaKey", (modifiers & HNUI_MOD_META) != 0);
}

/// The object passed to `emit`. Its fields per event are the table in the
/// plan's Task 11b; an event with no fields gets `{}`, not `undefined`, so
/// a listener may destructure its argument unconditionally.
napi_value buildPayload(napi_env env, const UiEvent &event) {
  napi_value payload = nullptr;
  if (napi_create_object(env, &payload) != napi_ok)
    return nullptr;

  switch (event.type) {
    case UiEventType::kKeyDown:
    case UiEventType::kKeyUp: {
      const char *code = domCodeForKey(event.keyCode);
      setString(env, payload, "code", code != nullptr ? code : "");
      setNumber(env, payload, "keyCode", event.keyCode);
      setBool(env, payload, "repeat", event.repeat);
      setModifiers(env, payload, event.modifiers);
      break;
    }
    case UiEventType::kTextInput:
      setString(env, payload, "char", event.text);
      setModifiers(env, payload, event.modifiers);
      break;
    case UiEventType::kMouseDown:
    case UiEventType::kMouseUp:
      setNumber(env, payload, "x", event.x);
      setNumber(env, payload, "y", event.y);
      setNumber(env, payload, "button", event.button);
      setModifiers(env, payload, event.modifiers);
      break;
    case UiEventType::kMouseMove:
      setNumber(env, payload, "x", event.x);
      setNumber(env, payload, "y", event.y);
      setNumber(env, payload, "dx", event.dx);
      setNumber(env, payload, "dy", event.dy);
      setModifiers(env, payload, event.modifiers);
      break;
    case UiEventType::kMouseWheel:
      // The converter carries the scroll distance in dx/dy
      // (sapp_event_convert.cpp).
      setNumber(env, payload, "x", event.x);
      setNumber(env, payload, "y", event.y);
      setNumber(env, payload, "deltaX", event.dx);
      setNumber(env, payload, "deltaY", event.dy);
      setModifiers(env, payload, event.modifiers);
      break;
    case UiEventType::kMouseEnter:
    case UiEventType::kMouseLeave:
      setNumber(env, payload, "x", event.x);
      setNumber(env, payload, "y", event.y);
      break;
    case UiEventType::kResize:
      setNumber(env, payload, "width", event.window.width);
      setNumber(env, payload, "height", event.window.height);
      setNumber(
          env, payload, "framebufferWidth", event.window.framebufferWidth);
      setNumber(
          env, payload, "framebufferHeight", event.window.framebufferHeight);
      setNumber(env, payload, "dpiScale", event.window.dpiScale);
      break;
    case UiEventType::kPaste:
      setString(env, payload, "text", event.text);
      break;
    case UiEventType::kFileDrop: {
      napi_value paths = nullptr;
      if (napi_create_array_with_length(env, event.paths.size(), &paths) ==
          napi_ok) {
        for (size_t i = 0; i < event.paths.size(); ++i) {
          napi_value path = nullptr;
          if (napi_create_string_utf8(
                  env, event.paths[i].data(), event.paths[i].size(), &path) ==
              napi_ok) {
            napi_set_element(env, paths, static_cast<uint32_t>(i), path);
          }
        }
        napi_set_named_property(env, payload, "paths", paths);
      }
      setNumber(env, payload, "x", event.x);
      setNumber(env, payload, "y", event.y);
      break;
    }
    case UiEventType::kFocus:
    case UiEventType::kBlur:
    case UiEventType::kMinimize:
    case UiEventType::kRestore:
    case UiEventType::kCloseRequested:
    case UiEventType::kInvalid:
    case UiEventType::kClipboardReply:
      break;
  }
  return payload;
}

/// `window.emit(name, payload)`. An exception goes to
/// handleCallbackException, which returns only if a listener took it.
void emitToJs(napi_env env, napi_value window, const UiEvent &event) {
  const char *name = jsEventName(event.type);
  if (name == nullptr)
    return;

  napi_value emit = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, window, "emit", &emit) != napi_ok ||
      napi_typeof(env, emit, &type) != napi_ok || type != napi_function) {
    // A program that replaced its window's `emit` with something
    // uncallable has opted out of events. Not silent, under tracing.
    handleCallbackException(env);
    uiTrace("event '%s': window.emit is not a function; not emitted", name);
    return;
  }

  napi_value argv[2] = {nullptr, nullptr};
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &argv[0]) !=
      napi_ok) {
    return;
  }
  argv[1] = buildPayload(env, event);
  if (argv[1] == nullptr)
    napi_get_undefined(env, &argv[1]);

  napi_value result = nullptr;
  if (napi_call_function(env, window, emit, 2, argv, &result) != napi_ok)
    handleCallbackException(env);
}

//===========================================================================
// readClipboard() Promises
//===========================================================================

/// The `readClipboard()` Promises not yet settled, by request id. JS thread
/// only. A file-static rather than a window member because a read may be
/// requested before the window object exists (between `createWindow()`
/// and its Promise resolving); its reply then waits in the input queue,
/// which is not drained until the object exists, and settles it from there.
///
/// Its own id space: these ids name entries here and nothing else. The sync
/// path's ids are `ClipboardRequests`', and the platform thread keeps the
/// two apart by `Command::replyThroughSlot`.
std::map<uint64_t, napi_deferred> gPendingReads;
uint64_t gNextReadId = 1;

/// Reject \p deferred with an Error carrying \p code. Inside a handle
/// scope the caller opened.
///
/// \return whether the rejection was performed, so a caller's trace can
/// record one that happened rather than one that was attempted.
bool rejectRead(
    napi_env env,
    napi_deferred deferred,
    const char *code,
    const std::string &message) {
  napi_value codeValue = nullptr;
  napi_value messageValue = nullptr;
  napi_value error = nullptr;
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &codeValue);
  napi_create_string_utf8(env, message.data(), message.size(), &messageValue);
  napi_create_error(env, codeValue, messageValue, &error);
  return napi_reject_deferred(env, deferred, error) == napi_ok;
}

/// Settle the Promise a `kClipboardReply` answers, then run the tick
/// callback, so its continuations run before the next event is delivered --
/// the same rule every other event follows.
void settleClipboardRead(napi_env env, const UiEvent &event) {
  auto it = gPendingReads.find(event.requestId);
  if (it == gPendingReads.end()) {
    // Already settled -- by uiFailPendingClipboardReads(), typically -- so
    // this late reply settles nothing.
    uiTrace(
        "clipboard read promise #%llu: no such pending read; reply ignored",
        static_cast<unsigned long long>(event.requestId));
    return;
  }

  // The scope first: if it cannot be opened the read stays pending, where
  // uiFailPendingClipboardReads() can still reach it, rather than being
  // erased and never settled.
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    uiTrace(
        "clipboard read promise #%llu: no handle scope; left pending",
        static_cast<unsigned long long>(event.requestId));
    return;
  }
  napi_deferred deferred = it->second;
  gPendingReads.erase(it);

  const char *code = nullptr;
  std::string message;
  switch (event.clipboardStatus) {
    case ClipboardStatus::kOk:
    case ClipboardStatus::kUnavailable:
      break;
    case ClipboardStatus::kTimeout:
      code = "ERR_CLIPBOARD_TIMEOUT";
      message = "readClipboard: the clipboard did not answer within " +
          std::to_string(kClipboardReadDefaultTimeoutMs) + " ms.";
      break;
    case ClipboardStatus::kTooLarge:
      code = "ERR_CLIPBOARD_TOO_LARGE";
      message = "readClipboard: the clipboard holds more than " +
          std::to_string(HNUI_CLIPBOARD_MAX_BYTES - 1) +
          " bytes, or offers its text only incrementally.";
      break;
    case ClipboardStatus::kError:
      code = "ERR_CLIPBOARD_FAILED";
      message = "readClipboard: the clipboard could not be read.";
      break;
  }

  if (code == nullptr) {
    // kUnavailable resolves with '': nothing to paste is an ordinary
    // answer. The text is empty for it in any case (channels.h).
    napi_value text = nullptr;
    const std::string &value = event.clipboardStatus == ClipboardStatus::kOk
        ? event.text
        : std::string();
    napi_create_string_utf8(env, value.data(), value.size(), &text);
    napi_resolve_deferred(env, deferred, text);
  } else {
    rejectRead(env, deferred, code, message);
  }
  napi_close_handle_scope(env, scope);

  uiRunTickCallback(env);
}

/// One event, start to finish. The order is the contract ui_events.h
/// states, and every step of it is pinned by test/ui-events.js.
void deliver(hnui_window *w, const UiEvent &event) {
  napi_env env = w->env;

  // 0. A clipboard reply is not an input event: it settles its Promise and
  //    is done. Before the mirror, deliberately -- its window snapshot is
  //    not a sample of anything -- and never shown to a listener.
  if (event.type == UiEventType::kClipboardReply) {
    settleClipboardRead(env, event);
    return;
  }

  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok)
    return;

  napi_value window = nullptr;
  if (napi_get_reference_value(env, w->jsWindow, &window) != napi_ok ||
      window == nullptr) {
    napi_close_handle_scope(env, scope);
    return;
  }

  // 1. The mirror, before anything that might read it (Ruling 52): a
  //    `blur` listener must already see `focused === false`, and so must a
  //    native listener calling hnui_get_window_state.
  w->state = event.window;
  applyState(env, window, w->state);

  // 2. The generation, after the mirror and before every listener: a
  //    `resize` listener that requests a frame must get a surface of the
  //    size it was just told about, not one of the old size.
  uiFollowFramebufferSize(w->state);

  // 3. Native listeners, in registration order. A copy of the list, so a
  //    listener that registers another one does not extend the iteration
  //    it is part of; the new one sees the next event. Each gets its own
  //    converter, which borrows from `event` -- which sits in the
  //    EventDispatcher's buffer and stays where it is for the whole call,
  //    because a nested dispatch returns at once (event_dispatcher.h).
  const std::vector<hnui_window::NativeEventListener> listeners =
      w->eventListeners;
  for (const hnui_window::NativeEventListener &listener : listeners) {
    HnuiEventConverter converter(event);
    listener.cb(w, listener.user, &converter.event());
  }

  // 4. JavaScript.
  emitToJs(env, window, event);

  // 5. nextTick and promise callbacks queued by this event's listeners run
  //    now, before the next event is delivered -- what Node's MakeCallback
  //    does after every callback into JavaScript.
  uiRunTickCallback(env);

  napi_close_handle_scope(env, scope);
}

//===========================================================================
// Platform commands (the window's methods and `title`)
//===========================================================================
//
// Everything below runs on the JS thread and only validates and pushes: a
// bad argument is refused HERE, with an exception the program can catch,
// because by the time the platform thread sees a command there is nobody
// left to throw to -- and some of what Sokol does with a bad one is an
// assertion that aborts the process (a cursor hotspot at the image's edge,
// a pixel buffer of the wrong size).

/// The cursor names JavaScript uses, and the `hnui_cursor` each stands for.
/// The mapping from `hnui_cursor` to Sokol's enumeration is the platform
/// thread's (ui_platform.cpp), so that `hnui_set_cursor` can push the same
/// command.
struct CursorName {
  const char *name;
  hnui_cursor cursor;
};
const CursorName kCursorNames[] = {
    {"default", HNUI_CURSOR_DEFAULT},
    {"arrow", HNUI_CURSOR_ARROW},
    {"ibeam", HNUI_CURSOR_IBEAM},
    {"crosshair", HNUI_CURSOR_CROSSHAIR},
    {"pointer", HNUI_CURSOR_POINTING_HAND},
    {"ew-resize", HNUI_CURSOR_RESIZE_EW},
    {"ns-resize", HNUI_CURSOR_RESIZE_NS},
    {"nwse-resize", HNUI_CURSOR_RESIZE_NWSE},
    {"nesw-resize", HNUI_CURSOR_RESIZE_NESW},
    {"move", HNUI_CURSOR_RESIZE_ALL},
    {"not-allowed", HNUI_CURSOR_NOT_ALLOWED},
};

/// Largest image side accepted. Bounds width * height * 4 well inside an
/// int, which is what Sokol compares the pixel size against.
constexpr int kMaxImageSide = 16384;

/// Read arguments of a window method. \p w is the method's data pointer.
bool readArgs(
    napi_env env,
    napi_callback_info info,
    size_t want,
    napi_value *argv,
    size_t *argc) {
  *argc = want;
  return napi_get_cb_info(env, info, argc, argv, nullptr, nullptr) == napi_ok;
}

/// A required boolean argument. A non-boolean is a TypeError rather than
/// being coerced: `win.setFullscreen('false')` coerced to true is a bug the
/// caller should hear about.
bool readBoolArg(
    napi_env env,
    napi_value value,
    const char *method,
    bool *out) {
  napi_valuetype type = napi_undefined;
  if (value != nullptr && napi_typeof(env, value, &type) == napi_ok &&
      type == napi_boolean && napi_get_value_bool(env, value, out) == napi_ok) {
    return true;
  }
  const std::string message = std::string(method) + ": expected a boolean.";
  napi_throw_type_error(env, nullptr, message.c_str());
  return false;
}

bool readIntProperty(
    napi_env env,
    napi_value object,
    const char *name,
    const char *method,
    int *out) {
  napi_value value = nullptr;
  napi_valuetype type = napi_undefined;
  double number = 0;
  if (napi_get_named_property(env, object, name, &value) == napi_ok &&
      napi_typeof(env, value, &type) == napi_ok && type == napi_number &&
      napi_get_value_double(env, value, &number) == napi_ok &&
      number == static_cast<double>(static_cast<int>(number)) &&
      number >= -kMaxImageSide && number <= kMaxImageSide) {
    *out = static_cast<int>(number);
    return true;
  }
  const std::string message = std::string(method) + ": '" + name +
      "' must be an integer (at most " + std::to_string(kMaxImageSide) +
      " in magnitude).";
  napi_throw_range_error(env, nullptr, message.c_str());
  return false;
}

/// Validate and copy `{width, height, pixels}` (+ hotspot for a cursor).
/// The pixels are copied out of the typed array: the command outlives this
/// call, and the array is the program's to change.
bool readImage(
    napi_env env,
    napi_value desc,
    const char *method,
    bool isCursor,
    std::vector<uint8_t> *rgba,
    int *width,
    int *height,
    int *hotspotX,
    int *hotspotY) {
  napi_valuetype type = napi_undefined;
  if (desc == nullptr || napi_typeof(env, desc, &type) != napi_ok ||
      type != napi_object) {
    const std::string message = std::string(method) +
        ": expected an object with width, height and pixels.";
    napi_throw_type_error(env, nullptr, message.c_str());
    return false;
  }
  if (!readIntProperty(env, desc, "width", method, width) ||
      !readIntProperty(env, desc, "height", method, height)) {
    return false;
  }
  if (*width <= 0 || *height <= 0) {
    const std::string message =
        std::string(method) + ": width and height must be positive.";
    napi_throw_range_error(env, nullptr, message.c_str());
    return false;
  }
  if (isCursor) {
    if (!readIntProperty(env, desc, "hotspotX", method, hotspotX) ||
        !readIntProperty(env, desc, "hotspotY", method, hotspotY)) {
      return false;
    }
    // Sokol asserts this on every platform (sokol_app.h:14632), because a
    // hotspot on the last row or column makes the cursor vanish on
    // Windows. An assertion aborts the process, so it is refused here.
    if (*hotspotX < 0 || *hotspotY < 0 || *hotspotX >= *width - 1 ||
        *hotspotY >= *height - 1) {
      const std::string message = std::string(method) +
          ": the hotspot must lie inside the image, at least one pixel "
          "away from its right and bottom edges.";
      napi_throw_range_error(env, nullptr, message.c_str());
      return false;
    }
  }

  napi_value pixels = nullptr;
  bool isTyped = false;
  napi_typedarray_type arrayType = napi_int8_array;
  size_t length = 0;
  void *data = nullptr;
  napi_value arrayBuffer = nullptr;
  size_t byteOffset = 0;
  if (napi_get_named_property(env, desc, "pixels", &pixels) != napi_ok ||
      napi_is_typedarray(env, pixels, &isTyped) != napi_ok || !isTyped ||
      napi_get_typedarray_info(
          env, pixels, &arrayType, &length, &data, &arrayBuffer, &byteOffset) !=
          napi_ok ||
      (arrayType != napi_uint8_array &&
       arrayType != napi_uint8_clamped_array)) {
    const std::string message = std::string(method) +
        ": 'pixels' must be a Uint8Array (or Buffer) of RGBA bytes.";
    napi_throw_type_error(env, nullptr, message.c_str());
    return false;
  }
  const size_t expected = static_cast<size_t>(*width) * *height * 4;
  if (length != expected) {
    const std::string message = std::string(method) + ": 'pixels' holds " +
        std::to_string(length) + " bytes, but " + std::to_string(*width) + "x" +
        std::to_string(*height) + " RGBA needs " + std::to_string(expected) +
        ".";
    napi_throw_range_error(env, nullptr, message.c_str());
    return false;
  }
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  rgba->assign(bytes, bytes + length);
  return true;
}

napi_value jsUndefined(napi_env env) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value windowSetFullscreen(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 0;
  bool on = false;
  if (!readArgs(env, info, 1, argv, &argc) ||
      !readBoolArg(env, argv[0], "setFullscreen", &on)) {
    return nullptr;
  }
  uiPushCommand(Command::setFullscreen(on));
  return jsUndefined(env);
}

napi_value windowSetIcon(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 0;
  std::vector<uint8_t> rgba;
  int width = 0, height = 0, unusedX = 0, unusedY = 0;
  if (!readArgs(env, info, 1, argv, &argc) ||
      !readImage(
          env,
          argv[0],
          "setIcon",
          false,
          &rgba,
          &width,
          &height,
          &unusedX,
          &unusedY)) {
    return nullptr;
  }
  uiPushCommand(Command::setIcon(std::move(rgba), width, height));
  return jsUndefined(env);
}

napi_value windowSetCursor(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 0;
  std::string name;
  napi_valuetype type = napi_undefined;
  bool haveName = false;
  if (readArgs(env, info, 1, argv, &argc) && argv[0] != nullptr &&
      napi_typeof(env, argv[0], &type) == napi_ok && type == napi_string) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) ==
        napi_ok) {
      name.assign(length, '\0');
      haveName = napi_get_value_string_utf8(
                     env, argv[0], &name[0], length + 1, &length) == napi_ok;
      name.resize(length);
    }
  }
  if (haveName) {
    for (const CursorName &entry : kCursorNames) {
      if (name == entry.name) {
        uiPushCommand(Command::setCursor(static_cast<int>(entry.cursor)));
        return jsUndefined(env);
      }
    }
  }
  std::string message = "setCursor: unknown cursor ";
  message += haveName ? "'" + name + "'" : "(not a string)";
  message += "; expected one of ";
  bool first = true;
  for (const CursorName &entry : kCursorNames) {
    message += (first ? "" : ", ") + std::string(entry.name);
    first = false;
  }
  message += ".";
  napi_throw_type_error(env, nullptr, message.c_str());
  return nullptr;
}

napi_value windowSetCursorImage(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 0;
  std::vector<uint8_t> rgba;
  int width = 0, height = 0, hotspotX = 0, hotspotY = 0;
  if (!readArgs(env, info, 1, argv, &argc) ||
      !readImage(
          env,
          argv[0],
          "setCursorImage",
          true,
          &rgba,
          &width,
          &height,
          &hotspotX,
          &hotspotY)) {
    return nullptr;
  }
  uiPushCommand(Command::setCursorImage(
      std::move(rgba), width, height, hotspotX, hotspotY));
  return jsUndefined(env);
}

napi_value windowShowCursor(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 0;
  bool show = false;
  if (!readArgs(env, info, 1, argv, &argc) ||
      !readBoolArg(env, argv[0], "showCursor", &show)) {
    return nullptr;
  }
  uiPushCommand(Command::showCursor(show));
  return jsUndefined(env);
}

napi_value windowLockPointer(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 0;
  bool lock = false;
  if (!readArgs(env, info, 1, argv, &argc) ||
      !readBoolArg(env, argv[0], "lockPointer", &lock)) {
    return nullptr;
  }
  uiPushCommand(Command::lockPointer(lock));
  return jsUndefined(env);
}

/// `win.title`'s getter: the last value set, from the JS-side copy.
napi_value windowGetTitle(napi_env env, napi_callback_info info) {
  void *data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  auto *w = static_cast<hnui_window *>(data);
  napi_value result = nullptr;
  if (w == nullptr ||
      napi_create_string_utf8(env, w->title.data(), w->title.size(), &result) !=
          napi_ok) {
    return jsUndefined(env);
  }
  return result;
}

/// `win.title = x`: coerced to a string, as a browser's `document.title`
/// is, remembered for the getter, and sent to the platform thread.
napi_value windowSetTitle(napi_env env, napi_callback_info info) {
  napi_value argv[1] = {nullptr};
  size_t argc = 1;
  void *data = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, &data) != napi_ok ||
      data == nullptr || argc < 1) {
    return jsUndefined(env);
  }
  auto *w = static_cast<hnui_window *>(data);
  napi_value text = nullptr;
  if (napi_coerce_to_string(env, argv[0], &text) != napi_ok)
    return nullptr; // the conversion's own exception is pending
  size_t length = 0;
  if (napi_get_value_string_utf8(env, text, nullptr, 0, &length) != napi_ok)
    return nullptr;
  std::string title(length, '\0');
  if (napi_get_value_string_utf8(env, text, &title[0], length + 1, &length) !=
      napi_ok) {
    return nullptr;
  }
  title.resize(length);
  w->title = title;
  uiPushCommand(Command::setTitle(std::move(title)));
  return jsUndefined(env);
}

/// Put the command methods and the `title` accessor on \p window. The data
/// pointer of each is \p w, which lives as long as the process.
bool defineWindowCommands(napi_env env, napi_value window, hnui_window *w) {
  const napi_property_descriptor properties[] = {
      {"setFullscreen",
       nullptr,
       windowSetFullscreen,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       w},
      {"setIcon",
       nullptr,
       windowSetIcon,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       w},
      {"setCursor",
       nullptr,
       windowSetCursor,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       w},
      {"setCursorImage",
       nullptr,
       windowSetCursorImage,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       w},
      {"showCursor",
       nullptr,
       windowShowCursor,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       w},
      {"lockPointer",
       nullptr,
       windowLockPointer,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       w},
      {"title",
       nullptr,
       nullptr,
       windowGetTitle,
       windowSetTitle,
       nullptr,
       napi_enumerable,
       w},
  };
  return napi_define_properties(
             env,
             window,
             sizeof(properties) / sizeof(properties[0]),
             properties) == napi_ok;
}

} // namespace

napi_value uiWriteClipboard(napi_env env, napi_callback_info info) {
  if (!uiWindowRequested()) {
    napi_throw_error(
        env,
        nullptr,
        "writeClipboard: no window exists. Call createWindow() first.");
    return nullptr;
  }
  napi_value argv[1] = {nullptr};
  size_t argc = 1;
  napi_valuetype type = napi_undefined;
  size_t length = 0;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
      argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok ||
      type != napi_string ||
      napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) !=
          napi_ok) {
    napi_throw_type_error(env, nullptr, "writeClipboard: expected a string.");
    return nullptr;
  }
  // Sokol copies into a buffer of HNUI_CLIPBOARD_MAX_BYTES including the
  // terminator, and a string that does not fit leaves the buffer empty
  // while the window already owns the selection. Refused here instead.
  if (length >= HNUI_CLIPBOARD_MAX_BYTES) {
    const std::string message = "writeClipboard: the text is " +
        std::to_string(length) +
        " bytes in UTF-8; the clipboard holds at most " +
        std::to_string(HNUI_CLIPBOARD_MAX_BYTES - 1) + ".";
    napi_throw_range_error(env, nullptr, message.c_str());
    return nullptr;
  }
  std::string text(length, '\0');
  if (napi_get_value_string_utf8(env, argv[0], &text[0], length + 1, &length) !=
      napi_ok) {
    napi_throw_type_error(
        env, nullptr, "writeClipboard: could not read the text.");
    return nullptr;
  }
  text.resize(length);
  uiPushCommand(Command::writeClipboard(std::move(text)));
  return jsUndefined(env);
}

size_t uiFailPendingClipboardReads(napi_env env) {
  // Taken out first, so a reply delivered afterwards finds nothing to
  // settle, and a rejection handler that starts a new read gets a fresh
  // entry this loop does not see.
  std::map<uint64_t, napi_deferred> pending;
  pending.swap(gPendingReads);
  if (pending.empty())
    return 0;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    // Put back rather than lost; nothing else can settle them.
    gPendingReads.insert(pending.begin(), pending.end());
    return 0;
  }
  for (const auto &entry : pending) {
    uiTrace(
        "clipboard read promise #%llu: failed by uiFailPendingClipboardReads",
        static_cast<unsigned long long>(entry.first));
    rejectRead(
        env,
        entry.second,
        "ERR_CLIPBOARD_FAILED",
        "readClipboard: the read was abandoned before it was answered.");
  }
  napi_close_handle_scope(env, scope);
  return pending.size();
}

napi_value uiReadClipboard(napi_env env, napi_callback_info info) {
  (void)info;
  // Without a window nothing drains the command queue, so the Promise
  // would never settle. Refused, as writeClipboard refuses.
  if (!uiWindowRequested()) {
    napi_throw_error(
        env,
        nullptr,
        "readClipboard: no window exists. Call createWindow() first.");
    return nullptr;
  }
  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok)
    return nullptr;

  const uint64_t id = gNextReadId++;
  const uint64_t deadline = clipboardClockNs() +
      static_cast<uint64_t>(kClipboardReadDefaultTimeoutMs) * 1000000u;
  // Recorded only once the command is accepted. A refused one -- the
  // window is CLOSING -- will never be answered, and its Promise is
  // rejected here rather than left for a uiFailPendingClipboardReads() that
  // has already run. Pushing before recording is safe: the reply comes
  // back through the input queue, which only this thread drains.
  if (!uiPushCommand(
          Command::readClipboard(id, /*replyThroughSlot=*/false, deadline))) {
    if (rejectRead(
            env,
            deferred,
            "ERR_CLIPBOARD_FAILED",
            "readClipboard: the window is closing.")) {
      uiTrace("clipboard read promise: refused (closing)");
    }
    return promise;
  }
  gPendingReads[id] = deferred;
  return promise;
}

napi_value uiCreateWindowObject(hnui_window *w, std::string &error) {
  napi_env env = w->env;

  napi_value global = nullptr;
  napi_value require = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_named_property(env, global, "require", &require) != napi_ok ||
      napi_typeof(env, require, &type) != napi_ok || type != napi_function) {
    napi_value pending = nullptr;
    napi_get_and_clear_last_exception(env, &pending);
    error =
        "could not create the window object: globalThis.require is not a "
        "function, so EventEmitter could not be loaded";
    return nullptr;
  }

  napi_value specifier = nullptr;
  napi_value eventEmitter = nullptr;
  if (napi_create_string_utf8(env, "events", NAPI_AUTO_LENGTH, &specifier) !=
          napi_ok ||
      napi_call_function(env, global, require, 1, &specifier, &eventEmitter) !=
          napi_ok ||
      napi_typeof(env, eventEmitter, &type) != napi_ok ||
      type != napi_function) {
    napi_value pending = nullptr;
    napi_get_and_clear_last_exception(env, &pending);
    error =
        "could not create the window object: require('events') did not "
        "return the EventEmitter constructor";
    return nullptr;
  }

  napi_value window = nullptr;
  if (napi_new_instance(env, eventEmitter, 0, nullptr, &window) != napi_ok) {
    napi_value pending = nullptr;
    napi_get_and_clear_last_exception(env, &pending);
    error = "could not create the window object: new EventEmitter() failed";
    return nullptr;
  }

  applyState(env, window, w->state);

  if (!defineWindowCommands(env, window, w)) {
    napi_value pending = nullptr;
    napi_get_and_clear_last_exception(env, &pending);
    error =
        "could not create the window object: its methods could not be "
        "defined";
    return nullptr;
  }

  if (napi_create_reference(env, window, 1, &w->jsWindow) != napi_ok) {
    w->jsWindow = nullptr;
    error = "could not create the window object: it could not be held";
    return nullptr;
  }
  return window;
}

void uiDispatchEvents(hnui_window *w) {
  // Not drained before the object exists: see ui_events.h.
  if (w == nullptr || w->jsWindow == nullptr || !w->events)
    return;
  w->events->dispatch([w](const UiEvent &event) { deliver(w, event); });
}

void uiRunTickCallback(napi_env env) {
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok)
    return;

  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value tick = nullptr;
  napi_valuetype type = napi_undefined;
  // Installed on `process` by the runtime's bootstrap
  // (lib/runtime/hermes_node_runtime.cpp, step 10). Looked up each call
  // rather than cached, because it is the property's current value that
  // Node's own MakeCallback would call.
  if (napi_get_global(env, &global) == napi_ok &&
      napi_get_named_property(env, global, "process", &process) == napi_ok &&
      napi_get_named_property(env, process, "_tickCallback", &tick) ==
          napi_ok &&
      napi_typeof(env, tick, &type) == napi_ok && type == napi_function) {
    napi_value result = nullptr;
    if (napi_call_function(env, process, tick, 0, nullptr, &result) !=
        napi_ok) {
      handleCallbackException(env);
    }
  } else {
    handleCallbackException(env);
  }

  napi_close_handle_scope(env, scope);
}

} // namespace node_compat
} // namespace hermes
