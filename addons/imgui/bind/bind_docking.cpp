/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Docking and settings group (surface table, "Docking and settings").

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `dockSpaceOverViewport(dockspaceId = 0, flags = 0)`, imgui.h:1002, with
/// `viewport` and `window_class` NULL: a dock space filling the main
/// viewport. Returns its ID.
napi_value dockSpaceOverViewport(napi_env env, napi_callback_info info) {
  Call c(env, info, "dockSpaceOverViewport");
  ImU32 id = 0;
  int flags = 0;
  if (!c.ok() || (!c.isMissing(0) && !c.u32(0, "dockspaceId", &id)) ||
      (!c.isMissing(1) && !c.flags(1, "flags", kDockNodeFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canDockSpaceOverViewport(id, flags))
    return c.throwUsage("%s", msg);
  c.enterImGui();
  const ImGuiID result =
      ImGui::DockSpaceOverViewport(id, nullptr, flags, nullptr);
  if (!c.leaveImGui())
    return nullptr;
  return c.resultU32(result);
}

/// `loadIniSettingsFromMemory(iniData)`, imgui.h:1182, with `ini_size` the
/// encoded length. Outside a frame only (design, "Calling convention"):
/// it rebuilds the dock nodes the frame's dock space is using.
napi_value loadIniSettingsFromMemory(napi_env env, napi_callback_info info) {
  Call c(env, info, "loadIniSettingsFromMemory");
  std::string data;
  if (!c.ok() || !c.string(0, "iniData", &data) || !c.needContext() ||
      !c.needNoFrame())
    return nullptr;
  c.enterImGui();
  // With ini_size 0 ImGui measures the string itself (imgui.cpp:16620-
  // 16621), which for "" is the same 0.
  ImGui::LoadIniSettingsFromMemory(data.c_str(), data.size());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `saveIniSettingsToMemory()`, imgui.h:1184: the settings as a string.
/// Inside or outside a frame.
napi_value saveIniSettingsToMemory(napi_env env, napi_callback_info info) {
  Call c(env, info, "saveIniSettingsToMemory");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  size_t size = 0;
  const char *data = ImGui::SaveIniSettingsToMemory(&size);
  if (!c.leaveImGui())
    return nullptr;
  napi_value out = nullptr;
  if (!c.napiOk(
          napi_create_string_utf8(env, data, size, &out),
          "returning",
          "the settings"))
    return nullptr;
  return out;
}

} // namespace

bool registerDocking(napi_env env, napi_value exports) {
  return registerFunction(
             env, exports, "dockSpaceOverViewport", dockSpaceOverViewport) &&
      registerFunction(
             env,
             exports,
             "loadIniSettingsFromMemory",
             loadIniSettingsFromMemory) &&
      registerFunction(
             env, exports, "saveIniSettingsToMemory", saveIniSettingsToMemory);
}

} // namespace bind
} // namespace hnimgui
