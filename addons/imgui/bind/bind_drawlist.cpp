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
// __test.bindCalls(). Colours are ImU32 numbers (G3). Every radius,
// thickness and rounding is a length (Call::length(), lengthLimit() in
// bind_util.cpp, which has the derivation).

#include "bind_guards.h"
#include "bind_util.h"

#include <string>

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
/// The largest magnitude of an addRect / addRectFilled corner coordinate.
/// Both convert the corners to int as they are (IM_IS_TRUNCATED4,
/// imgui_draw.cpp:2580), addRect after moving them out by the stroke's
/// offset (2377-2388, 2409), at most 1.5 times the thickness, itself
/// within lengthLimit() (bind_util.cpp): 2^30 + 1.5 * 2^20 < 2^31. That
/// conversion is a plain `(int)` in a build without SSE
/// (imgui_internal.h:583-586) and in a Debug build (imgui_draw.cpp:906-
/// 908); the other functions' points are not converted (they are stored as
/// floats and drawn).
constexpr float kMaxCornerCoordinate = 1073741824.0f;

/// \p p's components within +-kMaxCornerCoordinate; \p what names it.
bool cornerOk(Call &c, ImVec2 p, const char *what) {
  const std::string x = std::string(what) + "[0]";
  const std::string y = std::string(what) + "[1]";
  return c.inRange(
             p.x, x.c_str(), -kMaxCornerCoordinate, kMaxCornerCoordinate) &&
      c.inRange(p.y, y.c_str(), -kMaxCornerCoordinate, kMaxCornerCoordinate);
}

/// An automatic segment count's radius (numSegments <= 0) at most
/// lengthLimit(): _CalcCircleAutoSegmentCount() converts it (bind_util.cpp).
/// With an explicit count the radius is not converted, and a radius below
/// 0.01 is raised to it or drawn as nothing (imgui_draw.cpp:2716, 2772-
/// 2778), so only the upper side is bounded.
bool radiusOk(Call &c, float radius, int numSegments) {
  if (numSegments > 0 || radius <= lengthLimit())
    return true;
  c.throwRange(
      "radius must be at most %.9g (2^20 pixels at pixel density %g) with "
      "an automatic segment count; got %.9g",
      lengthLimit(),
      guard::pixelDensity(),
      radius);
  return false;
}

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
      !c.needFrame() || !c.length(thickness, "thickness", false))
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
      !cornerOk(c, pMin, "pMin") || !cornerOk(c, pMax, "pMax") ||
      !c.needFrame() || !c.length(rounding, "rounding", false) ||
      !c.length(thickness, "thickness", false))
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
      !cornerOk(c, pMin, "pMin") || !cornerOk(c, pMax, "pMax") ||
      !c.needFrame() || !c.length(rounding, "rounding", false))
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
      !c.needFrame() || !radiusOk(c, radius, numSegments) ||
      !c.length(thickness, "thickness", false))
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
      !c.needFrame() || !radiusOk(c, radius, numSegments))
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
      !c.needFrame() || !c.length(thickness, "thickness", false))
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
      !c.floats(0, "points", kMaxPolylinePoints * 2, &points))
    return nullptr;
  if (points.count % 2 != 0)
    return c.throwRange(
        "points must hold an even count of numbers (x, y pairs); got %zu",
        points.count);
  if (!c.needFrame() || !c.length(thickness, "thickness", false))
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
/// ImFont::RenderText converts the position to int as it is (IM_TRUNC,
/// imgui_draw.cpp:7353-7354; the draw list's flags, which could ask it not
/// to, are never set by a bound function), so each component is from
/// -2^31 to below 2^31.
napi_value addText(napi_env env, napi_callback_info info) {
  Call c(env, info, "drawList.addText");
  ImVec2 pos;
  ImU32 col = 0;
  std::string text;
  if (!c.ok() || !c.vec2(0, "pos", &pos) || !c.inIntRange(pos.x, "pos[0]") ||
      !c.inIntRange(pos.y, "pos[1]") || !c.u32(1, "col", &col) ||
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
