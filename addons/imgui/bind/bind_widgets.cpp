/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Widgets group (surface table, "Widgets"). Task 1 binds the part the
// core's tests need; Task 4 completes it, the label-id check included.

#include "bind_util.h"

#include "imgui_addon.h"

#include <cfloat>

namespace hnimgui {
namespace bind {

namespace {

/// `button(label, size = [0, 0])`, imgui.h:652.
napi_value button(napi_env env, napi_callback_info info) {
  Call c(env, info, "button");
  std::string label;
  ImVec2 size(0.0f, 0.0f);
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(1) && !c.vec2(1, "size", &size)) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::Button(label.c_str(), size);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(pressed);
}

/// `checkbox(label, v)`, imgui.h:656.
napi_value checkbox(napi_env env, napi_callback_info info) {
  Call c(env, info, "checkbox");
  std::string label;
  RefBool v;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refBool(1, "v", false, &v) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::Checkbox(label.c_str(), v.ptr());
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `sliderFloat3(label, v, vMin, vMax, format = '%.3f', flags = 0)`,
/// imgui.h:718.
napi_value sliderFloat3(napi_env env, napi_callback_info info) {
  Call c(env, info, "sliderFloat3");
  std::string label;
  RefFloat v;
  float vMin = 0.0f;
  float vMax = 0.0f;
  std::string format = "%.3f";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "v", 3, false, &v) || !c.f32(2, "vMin", &vMin) ||
      !c.f32(3, "vMax", &vMax) ||
      (!c.isMissing(4) && !c.format(4, "format", FormatKind::Float, &format)))
    return nullptr;
  if (!c.isMissing(5) && !c.flags(5, "flags", kSliderFlags, &flags))
    return nullptr;
  // SliderBehavior's plain assertions (widgets:3310, 3358).
  if (flags & ImGuiSliderFlags_WrapAround)
    return c.throwRange(
        "flags must not hold SliderFlags_WrapAround (drags only)");
  if (vMin < -FLT_MAX / 2.0f || vMax > FLT_MAX / 2.0f)
    return c.throwRange("vMin and vMax must be within -FLT_MAX/2 .. FLT_MAX/2");
  if (!c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::SliderFloat3(
      label.c_str(), v.ptr(), vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

} // namespace

bool registerWidgets(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "button", button) &&
      registerFunction(env, exports, "checkbox", checkbox) &&
      registerFunction(env, exports, "sliderFloat3", sliderFloat3);
}

} // namespace bind
} // namespace hnimgui
