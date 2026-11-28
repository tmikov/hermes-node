/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The addon's N-API surface, the frame state machine and tracing. Loaded by
// hermes-node-ui as `hermes-node:imgui` (tools/hermes-node-ui/
// named_addons.cpp), from hermes-node-imgui.node beside the binary.
//
// Everything here runs on the JS thread of the runtime that loaded the
// addon, which is the window's owner thread: N-API calls arrive on no other.

#include "imgui_addon.h"

#include "bind/bind_guards.h"
#include "bind/bind_util.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hnimgui {

namespace {

/// The host's rule for UI tracing (lib/compositor-gpu/ui_trace.cpp,
/// uiTracingEnabled), copied rather than shared: the addon includes no host
/// header. Read once; several threads may trace.
bool tracingEnabled() {
  static const bool enabled = [] {
    const char *dbg = std::getenv("HERMES_NODE_DEBUG_NATIVE");
    return dbg != nullptr && std::strstr(dbg, "UI") != nullptr;
  }();
  return enabled;
}

} // namespace

void trace(const char *fmt, ...) {
  if (!tracingEnabled())
    return;
  static const char kPrefix[] = "[ui] imgui: ";
  const size_t prefixLen = sizeof(kPrefix) - 1;

  va_list args;
  va_start(args, fmt);
  va_list measure;
  va_copy(measure, args);
  const int bodyLen = std::vsnprintf(nullptr, 0, fmt, measure);
  va_end(measure);
  if (bodyLen < 0) {
    va_end(args);
    std::fprintf(stderr, "%s<unformattable trace line> %s\n", kPrefix, fmt);
    std::fflush(stderr);
    return;
  }
  // The whole line in one buffer and one fwrite, so lines from different
  // threads cannot interleave (the host's ui_trace.cpp explains why one
  // write matters). On the stack unless the line is long.
  char stackBuf[512];
  const size_t needed = prefixLen + static_cast<size_t>(bodyLen) + 2;
  char *line = needed <= sizeof(stackBuf)
      ? stackBuf
      : static_cast<char *>(std::malloc(needed));
  if (line == nullptr) {
    va_end(args);
    return;
  }
  std::memcpy(line, kPrefix, prefixLen);
  std::vsnprintf(line + prefixLen, bodyLen + 1, fmt, args);
  va_end(args);
  line[prefixLen + bodyLen] = '\n';
  std::fwrite(line, 1, prefixLen + bodyLen + 1, stderr);
  std::fflush(stderr);
  if (line != stackBuf)
    std::free(line);
}

