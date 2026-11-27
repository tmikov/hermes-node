/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Windows group (surface table, "Windows"). Task 1 binds the part the
// core's tests need; Task 3 completes it.

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `begin(name, pOpen = null, flags = 0)`, imgui.h:451.
napi_value begin(napi_env env, napi_callback_info info) {
  Call c(env, info, "begin");
  std::string name;
  RefBool pOpen;
  int flags = 0;
  if (!c.ok() || !c.string(0, "name", &name) ||
      !c.refBool(1, "pOpen", true, &pOpen))
    return nullptr;
  // Task 2: EnumInfo
  if (!c.isMissing(2) && !c.int32(2, "flags", &flags))
    return nullptr;
  // imgui.cpp:8003 asserts plainly on an empty name.
  if (name.empty())
    return c.throwType("ERR_INVALID_ARG_VALUE", "name must not be empty");
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::Begin(name.c_str(), pOpen.ptr(), flags);
  if (!c.leaveImGui() || !c.write(pOpen))
    return nullptr;
  return c.result(shown);
}

/// `end()`, imgui.h:452.
napi_value end(napi_env env, napi_callback_info info) {
  Call c(env, info, "end");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::End();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginChild(strId, size = [0, 0], childFlags = 0, windowFlags = 0)`,
/// imgui.h:472.
napi_value beginChild(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginChild");
  std::string strId;
  ImVec2 size(0.0f, 0.0f);
  int childFlags = 0;
  int windowFlags = 0;
  if (!c.ok() || !c.string(0, "strId", &strId) ||
      (!c.isMissing(1) && !c.vec2(1, "size", &size)))
    return nullptr;
  // Task 2: EnumInfo
  if ((!c.isMissing(2) && !c.int32(2, "childFlags", &childFlags)) ||
      (!c.isMissing(3) && !c.int32(3, "windowFlags", &windowFlags)))
    return nullptr;
  // BeginChildEx's plain assertions (imgui.cpp:6904, 6907, 6908).
  if (windowFlags & ImGuiWindowFlags_AlwaysAutoResize)
    return c.throwRange(
        "windowFlags must not hold WindowFlags_AlwaysAutoResize (use "
        "ChildFlags_AlwaysAutoResize)");
  if (childFlags & ImGuiChildFlags_AlwaysAutoResize) {
    if (childFlags & (ImGuiChildFlags_ResizeX | ImGuiChildFlags_ResizeY))
      return c.throwRange(
          "childFlags: ChildFlags_AlwaysAutoResize cannot be combined with "
          "ChildFlags_ResizeX or ChildFlags_ResizeY");
    if ((childFlags &
         (ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY)) == 0)
      return c.throwRange(
          "childFlags: ChildFlags_AlwaysAutoResize needs "
          "ChildFlags_AutoResizeX or ChildFlags_AutoResizeY");
  }
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown =
      ImGui::BeginChild(strId.c_str(), size, childFlags, windowFlags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endChild()`, imgui.h:474.
napi_value endChild(napi_env env, napi_callback_info info) {
  Call c(env, info, "endChild");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndChild())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndChild();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setNextWindowPos(pos, cond = 0, pivot = [0, 0])`, imgui.h:492.
napi_value setNextWindowPos(napi_env env, napi_callback_info info) {
  Call c(env, info, "setNextWindowPos");
  ImVec2 pos;
  int cond = 0;
  ImVec2 pivot(0.0f, 0.0f);
  if (!c.ok() || !c.vec2(0, "pos", &pos))
    return nullptr;
  // Task 2: EnumInfo
  if ((!c.isMissing(1) && !c.int32(1, "cond", &cond)) ||
      (!c.isMissing(2) && !c.vec2(2, "pivot", &pivot)) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetNextWindowPos(pos, cond, pivot);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setNextWindowSize(size, cond = 0)`, imgui.h:493.
napi_value setNextWindowSize(napi_env env, napi_callback_info info) {
  Call c(env, info, "setNextWindowSize");
  ImVec2 size;
  int cond = 0;
  if (!c.ok() || !c.vec2(0, "size", &size))
    return nullptr;
  // Task 2: EnumInfo
  if ((!c.isMissing(1) && !c.int32(1, "cond", &cond)) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetNextWindowSize(size, cond);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerWindows(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "begin", begin) &&
      registerFunction(env, exports, "end", end) &&
      registerFunction(env, exports, "beginChild", beginChild) &&
      registerFunction(env, exports, "endChild", endChild) &&
      registerFunction(env, exports, "setNextWindowPos", setNextWindowPos) &&
      registerFunction(env, exports, "setNextWindowSize", setNextWindowSize);
}

} // namespace bind
} // namespace hnimgui
