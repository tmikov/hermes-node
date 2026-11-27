/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Plots group (surface table, "Plots"): the values come as an Array
// (copied) or a Float32Array (used in place), with no count and no stride
// (design, "Exceptions: Plots" and "Buffers").

#include "bind_util.h"

#include <cfloat>
#include <climits>

namespace hnimgui {
namespace bind {

namespace {

/// The most values a plot takes. PlotEx steps over `res_w =
/// min((int)frame_size.x, values_count)` columns (minus one for lines),
/// accumulating `t0 += 1.0f / res_w` in float, and for each takes
/// `v1_idx = (int)(t0 * item_count + 0.5f)`, then asserts plainly that
/// `v1_idx < values_count` (imgui_widgets.cpp:9133-9151). The float error
/// of t0 grows with res_w, and the index reaches values_count once res_w is
/// large: the first failing case, computed by running that arithmetic for
/// every res_w and count, is a histogram of 5921 values in a frame at
/// least 5921 wide (10123 for lines), with or without a fused multiply-add.
/// res_w is at most the count, and every (res_w, count) with count <= 4096
/// was run and holds, so 4096 values keep the assertion true for any frame
/// width. The other arithmetic, `index + values_offset + 1` (9151), stays
/// far inside `int` at that size.
constexpr size_t kMaxPlotValues = 4096;
/// PlotEx converts the frame's width to int (`(int)frame_size.x`,
/// imgui_widgets.cpp:9116), undefined at 2^31 or more. A positive
/// graphSize.x is that width (CalcItemSize, imgui.cpp:12609-12625). A zero
/// or negative one comes from the item width, whose own conversion in
/// CalcItemWidth (IM_TRUNC, imgui.cpp:12601) has the same limit and is
/// setNextItemWidth()'s and pushItemWidth()'s to bound, not this
/// function's.
constexpr float kMaxGraphWidth = 2147483648.0f;

enum class PlotKind { Lines, Histogram };

/// `plotLines` / `plotHistogram(label, values, valuesOffset = 0,
/// overlayText = null, scaleMin = FLT_MAX, scaleMax = FLT_MAX, graphSize =
/// [0, 0])`, imgui.h:812 and 814, `stride` passed as sizeof(float).
napi_value plot(napi_env env, napi_callback_info info, PlotKind kind) {
  Call c(env, info, kind == PlotKind::Lines ? "plotLines" : "plotHistogram");
  std::string label, overlay;
  bool noOverlay = true;
  int offset = 0;
  float scaleMin = FLT_MAX, scaleMax = FLT_MAX;
  ImVec2 graphSize(0, 0);
  FloatSpan values;
  if (!c.ok() || !c.string(0, "label", &label) ||
      (!c.isMissing(2) && !c.int32(2, "valuesOffset", &offset)) ||
      !c.stringOrNull(3, "overlayText", &overlay, &noOverlay) ||
      (!c.isMissing(4) && !c.f32(4, "scaleMin", &scaleMin)) ||
      (!c.isMissing(5) && !c.f32(5, "scaleMax", &scaleMax)) ||
      (!c.isMissing(6) && !c.vec2(6, "graphSize", &graphSize)) ||
      // Last: a Float32Array's data is borrowed from here to the call.
      !c.floats(1, "values", kMaxPlotValues, &values))
    return nullptr;
  if (!(graphSize.x < kMaxGraphWidth))
    return c.throwRange(
        "graphSize[0] must be less than 2147483648; got %g",
        static_cast<double>(graphSize.x));

  // PlotEx reads `(i + values_offset) % values_count` without a check on
  // the offset (imgui_widgets.cpp:9126-9127, 9138, 9151), so it must be a
  // valid index (design, "Buffers").
  if (values.count == 0 && offset != 0)
    return c.throwRange(
        "valuesOffset must be 0 for an empty values; got %d", offset);
  if (values.count != 0 &&
      (offset < 0 || static_cast<size_t>(offset) >= values.count))
    return c.throwRange(
        "valuesOffset must be an integer from 0 to %zu; got %d",
        values.count - 1,
        offset);
  if (!c.needFrame() || !c.needLabelId("label", label))
    return nullptr;

  const char *overlayText = noOverlay ? nullptr : overlay.c_str();
  const int count = static_cast<int>(values.count);
  c.enterImGui();
  if (kind == PlotKind::Lines)
    ImGui::PlotLines(
        label.c_str(),
        values.data,
        count,
        offset,
        overlayText,
        scaleMin,
        scaleMax,
        graphSize,
        sizeof(float));
  else
    ImGui::PlotHistogram(
        label.c_str(),
        values.data,
        count,
        offset,
        overlayText,
        scaleMin,
        scaleMax,
        graphSize,
        sizeof(float));
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

napi_value plotLines(napi_env env, napi_callback_info info) {
  return plot(env, info, PlotKind::Lines);
}

napi_value plotHistogram(napi_env env, napi_callback_info info) {
  return plot(env, info, PlotKind::Histogram);
}

} // namespace

bool registerPlots(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "plotLines", plotLines) &&
      registerFunction(env, exports, "plotHistogram", plotHistogram);
}

} // namespace bind
} // namespace hnimgui
