/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Frame group (surface table, "Frame"), except newFrame() and
// render(), which are the frame protocol and live in imgui_module.cpp, and
// `version`, a property set there.

#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `showDemoWindow(pOpen = null)`, imgui.h:423.
napi_value showDemoWindow(napi_env env, napi_callback_info info) {
  Call c(env, info, "showDemoWindow");
  RefBool pOpen;
  if (!c.ok() || !c.refBool(0, "pOpen", true, &pOpen) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::ShowDemoWindow(pOpen.ptr());
  if (!c.leaveImGui() || !c.write(pOpen))
    return nullptr;
  return c.undefined();
}

/// `showMetricsWindow(pOpen = null)`, imgui.h:424.
napi_value showMetricsWindow(napi_env env, napi_callback_info info) {
  Call c(env, info, "showMetricsWindow");
  RefBool pOpen;
  if (!c.ok() || !c.refBool(0, "pOpen", true, &pOpen) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::ShowMetricsWindow(pOpen.ptr());
  if (!c.leaveImGui() || !c.write(pOpen))
    return nullptr;
  return c.undefined();
}

/// `getTime()`, imgui.h:1090. Frame: either.
napi_value getTime(napi_env env, napi_callback_info info) {
  Call c(env, info, "getTime");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  const double t = ImGui::GetTime();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(t);
}

/// `getFrameRate()`: io.Framerate, imgui.h:2725. Frame: either.
napi_value getFrameRate(napi_env env, napi_callback_info info) {
  Call c(env, info, "getFrameRate");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetIO().Framerate;
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `getDeltaTime()`: io.DeltaTime, imgui.h:2551. Frame: either.
napi_value getDeltaTime(napi_env env, napi_callback_info info) {
  Call c(env, info, "getDeltaTime");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetIO().DeltaTime;
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `getDisplaySize(out?)`: io.DisplaySize, imgui.h:2549. Frame: either.
napi_value getDisplaySize(napi_env env, napi_callback_info info) {
  Call c(env, info, "getDisplaySize");
  if (!c.ok() || !c.outArray(0, "out") || !c.needContext())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetIO().DisplaySize;
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `wantCaptureMouse()`: io.WantCaptureMouse, imgui.h:2718. Frame: either.
napi_value wantCaptureMouse(napi_env env, napi_callback_info info) {
  Call c(env, info, "wantCaptureMouse");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::GetIO().WantCaptureMouse;
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `wantCaptureKeyboard()`: io.WantCaptureKeyboard, imgui.h:2719. Frame:
/// either.
napi_value wantCaptureKeyboard(napi_env env, napi_callback_info info) {
  Call c(env, info, "wantCaptureKeyboard");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::GetIO().WantCaptureKeyboard;
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

} // namespace

bool registerFrame(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "showDemoWindow", showDemoWindow) &&
      registerFunction(env, exports, "showMetricsWindow", showMetricsWindow) &&
      registerFunction(env, exports, "getTime", getTime) &&
      registerFunction(env, exports, "getFrameRate", getFrameRate) &&
      registerFunction(env, exports, "getDeltaTime", getDeltaTime) &&
      registerFunction(env, exports, "getDisplaySize", getDisplaySize) &&
      registerFunction(env, exports, "wantCaptureMouse", wantCaptureMouse) &&
      registerFunction(
             env, exports, "wantCaptureKeyboard", wantCaptureKeyboard);
}

} // namespace bind
} // namespace hnimgui
