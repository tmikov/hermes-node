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

#include <cstdint>
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

/// The recovery flags, set at context creation, every frame, and before
/// every call into ImGui (restoreRecoveryFlags()).
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
  kTableRowCallNoTable,
  kTableRowCallWrongWindow,
  kTableSetColumnIndexNoTable,
  kTableSetColumnIndexNoRow,
  kTableSetupScrollFreezeLayoutLocked,
  kEndTableCursorMoved,
  kTableSetupColumnNameOffset,
  kTableSetupColumnAllDisabled,
  kEndTableRegion,
  kEndChildTableRegion,
  kEndListBoxTableRegion,
  kListClipperWrongWindow,
  kDockSpaceTwice,
  kDockSpaceNotRoot,
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
    {"canTableRowCall:no-table", 0},
    {"canTableRowCall:wrong-window", 0},
    {"canTableSetColumnIndex:no-table", 0},
    {"canTableSetColumnIndex:no-row", 0},
    {"canTableSetupScrollFreeze:layout-locked", 0},
    {"canEndTable:cursor-moved", 0},
    {"canTableSetupColumn:name-offset", 0},
    {"canTableSetupColumn:all-disabled", 0},
    {"canEnd:table-region", 0},
    {"canEndChild:table-region", 0},
    {"canEndListBox:table-region", 0},
    {"canListClipper:wrong-window", 0},
    {"canDockSpaceOverViewport:twice", 0},
    {"canDockSpaceOverViewport:not-root", 0},
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

/// Whether End() of the current window would end a scrolling table's
/// region from inside itself. End() runs the window recovery before popping
/// the window (imgui.cpp:9038-9039), which calls EndTable() for as long as
/// the current table's inner window is the current window
/// (imgui.cpp:12019-12023). A table without ScrollX/ScrollY has the window
/// as both its outer and inner window; EndTable() of it makes current the
/// table of the temp-data slot below, if that slot was begun with this
/// window as its inner one (tables:470, 1670-1671), so the loop goes on to
/// the table the plain ones were nested in. A table with ScrollX/ScrollY
/// has a child of its own as inner window (tables:340, 432-433), and
/// EndTable() ends that child itself (tables:1612-1616): reached from this
/// loop, the window is ended twice and the clip rectangle stack is popped
/// past empty (imgui.cpp:6296-6297, imgui.h:2377). This walks the same
/// chain without changing anything. ErrorRecoveryTryToRecoverState(), at
/// the end of the frame, calls EndTable() instead of End() for such a
/// window (imgui.cpp:11990-11994), so only a closer called inside the
/// region meets it.
bool inTableRegion() {
  ImGuiContext &g = *GImGui;
  ImGuiTable *table = g.CurrentTable;
  // The slot EndTable() pops for `table`: the top one (tables:1670).
  int slot = g.TablesTempDataStacked - 1;
  while (table != nullptr && table->InnerWindow == g.CurrentWindow) {
    if (table->InnerWindow != table->OuterWindow)
      return true;
    // EndTable() of a plain table: the slot below, if begun in this window.
    --slot;
    table = nullptr;
    if (slot >= 0 && g.TablesTempData[slot].WindowID == g.CurrentWindow->ID)
      table = g.Tables.GetByIndex(g.TablesTempData[slot].TableIndex);
  }
  return false;
}

} // namespace

void configureContext() {
  setRecoveryFlags();
  ImGuiContext &g = *GImGui;
  g.ErrorCallback = onImGuiError;
  g.ErrorCallbackUserData = nullptr;
}

void restoreRecoveryFlags() {
  // colorConvertFloat4ToU32() calls ImGui with no context.
  if (GImGui != nullptr)
    setRecoveryFlags();
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
  if (inTableRegion())
    return refuse(
        kEndChildTableRegion,
        "EndChild() in a scrolling table's own region; call EndTable() "
        "first");
  return nullptr;
}

