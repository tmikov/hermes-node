/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Style group (surface table, "Style").

#include "bind_util.h"

#include "imgui_addon.h"

#include <cmath>
#include <string>

namespace hnimgui {
namespace bind {

namespace {

/// What a style variable may hold. A pushed value lasts until popped, at
/// the latest by the frame-end recovery (imgui.cpp:6432-6433, 12081), which
/// runs before the next NewFrame()'s checks of the style (imgui.cpp:11843-
/// 11844), so the domain is what ImGui's code inside a frame needs.
enum class VarDomain {
  /// 0 to 1.
  Unit,
  /// A length either way, within lengthLimit() (bind_util.h).
  Length,
  /// A length, not negative.
  NonNegative,
  /// 1 to lengthLimit().
  MinSize,
  /// Any float: no bound function reads it.
  Any,
  /// FramePadding: a length, and its y at most framePaddingYLimit().
  FramePadding,
  /// ItemInnerSpacing: a length, and its x at least -kMaxPickerSpacing.
  ItemInnerSpacing,
};

struct VarInfo {
  const char *name; // as exported, for messages
  int count; // 1 for a float, 2 for an ImVec2 (imgui.h:1945-1989)
  VarDomain domain;
};

/// Each variable's type, from imgui.h:1945-1989, and domain, from its uses:
/// - Unit: Alpha, which NewFrame() asserts (imgui.cpp:11843), which
///   RenderColorRectWithAlphaCheckerboard() asserts as `alpha` from a
///   colorButton with a translucent colour (imgui_draw.cpp:7749, from
///   imgui_widgets.cpp:6633, 6643), and which GetColorU32(ImU32) converts
///   to ImU32 after multiplying (imgui.cpp:3787-3791), undefined when
///   negative; DisabledAlpha, which BeginDisabled() multiplies Alpha by
///   (imgui.cpp:9082), reached by beginMenu() and menuItem() with
///   `enabled` false; and the alignments WindowTitleAlign,
///   ButtonTextAlign, SelectableTextAlign and SeparatorTextAlign, each a
///   factor of the free space placing text that ImGui then converts to int
///   (imgui.cpp:7921, 7931; imgui_widgets.cpp:820, 7688, 1760, 1770, and
///   RenderText's IM_TRUNC, imgui_draw.cpp:7353-7354): inside 0 .. 1 the
///   text stays inside its rectangle, outside it the product grows
///   without bound.
/// - MinSize: WindowMinSize, which NewFrame() asserts is at least 1
///   (imgui.cpp:11844), and which becomes a window's size as it is when
///   the content is smaller (imgui.cpp:7191-7197).
/// - NonNegative: ScrollbarSize, which GetWindowScrollbarRect() asserts
///   (imgui_widgets.cpp:989); and WindowBorderSize, ChildBorderSize,
///   PopupBorderSize (each a window's border size, imgui.cpp:8302-8304),
///   FrameBorderSize and ScrollbarPadding, which a negative value turns
///   from shrinking the scrollbar's rectangle into growing it
///   (imgui_widgets.cpp:990-995, 1054-1055): a rectangle at least one
///   unit long over an inner rectangle shorter than one then reaches
///   ScrollbarEx's plain assertion that the content or visible size is
///   positive (imgui_widgets.cpp:1060-1064, sizes from 1018-1019).
/// - Length: every other length, a padding, a spacing, a size, a border
///   or a rounding: each reaches the draw list as a rounding or a
///   thickness, or is converted to int as it is or with a small factor
///   (FramePadding.x as a table's minimum column width,
///   imgui_tables.cpp:905, 1047, 1103; ItemSpacing.x halved,
///   imgui_widgets.cpp:9581; ScrollbarPadding, 1054; TabMinWidthBase and
///   TabMinWidthShrink as a tab's width, 10221, 10196, 10285 and 1882-1925;
///   FrameBorderSize halved, 991), and is otherwise part of a sum (surface
///   table, "Not covered" 3f). A negative one is accepted: no
///   assertion or undefined operation was found for it.
/// - FramePadding and ItemInnerSpacing: lengths, and the frame height and
///   picker width bounds of framePaddingYLimit() (round 2, D3).
/// - Any: TableAngledHeadersAngle and TableAngledHeadersTextAlign, read only
///   by TableAngledHeadersRow() (imgui_tables.cpp:3498, 3551), which is not
///   bound.
VarInfo styleVarInfo(int idx) {
  static_assert(ImGuiStyleVar_COUNT == 45, "re-audit the style variables");
  using D = VarDomain;
  switch (idx) {
#define HNIMGUI_VAR(name, count, domain) \
  case ImGuiStyleVar_##name:             \
    return VarInfo{"StyleVar_" #name, count, D::domain};
    HNIMGUI_VAR(Alpha, 1, Unit)
    HNIMGUI_VAR(DisabledAlpha, 1, Unit)
    HNIMGUI_VAR(WindowPadding, 2, Length)
    HNIMGUI_VAR(WindowRounding, 1, Length)
    HNIMGUI_VAR(WindowBorderSize, 1, NonNegative)
    HNIMGUI_VAR(WindowMinSize, 2, MinSize)
    HNIMGUI_VAR(WindowTitleAlign, 2, Unit)
    HNIMGUI_VAR(ChildRounding, 1, Length)
    HNIMGUI_VAR(ChildBorderSize, 1, NonNegative)
    HNIMGUI_VAR(PopupRounding, 1, Length)
    HNIMGUI_VAR(PopupBorderSize, 1, NonNegative)
    HNIMGUI_VAR(FramePadding, 2, FramePadding)
    HNIMGUI_VAR(FrameRounding, 1, Length)
    HNIMGUI_VAR(FrameBorderSize, 1, NonNegative)
    HNIMGUI_VAR(ItemSpacing, 2, Length)
    HNIMGUI_VAR(ItemInnerSpacing, 2, ItemInnerSpacing)
    HNIMGUI_VAR(IndentSpacing, 1, Length)
    HNIMGUI_VAR(CellPadding, 2, Length)
    HNIMGUI_VAR(ScrollbarSize, 1, NonNegative)
    HNIMGUI_VAR(ScrollbarRounding, 1, Length)
    HNIMGUI_VAR(ScrollbarPadding, 1, NonNegative)
    HNIMGUI_VAR(GrabMinSize, 1, Length)
    HNIMGUI_VAR(GrabRounding, 1, Length)
    HNIMGUI_VAR(ImageRounding, 1, Length)
    HNIMGUI_VAR(ImageBorderSize, 1, Length)
    HNIMGUI_VAR(TabRounding, 1, Length)
    HNIMGUI_VAR(TabBorderSize, 1, Length)
    HNIMGUI_VAR(TabMinWidthBase, 1, Length)
    HNIMGUI_VAR(TabMinWidthShrink, 1, Length)
    HNIMGUI_VAR(TabBarBorderSize, 1, Length)
    HNIMGUI_VAR(TabBarOverlineSize, 1, Length)
    HNIMGUI_VAR(TableAngledHeadersAngle, 1, Any)
    HNIMGUI_VAR(TableAngledHeadersTextAlign, 2, Any)
    HNIMGUI_VAR(TreeLinesSize, 1, Length)
    HNIMGUI_VAR(TreeLinesRounding, 1, Length)
    HNIMGUI_VAR(MenuItemRounding, 1, Length)
    HNIMGUI_VAR(SelectableRounding, 1, Length)
    HNIMGUI_VAR(DragDropTargetRounding, 1, Length)
    HNIMGUI_VAR(ButtonTextAlign, 2, Unit)
    HNIMGUI_VAR(SelectableTextAlign, 2, Unit)
    HNIMGUI_VAR(SeparatorSize, 1, Length)
    HNIMGUI_VAR(SeparatorTextBorderSize, 1, Length)
    HNIMGUI_VAR(SeparatorTextAlign, 2, Unit)
    HNIMGUI_VAR(SeparatorTextPadding, 2, Length)
    HNIMGUI_VAR(DockingSeparatorSize, 1, Length)
#undef HNIMGUI_VAR
    default:
      return VarInfo{"StyleVar_COUNT", 0, D::Any};
  }
}

/// The most units ItemInnerSpacing.x may take away from the colour picker's
/// width (below): 2^14.
constexpr float kMaxPickerSpacing = 16384.0f;

/// The largest FramePadding.y: the bound that keeps the colour picker's hue
/// wheel within 16-bit indices (Codex's code review, round 2, D3). The
/// picker a colour editor opens is 12 frame heights wide
/// (SetNextItemWidth(square_sz * 12), imgui_widgets.cpp:6098), its square
/// `sv_picker_size = max(sq, width - k * (sq + ItemInnerSpacing.x))`, k 1
/// or 2 (6248), and its wheel six arcs of `max(4, (int)(sv / 2) / 12)`
/// segments each (6488), an explicit count ImGui does not clamp. Each arc
/// is stroked as one primitive (6494-6495) of 4 vertices per point, `(n -
/// 2) * 4 + 8` for an open line of n points (imgui_draw.cpp:1040-1052), and
/// PrimReserve() moves the vertex offset before a primitive but never
/// splits one (imgui_draw.cpp:768-775; the renderer draws with 16-bit
/// indices and sets RendererHasVtxOffset, imgui_renderer.cpp:320-321,
/// 577): an arc of N segments needs 4 (N + 1) <= 65536, N <= 16383, so
/// `(int)(sv / 2) <= 196607`, sv < 393216. With ItemInnerSpacing.x >=
/// -2^14, sv <= 11 sq + 2^15, so the frame height sq = font size + 2 *
/// FramePadding.y must stay below 2^15. The font size is the default
/// font's, which no bound function changes (no font, font-scale or window
/// font-scale function is bound), and it is read here rather than assumed.
/// A FramePadding.y below the font size's negative half makes the frame
/// height negative, which only shrinks the picker. Every other primitive a
/// style length scales is clamped by ImGui (the surface table, "Not
/// covered" 3f, the tessellation table).
float framePaddingYLimit() {
  return std::floor((32767.0f - ImGui::GetFontSize()) / 2.0f);
}

/// Checks component \p k of a value \p v for \p var, named \p what.
bool varValueOk(Call &c, const VarInfo &var, int k, float v, const char *what) {
  switch (var.domain) {
    case VarDomain::FramePadding:
      return c.length(v, what, false) &&
          (k != 1 || c.inRange(v, what, -lengthLimit(), framePaddingYLimit()));
    case VarDomain::ItemInnerSpacing:
      return c.length(v, what, false) &&
          (k != 0 || c.inRange(v, what, -kMaxPickerSpacing, lengthLimit()));
    case VarDomain::Unit:
      return c.inRange(v, what, 0.0f, 1.0f);
    case VarDomain::Length:
      return c.length(v, what, false);
    case VarDomain::NonNegative:
      return c.length(v, what, true);
    case VarDomain::MinSize:
      return c.inRange(v, what, 1.0f, lengthLimit());
    case VarDomain::Any:
      return true;
  }
  return true;
}

/// `styleColorsDark()`, imgui.h:435, with `dst` NULL: the current
/// context's style. Inside or outside a frame.
napi_value styleColorsDark(napi_env env, napi_callback_info info) {
  Call c(env, info, "styleColorsDark");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  ImGui::StyleColorsDark();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `styleColorsLight()`, imgui.h:436, as styleColorsDark().
napi_value styleColorsLight(napi_env env, napi_callback_info info) {
  Call c(env, info, "styleColorsLight");
  if (!c.ok() || !c.needContext())
    return nullptr;
  c.enterImGui();
  ImGui::StyleColorsLight();
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushStyleColor(idx, col)`, dispatched on `col`: an Array is the
/// ImVec4 overload (imgui.h:548), a number the ImU32 one (imgui.h:547).
napi_value pushStyleColor(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushStyleColor");
  int idx = 0;
  // An exported Col_ value, then not Col_COUNT, which would index past
  // Style.Colors[] (imgui.cpp:3801, 3812).
  if (!c.ok() || !c.enumValue(0, "idx", kCol, &idx))
    return nullptr;
  if (idx == ImGuiCol_COUNT)
    return c.throwRange("idx must not be Col_COUNT");
  if (c.isArray(1)) {
    ImVec4 col;
    if (!c.vec4(1, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleColor(idx, col);
  } else {
    if (c.typeOf(1) != napi_number)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE",
          "col must be a number or an array of 4 numbers");
    ImU32 col = 0;
    if (!c.u32(1, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleColor(idx, col);
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popStyleColor(count = 1)`, imgui.h:549.
napi_value popStyleColor(napi_env env, napi_callback_info info) {
  Call c(env, info, "popStyleColor");
  int count = 1;
  if (!c.ok() || (!c.isMissing(0) && !c.int32(0, "count", &count)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopStyleColor(count);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `pushStyleVar(idx, val)`, dispatched on `val`: an Array is the ImVec2
/// overload (imgui.h:551), a number the float one (imgui.h:550).
napi_value pushStyleVar(napi_env env, napi_callback_info info) {
  Call c(env, info, "pushStyleVar");
  int idx = 0;
  // An exported StyleVar_ value, then not StyleVar_COUNT, which
  // GetStyleVarInfo asserts against (imgui.cpp:3891).
  if (!c.ok() || !c.enumValue(0, "idx", kStyleVar, &idx))
    return nullptr;
  if (idx == ImGuiStyleVar_COUNT)
    return c.throwRange("idx must not be StyleVar_COUNT");
  // The value's domain (styleVarInfo) is checked for the overload of the
  // variable's own type; the other one is ImGui's recoverable error, which
  // pushes nothing (imgui.cpp:3900, 3930).
  const VarInfo var = styleVarInfo(idx);
  const std::string what = std::string("val for ") + var.name;
  if (c.isArray(1)) {
    ImVec2 val;
    if (!c.vec2(1, "val", &val) || !c.needFrame())
      return nullptr;
    if (var.count == 2) {
      const std::string x = std::string("val[0] for ") + var.name;
      const std::string y = std::string("val[1] for ") + var.name;
      if (!varValueOk(c, var, 0, val.x, x.c_str()) ||
          !varValueOk(c, var, 1, val.y, y.c_str()))
        return nullptr;
    }
    c.enterImGui();
    ImGui::PushStyleVar(idx, val);
  } else {
    if (c.typeOf(1) != napi_number)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE",
          "val must be a number or an array of 2 numbers");
    float val = 0.0f;
    if (!c.f32(1, "val", &val) || !c.needFrame())
      return nullptr;
    if (var.count == 1 && !varValueOk(c, var, 0, val, what.c_str()))
      return nullptr;
    c.enterImGui();
    ImGui::PushStyleVar(idx, val);
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `popStyleVar(count = 1)`, imgui.h:554.
napi_value popStyleVar(napi_env env, napi_callback_info info) {
  Call c(env, info, "popStyleVar");
  int count = 1;
  if (!c.ok() || (!c.isMissing(0) && !c.int32(0, "count", &count)) ||
      !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::PopStyleVar(count);
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

/// `getColorU32(idx, alphaMul = 1)` or `getColorU32(col)`, dispatched on
/// the first argument: an Array is the ImVec4 overload (imgui.h:570), a
/// number the ImGuiCol one (imgui.h:569). The ImU32 overload (imgui.h:571)
/// is not bound.
napi_value getColorU32(napi_env env, napi_callback_info info) {
  Call c(env, info, "getColorU32");
  if (!c.ok())
    return nullptr;
  ImU32 result = 0;
  if (c.isArray(0)) {
    ImVec4 col;
    if (!c.vec4(0, "col", &col) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    result = ImGui::GetColorU32(col);
  } else {
    if (c.typeOf(0) != napi_number)
      return c.throwType(
          "ERR_INVALID_ARG_TYPE",
          "idx must be a number, or col an array of 4 numbers");
    int idx = 0;
    float alphaMul = 1.0f;
    // Not Col_COUNT, which would index past Style.Colors[]
    // (imgui.cpp:3765).
    if (!c.enumValue(0, "idx", kCol, &idx))
      return nullptr;
    if (idx == ImGuiCol_COUNT)
      return c.throwRange("idx must not be Col_COUNT");
    if ((!c.isMissing(1) && !c.f32(1, "alphaMul", &alphaMul)) || !c.needFrame())
      return nullptr;
    c.enterImGui();
    result = ImGui::GetColorU32(idx, alphaMul);
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.resultU32(result);
}

/// `colorConvertFloat4ToU32(col)`, imgui.h:1102: a pure function reading
/// no context (imgui.cpp:3003-3011), so it needs neither a frame nor a
/// window.
napi_value colorConvertFloat4ToU32(napi_env env, napi_callback_info info) {
  Call c(env, info, "colorConvertFloat4ToU32");
  ImVec4 col;
  if (!c.ok() || !c.vec4(0, "col", &col))
    return nullptr;
  c.enterImGui();
  const ImU32 result = ImGui::ColorConvertFloat4ToU32(col);
  if (!c.leaveImGui())
    return nullptr;
  return c.resultU32(result);
}

} // namespace

bool registerStyle(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "styleColorsDark", styleColorsDark) &&
      registerFunction(env, exports, "styleColorsLight", styleColorsLight) &&
      registerFunction(env, exports, "pushStyleColor", pushStyleColor) &&
      registerFunction(env, exports, "popStyleColor", popStyleColor) &&
      registerFunction(env, exports, "pushStyleVar", pushStyleVar) &&
      registerFunction(env, exports, "popStyleVar", popStyleVar) &&
      registerFunction(env, exports, "getColorU32", getColorU32) &&
      registerFunction(
             env, exports, "colorConvertFloat4ToU32", colorConvertFloat4ToU32);
}

} // namespace bind
} // namespace hnimgui
