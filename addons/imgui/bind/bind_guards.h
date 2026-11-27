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
