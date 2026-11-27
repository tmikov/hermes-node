/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The binding's only use of imgui_internal.h: the error callback and the
// recovery configuration, the state reads of the guards, and the window
// list of the test hook. Every field read or written here is listed in the
// surface table's "Internal state read by bind_guards.cpp"; a bump of
// ImGui re-audits this file against ImGui's source.

#include "bind_guards.h"

#include "imgui_addon.h"

#include <imgui_internal.h>

#include <cstring>
#include <string>

namespace hnimgui {
namespace guard {

namespace {

/// The first message reported since the last take, and whether there is
/// one. ImGui runs on the JS thread only, so no locking.
std::string gPendingError;
bool gHasPendingError = false;
/// What takeUsageError() last returned, kept alive until the next take.
std::string gTakenError;

/// ImGui's error callback (`g.ErrorCallback`, called from ErrorLog(),
/// imgui.cpp:12137-12138) for every IM_ASSERT_USER_ERROR. Keeps the first
/// message, since later ones in the same call are usually its
/// consequences, and traces each.
void onImGuiError(ImGuiContext *, void *, const char *msg) {
  trace("usage error: %s", msg);
  if (!gHasPendingError) {
    gPendingError = msg;
    gHasPendingError = true;
  }
}

/// The recovery flags, set at context creation and again every frame.
/// Asserts off, so a recoverable error never aborts a Debug build; the
/// tooltip off, since it carries a button turning the asserts back on
/// (imgui.cpp:12186); and ImGui's own debug log off, which with its
/// default flags prints every error to stdout (ErrorLog, imgui.cpp:12108;
/// DebugLogV, imgui.cpp:24182): the error callback traces instead, under
/// the addon's tracing rule. With all three off the callback is what keeps
/// ErrorCheckNewFrameSanityChecks satisfied (imgui.cpp:11857-11858).
void setRecoveryFlags() {
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigErrorRecovery = true;
  io.ConfigErrorRecoveryEnableAssert = false;
  io.ConfigErrorRecoveryEnableTooltip = false;
  io.ConfigErrorRecoveryEnableDebugLog = false;
}

/// One guard condition: its name and how many times it refused.
struct GuardCondition {
  const char *name;
  int refused;
};

/// Every guard condition, registered here so that one never refused shows
/// up in guardRefusals() with a zero count.
enum GuardId {
  kEndGroupNoGroup,
  kEndGroupWrongWindow,
  kEndChildNotChild,
  kLabelIdWindowId,
  kEndListBoxNotChild,
  kEndListBoxNoGroup,
  kEndListBoxWrongWindow,
  kEndComboNoCombo,
  kEndComboWrongWindow,
  kTreePopNoNode,
  kTreePopIdMismatch,
  kTreePopIdStack,
  kEndTabItemNoTabItem,
  kBeginMenuBarAlreadyOpen,
  kEndMenuBarNoMenuBarFlag,
  kEndMenuBarNotOpen,
  kEndMenuBarNoGroup,
  kEndMenuBarWrongWindow,
  kEndMainMenuBarNoGroup,
  kEndMainMenuBarWrongWindow,
  kEndTooltipNotTooltip,
  kBeginPopupContextItemNoId,
  kGuardCount
};
GuardCondition gGuards[kGuardCount] = {
    {"canEndGroup:no-group", 0},
    {"canEndGroup:wrong-window", 0},
    {"canEndChild:not-child", 0},
    {"labelId:window-id", 0},
    {"canEndListBox:not-child", 0},
    {"canEndListBox:no-group", 0},
    {"canEndListBox:wrong-window", 0},
    {"canEndCombo:no-combo", 0},
    {"canEndCombo:wrong-window", 0},
    {"canTreePop:no-node", 0},
    {"canTreePop:id-mismatch", 0},
    {"canTreePop:id-stack", 0},
    {"canEndTabItem:no-tab-item", 0},
    {"canBeginMenuBar:already-open", 0},
    {"canEndMenuBar:no-menu-bar-flag", 0},
    {"canEndMenuBar:not-open", 0},
    {"canEndMenuBar:no-group", 0},
    {"canEndMenuBar:wrong-window", 0},
    {"canEndMainMenuBar:no-group", 0},
    {"canEndMainMenuBar:wrong-window", 0},
    {"canEndTooltip:not-tooltip", 0},
    {"canBeginPopupContextItem:no-id", 0},
};

/// The mask of tree depth \p depth in `DC.TreeHasStackDataDepthMask`: the
/// expression of TREE_NODE_GET_DEPTH_MASK (widgets:7019-7020), a macro
/// private to imgui_widgets.cpp.
ImU32 treeDepthMask(int depth) {
  return static_cast<ImU32>(depth) < 32 ? (1u << depth) : 0u;
}

/// Counts and traces a refusal; returns \p msg.
const char *refuse(GuardId id, const char *msg) {
  ++gGuards[id].refused;
  trace("usage error: %s", msg);
  return msg;
}

/// The two conditions EndGroup() asserts plainly (imgui.cpp:12719, 12722),
/// for an EndGroup() that will run in \p window: a group is open, and the
/// top one was begun in \p window (null when there is no such window,
/// which counts as the second condition failing). Every guard of a closer
/// that ends a group goes through this, each refusing under its own two
/// names and messages.
const char *groupOpenIn(
    const ImGuiWindow *window,
    GuardId noGroup,
    const char *noGroupMsg,
    GuardId wrongWindow,
    const char *wrongWindowMsg) {
  ImGuiContext &g = *GImGui;
  if (g.GroupStack.Size <= 0)
    return refuse(noGroup, noGroupMsg);
  if (window == nullptr || g.GroupStack.back().WindowID != window->ID)
    return refuse(wrongWindow, wrongWindowMsg);
  return nullptr;
}

} // namespace

void configureContext() {
  setRecoveryFlags();
  ImGuiContext &g = *GImGui;
  g.ErrorCallback = onImGuiError;
  g.ErrorCallbackUserData = nullptr;
}

void beginFrame() {
  setRecoveryFlags();
  // The binding's one write to ImGui's state besides the error callback
  // (surface table, "Internal state read by bind_guards.cpp"). The counter
  // is set to 0 at context creation (imgui.cpp:4549), incremented when a
  // combo popup's Begin() succeeds (widgets:2083) and decremented only by
  // EndCombo() (widgets:2091). A combo left open is ended by the window
  // recovery as a window, so nothing else would bring it back. Zero is
  // right here: no window is begun before NewFrame(), since EndFrame()'s
  // recovery ends every window left open (imgui.cpp:6432-6433,
  // 11981-12009) and NewFrame() asserts that the last frame was ended
  // (imgui.cpp:11839); newFrame() ends a frame it finds open first.
  GImGui->BeginComboDepth = 0;
}

const char *takeUsageError() {
  if (!gHasPendingError)
    return nullptr;
  gTakenError.swap(gPendingError);
  gPendingError.clear();
  gHasPendingError = false;
  return gTakenError.c_str();
}

/// ImGui::GetID() is `window->GetID(label)` (imgui.cpp:10164-10168): the
/// label hashed with the top of the current window's ID stack as seed,
/// exactly what the widget computes before its ItemAdd(). The hash is the
/// seed itself for "" and for a label ending in "###" (ImHashStr,
/// imgui.cpp:2636-2658), and the seed is the window's ID when nothing has
/// been pushed. Unlike ImGui's assertion, which a clipped item skips
/// (imgui.cpp:12304-12307), this refuses whether or not the item would be
/// visible.
bool labelIdIsWindowId(const char *label) {
  ImGuiContext &g = *GImGui;
  if (ImGui::GetID(label) != g.CurrentWindow->ID)
    return false;
  refuse(kLabelIdWindowId, "an item ID equal to the window's own ID");
  return true;
}

/// EndGroup() asserts plainly that a group is open (imgui.cpp:12719) and
/// that it was begun in the current window (imgui.cpp:12722).
const char *canEndGroup() {
  return groupOpenIn(
      GImGui->CurrentWindow,
      kEndGroupNoGroup,
      "EndGroup() with no group open",
      kEndGroupWrongWindow,
      "EndGroup() in another window than its BeginGroup()");
}

/// EndChild() asserts plainly that the current window is a child window
/// (imgui.cpp:7021). A child whose BeginChild() returned false is still
/// current, so this holds for it.
const char *canEndChild() {
  ImGuiContext &g = *GImGui;
  if ((g.CurrentWindow->Flags & ImGuiWindowFlags_ChildWindow) == 0)
    return refuse(kEndChildNotChild, "EndChild() outside a child window");
  return nullptr;
}

/// EndListBox() asserts plainly that the current window is a child window
/// (widgets:8997), then ends it and calls EndGroup() in the window below
/// it (widgets:9000-9001), which asserts plainly that a group is open and
/// that it was begun in that window (imgui.cpp:12719, 12722). The window
/// EndChild() returns to is the entry below the top of the window stack.
const char *canEndListBox() {
  ImGuiContext &g = *GImGui;
  if ((g.CurrentWindow->Flags & ImGuiWindowFlags_ChildWindow) == 0)
    return refuse(kEndListBoxNotChild, "EndListBox() outside a child window");
  const int n = g.CurrentWindowStack.Size;
  return groupOpenIn(
      n >= 2 ? g.CurrentWindowStack[n - 2].Window : nullptr,
      kEndListBoxNoGroup,
      "EndListBox() with no group open (the list box's group)",
      kEndListBoxWrongWindow,
      "EndListBox() with another window's group on top (not the list box's)");
}

/// EndCombo() decrements g.BeginComboDepth (widgets:2091) and only then
/// compares the current window's name with the combo popup's,
/// "##Combo_%02d" of the decremented depth (widgets:2094-2096), so a
/// mismatch it reports has already disturbed the counter. This makes the
/// same comparison first. With no combo begun the counter is 0, and the
/// decrement would take it below zero.
const char *canEndCombo() {
  ImGuiContext &g = *GImGui;
  if (g.BeginComboDepth <= 0)
    return refuse(kEndComboNoCombo, "EndCombo() with no combo open");
  char name[16];
  ImFormatString(name, IM_COUNTOF(name), "##Combo_%02d", g.BeginComboDepth - 1);
  if (strcmp(g.CurrentWindow->Name, name) != 0)
    return refuse(
        kEndComboWrongWindow,
        "EndCombo() in another window than the combo's popup");
  return nullptr;
}

/// TreePop() decrements and shifts by DC.TreeDepth unchecked
/// (widgets:7405-7406); when the node being popped keeps stack data it
/// asserts plainly that its ID is the top of the ID stack (widgets:7410);
/// and it asserts plainly that more than the window's own ID is on the
/// stack (widgets:7426). Tested in that order.
const char *canTreePop() {
  ImGuiContext &g = *GImGui;
  ImGuiWindow *window = g.CurrentWindow;
  if (window->DC.TreeDepth <= 0)
    return refuse(kTreePopNoNode, "TreePop() with no tree node open");
  if ((window->DC.TreeHasStackDataDepthMask &
       treeDepthMask(window->DC.TreeDepth - 1)) &&
      g.TreeNodeStack.back().ID != window->IDStack.back())
    return refuse(
        kTreePopIdMismatch,
        "TreePop() with an ID pushed inside the tree node still on the ID "
        "stack");
  if (window->IDStack.Size <= 1)
    return refuse(
        kTreePopIdStack, "TreePop() after the tree node's own ID was popped");
  return nullptr;
}

/// EndTabItem() returns when the window skips items (widgets:10789-10790)
/// and reports a missing tab bar recoverably (widgets:10793); then it
/// asserts plainly that a tab item was begun in the bar (widgets:10794).
const char *canEndTabItem() {
  ImGuiContext &g = *GImGui;
  if (g.CurrentWindow->SkipItems || g.CurrentTabBar == nullptr)
    return nullptr;
  if (g.CurrentTabBar->LastTabItemIdx < 0)
    return refuse(
        kEndTabItemNoTabItem,
        "EndTabItem() with no tab item begun in the tab bar");
  return nullptr;
}

/// BeginMenuBar() returns false when the window skips items or has no
/// WindowFlags_MenuBar (widgets:9325-9329), and otherwise asserts plainly
/// that the window's menu bar is not open already (widgets:9331).
const char *canBeginMenuBar() {
  ImGuiWindow *window = GImGui->CurrentWindow;
  if (window->SkipItems || (window->Flags & ImGuiWindowFlags_MenuBar) == 0)
    return nullptr;
  if (window->DC.MenuBarAppending)
    return refuse(
        kBeginMenuBarAlreadyOpen,
        "BeginMenuBar() while this window's menu bar is open");
  return nullptr;
}

/// EndMenuBar() returns when the window skips items (widgets:9357-9358),
/// then asserts plainly that the window has WindowFlags_MenuBar and that
/// its menu bar is open (widgets:9362-9363), and reads the group stack's
/// top and ends it (widgets:9401, 9405), which EndGroup() asserts is there
/// and belongs to the window (imgui.cpp:12719, 12722). That group is the
/// one BeginMenuBar() opened, unless an endGroup() inside the menu bar
/// ended it.
const char *canEndMenuBar() {
  ImGuiContext &g = *GImGui;
  ImGuiWindow *window = g.CurrentWindow;
  if (window->SkipItems)
    return nullptr;
  if ((window->Flags & ImGuiWindowFlags_MenuBar) == 0)
    return refuse(
        kEndMenuBarNoMenuBarFlag,
        "EndMenuBar() in a window without WindowFlags_MenuBar");
  if (!window->DC.MenuBarAppending)
    return refuse(kEndMenuBarNotOpen, "EndMenuBar() with no menu bar open");
  return groupOpenIn(
      window,
      kEndMenuBarNoGroup,
      "EndMenuBar() with no group open (the menu bar's own group was ended)",
      kEndMenuBarWrongWindow,
      "EndMenuBar() with another window's group on top (the menu bar's own "
      "group was ended)");
}

/// EndMainMenuBar() reports recoverably a current window whose menu bar is
/// not open (widgets:9483), then calls EndMenuBar() (widgets:9485), which
/// returns when the window skips items and otherwise reads the group
/// stack's top and ends it (widgets:9401, 9405): the same group conditions
/// as canEndMenuBar's, which ImGui's own check does not cover. With the
/// menu bar open the window has WindowFlags_MenuBar (BeginMenuBar() sets
/// MenuBarAppending only then, widgets:9328-9349), so EndMenuBar()'s two
/// other assertions (widgets:9362-9363) hold.
const char *canEndMainMenuBar() {
  ImGuiWindow *window = GImGui->CurrentWindow;
  if (!window->DC.MenuBarAppending || window->SkipItems)
    return nullptr;
  return groupOpenIn(
      window,
      kEndMainMenuBarNoGroup,
      "EndMainMenuBar() with no group open (the menu bar's own group was "
      "ended)",
      kEndMainMenuBarWrongWindow,
      "EndMainMenuBar() with another window's group on top (the menu bar's "
      "own group was ended)");
}

/// EndTooltip() asserts plainly that the current window is a tooltip
/// (imgui.cpp:13088).
const char *canEndTooltip() {
  if ((GImGui->CurrentWindow->Flags & ImGuiWindowFlags_Tooltip) == 0)
    return refuse(kEndTooltipNotTooltip, "EndTooltip() outside a tooltip");
  return nullptr;
}

/// BeginPopupContextItem() returns when the window skips items
/// (imgui.cpp:13614-13615); otherwise, with no strId it takes the last
/// item's ID, and asserts plainly that it is not 0 (imgui.cpp:13616-13617),
/// which it is after an item with no ID (`text`, `dummy`, ...).
const char *canBeginPopupContextItem(bool hasStrId) {
  ImGuiContext &g = *GImGui;
  if (hasStrId || g.CurrentWindow->SkipItems)
    return nullptr;
  if (g.LastItemData.ID == 0)
    return refuse(
        kBeginPopupContextItemNoId,
        "BeginPopupContextItem() with no strId after an item that has no "
        "ID");
  return nullptr;
}

std::vector<std::pair<std::string, int>> guardRefusals() {
  std::vector<std::pair<std::string, int>> result;
  for (const GuardCondition &c : gGuards)
    result.emplace_back(c.name, c.refused);
  return result;
}

std::vector<WindowInfo> windowsOfLastFrame() {
  std::vector<WindowInfo> result;
  ImGuiContext *g = ImGui::GetCurrentContext();
  if (g == nullptr)
    return result;
  // Inside a frame, Active describes the frame being built and WasActive
  // the last ended one (NewFrame copies one to the other); outside one,
  // Active is the last ended frame's.
  for (ImGuiWindow *w : g->Windows) {
    result.push_back(WindowInfo{
        w->Name,
        g->WithinFrameScope ? w->WasActive : w->Active,
        w->DockIsActive});
  }
  return result;
}

} // namespace guard
} // namespace hnimgui
