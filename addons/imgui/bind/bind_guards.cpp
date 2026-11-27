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
  kGuardCount
};
GuardCondition gGuards[kGuardCount] = {
    {"canEndGroup:no-group", 0},
    {"canEndGroup:wrong-window", 0},
    {"canEndChild:not-child", 0},
};

/// Counts and traces a refusal; returns \p msg.
const char *refuse(GuardId id, const char *msg) {
  ++gGuards[id].refused;
  trace("usage error: %s", msg);
  return msg;
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
}

const char *takeUsageError() {
  if (!gHasPendingError)
    return nullptr;
  gTakenError.swap(gPendingError);
  gPendingError.clear();
  gHasPendingError = false;
  return gTakenError.c_str();
}

/// EndGroup() asserts plainly that a group is open (imgui.cpp:12719) and
/// that it was begun in the current window (imgui.cpp:12722).
const char *canEndGroup() {
  ImGuiContext &g = *GImGui;
  if (g.GroupStack.Size <= 0)
    return refuse(kEndGroupNoGroup, "EndGroup() with no group open");
  if (g.GroupStack.back().WindowID != g.CurrentWindow->ID)
    return refuse(
        kEndGroupWrongWindow,
        "EndGroup() in another window than its BeginGroup()");
  return nullptr;
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