/// End() of a scrolling table's region ends it twice (inTableRegion()).
/// Everything else End() meets is recoverable: too many calls, a popup or
/// a child ended with End() (imgui.cpp:8990-9001).
const char *canEnd() {
  if (inTableRegion())
    return refuse(
        kEndTableRegion,
        "End() in a scrolling table's own region; call EndTable() first");
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
  if (inTableRegion())
    return refuse(
        kEndListBoxTableRegion,
        "EndListBox() in a scrolling table's own region; call EndTable() "
        "first");
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

/// Outside BeginTable() and EndTable(), the current table changes only
/// with the current window, in SetCurrentWindow(), which takes it from the
/// window's DC.CurrentTableIdx (imgui.cpp:4890). BeginTableEx() sets that index
/// on both the outer and the inner window (tables:551-553), so for a scrolling
/// table the outer window names a table whose inner window is the child: a
/// window appended to inside a cell (`begin()` of the outer window's own name)
/// is current with that table current. Each condition below is the one the call
/// reaches, in the order ImGui reaches it.
const char *canTableRowCall(TableRowCall call) {
  ImGuiContext &g = *GImGui;
  ImGuiTable *table = g.CurrentTable;
  if (table == nullptr) {
    if (call == TableRowCall::NextRow)
      return refuse(
          kTableRowCallNoTable, "TableNextRow() outside a BeginTable() scope");
    return nullptr;
  }
  if (g.CurrentWindow == table->InnerWindow)
    return nullptr;
  bool endsRow = false;
  const char *msg = nullptr;
  switch (call) {
    case TableRowCall::EndTable:
      endsRow = true; // tables:1476, before the row is ended
      msg =
          "EndTable() in another window than the table's own (a window "
          "begun inside a cell is current)";
      break;
    case TableRowCall::NextColumn:
      // TableNextRow() only when the row is full (tables:2316-2325).
      endsRow =
          table->IsInsideRow && table->CurrentColumn + 1 >= table->ColumnsCount;
      msg =
          "TableNextColumn() ending a row in another window than the "
          "table's own (a window begun inside a cell is current)";
      break;
    case TableRowCall::NextRow:
      endsRow = table->IsInsideRow; // TableEndRow(), tables:2066-2067
      msg =
          "TableNextRow() ending a row in another window than the table's "
          "own (a window begun inside a cell is current)";
      break;
    case TableRowCall::HeadersRow:
      endsRow = table->IsInsideRow; // through TableNextRow(), tables:3295
      msg =
          "TableHeadersRow() ending a row in another window than the "
          "table's own (a window begun inside a cell is current)";
      break;
  }
  if (!endsRow)
    return nullptr;
  return refuse(kTableRowCallWrongWindow, msg);
}

/// TableSetColumnIndex() returns false with no table (tables:2292-2293);
/// with one and no row open, TableBeginCell() would switch to a draw
/// channel the layout has not created (tables:2374-2381, draw:3483).
const char *canTableSetColumnIndex() {
  ImGuiContext &g = *GImGui;
  if (g.CurrentTable == nullptr)
    return refuse(
        kTableSetColumnIndexNoTable,
        "TableSetColumnIndex() outside a BeginTable() scope");
  if (!g.CurrentTable->IsInsideRow)
    return refuse(
        kTableSetColumnIndexNoRow,
        "TableSetColumnIndex() before TableNextRow()");
  return nullptr;
}

/// TableSetupScrollFreeze() reports a missing table recoverably
/// (tables:1857), then asserts plainly that the layout is not locked
/// (tables:1858), which the first row function locks (tables:1334).
const char *canTableSetupScrollFreeze() {
  ImGuiTable *table = GImGui->CurrentTable;
  if (table != nullptr && table->IsLayoutLocked)
    return refuse(
        kTableSetupScrollFreezeLayoutLocked,
        "TableSetupScrollFreeze() after the table's first row");
  return nullptr;
}

/// EndTable() asserts plainly that the cursor is at the bottom of the last
/// row (tables:1493), after ending a row that is open, which puts it there
/// (tables:2141). With no row open, that is where BeginTable() left it, the
/// top of the table (tables:533), unless setCursorScreenPos() moved it:
/// before the first row the cell items are skipped, but the cursor is not.
/// Neither value changes in the layout EndTable() runs first (tables:1468).
/// Checked after canTableRowCall(), whose condition EndTable() asserts
/// first (tables:1476).
const char *canEndTable() {
  ImGuiTable *table = GImGui->CurrentTable;
  if (table == nullptr || table->IsInsideRow ||
      table->RowPosY2 == table->InnerWindow->DC.CursorPos.y)
    return nullptr;
  return refuse(
      kEndTableCursorMoved,
      "EndTable() with the cursor moved before the table's first row");
}

/// TableSetupColumn() stores a non-empty label in the table's one names
/// buffer, at the buffer's size cast to the column's ImS16 NameOffset
/// (tables:1746-1750, internal:3262); the buffer is cleared when the table
/// begins (tables:632-633). An offset that does not fit wraps, and a later
/// TableGetColumnName() indexes the buffer with it (tables:1909), which
/// ImVector's operator[] asserts plainly is in range (imgui.h:2369); the
/// header row reads every name (tables:3259, 3307). Only when ImGui gets as
/// far as storing the name: its three recoverable checks return first
/// (tables:1739-1741).
///
/// Before that: the column that would complete the declaration with every
/// column carrying TableColumnFlags_Disabled. The layout every table runs
/// (the first row function, EndTable(), or the frame-end recovery's
/// EndTable()) enables a column only without that flag (tables:940),
/// whatever the user or the settings say, and asserts plainly that one is
/// enabled (tables:1005), so from this call on nothing could end the table.
/// A declared column's flags are in Columns[i].Flags (tables:823), or,
/// once the table is reconciling a changed set of columns, in the queued
/// request the layout applies (tables:1779-1786, 1840-1843). Columns left
/// undeclared get no flags (tables:924-930).
const char *canTableSetupColumn(bool hasLabel, int flags) {
  ImGuiTable *table = GImGui->CurrentTable;
  if (table == nullptr || table->DeclColumnsCount >= table->ColumnsCount ||
      table->IsLayoutLocked)
    return nullptr;
  if (table->DeclColumnsCount == table->ColumnsCount - 1 &&
      (flags & ImGuiTableColumnFlags_Disabled)) {
    bool allDisabled = true;
    for (int i = 0; i < table->DeclColumnsCount && allDisabled; ++i) {
      ImGuiTableColumnFlags f = table->Columns[i].Flags;
      for (const ImGuiTableReconcileColumnData &r :
           table->TempData->ReconcileColumnsRequests)
        if (r.ColumnNewIdx == i)
          f = r.Flags;
      allDisabled = (f & ImGuiTableColumnFlags_Disabled) != 0;
    }
    if (allDisabled)
      return refuse(
          kTableSetupColumnAllDisabled,
          "TableSetupColumn() would leave every column with "
          "TableColumnFlags_Disabled");
  }
  if (!hasLabel || table->ColumnsNames.size() <= INT16_MAX)
    return nullptr;
  return refuse(
      kTableSetupColumnNameOffset,
      "TableSetupColumn() with the table's column names already over 32767 "
      "bytes");
}

const char *canListClipper() {
  ImGuiContext &g = *GImGui;
  ImGuiTable *table = g.CurrentTable;
  if (table == nullptr || g.CurrentWindow == table->InnerWindow)
    return nullptr;
  return refuse(
      kListClipperWrongWindow,
      "listClipper() with a table current in another window than the "
      "table's own (a window begun inside a cell is current)");
}

const char *canDockSpaceOverViewport(unsigned dockspaceId, int flags) {
  ImGuiContext &g = *GImGui;
  // The ID DockSpaceOverViewport() gives a zero argument: GetID("DockSpace")
  // (imgui.cpp:21045-21046) inside its host window, whose name is
  // "WindowOverViewport_%08X" of the viewport's ID (imgui.cpp:21035-21036),
  // which for a root window is the ID-stack seed (the window's ID,
  // ImHashStr of its name, imgui.cpp:4852-4853).
  ImGuiID id = dockspaceId;
  if (id == 0) {
    char label[32];
    ImFormatString(
        label,
        IM_COUNTOF(label),
        "WindowOverViewport_%08X",
        ImGui::GetMainViewport()->ID);
    id = ImHashStr("DockSpace", 0, ImHashStr(label));
  }
  ImGuiDockNode *node = ImGui::DockContextFindNodeByID(&g, id);
  if (node == nullptr)
    return nullptr;
  if (node->ParentNode != nullptr)
    return refuse(
        kDockSpaceNotRoot,
        "DockSpaceOverViewport() with the ID of a split node's child, not a "
        "dock space");
  if (node->LastFrameActive == g.FrameCount && node->IsDockSpace() &&
      (flags & ImGuiDockNodeFlags_KeepAliveOnly) == 0)
    return refuse(
        kDockSpaceTwice,
        "DockSpaceOverViewport() twice in a frame with the same ID");
  return nullptr;
}

ScopeSnapshot snapshotScope() {
  ImGuiContext &g = *GImGui;
  return ScopeSnapshot{
      g.CurrentWindow,
      g.CurrentWindow->BeginCount,
      g.CurrentWindowStack.Size,
      g.CurrentTable,
      g.CurrentTable != nullptr ? g.CurrentTable->InstanceCurrent : -1};
}

bool sameScope(const ScopeSnapshot &s) {
  ImGuiContext &g = *GImGui;
  return g.CurrentWindow == s.window &&
      g.CurrentWindow->BeginCount == s.windowBeginCount &&
      g.CurrentWindowStack.Size == s.windowStackSize &&
      g.CurrentTable == s.table &&
      (g.CurrentTable == nullptr ||
       g.CurrentTable->InstanceCurrent == s.tableInstance);
}

TableScope tableScope() {
  ImGuiContext &g = *GImGui;
  TableScope s{g.CurrentTable != nullptr, "", ""};
  if (g.CurrentWindow != nullptr)
    s.currentWindow = g.CurrentWindow->Name;
  if (g.CurrentTable != nullptr && g.CurrentTable->InnerWindow != nullptr)
    s.innerWindow = g.CurrentTable->InnerWindow->Name;
  return s;
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