bool testHooksEnabled() {
  // The host's rule (lib/compositor-gpu/test_hooks.cpp,
  // uiTestHooksEnabledFor): unset, empty or exactly "0" is off, anything
  // else on. Read once.
  static const bool enabled = [] {
    const char *value = std::getenv("HERMES_NODE_UI_TEST_HOOKS");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

namespace {

/// The window the context was created for. One context per process, made
/// at the first newFrame() that finds a window.
hnui_window *gWindow = nullptr;
/// Between a newFrame() and the render() (or the next newFrame()) that
/// ends its frame.
bool gFrameOpen = false;
/// The target of the last render() that drew; all zero before any.
hnui_target gLastTarget = {};

void createContext(hnui_window *w) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  // A toolkit must not drop imgui.ini or imgui_log.txt into the current
  // directory of whatever program loaded it.
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  platformInit(w);
  rendererInit();
  gWindow = w;
  trace(
      "context created (imgui %s, docking=%d)",
      ImGui::GetVersion(),
      (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0 ? 1 : 0);
}

/// `newFrame()`: start an ImGui frame. Takes no target -- render() does,
/// because taking one is what tells the host the tick drew. Returns false,
/// having done nothing, only when this runtime has no window.
napi_value newFrame(napi_env env, napi_callback_info info) {
  bind::Call c(env, info, "newFrame");
  if (!c.ok())
    return nullptr;
  if (bind::insideCallback())
    return c.throwUsage("not allowed inside a list clipper's callback");
  if (!ensureContext(env))
    return c.result(false);
  if (gFrameOpen) {
    // A second newFrame() in one callback, or the first one after a throw
    // left a frame open: the two cannot be told apart, so neither throws.
    // What the discarded frame left open is reported (and traced) by
    // EndFrame()'s recovery; it belongs to no call, so it is dropped.
    // The frame's last call may have turned the asserts back on.
    guard::restoreRecoveryFlags();
    ImGui::EndFrame();
    guard::takeUsageError();
    gFrameOpen = false;
    trace("frame discarded");
  }
  guard::beginFrame();
  platformNewFrame(gWindow);
  c.enterImGui();
  ImGui::NewFrame();
  gFrameOpen = true;
  // Like every bound function. NewFrame()'s own body (imgui.cpp:5867-6174)
  // has no IM_ASSERT_USER_ERROR; should one be reached through a function
  // it calls, the frame is open and the error is thrown.
  if (!c.leaveImGui())
    return nullptr;
  return c.result(true);
}

/// `render()`: end the frame, then draw it into this tick's target if
/// there is one. Returns whether it drew: false outside a frame callback
/// and in CLOSING, where hnui_frame_target has no target to give. A usage
/// error ImGui reports while ending the frame takes precedence: the frame
/// is closed, no target is taken, nothing is drawn, and it throws.
napi_value render(napi_env env, napi_callback_info info) {
  bind::Call c(env, info, "render");
  if (!c.ok())
    return nullptr;
  if (bind::insideCallback())
    return c.throwUsage("not allowed inside a list clipper's callback");
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Render();
  gFrameOpen = false;
  // Before hnui_frame_target: taking the target is what tells the host the
  // tick drew.
  if (!c.leaveImGui())
    return nullptr;
  hnui_target target = {};
  target.size = static_cast<uint32_t>(sizeof(target));
  if (!hnui_frame_target(gWindow, &target))
    return c.result(false);
  gLastTarget = target;
  rendererDraw(ImGui::GetDrawData(), target);
  platformAfterRender(gWindow);
  return c.result(true);
}

} // namespace

bool frameOpen() {
  return gFrameOpen;
}

const hnui_target &lastTarget() {
  return gLastTarget;
}

bool ensureContext(napi_env env) {
  // Asked every time, as part A's newFrame() did, so that a runtime with
  // no window gets false whether or not the context exists.
  hnui_window *w = hnui_get_window(env);
  if (w == nullptr)
    return false;
  if (gWindow != nullptr)
    return true;
  createContext(w);
  guard::configureContext();
  return true;
}

napi_value throwNoFrame(napi_env env, const char *name) {
  char message[160];
  std::snprintf(
      message,
      sizeof(message),
      "imgui.%s(): no frame is open; call imgui.newFrame() first",
      name);
  napi_throw_error(env, "ERR_IMGUI_NO_FRAME", message);
  return nullptr;
}

} // namespace hnimgui

NAPI_MODULE_INIT() {
  // First, before anything touches the host: an addon built against a
  // different hnui ABI or Sokol must fail to load rather than run.
  HNUI_CHECK_VERSION(env);

  napi_value version = nullptr;
  if (napi_create_string_utf8(env, IMGUI_VERSION, NAPI_AUTO_LENGTH, &version) !=
          napi_ok ||
      napi_set_named_property(env, exports, "version", version) != napi_ok)
    return nullptr;
  namespace bind = hnimgui::bind;
  if (!bind::registerFunction(env, exports, "newFrame", hnimgui::newFrame) ||
      !bind::registerFunction(env, exports, "render", hnimgui::render) ||
      !bind::registerConstants(env, exports) ||
      !bind::registerFrame(env, exports) ||
      !bind::registerWindows(env, exports) ||
      !bind::registerLayout(env, exports) ||
      !bind::registerText(env, exports) ||
      !bind::registerWidgets(env, exports) ||
      !bind::registerTreesTabs(env, exports) ||
      !bind::registerMenus(env, exports) ||
      !bind::registerTables(env, exports) ||
      !bind::registerPopups(env, exports) ||
      !bind::registerQueries(env, exports) ||
      !bind::registerStyle(env, exports) ||
      !bind::registerPlots(env, exports) ||
      !bind::registerDrawList(env, exports) ||
      !bind::registerClipper(env, exports) ||
      !bind::registerDocking(env, exports))
    return nullptr;
  if (hnimgui::testHooksEnabled()) {
    napi_value hooks = hnimgui::createTestHooks(env);
    if (hooks == nullptr ||
        napi_set_named_property(env, exports, "__test", hooks) != napi_ok)
      return nullptr;
  }

  hnimgui::trace("addon loaded (imgui %s)", IMGUI_VERSION);
  return exports;
}
