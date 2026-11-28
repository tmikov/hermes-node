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

#include <algorithm>
#include <cfloat>
#include <climits>
#include <string>
#include <utility>

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

/// The largest RGB component a colour editor takes. ColorEdit4 converts
/// every component it shows to int with IM_F32_TO_INT8_UNBOUND,
/// `(int)(v * 255.0f + 0.5f)` (imgui_widgets.cpp:5980,
/// imgui_internal.h:296), whatever the flags (the Float and Hex displays
/// included, and HDR, which lifts only the editor's own 0..1 clamp), and
/// that is defined while |v| * 255 + 0.5 < 2^31, |v| < 8421504.25; 2^23 is
/// the power of two below.
constexpr float kMaxRgbComponent = 8388608.0f;
/// The largest HSV component magnitude. With InputHSV and an RGB display
/// the components are converted by ColorConvertHSVtoRGB first
/// (imgui_widgets.cpp:5972-5973), whose outputs are `v`, `v * (1 - s)`,
/// `v * (1 - s * f)` and `v * (1 - s * (1 - f))` with f in (-1, 1)
/// (imgui.cpp:3037-3062): at |s|, |v| <= 2048 each is at most 2048 * 2049
/// = 4196352 in magnitude, inside kMaxRgbComponent.
constexpr float kMaxHsvComponent = 2048.0f;

/// The colour editors' component domain, checked on `col` (\p n of them)
/// for \p flags; inside a frame. The input is the flags' InputRGB or
/// InputHSV, else io.ConfigColorEditFlags' (imgui_widgets.cpp:5957-5958),
/// which no bound function changes and ColorEditOptionsPopup() never
/// touches (it sets the display and the data type only, 6742-6754):
/// - RGB: 0 to kMaxRgbComponent. Not negative because the HSV display
///   converts with ColorConvertRGBtoHSV (5974-5978), whose saturation is
///   `chroma / (r + 1e-20f)` (imgui.cpp:3031), r being the largest
///   component: at most 1 when no component is negative, unbounded
///   otherwise (r = 0 and a component of -1 give 1e20), and the user can
///   select that display from the editor's own options menu during the
///   call (6745-6747, called at 5948 and read back at 5951-5952) unless the
///   flags fix one.
/// - HSV: -kMaxHsvComponent to kMaxHsvComponent. ColorConvertHSVtoRGB's
///   fractional part f is in (-1, 1), negative for a negative hue
///   (imgui.cpp:3046-3049), so its factors `1 - s * f` and
///   `1 - s * (1 - f)` are at most 1 + 2 * 2048 in magnitude and no product
///   overflows. With s unbounded, `s * (1 - f)` overflows for f < 0 and
///   times a zero v gives NaN (3050-3055), which ImSaturate keeps
///   (imgui_internal.h:552) and ColorConvertFloat4ToU32 converts to int
///   (imgui.cpp:3006-3009): Codex's colorButton case (round 2, D2), which is
///   why colorButton takes the same domain.
/// The editor's own DragInt on `255 * component` (5980, 6032) then adds a
/// drag's accumulator at speed 1, below 1 + 10 * 2^17 (dragIntDomainOk()),
/// to at most 255 * 2^23 = 2139095040: still below 2^31.
/// The domain colorComponentsOk() checks: [*lo, *hi] for \p flags.
void colorDomain(int flags, float *lo, float *hi) {
  int input = flags & ImGuiColorEditFlags_InputMask_;
  if (input == 0)
    input =
        ImGui::GetIO().ConfigColorEditFlags & ImGuiColorEditFlags_InputMask_;
  const bool hsv = (input & ImGuiColorEditFlags_InputHSV) != 0;
  *lo = hsv ? -kMaxHsvComponent : 0.0f;
  *hi = hsv ? kMaxHsvComponent : kMaxRgbComponent;
}

/// The domain checked on \p n components \p v of `col`, for the colour
/// editors and colorButton alike.
bool colorComponentsOk(Call &c, const float *v, int n, int flags) {
  float lo = 0.0f, hi = 0.0f;
  colorDomain(flags, &lo, &hi);
  for (int k = 0; k < n; ++k) {
    const std::string what = "col[" + std::to_string(k) + "]";
    if (!c.inRange(v[k], what.c_str(), lo, hi))
      return false;
  }
  return true;
}

