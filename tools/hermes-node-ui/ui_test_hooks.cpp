/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "event_injector.h"
#include "ui_events.h"
#include "ui_internal.h"
#include "ui_module.h"

#include <test_hooks.h>
#include <ui_trace.h>

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/owner_dispatch.h>
#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>

#include <sokol_app.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

//===========================================================================
// Test hooks (lib/compositor-gpu/test_hooks.h)
//===========================================================================
//
// Compiled always, enabled only by HERMES_NODE_UI_TEST_HOOKS, and every
// call site -- here, in ui_module.cpp and in ui_platform.cpp -- checks
// `uiTestHooksEnabled()` first. Nothing in test_hooks.h enforces that, so
// it is a convention these files have to keep rather than one they inherit.

CompletionGate gGate;
FrameCapture gCapture;

/// Test hook: platform events injected by `__injectEvent`, taken by
/// `onFrame`. Carries its own lock.
PlatformEventInjector gInjector;

std::mutex gCaptureMutex;
CaptureState gCaptureState;

namespace {

/// JS thread only.
napi_deferred gCaptureDeferred = nullptr;

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

} // namespace

//===========================================================================
// Frame capture (test hook)
//===========================================================================

/// Hand the finished capture back to the JavaScript that asked for it.
/// JS thread, posted by the platform thread.
void onCaptureFinished() {
  // Posted before CLOSING and run by the dispatcher's closing drain. The
  // capture's Promise stays unsettled, as every outstanding Promise does
  // once the process is on its way out.
  if (uiClosing()) {
    uiTrace("capture: result dropped (closing)");
    return;
  }
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

/// `__captureFrame(options)` -- the JavaScript end of the frame-capture
/// test hook (`test_hooks.h`).
///
/// **The caller supplies every agreement field**, and that is the point
/// rather than an inconvenience: the capture is taken only when the
/// generation, the publication id, the target size, the framebuffer size
/// and the DPI scale all match what the presenter
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
  int giveUpAfterFrames = 0;
  std::string path;

  readInt(env, argv[0], "generation", &generation);
  readDouble(env, argv[0], "publicationId", &publicationId);
  readInt(env, argv[0], "targetWidth", &targetWidth);
  readInt(env, argv[0], "targetHeight", &targetHeight);
  readInt(env, argv[0], "framebufferWidth", &framebufferWidth);
  readInt(env, argv[0], "framebufferHeight", &framebufferHeight);
  readDouble(env, argv[0], "dpiScale", &dpiScale);
  readInt(env, argv[0], "giveUpAfterFrames", &giveUpAfterFrames);
  readString(env, argv[0], "path", &path);

  want.generation = GenerationId{static_cast<uint32_t>(generation)};
  want.publicationId = static_cast<uint64_t>(publicationId);
  want.targetWidth = targetWidth;
  want.targetHeight = targetHeight;
  want.framebufferWidth = framebufferWidth;
  want.framebufferHeight = framebufferHeight;
  want.dpiScale = dpiScale;

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

//===========================================================================
// Platform frame barrier (test hook)
//===========================================================================

namespace {

/// Both threads, under gFrameCountMutex: how many onFrame() calls have
/// begun and how many have returned. onFrame() runs on one thread and never
/// re-enters, so the n-th call to begin is also the n-th to end, which is
/// what lets two counters stand for "the frames that began after now".
std::mutex gFrameCountMutex;
std::condition_variable gFrameCountChanged;
uint64_t gFramesBegan = 0;
uint64_t gFramesEnded = 0;

/// How long __waitPlatformFrames() waits before answering false. A bound,
/// not a timing assumption: a platform thread that has stopped producing
/// frames is a failure the test should report, not a hang.
constexpr auto kFrameBarrierTimeout = std::chrono::seconds(10);

} // namespace

void uiTestHookFrameBegan() {
  std::lock_guard<std::mutex> lock(gFrameCountMutex);
  ++gFramesBegan;
}

void uiTestHookFrameEnded() {
  {
    std::lock_guard<std::mutex> lock(gFrameCountMutex);
    ++gFramesEnded;
  }
  gFrameCountChanged.notify_all();
}

/// `__waitPlatformFrames(n)` -> boolean. Blocks the calling thread until
/// the platform thread has returned from n onFrame() calls that BEGAN after
/// this call was made; false if that has not happened within 10 s.
///
/// A frame already in progress when this is called does not count: it may
/// have passed the point a test cares about (the injected-event drain, the
/// tick post) before the test's own request existed. Works in every
/// lifecycle state, CLOSING included: it is a mutex, a condition variable
/// and two counters, not a command, and the platform thread keeps running
/// frames until the process ends.
napi_value waitPlatformFrames(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t n = 1;
  napi_valuetype type = napi_undefined;
  if (argc >= 1 && napi_typeof(env, argv[0], &type) == napi_ok &&
      type == napi_number) {
    napi_get_value_int32(env, argv[0], &n);
  }
  if (n < 0)
    n = 0;

  bool reached;
  {
    std::unique_lock<std::mutex> lock(gFrameCountMutex);
    const uint64_t target = gFramesBegan + static_cast<uint64_t>(n);
    reached = gFrameCountChanged.wait_for(
        lock, kFrameBarrierTimeout, [&] { return gFramesEnded >= target; });
  }
  uiTrace(
      "test hook: waited for platform frames: %s",
      reached ? "ok" : "timed out");

  napi_value result = nullptr;
  napi_get_boolean(env, reached, &result);
  return result;
}

namespace {

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

} // namespace

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

} // namespace node_compat
} // namespace hermes
