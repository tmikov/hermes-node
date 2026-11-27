/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Widgets group (surface table, "Widgets").
//
// Every function whose entry says "Label-id: yes" checks its label with
// Call::needLabelId() after needFrame(). endCombo and endListBox run their
// state guards (guard::canEndCombo, guard::canEndListBox); canEndCombo
// rests on guard::beginFrame()'s reset of g.BeginComboDepth.

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

#include <cfloat>
#include <climits>

namespace hnimgui {
namespace bind {

namespace {

/// The largest capacity of an inputText buffer (design, "Buffers").
constexpr int kMaxTextCapacity = 1 << 24;

/// The six InputTextFlags_Callback* bits (imgui.h:1366-1371): each needs
/// a callback, which the binding never passes.
constexpr int kInputTextCallbackBits = ImGuiInputTextFlags_CallbackCompletion |
    ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways |
    ImGuiInputTextFlags_CallbackCharFilter |
    ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackEdit;

/// Whether more than one bit of \p mask is set in \p flags.
bool moreThanOne(int flags, int mask) {
  const unsigned bits = static_cast<unsigned>(flags & mask);
  return (bits & (bits - 1)) != 0;
}

/// Reads an optional `ImGuiSliderFlags` at \p i into \p flags. A slider
/// (\p slider) refuses SliderFlags_WrapAround, which SliderBehavior
/// asserts plainly against (widgets:3310); a drag accepts it.
bool sliderFlags(Call &c, size_t i, bool slider, int *flags) {
  if (!c.isMissing(i) && !c.flags(i, "flags", kSliderFlags, flags))
    return false;
  if (slider && (*flags & ImGuiSliderFlags_WrapAround)) {
    c.throwRange("flags must not hold SliderFlags_WrapAround (drags only)");
    return false;
  }
  return true;
}

/// Reads an optional `ImGuiInputTextFlags` at \p i, named \p what, into
/// \p flags, refusing what InputTextEx asserts plainly
/// (widgets:4802-4807): any Callback* bit, since no callback is passed;
/// for a single-line field WordWrap; for a multi-line one ElideLeft, and
/// WordWrap together with Password.
bool inputTextFlags(
    Call &c,
    size_t i,
    const char *what,
    bool multiline,
    int *flags) {
  if (!c.isMissing(i) && !c.flags(i, what, kInputTextFlags, flags))
    return false;
  if (*flags & kInputTextCallbackBits) {
    c.throwRange(
        "%s must not hold an InputTextFlags_Callback* bit (no callback can "
        "be passed); got 0x%x",
        what,
        static_cast<unsigned>(*flags & kInputTextCallbackBits));
    return false;
  }
  if (!multiline && (*flags & ImGuiInputTextFlags_WordWrap)) {
    c.throwRange(
        "%s must not hold InputTextFlags_WordWrap (multi-line only)", what);
    return false;
  }
  if (multiline && (*flags & ImGuiInputTextFlags_ElideLeft)) {
    c.throwRange(
        "%s must not hold InputTextFlags_ElideLeft (single-line only)", what);
    return false;
  }
  if ((*flags & ImGuiInputTextFlags_WordWrap) &&
      (*flags & ImGuiInputTextFlags_Password)) {
    c.throwRange(
        "%s: InputTextFlags_WordWrap cannot be combined with "
        "InputTextFlags_Password",
        what);
    return false;
  }
  return true;
}

/// Reads `buf` at \p bufArg with its capacity at \p sizeArg (an integer
/// 1 .. 2^24, design, "Buffers").
bool textBuffer(Call &c, size_t bufArg, size_t sizeArg, RefString *buf) {
  int capacity = 0;
  return c.intIn(sizeArg, "bufSize", 1, kMaxTextCapacity, &capacity) &&
      c.refString(bufArg, "buf", static_cast<size_t>(capacity), buf);
}

/// Reads ColorEdit4's flags at \p i, refusing two bits of one of its
/// three exclusive groups (widgets:5960, 5961; the picker's, widgets:6237).
bool colorEditFlags(Call &c, size_t i, int *flags) {
  if (!c.isMissing(i) && !c.flags(i, "flags", kColorEditFlags, flags))
    return false;
  if (moreThanOne(*flags, ImGuiColorEditFlags_DisplayMask_)) {
    c.throwRange(
        "flags must hold at most one of ColorEditFlags_DisplayRGB, "
        "_DisplayHSV and _DisplayHex");
    return false;
  }
  if (moreThanOne(*flags, ImGuiColorEditFlags_InputMask_)) {
    c.throwRange(
        "flags must hold at most one of ColorEditFlags_InputRGB and "
        "_InputHSV");
    return false;
  }
  if (moreThanOne(*flags, ImGuiColorEditFlags_PickerMask_)) {
    c.throwRange(
        "flags must hold at most one of ColorEditFlags_PickerHueBar and "
        "_PickerHueWheel");
    return false;
  }
  return true;
}

/// SliderBehavior's bounds for a float slider (widgets:3358); NaN cannot
/// reach here (f32 refuses it).
bool floatSliderBounds(Call &c, float vMin, float vMax) {
  if (vMin < -FLT_MAX / 2.0f || vMax > FLT_MAX / 2.0f) {
    c.throwRange("vMin and vMax must be within -FLT_MAX/2 .. FLT_MAX/2");
    return false;
  }
  return true;
}

/// `button(label, size = [0, 0])`, imgui.h:652.
napi_value button(napi_env env, napi_callback_info info) {
  Call c(env, info, "button");
  std::string label;
  ImVec2 size(0.0f, 0.0f);
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(1) && !c.vec2(1, "size", &size)) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::Button(label.c_str(), size);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(pressed);
}

/// `smallButton(label)`, imgui.h:653.
napi_value smallButton(napi_env env, napi_callback_info info) {
  Call c(env, info, "smallButton");
  std::string label;
  if (!c.ok() || !c.string(0, "label", &label) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::SmallButton(label.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.result(pressed);
}

/// `invisibleButton(strId, size, flags = 0)`, imgui.h:654.
napi_value invisibleButton(napi_env env, napi_callback_info info) {
  Call c(env, info, "invisibleButton");
  std::string strId;
  ImVec2 size;
  int flags = 0;
  if (!c.ok() || !c.string(0, "strId", &strId) || !c.vec2(1, "size", &size) ||
      (!c.isMissing(2) && !c.flags(2, "flags", kButtonFlags, &flags)) ||
      !c.needFrame() || !c.needLabelId("strId", strId))
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::InvisibleButton(strId.c_str(), size, flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(pressed);
}

/// `arrowButton(strId, dir)`, imgui.h:655.
napi_value arrowButton(napi_env env, napi_callback_info info) {
  Call c(env, info, "arrowButton");
  std::string strId;
  int dir = 0;
  if (!c.ok() || !c.string(0, "strId", &strId) ||
      !c.enumValue(1, "dir", kDir, &dir))
    return nullptr;
  // RenderArrow's IM_ASSERT(0) for the other two (draw:7593-7596).
  if (dir == ImGuiDir_None || dir == ImGuiDir_COUNT)
    return c.throwRange(
        "dir must be Dir_Left, Dir_Right, Dir_Up or Dir_Down; got %d", dir);
  if (!c.needFrame() || !c.needLabelId("strId", strId))
    return nullptr;
  c.enterImGui();
  const bool pressed =
      ImGui::ArrowButton(strId.c_str(), static_cast<ImGuiDir>(dir));
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
      !c.refBool(1, "v", false, &v) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::Checkbox(label.c_str(), v.ptr());
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `radioButton(label, active)` (imgui.h:659) or `radioButton(label, v,
/// vButton)` (imgui.h:660), dispatched on the second argument's type.
napi_value radioButton(napi_env env, napi_callback_info info) {
  Call c(env, info, "radioButton");
  std::string label;
  if (!c.ok() || !c.string(0, "label", &label))
    return nullptr;
  if (c.typeOf(1) == napi_boolean) {
    bool active = false;
    if (!c.boolean(1, "active", &active) || !c.needFrame() ||
        !c.needLabelId("label", label))
      return nullptr;
    c.enterImGui();
    const bool pressed = ImGui::RadioButton(label.c_str(), active);
    if (!c.leaveImGui())
      return nullptr;
    return c.result(pressed);
  }
  if (!c.isArray(1))
    return c.throwType(
        "ERR_INVALID_ARG_TYPE", "active must be a boolean, or v an Array");
  RefInt v;
  int vButton = 0;
  if (!c.refInt(1, "v", 1, false, &v) || !c.int32(2, "vButton", &vButton) ||
      !c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::RadioButton(label.c_str(), v.ptr(), vButton);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(pressed);
}

/// `progressBar(fraction, size = [-FLT_MIN, 0], overlay = null)`,
/// imgui.h:661.
napi_value progressBar(napi_env env, napi_callback_info info) {
  Call c(env, info, "progressBar");
  float fraction = 0.0f;
  ImVec2 size(-FLT_MIN, 0.0f);
  std::string overlay;
  bool noOverlay = true;
  if (!c.ok() || !c.f32(0, "fraction", &fraction) ||
      (!c.isMissing(1) && !c.vec2(1, "size", &size)) ||
      !c.stringOrNull(2, "overlay", &overlay, &noOverlay) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::ProgressBar(fraction, size, noOverlay ? nullptr : overlay.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `bullet()`, imgui.h:662.
napi_value bullet(napi_env env, napi_callback_info info) {
  Call c(env, info, "bullet");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::Bullet();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `sliderFloat(label, v, vMin, vMax, format = '%.3f', flags = 0)`,
/// imgui.h:716.
napi_value sliderFloat(napi_env env, napi_callback_info info) {
  Call c(env, info, "sliderFloat");
  std::string label;
  RefFloat v;
  float vMin = 0.0f;
  float vMax = 0.0f;
  std::string format = "%.3f";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "v", 1, false, &v) || !c.f32(2, "vMin", &vMin) ||
      !c.f32(3, "vMax", &vMax) ||
      (!c.isMissing(4) && !c.format(4, "format", FormatKind::Float, &format)) ||
      !sliderFlags(c, 5, true, &flags) || !floatSliderBounds(c, vMin, vMax) ||
      !c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::SliderFloat(
      label.c_str(), v.ptr(), vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `sliderInt(label, v, vMin, vMax, format = '%d', flags = 0)`,
/// imgui.h:721.
napi_value sliderInt(napi_env env, napi_callback_info info) {
  Call c(env, info, "sliderInt");
  std::string label;
  RefInt v;
  int vMin = 0;
  int vMax = 0;
  std::string format = "%d";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refInt(1, "v", 1, false, &v) || !c.int32(2, "vMin", &vMin) ||
      !c.int32(3, "vMax", &vMax) ||
      (!c.isMissing(4) && !c.format(4, "format", FormatKind::Int, &format)) ||
      !sliderFlags(c, 5, true, &flags))
    return nullptr;
  // SliderBehavior's bounds for S32 (widgets:3346).
  if (vMin < INT_MIN / 2 || vMax > INT_MAX / 2)
    return c.throwRange(
        "vMin and vMax must be within %d .. %d", INT_MIN / 2, INT_MAX / 2);
  if (!c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::SliderInt(
      label.c_str(), v.ptr(), vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// The SliderFloatN functions (imgui.h:717-719): one shape, `n`
/// components. SliderScalarN pushes the label and the component index on
/// the ID stack (widgets:3477, 3482), so no label-id check.
template <int N>
napi_value
sliderFloatN(napi_env env, napi_callback_info info, const char *name) {
  Call c(env, info, name);
  std::string label;
  RefFloat v;
  float vMin = 0.0f;
  float vMax = 0.0f;
  std::string format = "%.3f";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "v", N, false, &v) || !c.f32(2, "vMin", &vMin) ||
      !c.f32(3, "vMax", &vMax) ||
      (!c.isMissing(4) && !c.format(4, "format", FormatKind::Float, &format)) ||
      !sliderFlags(c, 5, true, &flags) || !floatSliderBounds(c, vMin, vMax) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  static_assert(N >= 2 && N <= 4, "SliderFloat2 .. SliderFloat4");
  bool changed = false;
  if constexpr (N == 2)
    changed = ImGui::SliderFloat2(
        label.c_str(), v.ptr(), vMin, vMax, format.c_str(), flags);
  else if constexpr (N == 3)
    changed = ImGui::SliderFloat3(
        label.c_str(), v.ptr(), vMin, vMax, format.c_str(), flags);
  else
    changed = ImGui::SliderFloat4(
        label.c_str(), v.ptr(), vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `sliderFloat2(label, v, vMin, vMax, format = '%.3f', flags = 0)`,
/// imgui.h:717.
napi_value sliderFloat2(napi_env env, napi_callback_info info) {
  return sliderFloatN<2>(env, info, "sliderFloat2");
}

/// `sliderFloat3(label, v, vMin, vMax, format = '%.3f', flags = 0)`,
/// imgui.h:718.
napi_value sliderFloat3(napi_env env, napi_callback_info info) {
  return sliderFloatN<3>(env, info, "sliderFloat3");
}

/// `sliderFloat4(label, v, vMin, vMax, format = '%.3f', flags = 0)`,
/// imgui.h:719.
napi_value sliderFloat4(napi_env env, napi_callback_info info) {
  return sliderFloatN<4>(env, info, "sliderFloat4");
}

/// `dragFloat(label, v, vSpeed = 1, vMin = 0, vMax = 0, format = '%.3f',
/// flags = 0)`, imgui.h:697.
napi_value dragFloat(napi_env env, napi_callback_info info) {
  Call c(env, info, "dragFloat");
  std::string label;
  RefFloat v;
  float vSpeed = 1.0f;
  float vMin = 0.0f;
  float vMax = 0.0f;
  std::string format = "%.3f";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "v", 1, false, &v) ||
      (!c.isMissing(2) && !c.f32(2, "vSpeed", &vSpeed)) ||
      (!c.isMissing(3) && !c.f32(3, "vMin", &vMin)) ||
      (!c.isMissing(4) && !c.f32(4, "vMax", &vMax)) ||
      (!c.isMissing(5) && !c.format(5, "format", FormatKind::Float, &format)) ||
      !sliderFlags(c, 6, false, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::DragFloat(
      label.c_str(), v.ptr(), vSpeed, vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `dragInt(label, v, vSpeed = 1, vMin = 0, vMax = 0, format = '%d',
/// flags = 0)`, imgui.h:702.
napi_value dragInt(napi_env env, napi_callback_info info) {
  Call c(env, info, "dragInt");
  std::string label;
  RefInt v;
  float vSpeed = 1.0f;
  int vMin = 0;
  int vMax = 0;
  std::string format = "%d";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refInt(1, "v", 1, false, &v) ||
      (!c.isMissing(2) && !c.f32(2, "vSpeed", &vSpeed)) ||
      (!c.isMissing(3) && !c.int32(3, "vMin", &vMin)) ||
      (!c.isMissing(4) && !c.int32(4, "vMax", &vMax)) ||
      (!c.isMissing(5) && !c.format(5, "format", FormatKind::Int, &format)) ||
      !sliderFlags(c, 6, false, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::DragInt(
      label.c_str(), v.ptr(), vSpeed, vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `inputText(label, buf, bufSize, flags = 0)`, imgui.h:734; `callback`
/// and `user_data` omitted. `buf` is written back whenever the buffer
/// differs from the string passed in, whatever ImGui returned (with
/// InputTextFlags_EnterReturnsTrue it edits on every keystroke and
/// returns true only on Enter).
napi_value inputText(napi_env env, napi_callback_info info) {
  Call c(env, info, "inputText");
  std::string label;
  RefString buf;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) || !textBuffer(c, 1, 2, &buf) ||
      !inputTextFlags(c, 3, "flags", false, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed =
      ImGui::InputText(label.c_str(), buf.data(), buf.capacity(), flags);
  if (!c.leaveImGui() || !c.write(buf))
    return nullptr;
  return c.result(changed);
}

/// `inputTextMultiline(label, buf, bufSize, size = [0, 0], flags = 0)`,
/// imgui.h:735. Each component of `size` is bounded below 2^31: the field
/// is a child window of the frame's size (InputTextEx, widgets:4821,
/// 4856), whose positive components SetWindowSize converts to int
/// (IM_TRUNC, imgui.cpp:9361, 9365). A zero width is the item width,
/// bounded by setNextItemWidth() and pushItemWidth(); a negative component
/// becomes max(4, avail + size) (CalcItemSize, imgui.cpp:12615-12622).
napi_value inputTextMultiline(napi_env env, napi_callback_info info) {
  Call c(env, info, "inputTextMultiline");
  std::string label;
  RefString buf;
  ImVec2 size(0.0f, 0.0f);
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) || !textBuffer(c, 1, 2, &buf) ||
      (!c.isMissing(3) && !c.vec2(3, "size", &size)) ||
      !c.belowIntLimit(size.x, "size[0]") ||
      !c.belowIntLimit(size.y, "size[1]") ||
      !inputTextFlags(c, 4, "flags", true, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::InputTextMultiline(
      label.c_str(), buf.data(), buf.capacity(), size, flags);
  if (!c.leaveImGui() || !c.write(buf))
    return nullptr;
  return c.result(changed);
}

/// `inputTextWithHint(label, hint, buf, bufSize, flags = 0)`,
/// imgui.h:736.
napi_value inputTextWithHint(napi_env env, napi_callback_info info) {
  Call c(env, info, "inputTextWithHint");
  std::string label;
  std::string hint;
  RefString buf;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) || !c.string(1, "hint", &hint) ||
      !textBuffer(c, 2, 3, &buf) ||
      !inputTextFlags(c, 4, "flags", false, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::InputTextWithHint(
      label.c_str(), hint.c_str(), buf.data(), buf.capacity(), flags);
  if (!c.leaveImGui() || !c.write(buf))
    return nullptr;
  return c.result(changed);
}

/// `inputFloat(label, v, step = 0, stepFast = 0, format = '%.3f', flags =
/// 0)`, imgui.h:737.
napi_value inputFloat(napi_env env, napi_callback_info info) {
  Call c(env, info, "inputFloat");
  std::string label;
  RefFloat v;
  float step = 0.0f;
  float stepFast = 0.0f;
  std::string format = "%.3f";
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "v", 1, false, &v) ||
      (!c.isMissing(2) && !c.f32(2, "step", &step)) ||
      (!c.isMissing(3) && !c.f32(3, "stepFast", &stepFast)) ||
      (!c.isMissing(4) && !c.format(4, "format", FormatKind::Float, &format)) ||
      !inputTextFlags(c, 5, "flags", false, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::InputFloat(
      label.c_str(), v.ptr(), step, stepFast, format.c_str(), flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `inputInt(label, v, step = 1, stepFast = 100, flags = 0)`,
/// imgui.h:741.
napi_value inputInt(napi_env env, napi_callback_info info) {
  Call c(env, info, "inputInt");
  std::string label;
  RefInt v;
  int step = 1;
  int stepFast = 100;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refInt(1, "v", 1, false, &v) ||
      (!c.isMissing(2) && !c.int32(2, "step", &step)) ||
      (!c.isMissing(3) && !c.int32(3, "stepFast", &stepFast)) ||
      !inputTextFlags(c, 4, "flags", false, &flags) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed =
      ImGui::InputInt(label.c_str(), v.ptr(), step, stepFast, flags);
  if (!c.leaveImGui() || !c.write(v))
    return nullptr;
  return c.result(changed);
}

/// `beginCombo(label, previewValue, flags = 0)`, imgui.h:679.
napi_value beginCombo(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginCombo");
  std::string label;
  std::string preview;
  bool noPreview = true;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.stringOrNull(1, "previewValue", &preview, &noPreview) ||
      (!c.isMissing(2) && !c.flags(2, "flags", kComboFlags, &flags)))
    return nullptr;
  // BeginCombo's and BeginComboPopup's plain assertions (widgets:1959,
  // 1961, 2040).
  if ((flags & ImGuiComboFlags_NoPreview) &&
      (flags &
       (ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_WidthFitPreview)))
    return c.throwRange(
        "flags: ComboFlags_NoPreview cannot be combined with "
        "ComboFlags_NoArrowButton or ComboFlags_WidthFitPreview");
  if (moreThanOne(flags, ImGuiComboFlags_HeightMask_))
    return c.throwRange(
        "flags must hold at most one of ComboFlags_HeightSmall, "
        "_HeightRegular, _HeightLarge and _HeightLargest");
  if (!c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool open = ImGui::BeginCombo(
      label.c_str(), noPreview ? nullptr : preview.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(open);
}

/// `endCombo()`, imgui.h:680.
napi_value endCombo(napi_env env, napi_callback_info info) {
  Call c(env, info, "endCombo");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndCombo())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndCombo();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `selectable(label, selected = false, flags = 0, size = [0, 0])`
/// (imgui.h:783) or `selectable(label, pSelected, flags = 0, size = [0,
/// 0])` (imgui.h:784), dispatched on the second argument's type.
napi_value selectable(napi_env env, napi_callback_info info) {
  Call c(env, info, "selectable");
  std::string label;
  if (!c.ok() || !c.string(0, "label", &label))
    return nullptr;
  const bool byRef = c.isArray(1);
  bool selected = false;
  RefBool pSelected;
  if (byRef) {
    if (!c.refBool(1, "pSelected", false, &pSelected))
      return nullptr;
  } else if (c.isMissing(1) || c.isNull(1)) {
    selected = false;
  } else if (c.typeOf(1) == napi_boolean) {
    if (!c.boolean(1, "selected", &selected))
      return nullptr;
  } else {
    return c.throwType(
        "ERR_INVALID_ARG_TYPE",
        "selected must be a boolean, or pSelected an Array");
  }
  int flags = 0;
  ImVec2 size(0.0f, 0.0f);
  if ((!c.isMissing(2) && !c.flags(2, "flags", kSelectableFlags, &flags)) ||
      (!c.isMissing(3) && !c.vec2(3, "size", &size)) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool pressed = byRef
      ? ImGui::Selectable(label.c_str(), pSelected.ptr(), flags, size)
      : ImGui::Selectable(label.c_str(), selected, flags, size);
  if (!c.leaveImGui() || !c.write(pSelected))
    return nullptr;
  return c.result(pressed);
}

/// `colorEdit3(label, col, flags = 0)`, imgui.h:752. ColorEdit4 pushes the
/// label on the ID stack (widgets:5935-5936), so no label-id check.
napi_value colorEdit3(napi_env env, napi_callback_info info) {
  Call c(env, info, "colorEdit3");
  std::string label;
  RefFloat col;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "col", 3, false, &col) || !colorEditFlags(c, 2, &flags) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::ColorEdit3(label.c_str(), col.ptr(), flags);
  if (!c.leaveImGui() || !c.write(col))
    return nullptr;
  return c.result(changed);
}

/// `colorEdit4(label, col, flags = 0)`, imgui.h:753.
napi_value colorEdit4(napi_env env, napi_callback_info info) {
  Call c(env, info, "colorEdit4");
  std::string label;
  RefFloat col;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "col", 4, false, &col) || !colorEditFlags(c, 2, &flags) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::ColorEdit4(label.c_str(), col.ptr(), flags);
  if (!c.leaveImGui() || !c.write(col))
    return nullptr;
  return c.result(changed);
}

/// `colorButton(descId, col, flags = 0, size = [0, 0])`, imgui.h:756.
napi_value colorButton(napi_env env, napi_callback_info info) {
  Call c(env, info, "colorButton");
  std::string descId;
  ImVec4 col;
  int flags = 0;
  ImVec2 size(0.0f, 0.0f);
  if (!c.ok() || !c.string(0, "descId", &descId) || !c.vec4(1, "col", &col) ||
      (!c.isMissing(2) && !c.flags(2, "flags", kColorEditFlags, &flags)) ||
      (!c.isMissing(3) && !c.vec2(3, "size", &size)) || !c.needFrame() ||
      !c.needLabelId("descId", descId))
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::ColorButton(descId.c_str(), col, flags, size);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(pressed);
}

/// `beginListBox(label, size = [0, 0])`, imgui.h:805.
napi_value beginListBox(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginListBox");
  std::string label;
  ImVec2 size(0.0f, 0.0f);
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(1) && !c.vec2(1, "size", &size)) || !c.needFrame() ||
      !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool shown = ImGui::BeginListBox(label.c_str(), size);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(shown);
}

/// `endListBox()`, imgui.h:806.
napi_value endListBox(napi_env env, napi_callback_info info) {
  Call c(env, info, "endListBox");
  if (!c.ok() || !c.needFrame())
    return nullptr;
  if (const char *msg = guard::canEndListBox())
    return c.throwUsage("%s", msg);
  c.enterImGui();
  ImGui::EndListBox();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerWidgets(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "button", button) &&
      registerFunction(env, exports, "smallButton", smallButton) &&
      registerFunction(env, exports, "invisibleButton", invisibleButton) &&
      registerFunction(env, exports, "arrowButton", arrowButton) &&
      registerFunction(env, exports, "checkbox", checkbox) &&
      registerFunction(env, exports, "radioButton", radioButton) &&
      registerFunction(env, exports, "progressBar", progressBar) &&
      registerFunction(env, exports, "bullet", bullet) &&
      registerFunction(env, exports, "sliderFloat", sliderFloat) &&
      registerFunction(env, exports, "sliderInt", sliderInt) &&
      registerFunction(env, exports, "sliderFloat2", sliderFloat2) &&
      registerFunction(env, exports, "sliderFloat3", sliderFloat3) &&
      registerFunction(env, exports, "sliderFloat4", sliderFloat4) &&
      registerFunction(env, exports, "dragFloat", dragFloat) &&
      registerFunction(env, exports, "dragInt", dragInt) &&
      registerFunction(env, exports, "inputText", inputText) &&
      registerFunction(
             env, exports, "inputTextMultiline", inputTextMultiline) &&
      registerFunction(env, exports, "inputTextWithHint", inputTextWithHint) &&
      registerFunction(env, exports, "inputFloat", inputFloat) &&
      registerFunction(env, exports, "inputInt", inputInt) &&
      registerFunction(env, exports, "beginCombo", beginCombo) &&
      registerFunction(env, exports, "endCombo", endCombo) &&
      registerFunction(env, exports, "selectable", selectable) &&
      registerFunction(env, exports, "colorEdit3", colorEdit3) &&
      registerFunction(env, exports, "colorEdit4", colorEdit4) &&
      registerFunction(env, exports, "colorButton", colorButton) &&
      registerFunction(env, exports, "beginListBox", beginListBox) &&
      registerFunction(env, exports, "endListBox", endListBox);
}

} // namespace bind
} // namespace hnimgui