/// Clamps what a colour editor wrote back into the domain the next call
/// accepts (the surface table's general rule G6): an HDR drag below 0, a
/// saturation dragged past 1 in the HSV display, or a value typed in,
/// stops at the domain's edge. The editor reads `col` afresh every frame
/// (imgui_widgets.cpp:5971) and converts it then, so the clamp is what it
/// converts next. What ImGui keeps between frames, the saved hue and
/// saturation, is brought into its own domain before each editor call
/// (guard::sanitizeColorEditState()).
void clampColor(RefFloat &col, int flags) {
  float lo = 0.0f, hi = 0.0f;
  colorDomain(flags, &lo, &hi);
  for (int k = 0; k < col.n; ++k)
    col.values[k] = std::clamp(col.values[k], lo, hi);
}

/// The largest magnitude of a dragInt value or bound: 2^30 - 1.
constexpr int kMaxDragIntValue = (1 << 30) - 1;
/// The largest magnitude of a dragInt speed, given or automatic: the
/// largest whole number S with 1 + 10 * 2^17 * S < 2^30 (below).
constexpr float kMaxDragIntSpeed = 819.0f;

/// dragInt's domain, which keeps DragBehaviorT<ImS32> (imgui_widgets.cpp:
/// 2551-2678) inside int arithmetic. It adds the accumulated delta to the
/// value as `v_cur += (SIGNEDTYPE)g.DragCurrentAccum` (2631), converting
/// the float accumulator and adding before any clamp, and computes
/// `v_max - v_min` in int for the automatic speed (2562-2563), the
/// logarithmic scale (2593) and wrapping, with one more (2659-2661).
/// - The accumulator. Each frame adds the mouse delta times the speed
///   (2567-2584), times 10 with Shift and 0.01 with Alt, and the flush
///   leaves only the fraction ImGui could not apply (2648), or resets it
///   (2598-2602). The mouse delta is the difference of two positions
///   (imgui.cpp:11126-11129) the platform half clamps to
///   +-kMaxMouseCoordinate (imgui_addon.h), at most 2^17, so the
///   accumulator is below 1 + 10 * 2^17 * |speed|, which at the speed
///   bound is 1073479681 < 2^30. The keyboard and gamepad source (2575-
///   2583) is the surface table's "Not covered" 3i.
/// - The value. It is within +-(2^30 - 1), so the sum stays inside int.
/// - The bounds. Each is within +-(2^30 - 1), so `v_max - v_min + 1` is
///   at most 2^31 - 1.
/// - The speed: |vSpeed| <= kMaxDragIntSpeed. With vSpeed 0 on a bounded
///   drag (ImGui's test, 2556) ImGui would use `(v_max - v_min) *
///   g.DragSpeedDefaultRatio` (2562-2563; 1/100, imgui.cpp:4561, which no
///   bound function changes); the binding computes that speed itself and,
///   when it is above the bound, passes the bound instead (\p speed), so
///   a wide range drags at 819 per unit rather than being refused. Below
///   the bound it passes 0, leaving ImGui's own computation untouched.
bool dragIntDomainOk(
    Call &c,
    int v,
    float vSpeed,
    int vMin,
    int vMax,
    int flags,
    float *speed) {
  if (v < -kMaxDragIntValue || v > kMaxDragIntValue) {
    c.throwRange(
        "v[0] must be from %d to %d; got %d",
        -kMaxDragIntValue,
        kMaxDragIntValue,
        v);
    return false;
  }
  for (const auto &[bound, what] :
       {std::pair<int, const char *>{vMin, "vMin"}, {vMax, "vMax"}}) {
    if (bound < -kMaxDragIntValue || bound > kMaxDragIntValue) {
      c.throwRange(
          "%s must be from %d to %d; got %d",
          what,
          -kMaxDragIntValue,
          kMaxDragIntValue,
          bound);
      return false;
    }
  }
  if (!c.inRange(vSpeed, "vSpeed", -kMaxDragIntSpeed, kMaxDragIntSpeed))
    return false;
  *speed = vSpeed;
  const bool bounded = vMin < vMax ||
      (vMin == vMax &&
       (vMin != 0 || (flags & ImGuiSliderFlags_ClampZeroRange)));
  if (vSpeed == 0.0f && bounded) {
    const float automatic = static_cast<float>((vMax - vMin) * (1.0f / 100.0f));
    if (!(automatic <= kMaxDragIntSpeed))
      *speed = kMaxDragIntSpeed;
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
  float speed = 1.0f;
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
      !sliderFlags(c, 6, false, &flags) ||
      !dragIntDomainOk(c, v.values[0], vSpeed, vMin, vMax, flags, &speed) ||
      !c.needFrame() || !c.needLabelId("label", label))
    return nullptr;
  c.enterImGui();
  const bool changed = ImGui::DragInt(
      label.c_str(), v.ptr(), speed, vMin, vMax, format.c_str(), flags);
  if (!c.leaveImGui())
    return nullptr;
  // Written back within the domain the next call accepts (the surface
  // table's general rule G6): an unbounded drag, or a number typed with
  // Ctrl+click, stops at +-(2^30 - 1). Clamping the array between frames
  // keeps ImGui's arithmetic defined: the drag carries over only the
  // accumulator's fraction (imgui_widgets.cpp:2648) and re-reads the value
  // from the array every frame (2612), and the text entry is re-parsed
  // from its own buffer into the array (2416).
  v.values[0] = std::clamp(v.values[0], -kMaxDragIntValue, kMaxDragIntValue);
  if (!c.write(v))
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
/// The bound covers the argument, not the sum: the child is begun with
/// `frame_bb.GetSize()`, `fl(cursor + size) - cursor` (widgets:4824,
/// 4856), which rounds up to 2^31 for an accepted size near the limit
/// with the cursor at 64 or more ("Not covered" 3f). No lower bound makes
/// that sum safe for every cursor, since the cursor itself is unbounded.
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
      !c.needFrame() || !colorComponentsOk(c, col.values, col.n, flags))
    return nullptr;
  guard::sanitizeColorEditState();
  c.enterImGui();
  const bool changed = ImGui::ColorEdit3(label.c_str(), col.ptr(), flags);
  if (!c.leaveImGui())
    return nullptr;
  clampColor(col, flags);
  if (!c.write(col))
    return nullptr;
  return c.result(changed);
}

