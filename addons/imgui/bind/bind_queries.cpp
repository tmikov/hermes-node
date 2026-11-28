/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Item and input queries group (surface table, "Item and input
// queries").

#include "bind_util.h"

#include "imgui_addon.h"

#include <cstdint>

namespace hnimgui {
namespace bind {

namespace {

/// ImGui has five mouse buttons and names three; its mouse functions index
/// io.MouseDown[5] (imgui.h:2742) and assert the range plainly (G1's
/// exception).
constexpr int kLastMouseButton = 4;

/// Reads a key argument: an exported ImGuiKey enumerator (G1) that is also
/// a key GetKeyData() accepts (imgui.cpp:10283-10286): a named key, or
/// exactly one of the four modifiers (ConvertSingleModFlagToKey,
/// internal:3879). That refuses the exported non-keys Key_None / Mod_None,
/// Key_NamedKey_END and Key_NamedKey_COUNT.
bool readKey(Call &c, size_t i, ImGuiKey *out) {
  int key = 0;
  if (!c.enumValue(i, "key", kKey, &key))
    return false;
  const bool named =
      key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END;
  const bool mod = key == ImGuiMod_Ctrl || key == ImGuiMod_Shift ||
      key == ImGuiMod_Alt || key == ImGuiMod_Super;
  if (!named && !mod) {
    c.throwRange(
        "key must be a named key or a single Mod_ constant; got %d", key);
    return false;
  }
  *out = static_cast<ImGuiKey>(key);
  return true;
}

/// `getItemRectMin(out?)`, imgui.h:1071.
napi_value getItemRectMin(napi_env env, napi_callback_info info) {
  Call c(env, info, "getItemRectMin");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetItemRectMin();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `getItemRectMax(out?)`, imgui.h:1072.
napi_value getItemRectMax(napi_env env, napi_callback_info info) {
  Call c(env, info, "getItemRectMax");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetItemRectMax();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `isItemHovered(flags = 0)`, imgui.h:1057. Flags outside
/// ImGuiHoveredFlags_AllowedMaskForIsItemHovered are ImGui's own
/// recoverable error (imgui.cpp:5114), thrown by leaveImGui().
napi_value isItemHovered(napi_env env, napi_callback_info info) {
  Call c(env, info, "isItemHovered");
  int flags = 0;
  if (!c.ok() ||
      (!c.isMissing(0) && !c.flags(0, "flags", kHoveredFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsItemHovered(flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isItemClicked(mouseButton = 0)`, imgui.h:1060.
napi_value isItemClicked(napi_env env, napi_callback_info info) {
  Call c(env, info, "isItemClicked");
  int button = 0;
  if (!c.ok() ||
      (!c.isMissing(0) &&
       !c.intIn(0, "mouseButton", 0, kLastMouseButton, &button)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsItemClicked(button);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isItemActive()`, imgui.h:1058.
napi_value isItemActive(napi_env env, napi_callback_info info) {
  Call c(env, info, "isItemActive");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsItemActive();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isItemDeactivatedAfterEdit()`, imgui.h:1065.
napi_value isItemDeactivatedAfterEdit(napi_env env, napi_callback_info info) {
  Call c(env, info, "isItemDeactivatedAfterEdit");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsItemDeactivatedAfterEdit();
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `setItemDefaultFocus()`, imgui.h:1045.
napi_value setItemDefaultFocus(napi_env env, napi_callback_info info) {
  Call c(env, info, "setItemDefaultFocus");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetItemDefaultFocus();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `setKeyboardFocusHere(offset = 0)`, imgui.h:1046.
napi_value setKeyboardFocusHere(napi_env env, napi_callback_info info) {
  Call c(env, info, "setKeyboardFocusHere");
  int offset = 0;
  // imgui.cpp:9687 asserts plainly `offset >= -1`; and imgui.cpp:9712
  // stores `offset + 1` in an int, which overflows for INT32_MAX.
  if (!c.ok() ||
      (!c.isMissing(0) && !c.intIn(0, "offset", -1, INT32_MAX - 1, &offset)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::SetKeyboardFocusHere(offset);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `isKeyPressed(key, repeat = true)`, imgui.h:1111.
napi_value isKeyPressed(napi_env env, napi_callback_info info) {
  Call c(env, info, "isKeyPressed");
  ImGuiKey key = ImGuiKey_None;
  bool repeat = true;
  if (!c.ok() || !readKey(c, 0, &key) ||
      (!c.isMissing(1) && !c.boolean(1, "repeat", &repeat)) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsKeyPressed(key, repeat);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isKeyDown(key)`, imgui.h:1110.
napi_value isKeyDown(napi_env env, napi_callback_info info) {
  Call c(env, info, "isKeyDown");
  ImGuiKey key = ImGuiKey_None;
  if (!c.ok() || !readKey(c, 0, &key) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsKeyDown(key);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isMouseDown(button)`, imgui.h:1154.
napi_value isMouseDown(napi_env env, napi_callback_info info) {
  Call c(env, info, "isMouseDown");
  int button = 0;
  if (!c.ok() || !c.intIn(0, "button", 0, kLastMouseButton, &button) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsMouseDown(button);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isMouseClicked(button, repeat = false)`, imgui.h:1155.
napi_value isMouseClicked(napi_env env, napi_callback_info info) {
  Call c(env, info, "isMouseClicked");
  int button = 0;
  bool repeat = false;
  if (!c.ok() || !c.intIn(0, "button", 0, kLastMouseButton, &button) ||
      (!c.isMissing(1) && !c.boolean(1, "repeat", &repeat)) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsMouseClicked(button, repeat);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `isMouseDoubleClicked(button)`, imgui.h:1157.
napi_value isMouseDoubleClicked(napi_env env, napi_callback_info info) {
  Call c(env, info, "isMouseDoubleClicked");
  int button = 0;
  if (!c.ok() || !c.intIn(0, "button", 0, kLastMouseButton, &button) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool v = ImGui::IsMouseDoubleClicked(button);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(v);
}

/// `getMousePos(out?)`, imgui.h:1163.
napi_value getMousePos(napi_env env, napi_callback_info info) {
  Call c(env, info, "getMousePos");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetMousePos();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `getMouseDragDelta(button = 0, lockThreshold = -1, out?)`, imgui.h:1166.
napi_value getMouseDragDelta(napi_env env, napi_callback_info info) {
  Call c(env, info, "getMouseDragDelta");
  int button = 0;
  float lockThreshold = -1.0f;
  if (!c.ok() ||
      (!c.isMissing(0) &&
       !c.intIn(0, "button", 0, kLastMouseButton, &button)) ||
      (!c.isMissing(1) && !c.f32(1, "lockThreshold", &lockThreshold)) ||
      !c.outArray(2, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetMouseDragDelta(button, lockThreshold);
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(2, v);
}

} // namespace

bool registerQueries(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "isItemHovered", isItemHovered) &&
      registerFunction(env, exports, "isItemClicked", isItemClicked) &&
      registerFunction(env, exports, "isItemActive", isItemActive) &&
      registerFunction(
             env,
             exports,
             "isItemDeactivatedAfterEdit",
             isItemDeactivatedAfterEdit) &&
      registerFunction(env, exports, "getItemRectMin", getItemRectMin) &&
      registerFunction(env, exports, "getItemRectMax", getItemRectMax) &&
      registerFunction(
             env, exports, "setItemDefaultFocus", setItemDefaultFocus) &&
      registerFunction(
             env, exports, "setKeyboardFocusHere", setKeyboardFocusHere) &&
      registerFunction(env, exports, "isKeyPressed", isKeyPressed) &&
      registerFunction(env, exports, "isKeyDown", isKeyDown) &&
      registerFunction(env, exports, "isMouseDown", isMouseDown) &&
      registerFunction(env, exports, "isMouseClicked", isMouseClicked) &&
      registerFunction(
             env, exports, "isMouseDoubleClicked", isMouseDoubleClicked) &&
      registerFunction(env, exports, "getMousePos", getMousePos) &&
      registerFunction(env, exports, "getMouseDragDelta", getMouseDragDelta);
}

} // namespace bind
} // namespace hnimgui
