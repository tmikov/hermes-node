/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Style group (surface table, "Style"). Task 1 binds the push and pop
// functions the core's tests need; Task 7 completes it.

#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `pushStyleColor(idx, col)`, dispatched on `col`: an Array is the
/// ImVec4 overload (imgui.h:548), a number the ImU32 one (imgui.h:547).
napi_value pushStyleColor(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushStyleColor");
  int idx = 0;
  // Task 2: EnumInfo. Until then the range is the one Style.Colors[]
  // indexing needs, Col_COUNT excluded (imgui.cpp:3801, 3812).
  if (!c.ok() || !c.intIn(0, "idx", 0, ImGuiCol_COUNT - 1, &idx))
    return nullptr;
  if (c.isArray(1)) {
    ImVec4 col;
    if (!c.vec4(1, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleColor(idx, col);
  } else {
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
  // Task 2: EnumInfo. Until then the range GetStyleVarInfo asserts
  // (imgui.cpp:3891), StyleVar_COUNT excluded.
  if (!c.ok() || !c.intIn(0, "idx", 0, ImGuiStyleVar_COUNT - 1, &idx))
    return nullptr;
  if (c.isArray(1)) {
    ImVec2 val;
    if (!c.vec2(1, "val", &val) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleVar(idx, val);
  } else {
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

} // namespace

bool registerStyle(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "pushStyleColor", pushStyleColor) &&
      registerFunction(env, exports, "popStyleColor", popStyleColor) &&
      registerFunction(env, exports, "pushStyleVar", pushStyleVar) &&
      registerFunction(env, exports, "popStyleVar", popStyleVar);
}

} // namespace bind
} // namespace hnimgui
