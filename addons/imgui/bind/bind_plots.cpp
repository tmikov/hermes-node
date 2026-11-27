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

/// The most values a plot takes. PlotEx reads value
/// `(index + values_offset + 1) % values_count` (imgui_widgets.cpp:9151),
/// with `index` and `values_offset` each below the count, so the sum stays
/// within `int` only while the count is at most INT_MAX / 2.
constexpr size_t kMaxPlotValues = INT_MAX / 2;

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
      !c.floats(1, "values", &values))
    return nullptr;
  if (values.count > kMaxPlotValues)
    return c.throwRange(
        "values must hold at most %zu numbers; got %zu",
        kMaxPlotValues,
        values.count);
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
