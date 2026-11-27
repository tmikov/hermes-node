/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Menus group (surface table, "Menus").

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `beginMainMenuBar()`, imgui.h:831. A second call before
/// endMainMenuBar() is not guarded (surface table, "Not covered").
napi_value beginMainMenuBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginMainMenuBar");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::BeginMainMenuBar();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endMainMenuBar()`, imgui.h:832. Outside a menu bar ImGui reports it
/// recoverably (widgets:9483).
napi_value endMainMenuBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "endMainMenuBar");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::EndMainMenuBar();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginMenuBar()`, imgui.h:829.
napi_value beginMenuBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginMenuBar");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canBeginMenuBar())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  const bool shown = ImGui::BeginMenuBar();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endMenuBar()`, imgui.h:830.
napi_value endMenuBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "endMenuBar");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndMenuBar())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndMenuBar();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginMenu(label, enabled = true)`, imgui.h:833. Label-id: BeginMenuEx
/// submits `Selectable("")` under `PushID(label)` (widgets:9601).
napi_value beginMenu(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginMenu");
  std::string label;
  bool enabled = true;
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(1) && !c.boolean(1, "enabled", &enabled)) ||
      !c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool open = ImGui::BeginMenu(label.c_str(), enabled);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(open);
}

/// `endMenu()`, imgui.h:834. In a window that is not a menu ImGui reports
/// it recoverably (widgets:9755).
napi_value endMenu(napi_env env, napi_callback_info info) {
  Call c(env, info, "endMenu");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::EndMenu();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `menuItem(label, shortcut = null, selected = false, enabled = true)`
/// (imgui.h:835) or `menuItem(label, shortcut, pSelected, enabled = true)`
/// (imgui.h:836), dispatched on the third argument: a boolean, undefined
/// or null is the first form, an Array the second. Label-id: MenuItemEx
/// submits `Selectable("")` under `PushID(label)` (widgets:9805).
napi_value menuItem(napi_env env, napi_callback_info info) {
  Call c(env, info, "menuItem");
  std::string label;
  std::string shortcut;
  bool noShortcut = true;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.stringOrNull(1, "shortcut", &shortcut, &noShortcut))
    return nullptr;
  const bool byRef = c.isArray(2);
  bool selected = false;
  RefBool pSelected;
  if (byRef) {
    if (!c.refBool(2, "pSelected", false, &pSelected))
      return nullptr;
  } else if (c.isMissing(2) || c.isNull(2)) {
    selected = false;
  } else if (c.typeOf(2) == napi_boolean) {
    if (!c.boolean(2, "selected", &selected))
      return nullptr;
  } else {
    return c.throwType(
        "ERR_INVALID_ARG_TYPE",
        "selected must be a boolean, or pSelected an Array");
  }
  bool enabled = true;
  if ((!c.isMissing(3) && !c.boolean(3, "enabled", &enabled)) ||
      !c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  const char *sc = noShortcut ? nullptr : shortcut.c_str();
  c.enterImGui();
  const bool activated = byRef
      ? ImGui::MenuItem(label.c_str(), sc, pSelected.ptr(), enabled)
      : ImGui::MenuItem(label.c_str(), sc, selected, enabled);
  if (!c.leaveImGui() || !c.write(pSelected))
    return nullptr;
  return c.result(activated);
}

} // namespace

bool registerMenus(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "beginMainMenuBar", beginMainMenuBar) &&
      registerFunction(env, exports, "endMainMenuBar", endMainMenuBar) &&
      registerFunction(env, exports, "beginMenuBar", beginMenuBar) &&
      registerFunction(env, exports, "endMenuBar", endMenuBar) &&
      registerFunction(env, exports, "beginMenu", beginMenu) &&
      registerFunction(env, exports, "endMenu", endMenu) &&
      registerFunction(env, exports, "menuItem", menuItem);
}

} // namespace bind
} // namespace hnimgui
