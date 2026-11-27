/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Layout and IDs groups (surface table, "Layout" and "IDs"). Task 1
// binds the part the core's tests need; Task 3 completes them.

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `beginGroup()`, imgui.h:603.
napi_value beginGroup(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginGroup");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::BeginGroup();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `endGroup()`, imgui.h:604.
napi_value endGroup(napi_env env, napi_callback_info info) {
  Call c(env, info, "endGroup");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndGroup())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndGroup();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setCursorScreenPos(pos)`, imgui.h:585.
napi_value setCursorScreenPos(napi_env env, napi_callback_info info) {
  Call c(env, info, "setCursorScreenPos");
  ImVec2 pos;
  if (!c.ok() || !c.vec2(0, "pos", &pos) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetCursorScreenPos(pos);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushID(strId)` / `pushID(intId)`, dispatched on the argument's type:
/// imgui.h:622 and 625.
napi_value pushID(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushID");
  if (!c.ok())
    return nullptr;
  if (c.typeOf(0) == napi_number) {
    int id = 0;
    if (!c.int32(0, "intId", &id) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushID(id);
  } else {
    if (c.typeOf(0) != napi_string)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE", "strId must be a string, or intId a number");
    std::string id;
    if (!c.string(0, "strId", &id) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushID(id.c_str());
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popID()`, imgui.h:626.
napi_value popID(napi_env env, napi_callback_info info) {
  Call c(env, info, "popID");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopID();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerLayout(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "beginGroup", beginGroup) &&
      registerFunction(env, exports, "endGroup", endGroup) &&
      registerFunction(
             env, exports, "setCursorScreenPos", setCursorScreenPos) &&
      registerFunction(env, exports, "pushID", pushID) &&
      registerFunction(env, exports, "popID", popID);
}

} // namespace bind
} // namespace hnimgui
