/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Everything the binding needs from ImGui's internal state, behind
// functions, so that bind_guards.cpp is the one unit including
// imgui_internal.h. A bump of ImGui re-audits that file against ImGui's
// source (the surface table's "Internal state read by bind_guards.cpp").

#ifndef HERMES_NODE_IMGUI_BIND_GUARDS_H
#define HERMES_NODE_IMGUI_BIND_GUARDS_H

#include <string>
#include <utility>
#include <vector>

namespace hnimgui {
namespace guard {

/// Error recovery without asserts or the error tooltip, and the error
/// callback; called from context creation, before any binding call can
/// meet the context.
void configureContext();
/// Sets the recovery flags again (configureContext() says which), when a
/// context exists. Called before every call into ImGui that can report a
/// usage error: by Call::enterImGui(), by newFrame() before the EndFrame()
/// of a discarded frame, and by CallbackScope's destructor, before the
/// list clipper's Step() and End() that follow a callback. Anything ImGui
/// runs can set the flags back on: ShowDemoWindow() does, with its
/// Configuration > Configuration node open (imgui_demo.cpp:626-627), and
/// leaves them on for the rest of the frame.
void restoreRecoveryFlags();
/// Re-asserts the recovery flags and sets g.BeginComboDepth to 0; called by
/// newFrame() before ImGui::NewFrame(). The counter counts the combo popup
/// windows currently begun, none is at that point (EndFrame() has closed
/// every window), and a combo left open is closed by ImGui's recovery
/// without the decrement (surface table, endCombo).
void beginFrame();
/// The first message ImGui reported since the last take, or nullptr;
/// clears it. The pointer is valid until the next take.
const char *takeUsageError();

/// The label-id check (surface table, "Shared preconditions"): whether
/// the ID \p label would get under the current ID stack equals the current
/// window's own ID, which ItemAdd() asserts plainly against
/// (imgui.cpp:12319). That happens for an empty label, or one ending in
/// `###`, with nothing pushed. A true answer is counted as a refusal under
/// `labelId:window-id` and traced as a usage error; the caller throws.
bool labelIdIsWindowId(const char *label);

// One per precondition of kind (b) in the surface table, named
// can<Function>: returns nullptr when the call is safe, else the message
// to throw. A refusal is counted under its condition's name and traced as
// a usage error. Later tasks add theirs. Each one returns nullptr where
// ImGui returns before its own assertion (a window with SkipItems set, for
// the closers that test it), so a guard never refuses a call ImGui accepts.
const char *canEndGroup();
const char *canEnd();
const char *canEndChild();
const char *canEndListBox();
const char *canEndCombo();
const char *canTreePop();
const char *canEndTabItem();
const char *canBeginMenuBar();
const char *canEndMenuBar();
const char *canEndMainMenuBar();
const char *canEndTooltip();
/// \p hasStrId: whether a strId was passed (with none, the popup takes the
/// last item's ID).
const char *canBeginPopupContextItem(bool hasStrId);

/// The table functions that end a row (directly, or through TableNextRow()
/// or EndTable()), each guarded by canTableRowCall().
enum class TableRowCall { NextRow, NextColumn, HeadersRow, EndTable };
/// TableNextRow() dereferences g.CurrentTable unchecked (tables:2062-2064);
/// TableEndRow() asserts plainly that the current window is the table's
/// inner window (tables:2126), and EndTable() asserts the same whether or
/// not a row is open (tables:1476). \p call says which of those the call
/// reaches: only TableNextRow() needs a table, the other three report a
/// missing one themselves or return false.
const char *canTableRowCall(TableRowCall call);
/// TableSetColumnIndex() with a table but no row open switches to a draw
/// channel that does not exist yet (tables:2300, 2374-2381; draw:3483).
/// Stricter than ImGui with no table, where ImGui returns false: the
/// binding refuses both (surface table, ruling 12).
const char *canTableSetColumnIndex();
/// TableSetupColumn() stores a non-empty label at an ImS16 offset into the
/// table's names buffer (tables:1746-1750); one that does not fit is later
/// read out of range (tables:1909, imgui.h:2369). \p hasLabel: a label
/// that is neither null nor empty.
/// Also refuses the call that would declare the last column with every
/// column TableColumnFlags_Disabled, which the layout asserts against
/// (tables:1005). \p flags: the call's column flags.
const char *canTableSetupColumn(bool hasLabel, int flags);
/// EndTable() asserts plainly that the cursor is at the bottom of the last
/// row (tables:1493); with no row begun, it is wherever the caller moved it.
/// Called after canTableRowCall(TableRowCall::EndTable).
const char *canEndTable();
/// TableSetupScrollFreeze() asserts plainly that the table's layout is not
/// locked, that is, that no row was begun (tables:1858).
const char *canTableSetupScrollFreeze();

/// The clipper works on the current table whenever there is one, and
/// assumes that the current window is the table's inner window: Begin()
/// and Step() end an open row with TableEndRow() (imgui.cpp:3494-3496,
/// 3566-3567), which asserts plainly on that (tables:2126), and every seek,
/// End()'s included (imgui.cpp:3521-3522), sets the table's RowPosY2 from
/// the current window's cursor (imgui.cpp:3461-3472), which EndTable() then
/// asserts plainly against the inner window's (tables:1493). Refused when a
/// table is current in another window than its inner one (the window of
/// canTableRowCall's wrong-window condition), whether or not a row is open.
/// The scope comparison below does not catch this: it is the state the
/// clipper was begun in, and nothing changes it.
const char *canListClipper();

/// DockSpace(), called by DockSpaceOverViewport() with the ImGuiID \p
/// dockspaceId gives it (a zero argument is replaced by the dock space's
/// default ID, computed as ImGui computes it), asserts plainly that the
/// node is not already a dock space submitted this frame
/// (`canDockSpaceOverViewport:twice`, imgui.cpp:20937-20939, unless
/// DockNodeFlags_KeepAliveOnly is in \p flags) and that it is a root node
/// (`canDockSpaceOverViewport:not-root`, imgui.cpp:20985). The second is
/// refused whatever the flags and whether or not the node was updated this
/// frame, which is stricter than ImGui: there DockSpace() returns early
/// (imgui.cpp:20937-20950) and only marks the split node as a dock space
/// (imgui.cpp:20940, 20943), which then asserts on the next submission of
/// that ID. The host window's SkipItems, which would make ImGui take the
/// keep-alive path too (imgui.cpp:20914-20915), is set only inside the
/// call and is not read, so both conditions are stricter than ImGui
/// there: with it set, ImGui accepts a second call or a split node's ID
/// that the guard refuses.
const char *canDockSpaceOverViewport(unsigned dockspaceId, int flags);

/// What listClipper() compares after each callback (design, "Exceptions:
/// List clipper"): the current window, the depth of the window stack, and
/// the current table. A table that does not scroll changes the current
/// table without opening a window, so the table is compared too.
/// The window and the table are also compared by how many times they were
/// begun, so that one ended and begun again with the same identity inside
/// the callback (end() then begin() of the clipper's window; endTable()
/// then beginTable() with the same ID) is not taken for the same scope:
/// the window's BeginCount, raised by every Begin() of it in a frame
/// (imgui.cpp:8892), and the table's InstanceCurrent, raised by every
/// BeginTable() of it in a frame (imgui_tables.cpp:376, 391).
///
/// The window is kept as its address, which is stable: each window is an
/// allocation of its own (imgui.cpp:7153), freed only when the context is
/// destroyed (imgui.cpp:4744), and g.Windows holds pointers to them. The
/// table is not: g.Tables is an ImPool, whose storage is one ImVector that
/// moves when a table is added ("Add/remove invalidate all pointers",
/// imgui_internal.h:811; ImVector::reserve, imgui.h:2385), so a callback
/// that begins a new table can move the clipper's own table (Codex's code
/// review, C6). It is kept as its pool index, stable for the table's
/// lifetime (the same comment), which is how ImGui itself finds the outer
/// table again after a nested one ends (imgui_tables.cpp:1671), with its ID.
struct ScopeSnapshot {
  const void *window;
  int windowBeginCount;
  int windowStackSize;
  int tableIndex; // in g.Tables; -1 with no current table
  unsigned tableId;
  int tableInstance;
};
ScopeSnapshot snapshotScope();
bool sameScope(const ScopeSnapshot &s);

/// For __test.tableScope(): whether a table is current, and the names of
/// the current window and of that table's inner window ("" without one).
struct TableScope {
  bool table;
  std::string currentWindow;
  std::string innerWindow;
  int poolCapacity; // g.Tables' storage capacity, which grows by moving
};
TableScope tableScope();

/// Brings ImGui's saved colour-edit state into its domain before a colour
/// editor runs (Codex's code review, round 2, D1): the hue and saturation
/// ImGui keeps from the last edit (g.ColorEditSavedHue / SavedSat) and
/// restores, while the RGB value still matches, when it reads back a
/// saturation or value of 0 (ColorEditRestoreHS, imgui_widgets.cpp:5902-
/// 5917; called at 5978 by the editor and at 6274 and 6461 by its picker).
/// The editor converts the restored values to int (5980), and the picker
/// places its hue cursor at the hue times its size (IM_ROUND, 6543). A
/// value typed into the HSV display with HDR is saved as it is (6124-
/// 6128), so the state is forgotten (SavedID = 0, which no editor's ID
/// equals) when either is outside 0 .. 1 or not finite; a state inside
/// 0 .. 1, everything an ordinary edit or the picker saves, is kept, so
/// hue preservation is unchanged for it. Traced when it forgets.
void sanitizeColorEditState();
/// Sets ImGui's drag accumulator (g.DragCurrentAccum) to 0 when it is not
/// finite, before a dragFloat runs (round 2, D4). A float drag that
/// overflows the value to infinity leaves `accum - (inf - v)`, an infinity
/// or NaN, in the accumulator (imgui_widgets.cpp:2648), which ImGui would
/// add to the value at the next step (2631): the value the binding wrote
/// back as FLT_MAX would then flip to -FLT_MAX. Reset, the next step
/// overflows again and the value stays at FLT_MAX. Only an active drag
/// reads the accumulator, and activation resets it anyway (2598-2602).
void resetNonFiniteDragAccumulator();
/// Sets g.DragCurrentAccum to 0 unless its magnitude is below 1, before a
/// call whose drag may be an integer one: dragInt, and colorEdit3 /
/// colorEdit4, whose component drags are DragInt in the 0..255 data type
/// (imgui_widgets.cpp:6032) (Codex's code review, round 3, E1). The
/// integer path converts the accumulator plus the frame's delta to int and
/// adds it to the value (2631); between calls it leaves only the fraction
/// it could not apply, below 1 (2648), or 0 (2598-2602), so 1 or more can
/// only be a float drag's remainder (a delta too small to change its value
/// is kept whole, 2648 subtracting zero), carried to an integer drag that
/// took over the same active ID while the mouse stayed down: DragBehavior
/// dispatches on the new data type and resets nothing (2700-2727).
void resetDragAccumulatorForInt();
/// For __test.colorEditState(): whether a saved state is held, and its
/// hue and saturation.
struct ColorEditState {
  bool held;
  float hue;
  float sat;
};
ColorEditState colorEditState();

/// The current window's draw list's pixel density, the factor the draw
/// list multiplies a length by before converting it (`_InvFringeScale`,
/// an "[internal]" member of ImDrawList, imgui.h:3560). Inside a frame.
float pixelDensity();

/// Every registered guard condition, `can<Function>:<condition>`, with how
/// many times it has refused.
std::vector<std::pair<std::string, int>> guardRefusals();

/// One ImGui window, for __test.state().windows.
struct WindowInfo {
  std::string name;
  bool active;
  bool docked;
};
/// Every window ImGui knows, as of the last ended frame.
std::vector<WindowInfo> windowsOfLastFrame();

} // namespace guard
} // namespace hnimgui

#endif // HERMES_NODE_IMGUI_BIND_GUARDS_H