/// `colorEdit4(label, col, flags = 0)`, imgui.h:753. Both editors check
/// `col` with colorComponentsOk().
napi_value colorEdit4(napi_env env, napi_callback_info info) {
  Call c(env, info, "colorEdit4");
  std::string label;
  RefFloat col;
  int flags = 0;
  if (!c.ok() || !c.string(0, "label", &label) ||
      !c.refFloat(1, "col", 4, false, &col) || !colorEditFlags(c, 2, &flags) ||
      !c.needFrame() || !colorComponentsOk(c, col.values, col.n, flags))
    return nullptr;
  guard::sanitizeColorEditState();
  c.enterImGui();
  const bool changed = ImGui::ColorEdit4(label.c_str(), col.ptr(), flags);
  if (!c.leaveImGui())
    return nullptr;
  clampColor(col, flags);
  if (!c.write(col))
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
  // The editors' domain (colorComponentsOk()): with InputHSV ColorButton
  // converts through ColorConvertHSVtoRGB (imgui_widgets.cpp:6610-6611).
  const float components[4] = {col.x, col.y, col.z, col.w};
  if (!colorComponentsOk(c, components, 4, flags))
    return nullptr;
  c.enterImGui();
  const bool pressed = ImGui::ColorButton(descId.c_str(), col, flags, size);
  if (!c.leaveImGui())
    return nullptr;
  return c.result(pressed);
}

/// `beginListBox(label, size = [0, 0])`, imgui.h:805. Each component of
/// `size` is bounded below 2^31: BeginListBox truncates the size with
/// ImTrunc (widgets:8965), and a positive component is the argument as it
/// is (CalcItemSize, imgui.cpp:12615-12622; a negative one becomes
/// max(4, avail + size), zero the default). Its child window is then begun
/// with `frame_bb.GetSize()`, `fl(cursor + size) - cursor` (widgets:8967,
/// 8989), which can round up to 2^31 for an accepted size: a sum, as in
/// "Not covered" 3f.
napi_value beginListBox(napi_env env, napi_callback_info info) {
  Call c(env, info, "beginListBox");
  std::string label;
  ImVec2 size(0.0f, 0.0f);
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(1) && !c.vec2(1, "size", &size)) ||
      !c.belowIntLimit(size.x, "size[0]") ||
      !c.belowIntLimit(size.y, "size[1]") || !c.needFrame() ||
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
