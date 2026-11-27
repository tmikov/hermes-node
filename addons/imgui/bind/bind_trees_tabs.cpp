/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Trees and tabs group (surface table, "Trees and tabs").

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `treeNode(label)`, imgui.h:760.
napi_value treeNode(napi_env env, napi_callback_info info) {
  Call c(env, info, "treeNode");
  std::string label;
  if (!c.ok() || !c.string(0, "label", &label) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool open = ImGui::TreeNode(label.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.result(open);
}

/// `treeNodeEx(label, flags = 0)`, imgui.h:765.
napi_value treeNodeEx(napi_env env, napi_callback_info info) {
  Call c(env, info, "treeNodeEx");
  std::string label;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(1) && !c.flags(1, "flags", kTreeNodeFlags, &flags)) ||
      !c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(open);
}

/// `treePop()`, imgui.h:772.
napi_value treePop(napi_env env, napi_callback_info info) {
  Call c(env, info, "treePop");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTreePop())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::TreePop();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `collapsingHeader(label, flags = 0)` (imgui.h:774) or
/// `collapsingHeader(label, pVisible, flags = 0)` (imgui.h:775),
/// dispatched on the second argument: a number or undefined is the first
/// form, an Array or null the second (null passing NULL, which C++
/// accepts, widgets:7479).
napi_value collapsingHeader(napi_env env, napi_callback_info info) {
  Call c(env, info, "collapsingHeader");
  std::string label;
  if (!c.ok() || !c.string(0, "label", &label))
    return nullptr;
  const bool second = c.isArray(1) || c.isNull(1);
  if (!second && !c.isMissing(1) && c.typeOf(1) != napi_number)
    return c.throwType(
        "ERR_INVALID_ARG_TYPE",
        "flags must be a number, or pVisible an Array or null");
  RefBool pVisible;
  int flags = 0;
  if (second) {
    if (!c.refBool(1, "pVisible", true, &pVisible) ||
        (!c.isMissing(2) && !c.flags(2, "flags", kTreeNodeFlags, &flags)))
      return nullptr;
  } else if (!c.isMissing(1) && !c.flags(1, "flags", kTreeNodeFlags, &flags)) {
    return nullptr;
  }
  if (!c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool open = second
      ? ImGui::CollapsingHeader(label.c_str(), pVisible.ptr(), flags)
      : ImGui::CollapsingHeader(label.c_str(), flags);
  if (!c.leaveImGui() || !c.write(pVisible))
    return nullptr;
  return c.result(open);
}

/// `setNextItemOpen(isOpen, cond = 0)`, imgui.h:776.
napi_value setNextItemOpen(napi_env env, napi_callback_info info) {
  Call c(env, info, "setNextItemOpen");
  bool isOpen = false;
  int cond = 0;
  if (!c.ok() || !c.boolean(0, "isOpen", &isOpen) ||
      (!c.isMissing(1) && !c.enumValue(1, "cond", kCond, &cond)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetNextItemOpen(isOpen, cond);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginTabBar(strId, flags = 0)`, imgui.h:973.
napi_value beginTabBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginTabBar");
  std::string strId;
  int flags = 0;
  if (!c.ok() || !c.string(0, "strId", &strId) ||
      (!c.isMissing(1) && !c.flags(1, "flags", kTabBarFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::BeginTabBar(strId.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endTabBar()`, imgui.h:974. Outside a tab bar ImGui reports it
/// recoverably (widgets:10068).
napi_value endTabBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "endTabBar");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::EndTabBar();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginTabItem(label, pOpen = null, flags = 0)`, imgui.h:975.
napi_value beginTabItem(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginTabItem");
  std::string label;
  RefBool pOpen;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refBool(1, "pOpen", true, &pOpen) ||
      (!c.isMissing(2) && !c.flags(2, "flags", kTabItemFlags, &flags)))
    return nullptr;
  // TabItemEx's plain assertion (widgets:10852).
  if ((flags & ImGuiTabItemFlags_Leading) &&
      (flags & ImGuiTabItemFlags_Trailing))
    return c.throwRange(
        "flags: TabItemFlags_Leading cannot be combined with "
        "TabItemFlags_Trailing");
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool selected = ImGui::BeginTabItem(label.c_str(), pOpen.ptr(), flags);
  if (!c.leaveImGui() || !c.write(pOpen))
    return nullptr;
  return c.result(selected);
}

/// `endTabItem()`, imgui.h:976.
napi_value endTabItem(napi_env env, napi_callback_info info) {
  Call c(env, info, "endTabItem");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndTabItem())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndTabItem();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerTreesTabs(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "treeNode", treeNode) &&
      registerFunction(env, exports, "treeNodeEx", treeNodeEx) &&
      registerFunction(env, exports, "treePop", treePop) &&
      registerFunction(env, exports, "collapsingHeader", collapsingHeader) &&
      registerFunction(env, exports, "setNextItemOpen", setNextItemOpen) &&
      registerFunction(env, exports, "beginTabBar", beginTabBar) &&
      registerFunction(env, exports, "endTabBar", endTabBar) &&
      registerFunction(env, exports, "beginTabItem", beginTabItem) &&
      registerFunction(env, exports, "endTabItem", endTabItem);
}

} // namespace bind
} // namespace hnimgui
