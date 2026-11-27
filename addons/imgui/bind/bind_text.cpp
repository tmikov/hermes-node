/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Text group (surface table, "Text"). Every text function passes a
// finished string: ImGui never interprets a `%` in it.

#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `text(text)`: TextUnformatted, imgui.h:634.
napi_value text(napi_env env, napi_callback_info info) {
  Call c(env, info, "text");
  std::string s;
  if (!c.ok() || !c.string(0, "text", &s) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::TextUnformatted(s.c_str(), s.c_str() + s.size());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `textColored(col, text)`: `TextColored(col, "%s", text)`, imgui.h:637.
napi_value textColored(napi_env env, napi_callback_info info) {
  Call c(env, info, "textColored");
  ImVec4 col;
  std::string s;
  if (!c.ok() || !c.vec4(0, "col", &col) || !c.string(1, "text", &s) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::TextColored(col, "%s", s.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `textDisabled(text)`: `TextDisabled("%s", text)`, imgui.h:639.
napi_value textDisabled(napi_env env, napi_callback_info info) {
  Call c(env, info, "textDisabled");
  std::string s;
  if (!c.ok() || !c.string(0, "text", &s) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::TextDisabled("%s", s.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `textWrapped(text)`: `TextWrapped("%s", text)`, imgui.h:641.
napi_value textWrapped(napi_env env, napi_callback_info info) {
  Call c(env, info, "textWrapped");
  std::string s;
  if (!c.ok() || !c.string(0, "text", &s) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::TextWrapped("%s", s.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `bulletText(text)`: `BulletText("%s", text)`, imgui.h:645.
napi_value bulletText(napi_env env, napi_callback_info info) {
  Call c(env, info, "bulletText");
  std::string s;
  if (!c.ok() || !c.string(0, "text", &s) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::BulletText("%s", s.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `labelText(label, text)`: `LabelText(label, "%s", text)`, imgui.h:643.
napi_value labelText(napi_env env, napi_callback_info info) {
  Call c(env, info, "labelText");
  std::string label;
  std::string s;
  if (!c.ok() || !c.string(0, "label", &label) || !c.string(1, "text", &s) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::LabelText(label.c_str(), "%s", s.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerText(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "text", text) &&
      registerFunction(env, exports, "textColored", textColored) &&
      registerFunction(env, exports, "textDisabled", textDisabled) &&
      registerFunction(env, exports, "textWrapped", textWrapped) &&
      registerFunction(env, exports, "bulletText", bulletText) &&
      registerFunction(env, exports, "labelText", labelText);
}

} // namespace bind
} // namespace hnimgui
