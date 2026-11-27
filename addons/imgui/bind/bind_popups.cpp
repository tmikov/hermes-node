/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Popups and tooltips group (surface table, "Popups and tooltips").

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `openPopup(strId, popupFlags = 0)`, imgui.h:878. The ImGuiID overload
/// (imgui.h:879) is not bound.
napi_value openPopup(napi_env env, napi_callback_info info) {
  Call c(env, info, "openPopup");
  std::string strId;
  int flags = 0;
  if (!c.ok() || !c.string(0, "strId", &strId) ||
      (!c.isMissing(1) && !c.flags(1, "popupFlags", kPopupFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool opened = ImGui::OpenPopup(strId.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(opened);
}

/// `beginPopup(strId, flags = 0)`, imgui.h:865.
napi_value beginPopup(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginPopup");
  std::string strId;
  int flags = 0;
  if (!c.ok() || !c.string(0, "strId", &strId) ||
      (!c.isMissing(1) && !c.flags(1, "flags", kWindowFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::BeginPopup(strId.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `beginPopupModal(name, pOpen = null, flags = 0)`, imgui.h:866. pOpen
/// is written back whatever it returned: BeginPopupModal sets it to false
/// when the popup is not open (imgui.cpp:13495-13496).
napi_value beginPopupModal(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginPopupModal");
  std::string name;
  RefBool pOpen;
  int flags = 0;
  if (!c.ok() || !c.string(0, "name", &name) ||
      !c.refBool(1, "pOpen", true, &pOpen) ||
      (!c.isMissing(2) && !c.flags(2, "flags", kWindowFlags, &flags)))
    return nullptr;
  // The name reaches Begin(), which asserts plainly that it is not empty
  // (imgui.cpp:13510, 8003).
  if (name.empty())
    return c.throwType("ERR_INVALID_ARG_VALUE", "name must not be empty");
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::BeginPopupModal(name.c_str(), pOpen.ptr(), flags);
  if (!c.leaveImGui() || !c.write(pOpen))
    return nullptr;
  return c.result(shown);
}

/// `beginPopupContextItem(strId = null, popupFlags = 0)`, imgui.h:892.
napi_value beginPopupContextItem(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginPopupContextItem");
  std::string strId;
  bool noStrId = true;
  int flags = 0;
  if (!c.ok() || !c.stringOrNull(0, "strId", &strId, &noStrId) ||
      (!c.isMissing(1) && !c.flags(1, "popupFlags", kPopupFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canBeginPopupContextItem(!noStrId))
    return c.throwUsage("%s", msg);
  c.enterImGui();
  const bool shown =
      ImGui::BeginPopupContextItem(noStrId ? nullptr : strId.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endPopup()`, imgui.h:867. In a window that is not a popup ImGui
/// reports it recoverably (imgui.cpp:13525).
napi_value endPopup(napi_env env, napi_callback_info info) {
  Call c(env, info, "endPopup");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::EndPopup();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `closeCurrentPopup()`, imgui.h:881.
napi_value closeCurrentPopup(napi_env env, napi_callback_info info) {
  Call c(env, info, "closeCurrentPopup");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::CloseCurrentPopup();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setTooltip(text)`, imgui.h:844, called as `SetTooltip("%s", text)`.
napi_value setTooltip(napi_env env, napi_callback_info info) {
  Call c(env, info, "setTooltip");
  std::string text;
  if (!c.ok() || !c.string(0, "text", &text) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetTooltip("%s", text.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setItemTooltip(text)`, imgui.h:852, called as
/// `SetItemTooltip("%s", text)`.
napi_value setItemTooltip(napi_env env, napi_callback_info info) {
  Call c(env, info, "setItemTooltip");
  std::string text;
  if (!c.ok() || !c.string(0, "text", &text) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetItemTooltip("%s", text.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginTooltip()`, imgui.h:842.
napi_value beginTooltip(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginTooltip");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::BeginTooltip();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endTooltip()`, imgui.h:843.
napi_value endTooltip(napi_env env, napi_callback_info info) {
  Call c(env, info, "endTooltip");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndTooltip())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndTooltip();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerPopups(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "openPopup", openPopup) &&
      registerFunction(env, exports, "beginPopup", beginPopup) &&
      registerFunction(env, exports, "beginPopupModal", beginPopupModal) &&
      registerFunction(
             env, exports, "beginPopupContextItem", beginPopupContextItem) &&
      registerFunction(env, exports, "endPopup", endPopup) &&
      registerFunction(env, exports, "closeCurrentPopup", closeCurrentPopup) &&
      registerFunction(env, exports, "setTooltip", setTooltip) &&
      registerFunction(env, exports, "setItemTooltip", setItemTooltip) &&
      registerFunction(env, exports, "beginTooltip", beginTooltip) &&
      registerFunction(env, exports, "endTooltip", endTooltip);
}

} // namespace bind
} // namespace hnimgui
