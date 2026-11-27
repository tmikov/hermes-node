/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Windows group (surface table, "Windows").

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
  if (!c.isMissing(2) && !c.flags(2, "flags", kWindowFlags, &flags))
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
  if ((!c.isMissing(2) &&
       !c.flags(2, "childFlags", kChildFlags, &childFlags)) ||
      (!c.isMissing(3) &&
       !c.flags(3, "windowFlags", kWindowFlags, &windowFlags)))
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
  if ((!c.isMissing(1) && !c.enumValue(1, "cond", kCond, &cond)) ||
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
  if ((!c.isMissing(1) && !c.enumValue(1, "cond", kCond, &cond)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetNextWindowSize(size, cond);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setNextWindowBgAlpha(alpha)`, imgui.h:499.
napi_value setNextWindowBgAlpha(napi_env env, napi_callback_info info) {
  Call c(env, info, "setNextWindowBgAlpha");
  float alpha = 0.0f;
  if (!c.ok() || !c.f32(0, "alpha", &alpha) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetNextWindowBgAlpha(alpha);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `getWindowPos(out?)`, imgui.h:484.
napi_value getWindowPos(napi_env env, napi_callback_info info) {
  Call c(env, info, "getWindowPos");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetWindowPos();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `getWindowSize(out?)`, imgui.h:485.
napi_value getWindowSize(napi_env env, napi_callback_info info) {
  Call c(env, info, "getWindowSize");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetWindowSize();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `getContentRegionAvail(out?)`, imgui.h:586.
napi_value getContentRegionAvail(napi_env env, napi_callback_info info) {
  Call c(env, info, "getContentRegionAvail");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetContentRegionAvail();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `isWindowFocused(flags = 0)`, imgui.h:480.
napi_value isWindowFocused(napi_env env, napi_callback_info info) {
  Call c(env, info, "isWindowFocused");
  int flags = 0;
  if (!c.ok() ||
      (!c.isMissing(0) && !c.flags(0, "flags", kFocusedFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool focused = ImGui::IsWindowFocused(flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(focused);
}

/// `isWindowHovered(flags = 0)`, imgui.h:481. Flags outside
/// ImGuiHoveredFlags_AllowedMaskForIsWindowHovered are ImGui's own
/// recoverable error (imgui.cpp:9225), thrown by leaveImGui().
napi_value isWindowHovered(napi_env env, napi_callback_info info) {
  Call c(env, info, "isWindowHovered");
  int flags = 0;
  if (!c.ok() ||
      (!c.isMissing(0) && !c.flags(0, "flags", kHoveredFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool hovered = ImGui::IsWindowHovered(flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(hovered);
}

/// `getScrollY()`, imgui.h:514.
napi_value getScrollY(napi_env env, napi_callback_info info) {
  Call c(env, info, "getScrollY");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetScrollY();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `getScrollMaxY()`, imgui.h:518.
napi_value getScrollMaxY(napi_env env, napi_callback_info info) {
  Call c(env, info, "getScrollMaxY");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetScrollMaxY();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `setScrollHereY(centerYRatio = 0.5)`, imgui.h:520.
napi_value setScrollHereY(napi_env env, napi_callback_info info) {
  Call c(env, info, "setScrollHereY");
  float ratio = 0.5f;
  if (!c.ok() || (!c.isMissing(0) && !c.f32(0, "centerYRatio", &ratio)))
    return nullptr;
  // SetScrollFromPosY asserts plainly on the ratio (imgui.cpp:12979); NaN
  // is already refused by f32().
  if (!(ratio >= 0.0f && ratio <= 1.0f))
    return c.throwRange("centerYRatio must be from 0 to 1; got %g", ratio);
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetScrollHereY(ratio);
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
      registerFunction(env, exports, "setNextWindowSize", setNextWindowSize) &&
      registerFunction(
             env, exports, "setNextWindowBgAlpha", setNextWindowBgAlpha) &&
      registerFunction(env, exports, "getWindowPos", getWindowPos) &&
      registerFunction(env, exports, "getWindowSize", getWindowSize) &&
      registerFunction(
             env, exports, "getContentRegionAvail", getContentRegionAvail) &&
      registerFunction(env, exports, "isWindowFocused", isWindowFocused) &&
      registerFunction(env, exports, "isWindowHovered", isWindowHovered) &&
      registerFunction(env, exports, "getScrollY", getScrollY) &&
      registerFunction(env, exports, "getScrollMaxY", getScrollMaxY) &&
      registerFunction(env, exports, "setScrollHereY", setScrollHereY);
}

} // namespace bind
} // namespace hnimgui
