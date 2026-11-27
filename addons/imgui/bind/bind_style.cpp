/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Style group (surface table, "Style").

#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `styleColorsDark()`, imgui.h:435, with `dst` NULL: the current
/// context's style. Inside or outside a frame.
napi_value styleColorsDark(napi_env env, napi_callback_info info) {
  Call c(env, info, "styleColorsDark");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  ImGui::StyleColorsDark();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `styleColorsLight()`, imgui.h:436, as styleColorsDark().
napi_value styleColorsLight(napi_env env, napi_callback_info info) {
  Call c(env, info, "styleColorsLight");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  ImGui::StyleColorsLight();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushStyleColor(idx, col)`, dispatched on `col`: an Array is the
/// ImVec4 overload (imgui.h:548), a number the ImU32 one (imgui.h:547).
napi_value pushStyleColor(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushStyleColor");
  int idx = 0;
  // An exported Col_ value, then not Col_COUNT, which would index past
  // Style.Colors[] (imgui.cpp:3801, 3812).
  if (!c.ok() || !c.enumValue(0, "idx", kCol, &idx))
    return nullptr;
  if (idx == ImGuiCol_COUNT)
    return c.throwRange("idx must not be Col_COUNT");
  if (c.isArray(1)) {
    ImVec4 col;
    if (!c.vec4(1, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleColor(idx, col);
  } else {
    if (c.typeOf(1) != napi_number)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE",
          "col must be a number or an array of 4 numbers");
    ImU32 col = 0;
    if (!c.u32(1, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleColor(idx, col);
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popStyleColor(count = 1)`, imgui.h:549.
napi_value popStyleColor(napi_env env, napi_callback_info info) {
  Call c(env, info, "popStyleColor");
  int count = 1;
  if (!c.ok() || (!c.isMissing(0) && !c.int32(0, "count", &count)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopStyleColor(count);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushStyleVar(idx, val)`, dispatched on `val`: an Array is the ImVec2
/// overload (imgui.h:551), a number the float one (imgui.h:550).
napi_value pushStyleVar(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushStyleVar");
  int idx = 0;
  // An exported StyleVar_ value, then not StyleVar_COUNT, which
  // GetStyleVarInfo asserts against (imgui.cpp:3891).
  if (!c.ok() || !c.enumValue(0, "idx", kStyleVar, &idx))
    return nullptr;
  if (idx == ImGuiStyleVar_COUNT)
    return c.throwRange("idx must not be StyleVar_COUNT");
  if (c.isArray(1)) {
    ImVec2 val;
    if (!c.vec2(1, "val", &val) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleVar(idx, val);
  } else {
    if (c.typeOf(1) != napi_number)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE",
          "val must be a number or an array of 2 numbers");
    float val = 0.0f;
    if (!c.f32(1, "val", &val) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleVar(idx, val);
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popStyleVar(count = 1)`, imgui.h:554.
napi_value popStyleVar(napi_env env, napi_callback_info info) {
  Call c(env, info, "popStyleVar");
  int count = 1;
  if (!c.ok() || (!c.isMissing(0) && !c.int32(0, "count", &count)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopStyleVar(count);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `getColorU32(idx, alphaMul = 1)` or `getColorU32(col)`, dispatched on
/// the first argument: an Array is the ImVec4 overload (imgui.h:570), a
/// number the ImGuiCol one (imgui.h:569). The ImU32 overload (imgui.h:571)
/// is not bound.
napi_value getColorU32(napi_env env, napi_callback_info info) {
  Call c(env, info, "getColorU32");
  if (!c.ok())
    return nullptr;
  ImU32 result = 0;
  if (c.isArray(0)) {
    ImVec4 col;
    if (!c.vec4(0, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    result = ImGui::GetColorU32(col);
  } else {
    if (c.typeOf(0) != napi_number)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE",
          "idx must be a number, or col an array of 4 numbers");
    int idx = 0;
    float alphaMul = 1.0f;
    // Not Col_COUNT, which would index past Style.Colors[]
    // (imgui.cpp:3765).
    if (!c.enumValue(0, "idx", kCol, &idx))
      return nullptr;
    if (idx == ImGuiCol_COUNT)
      return c.throwRange("idx must not be Col_COUNT");
    if ((!c.isMissing(1) && !c.f32(1, "alphaMul", &alphaMul)) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    result = ImGui::GetColorU32(idx, alphaMul);
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.resultU32(result);
}

/// `colorConvertFloat4ToU32(col)`, imgui.h:1102: a pure function reading
/// no context (imgui.cpp:3003-3011), so it needs neither a frame nor a
/// window.
napi_value colorConvertFloat4ToU32(napi_env env, napi_callback_info info) {
  Call c(env, info, "colorConvertFloat4ToU32");
  ImVec4 col;
  if (!c.ok() || !c.vec4(0, "col", &col))
    return nullptr;
  c.enterImGui();
  const ImU32 result = ImGui::ColorConvertFloat4ToU32(col);
  if (!c.leaveImGui())
    return nullptr;
  return c.resultU32(result);
}

} // namespace

bool registerStyle(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "styleColorsDark", styleColorsDark) &&
      registerFunction(env, exports, "styleColorsLight", styleColorsLight) &&
      registerFunction(env, exports, "pushStyleColor", pushStyleColor) &&
      registerFunction(env, exports, "popStyleColor", popStyleColor) &&
      registerFunction(env, exports, "pushStyleVar", pushStyleVar) &&
      registerFunction(env, exports, "popStyleVar", popStyleVar) &&
      registerFunction(env, exports, "getColorU32", getColorU32) &&
      registerFunction(
             env, exports, "colorConvertFloat4ToU32", colorConvertFloat4ToU32);
}

} // namespace bind
} // namespace hnimgui
