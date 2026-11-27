/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Tables group (surface table, "Tables").

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// IMGUI_TABLE_MAX_COLUMNS (imgui_internal.h:3235), which BeginTableEx and
/// TableSetupScrollFreeze compare with `<`.
constexpr int kMaxColumns = 512;
/// TableSetupScrollFreeze's limit on frozen rows (tables:1860, `< 128`).
constexpr int kMaxFrozenRows = 128;

/// `beginTable(strId, columns, flags = 0, outerSize = [0, 0], innerWidth =
/// 0)`, imgui.h:923.
napi_value beginTable(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginTable");
  std::string strId;
  int columns = 0;
  int flags = 0;
  ImVec2 outerSize(0.0f, 0.0f);
  float innerWidth = 0.0f;
  if (!c.ok() || !c.string(0, "strId", &strId) ||
      !c.intIn(1, "columns", 1, kMaxColumns - 1, &columns) ||
      (!c.isMissing(2) && !c.flags(2, "flags", kTableFlags, &flags)) ||
      (!c.isMissing(3) && !c.vec2(3, "outerSize", &outerSize)) ||
      (!c.isMissing(4) && !c.f32(4, "innerWidth", &innerWidth)))
    return nullptr;
  // BeginTableEx's plain assertion (tables:330-331).
  if ((flags & ImGuiTableFlags_ScrollX) && !(innerWidth >= 0.0f))
    return c.throwRange(
        "innerWidth must not be negative with TableFlags_ScrollX; got %g",
        innerWidth);
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool shown =
      ImGui::BeginTable(strId.c_str(), columns, flags, outerSize, innerWidth);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endTable()`, imgui.h:924. With no table ImGui reports it recoverably
/// (tables:1461).
napi_value endTable(napi_env env, napi_callback_info info) {
  Call c(env, info, "endTable");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTableRowCall(guard::TableRowCall::EndTable))
    return c.throwUsage("%s", msg);
  if (const char *msg = guard::canEndTable())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndTable();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `tableSetupColumn(label, flags = 0, initWidthOrWeight = 0, userData =
/// 0)`, imgui.h:938. `label` may be null. Outside a table, after the first
/// row and when called too often ImGui reports it recoverably
/// (tables:1739-1741).
napi_value tableSetupColumn(napi_env env, napi_callback_info info) {
  Call c(env, info, "tableSetupColumn");
  std::string label;
  bool noLabel = true;
  int flags = 0;
  float initWidthOrWeight = 0.0f;
  ImU32 userData = 0;
  if (!c.ok() || !c.stringOrNull(0, "label", &label, &noLabel) ||
      (!c.isMissing(1) && !c.flags(1, "flags", kTableColumnFlags, &flags)) ||
      (!c.isMissing(2) && !c.f32(2, "initWidthOrWeight", &initWidthOrWeight)) ||
      (!c.isMissing(3) && !c.u32(3, "userData", &userData)))
    return nullptr;
  // TableSetupColumn's plain assertion (tables:1742), and
  // TableSetupColumnFlags' (tables:802).
  if (flags & ImGuiTableColumnFlags_StatusMask_)
    return c.throwRange(
        "flags must not hold TableColumnFlags_IsEnabled, _IsVisible, "
        "_IsSorted or _IsHovered");
  if ((flags & ImGuiTableColumnFlags_WidthMask_) ==
      ImGuiTableColumnFlags_WidthMask_)
    return c.throwRange(
        "flags must not hold both TableColumnFlags_WidthStretch and "
        "TableColumnFlags_WidthFixed");
  if (!c.needFrame())
    return nullptr;
  if (const char *msg =
          guard::canTableSetupColumn(!noLabel && !label.empty(), flags))
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::TableSetupColumn(
      noLabel ? nullptr : label.c_str(), flags, initWidthOrWeight, userData);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `tableSetupScrollFreeze(cols, rows)`, imgui.h:939. With no table ImGui
/// reports it recoverably (tables:1857).
napi_value tableSetupScrollFreeze(napi_env env, napi_callback_info info) {
  Call c(env, info, "tableSetupScrollFreeze");
  int cols = 0;
  int rows = 0;
  if (!c.ok() || !c.intIn(0, "cols", 0, kMaxColumns - 1, &cols) ||
      !c.intIn(1, "rows", 0, kMaxFrozenRows - 1, &rows) || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTableSetupScrollFreeze())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::TableSetupScrollFreeze(cols, rows);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `tableHeadersRow()`, imgui.h:941. With no table ImGui reports it
/// recoverably (tables:3286).
napi_value tableHeadersRow(napi_env env, napi_callback_info info) {
  Call c(env, info, "tableHeadersRow");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTableRowCall(guard::TableRowCall::HeadersRow))
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::TableHeadersRow();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `tableNextRow(rowFlags = 0, minRowHeight = 0)`, imgui.h:925.
napi_value tableNextRow(napi_env env, napi_callback_info info) {
  Call c(env, info, "tableNextRow");
  int rowFlags = 0;
  float minRowHeight = 0.0f;
  if (!c.ok() ||
      (!c.isMissing(0) && !c.flags(0, "rowFlags", kTableRowFlags, &rowFlags)) ||
      (!c.isMissing(1) && !c.f32(1, "minRowHeight", &minRowHeight)) ||
      !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTableRowCall(guard::TableRowCall::NextRow))
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::TableNextRow(rowFlags, minRowHeight);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `tableNextColumn()`, imgui.h:926. With no table ImGui returns false
/// (tables:2313-2314).
napi_value tableNextColumn(napi_env env, napi_callback_info info) {
  Call c(env, info, "tableNextColumn");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTableRowCall(guard::TableRowCall::NextColumn))
    return c.throwUsage("%s", msg);
  c.enterImGui();
  const bool visible = ImGui::TableNextColumn();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(visible);
}

/// `tableSetColumnIndex(columnN)`, imgui.h:927. An index at or above the
/// column count is ImGui's recoverable error (tables:2299).
napi_value tableSetColumnIndex(napi_env env, napi_callback_info info) {
  Call c(env, info, "tableSetColumnIndex");
  int columnN = 0;
  // A negative index skips ImGui's range check when it equals the current
  // column, which is -1 after TableNextRow() (tables:2092, 2295-2305).
  if (!c.ok() || !c.intIn(0, "columnN", 0, INT32_MAX, &columnN) ||
      !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canTableSetColumnIndex())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  const bool visible = ImGui::TableSetColumnIndex(columnN);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(visible);
}

} // namespace

bool registerTables(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "beginTable", beginTable) &&
      registerFunction(env, exports, "endTable", endTable) &&
      registerFunction(env, exports, "tableSetupColumn", tableSetupColumn) &&
      registerFunction(
             env, exports, "tableSetupScrollFreeze", tableSetupScrollFreeze) &&
      registerFunction(env, exports, "tableHeadersRow", tableHeadersRow) &&
      registerFunction(env, exports, "tableNextRow", tableNextRow) &&
      registerFunction(env, exports, "tableNextColumn", tableNextColumn) &&
      registerFunction(
             env, exports, "tableSetColumnIndex", tableSetColumnIndex);
}

} // namespace bind
} // namespace hnimgui
