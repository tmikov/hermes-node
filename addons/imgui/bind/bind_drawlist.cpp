/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Draw list group (surface table, "Draw list"): functions on the
// object `imgui.drawList`, each acting on the current window's draw list,
// ImGui::GetWindowDrawList() (imgui.h:482; design, "Exceptions: Draw
// list"). Each is named `drawList.<name>` in its messages and in
// __test.bindCalls(). Colours are ImU32 numbers (G3).

#include "bind_util.h"

namespace hnimgui {
namespace bind {

namespace {

/// The most points addPolyline takes. `_AddPolyline` reserves up to four
/// vertices per point plus two for a closed line (imgui_draw.cpp:1040-1049)
/// in one primitive, which PrimReserve() does not split
/// (imgui_draw.cpp:768-775), and Render() asserts plainly that a command's
/// vertices fit the 16-bit index (imgui_draw.cpp:3554): 16383 * 4 + 2 =
/// 65534 (surface table, ruling 14).
constexpr size_t kMaxPolylinePoints = 16383;
/// The most segments addBezierCubic takes: each pushes one path point
/// (imgui_draw.cpp:1812-1814), which `_AddPolyline` then strokes, so the
/// polyline's bound holds with room to spare. ImGui's own circle maximum,
/// IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_MAX (imgui_internal.h:924).
constexpr int kMaxBezierSegments = 512;

/// `drawList.addLine(p1, p2, col, thickness = 1, flags = 0)`,
/// imgui.h:3586.
napi_value addLine(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addLine");
  ImVec2 p1, p2;
  ImU32 col = 0;
  float thickness = 1.0f;
  int flags = 0;
  if (!c.ok() || !c.vec2(0, "p1", &p1) || !c.vec2(1, "p2", &p2) ||
      !c.u32(2, "col", &col) ||
      (!c.isMissing(3) && !c.f32(3, "thickness", &thickness)) ||
      (!c.isMissing(4) && !c.flags(4, "flags", kDrawFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddLine(p1, p2, col, thickness, flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addRect(pMin, pMax, col, rounding = 0, thickness = 1, flags =
/// 0)`, imgui.h:3589.
napi_value addRect(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addRect");
  ImVec2 pMin, pMax;
  ImU32 col = 0;
  float rounding = 0.0f, thickness = 1.0f;
  int flags = 0;
  if (!c.ok() || !c.vec2(0, "pMin", &pMin) || !c.vec2(1, "pMax", &pMax) ||
      !c.u32(2, "col", &col) ||
      (!c.isMissing(3) && !c.f32(3, "rounding", &rounding)) ||
      (!c.isMissing(4) && !c.f32(4, "thickness", &thickness)) ||
      (!c.isMissing(5) && !c.flags(5, "flags", kDrawFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddRect(
      pMin, pMax, col, rounding, thickness, flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addRectFilled(pMin, pMax, col, rounding = 0, flags = 0)`,
/// imgui.h:3590.
napi_value addRectFilled(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addRectFilled");
  ImVec2 pMin, pMax;
  ImU32 col = 0;
  float rounding = 0.0f;
  int flags = 0;
  if (!c.ok() || !c.vec2(0, "pMin", &pMin) || !c.vec2(1, "pMax", &pMax) ||
      !c.u32(2, "col", &col) ||
      (!c.isMissing(3) && !c.f32(3, "rounding", &rounding)) ||
      (!c.isMissing(4) && !c.flags(4, "flags", kDrawFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddRectFilled(pMin, pMax, col, rounding, flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addCircle(center, radius, col, numSegments = 0, thickness =
/// 1, flags = 0)`, imgui.h:3596. ImGui clamps numSegments itself
/// (imgui_draw.cpp:2747).
napi_value addCircle(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addCircle");
  ImVec2 center;
  float radius = 0.0f, thickness = 1.0f;
  ImU32 col = 0;
  int numSegments = 0, flags = 0;
  if (!c.ok() || !c.vec2(0, "center", &center) ||
      !c.f32(1, "radius", &radius) || !c.u32(2, "col", &col) ||
      (!c.isMissing(3) && !c.int32(3, "numSegments", &numSegments)) ||
      (!c.isMissing(4) && !c.f32(4, "thickness", &thickness)) ||
      (!c.isMissing(5) && !c.flags(5, "flags", kDrawFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddCircle(
      center, radius, col, numSegments, thickness, flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addCircleFilled(center, radius, col, numSegments = 0)`,
/// imgui.h:3597; numSegments clamped by ImGui (imgui_draw.cpp:2791).
napi_value addCircleFilled(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addCircleFilled");
  ImVec2 center;
  float radius = 0.0f;
  ImU32 col = 0;
  int numSegments = 0;
  if (!c.ok() || !c.vec2(0, "center", &center) ||
      !c.f32(1, "radius", &radius) || !c.u32(2, "col", &col) ||
      (!c.isMissing(3) && !c.int32(3, "numSegments", &numSegments)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, col, numSegments);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addTriangleFilled(p1, p2, p3, col)`, imgui.h:3595.
napi_value addTriangleFilled(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addTriangleFilled");
  ImVec2 p1, p2, p3;
  ImU32 col = 0;
  if (!c.ok() || !c.vec2(0, "p1", &p1) || !c.vec2(1, "p2", &p2) ||
      !c.vec2(2, "p3", &p3) || !c.u32(3, "col", &col) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddTriangleFilled(p1, p2, p3, col);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addBezierCubic(p1, p2, p3, p4, col, thickness, numSegments =
/// 0, flags = 0)`, imgui.h:3604. numSegments is not clamped by ImGui
/// (imgui_draw.cpp:1812-1814), so the binding bounds it.
napi_value addBezierCubic(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addBezierCubic");
  ImVec2 p1, p2, p3, p4;
  ImU32 col = 0;
  float thickness = 0.0f;
  int numSegments = 0, flags = 0;
  if (!c.ok() || !c.vec2(0, "p1", &p1) || !c.vec2(1, "p2", &p2) ||
      !c.vec2(2, "p3", &p3) || !c.vec2(3, "p4", &p4) ||
      !c.u32(4, "col", &col) || !c.f32(5, "thickness", &thickness) ||
      (!c.isMissing(6) &&
       !c.intIn(6, "numSegments", 0, kMaxBezierSegments, &numSegments)) ||
      (!c.isMissing(7) && !c.flags(7, "flags", kDrawFlags, &flags)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddBezierCubic(
      p1, p2, p3, p4, col, thickness, numSegments, flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addPolyline(points, col, thickness, flags = 0)`, imgui.h:3610:
/// `points` is one flat sequence `[x0, y0, x1, y1, ...]` (design,
/// "Exceptions: Point lists"). ImVec2 is two floats, so the sequence's
/// floats are the `const ImVec2*` ImGui reads, borrowed from a
/// Float32Array as a plot's values are.
napi_value addPolyline(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addPolyline");
  ImU32 col = 0;
  float thickness = 0.0f;
  int flags = 0;
  FloatSpan points;
  if (!c.ok() || !c.u32(1, "col", &col) || !c.f32(2, "thickness", &thickness) ||
      (!c.isMissing(3) && !c.flags(3, "flags", kDrawFlags, &flags)) ||
      // Last: a Float32Array's data is borrowed from here to the call.
      !c.floats(0, "points", &points))
    return nullptr;
  if (points.count % 2 != 0)
    return c.throwRange(
        "points must hold an even count of numbers (x, y pairs); got %zu",
        points.count);
  if (points.count / 2 > kMaxPolylinePoints)
    return c.throwRange(
        "points must hold at most %zu points (%zu numbers); got %zu numbers",
        kMaxPolylinePoints,
        kMaxPolylinePoints * 2,
        points.count);
  if (!c.needFrame())
    return nullptr;
  static_assert(sizeof(ImVec2) == 2 * sizeof(float), "ImVec2 is two floats");
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddPolyline(
      reinterpret_cast<const ImVec2 *>(points.data),
      static_cast<int>(points.count / 2),
      col,
      thickness,
      flags);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `drawList.addText(pos, col, text)`, imgui.h:3602, `text_end` NULL.
napi_value addText(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addText");
  ImVec2 pos;
  ImU32 col = 0;
  std::string text;
  if (!c.ok() || !c.vec2(0, "pos", &pos) || !c.u32(1, "col", &col) ||
      !c.string(2, "text", &text) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::GetWindowDrawList()->AddText(pos, col, text.c_str());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerDrawList(napi_env env, napi_value exports) {
  napi_value drawList = nullptr;
  return napi_create_object(env, &drawList) == napi_ok &&
      registerFunction(env, drawList, "addLine", addLine) &&
      registerFunction(env, drawList, "addRect", addRect) &&
      registerFunction(env, drawList, "addRectFilled", addRectFilled) &&
      registerFunction(env, drawList, "addCircle", addCircle) &&
      registerFunction(env, drawList, "addCircleFilled", addCircleFilled) &&
      registerFunction(env, drawList, "addTriangleFilled", addTriangleFilled) &&
      registerFunction(env, drawList, "addBezierCubic", addBezierCubic) &&
      registerFunction(env, drawList, "addPolyline", addPolyline) &&
      registerFunction(env, drawList, "addText", addText) &&
      napi_set_named_property(env, exports, "drawList", drawList) == napi_ok;
}

} // namespace bind
} // namespace hnimgui
