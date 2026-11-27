/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Layout and IDs groups (surface table, "Layout" and "IDs").

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `sameLine(offsetFromStartX = 0, spacing = -1)`, imgui.h:597.
napi_value sameLine(napi_env env, napi_callback_info info) {
  Call c(env, info, "sameLine");
  float offset = 0.0f;
  float spacing = -1.0f;
  if (!c.ok() || (!c.isMissing(0) && !c.f32(0, "offsetFromStartX", &offset)) ||
      (!c.isMissing(1) && !c.f32(1, "spacing", &spacing)) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SameLine(offset, spacing);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `separator()`, imgui.h:596.
napi_value separator(napi_env env, napi_callback_info info) {
  Call c(env, info, "separator");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Separator();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `separatorText(label)`, imgui.h:647.
napi_value separatorText(napi_env env, napi_callback_info info) {
  Call c(env, info, "separatorText");
  std::string label;
  if (!c.ok() || !c.string(0, "label", &label) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SeparatorText(label.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `spacing()`, imgui.h:599.
napi_value spacing(napi_env env, napi_callback_info info) {
  Call c(env, info, "spacing");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Spacing();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `dummy(size)`, imgui.h:600.
napi_value dummy(napi_env env, napi_callback_info info) {
  Call c(env, info, "dummy");
  ImVec2 size;
  if (!c.ok() || !c.vec2(0, "size", &size) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Dummy(size);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `newLine()`, imgui.h:598.
napi_value newLine(napi_env env, napi_callback_info info) {
  Call c(env, info, "newLine");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::NewLine();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `indent(indentW = 0)`, imgui.h:601.
napi_value indent(napi_env env, napi_callback_info info) {
  Call c(env, info, "indent");
  float v = 0.0f;
  if (!c.ok() || (!c.isMissing(0) && !c.f32(0, "indentW", &v)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Indent(v);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `unindent(indentW = 0)`, imgui.h:602.
napi_value unindent(napi_env env, napi_callback_info info) {
  Call c(env, info, "unindent");
  float v = 0.0f;
  if (!c.ok() || (!c.isMissing(0) && !c.f32(0, "indentW", &v)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Unindent(v);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `beginGroup()`, imgui.h:603.
napi_value beginGroup(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginGroup");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::BeginGroup();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `endGroup()`, imgui.h:604.
napi_value endGroup(napi_env env, napi_callback_info info) {
  Call c(env, info, "endGroup");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndGroup())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndGroup();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setNextItemWidth(itemWidth)`, imgui.h:561.
napi_value setNextItemWidth(napi_env env, napi_callback_info info) {
  Call c(env, info, "setNextItemWidth");
  float v = 0.0f;
  if (!c.ok() || !c.f32(0, "itemWidth", &v) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetNextItemWidth(v);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushItemWidth(itemWidth)`, imgui.h:559.
napi_value pushItemWidth(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushItemWidth");
  float v = 0.0f;
  if (!c.ok() || !c.f32(0, "itemWidth", &v) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PushItemWidth(v);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popItemWidth()`, imgui.h:560.
napi_value popItemWidth(napi_env env, napi_callback_info info) {
  Call c(env, info, "popItemWidth");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopItemWidth();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `getCursorScreenPos(out?)`, imgui.h:584.
napi_value getCursorScreenPos(napi_env env, napi_callback_info info) {
  Call c(env, info, "getCursorScreenPos");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetCursorScreenPos();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `setCursorScreenPos(pos)`, imgui.h:585.
napi_value setCursorScreenPos(napi_env env, napi_callback_info info) {
  Call c(env, info, "setCursorScreenPos");
  ImVec2 pos;
  if (!c.ok() || !c.vec2(0, "pos", &pos) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetCursorScreenPos(pos);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `getTextLineHeight()`, imgui.h:606.
napi_value getTextLineHeight(napi_env env, napi_callback_info info) {
  Call c(env, info, "getTextLineHeight");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetTextLineHeight();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `getTextLineHeightWithSpacing()`, imgui.h:607.
napi_value getTextLineHeightWithSpacing(napi_env env, napi_callback_info info) {
  Call c(env, info, "getTextLineHeightWithSpacing");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetTextLineHeightWithSpacing();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `getFrameHeight()`, imgui.h:608.
napi_value getFrameHeight(napi_env env, napi_callback_info info) {
  Call c(env, info, "getFrameHeight");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const float v = ImGui::GetFrameHeight();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(static_cast<double>(v));
}

/// `calcTextSize(text, hideTextAfterDoubleHash = false, wrapWidth = -1,
/// out?)`, imgui.h:1098; `text_end` is passed as NULL.
napi_value calcTextSize(napi_env env, napi_callback_info info) {
  Call c(env, info, "calcTextSize");
  std::string text;
  bool hide = false;
  float wrapWidth = -1.0f;
  if (!c.ok() || !c.string(0, "text", &text) ||
      (!c.isMissing(1) && !c.boolean(1, "hideTextAfterDoubleHash", &hide)) ||
      (!c.isMissing(2) && !c.f32(2, "wrapWidth", &wrapWidth)) ||
      !c.outArray(3, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::CalcTextSize(text.c_str(), nullptr, hide, wrapWidth);
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(3, v);
}

/// `alignTextToFramePadding()`, imgui.h:605.
napi_value alignTextToFramePadding(napi_env env, napi_callback_info info) {
  Call c(env, info, "alignTextToFramePadding");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::AlignTextToFramePadding();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushID(strId)` / `pushID(intId)`, dispatched on the argument's type:
/// imgui.h:622 and 625.
napi_value pushID(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushID");
  if (!c.ok())
    return nullptr;
  if (c.typeOf(0) == napi_number) {
    int id = 0;
    if (!c.int32(0, "intId", &id) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushID(id);
  } else {
    if (c.typeOf(0) != napi_string)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE", "strId must be a string, or intId a number");
    std::string id;
    if (!c.string(0, "strId", &id) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushID(id.c_str());
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popID()`, imgui.h:626.
napi_value popID(napi_env env, napi_callback_info info) {
  Call c(env, info, "popID");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopID();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `getID(strId)` / `getID(intId)`, dispatched on the argument's type:
/// imgui.h:627 and 630. Returns an ImGuiID, 0 .. 2^32-1.
napi_value getID(napi_env env, napi_callback_info info) {
  Call c(env, info, "getID");
  if (!c.ok())
    return nullptr;
  ImGuiID id = 0;
  if (c.typeOf(0) == napi_number) {
    int intId = 0;
    if (!c.int32(0, "intId", &intId) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    id = ImGui::GetID(intId);
  } else {
    if (c.typeOf(0) != napi_string)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE", "strId must be a string, or intId a number");
    std::string strId;
    if (!c.string(0, "strId", &strId) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    id = ImGui::GetID(strId.c_str());
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.resultU32(id);
}

} // namespace

bool registerLayout(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "sameLine", sameLine) &&
      registerFunction(env, exports, "separator", separator) &&
      registerFunction(env, exports, "separatorText", separatorText) &&
      registerFunction(env, exports, "spacing", spacing) &&
      registerFunction(env, exports, "dummy", dummy) &&
      registerFunction(env, exports, "newLine", newLine) &&
      registerFunction(env, exports, "indent", indent) &&
      registerFunction(env, exports, "unindent", unindent) &&
      registerFunction(env, exports, "beginGroup", beginGroup) &&
      registerFunction(env, exports, "endGroup", endGroup) &&
      registerFunction(env, exports, "setNextItemWidth", setNextItemWidth) &&
      registerFunction(env, exports, "pushItemWidth", pushItemWidth) &&
      registerFunction(env, exports, "popItemWidth", popItemWidth) &&
      registerFunction(
             env, exports, "getCursorScreenPos", getCursorScreenPos) &&
      registerFunction(
             env, exports, "setCursorScreenPos", setCursorScreenPos) &&
      registerFunction(env, exports, "getTextLineHeight", getTextLineHeight) &&
      registerFunction(
             env,
             exports,
             "getTextLineHeightWithSpacing",
             getTextLineHeightWithSpacing) &&
      registerFunction(env, exports, "getFrameHeight", getFrameHeight) &&
      registerFunction(env, exports, "calcTextSize", calcTextSize) &&
      registerFunction(
             env,
             exports,
             "alignTextToFramePadding",
             alignTextToFramePadding) &&
      registerFunction(env, exports, "pushID", pushID) &&
      registerFunction(env, exports, "popID", popID) &&
      registerFunction(env, exports, "getID", getID);
}

} // namespace bind
} // namespace hnimgui
