/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The exported constants (surface table, "Constants") and the EnumInfo of
// each enum type, which is what G1 accepts for it.
//
// Every constant is one HNIMGUI_CONST line naming the C++ enumerator, so no
// value is typed: the value is the enumerator's own, and the JavaScript
// name is its spelling minus the `ImGui` or `Im` prefix. A type's EnumInfo
// (its valid mask for a flags type, its value list for an enum type) is
// computed from the same list that registerConstants() exports, so what a
// function accepts and what the module exports cannot differ.
//
// What is in each list is the table's ruling 9: an enumerator a caller may
// pass or compare with, and the _None, _COUNT and named range markers. Not
// in any list: what the header marks [Internal], every trailing-underscore
// mask, and the obsolete names (IMGUI_DISABLE_OBSOLETE_FUNCTIONS is defined
// in imgui_config.h, so they do not compile).

#include "bind_util.h"

#include <cfloat>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace hnimgui {
namespace bind {

namespace {

/// One exported enumerator: its C++ spelling and its value.
struct Constant {
  const char *cName;
  int value;
};

#define HNIMGUI_CONST(e)    \
  Constant {                \
    #e, static_cast<int>(e) \
  }

// ImGuiWindowFlags_, imgui.h:1225, flags. Not exported: the six marked
// [Internal] (imgui.h:1252-1258).
constexpr Constant kWindowFlagsList[] = {
    HNIMGUI_CONST(ImGuiWindowFlags_None),
    HNIMGUI_CONST(ImGuiWindowFlags_NoTitleBar),
    HNIMGUI_CONST(ImGuiWindowFlags_NoResize),
    HNIMGUI_CONST(ImGuiWindowFlags_NoMove),
    HNIMGUI_CONST(ImGuiWindowFlags_NoScrollbar),
    HNIMGUI_CONST(ImGuiWindowFlags_NoScrollWithMouse),
    HNIMGUI_CONST(ImGuiWindowFlags_NoCollapse),
    HNIMGUI_CONST(ImGuiWindowFlags_AlwaysAutoResize),
    HNIMGUI_CONST(ImGuiWindowFlags_NoBackground),
    HNIMGUI_CONST(ImGuiWindowFlags_NoSavedSettings),
    HNIMGUI_CONST(ImGuiWindowFlags_NoMouseInputs),
    HNIMGUI_CONST(ImGuiWindowFlags_MenuBar),
    HNIMGUI_CONST(ImGuiWindowFlags_HorizontalScrollbar),
    HNIMGUI_CONST(ImGuiWindowFlags_NoFocusOnAppearing),
    HNIMGUI_CONST(ImGuiWindowFlags_NoBringToFrontOnFocus),
    HNIMGUI_CONST(ImGuiWindowFlags_AlwaysVerticalScrollbar),
    HNIMGUI_CONST(ImGuiWindowFlags_AlwaysHorizontalScrollbar),
    HNIMGUI_CONST(ImGuiWindowFlags_NoNavInputs),
    HNIMGUI_CONST(ImGuiWindowFlags_NoNavFocus),
    HNIMGUI_CONST(ImGuiWindowFlags_UnsavedDocument),
    HNIMGUI_CONST(ImGuiWindowFlags_NoDocking),
    HNIMGUI_CONST(ImGuiWindowFlags_NoNav),
    HNIMGUI_CONST(ImGuiWindowFlags_NoDecoration),
    HNIMGUI_CONST(ImGuiWindowFlags_NoInputs),
};

// ImGuiChildFlags_, imgui.h:1276, flags.
constexpr Constant kChildFlagsList[] = {
    HNIMGUI_CONST(ImGuiChildFlags_None),
    HNIMGUI_CONST(ImGuiChildFlags_Borders),
    HNIMGUI_CONST(ImGuiChildFlags_AlwaysUseWindowPadding),
    HNIMGUI_CONST(ImGuiChildFlags_ResizeX),
    HNIMGUI_CONST(ImGuiChildFlags_ResizeY),
    HNIMGUI_CONST(ImGuiChildFlags_AutoResizeX),
    HNIMGUI_CONST(ImGuiChildFlags_AutoResizeY),
    HNIMGUI_CONST(ImGuiChildFlags_AlwaysAutoResize),
    HNIMGUI_CONST(ImGuiChildFlags_FrameStyle),
    HNIMGUI_CONST(ImGuiChildFlags_NavFlattened),
};

// ImGuiInputTextFlags_, imgui.h:1335, flags.
constexpr Constant kInputTextFlagsList[] = {
    HNIMGUI_CONST(ImGuiInputTextFlags_None),
    HNIMGUI_CONST(ImGuiInputTextFlags_CharsDecimal),
    HNIMGUI_CONST(ImGuiInputTextFlags_CharsHexadecimal),
    HNIMGUI_CONST(ImGuiInputTextFlags_CharsScientific),
    HNIMGUI_CONST(ImGuiInputTextFlags_CharsUppercase),
    HNIMGUI_CONST(ImGuiInputTextFlags_CharsNoBlank),
    HNIMGUI_CONST(ImGuiInputTextFlags_AllowTabInput),
    HNIMGUI_CONST(ImGuiInputTextFlags_EnterReturnsTrue),
    HNIMGUI_CONST(ImGuiInputTextFlags_EscapeClearsAll),
    HNIMGUI_CONST(ImGuiInputTextFlags_CtrlEnterForNewLine),
    HNIMGUI_CONST(ImGuiInputTextFlags_ReadOnly),
    HNIMGUI_CONST(ImGuiInputTextFlags_Password),
    HNIMGUI_CONST(ImGuiInputTextFlags_AlwaysOverwrite),
    HNIMGUI_CONST(ImGuiInputTextFlags_AutoSelectAll),
    HNIMGUI_CONST(ImGuiInputTextFlags_ParseEmptyRefVal),
    HNIMGUI_CONST(ImGuiInputTextFlags_DisplayEmptyRefVal),
    HNIMGUI_CONST(ImGuiInputTextFlags_NoHorizontalScroll),
    HNIMGUI_CONST(ImGuiInputTextFlags_NoUndoRedo),
    HNIMGUI_CONST(ImGuiInputTextFlags_ElideLeft),
    HNIMGUI_CONST(ImGuiInputTextFlags_CallbackCompletion),
    HNIMGUI_CONST(ImGuiInputTextFlags_CallbackHistory),
    HNIMGUI_CONST(ImGuiInputTextFlags_CallbackAlways),
    HNIMGUI_CONST(ImGuiInputTextFlags_CallbackCharFilter),
    HNIMGUI_CONST(ImGuiInputTextFlags_CallbackResize),
    HNIMGUI_CONST(ImGuiInputTextFlags_CallbackEdit),
    HNIMGUI_CONST(ImGuiInputTextFlags_WordWrap),
};

// ImGuiTreeNodeFlags_, imgui.h:1387, flags.
constexpr Constant kTreeNodeFlagsList[] = {
    HNIMGUI_CONST(ImGuiTreeNodeFlags_None),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_Selected),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_Framed),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_AllowOverlap),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_NoTreePushOnOpen),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_NoAutoOpenOnLog),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_DefaultOpen),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_OpenOnDoubleClick),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_OpenOnArrow),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_Leaf),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_Bullet),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_FramePadding),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_SpanAvailWidth),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_SpanFullWidth),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_SpanLabelWidth),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_SpanAllColumns),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_LabelSpanAllColumns),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_NavLeftJumpsToParent),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_CollapsingHeader),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_DrawLinesNone),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_DrawLinesFull),
    HNIMGUI_CONST(ImGuiTreeNodeFlags_DrawLinesToNodes),
};

// ImGuiPopupFlags_, imgui.h:1426, flags. Not exported: the three trailing-
// underscore ones (imgui.h:1439-1441).
constexpr Constant kPopupFlagsList[] = {
    HNIMGUI_CONST(ImGuiPopupFlags_None),
    HNIMGUI_CONST(ImGuiPopupFlags_MouseButtonLeft),
    HNIMGUI_CONST(ImGuiPopupFlags_MouseButtonRight),
    HNIMGUI_CONST(ImGuiPopupFlags_MouseButtonMiddle),
    HNIMGUI_CONST(ImGuiPopupFlags_NoReopen),
    HNIMGUI_CONST(ImGuiPopupFlags_NoOpenOverExistingPopup),
    HNIMGUI_CONST(ImGuiPopupFlags_NoOpenOverItems),
    HNIMGUI_CONST(ImGuiPopupFlags_AnyPopupId),
    HNIMGUI_CONST(ImGuiPopupFlags_AnyPopupLevel),
    HNIMGUI_CONST(ImGuiPopupFlags_AnyPopup),
};

// ImGuiSelectableFlags_, imgui.h:1445, flags.
constexpr Constant kSelectableFlagsList[] = {
    HNIMGUI_CONST(ImGuiSelectableFlags_None),
    HNIMGUI_CONST(ImGuiSelectableFlags_NoAutoClosePopups),
    HNIMGUI_CONST(ImGuiSelectableFlags_SpanAllColumns),
    HNIMGUI_CONST(ImGuiSelectableFlags_AllowDoubleClick),
    HNIMGUI_CONST(ImGuiSelectableFlags_Disabled),
    HNIMGUI_CONST(ImGuiSelectableFlags_AllowOverlap),
    HNIMGUI_CONST(ImGuiSelectableFlags_Highlight),
    HNIMGUI_CONST(ImGuiSelectableFlags_SelectOnNav),
};

// ImGuiComboFlags_, imgui.h:1463, flags. Not exported: _HeightMask_
// (imgui.h:1474).
constexpr Constant kComboFlagsList[] = {
    HNIMGUI_CONST(ImGuiComboFlags_None),
    HNIMGUI_CONST(ImGuiComboFlags_PopupAlignLeft),
    HNIMGUI_CONST(ImGuiComboFlags_HeightSmall),
    HNIMGUI_CONST(ImGuiComboFlags_HeightRegular),
    HNIMGUI_CONST(ImGuiComboFlags_HeightLarge),
    HNIMGUI_CONST(ImGuiComboFlags_HeightLargest),
    HNIMGUI_CONST(ImGuiComboFlags_NoArrowButton),
    HNIMGUI_CONST(ImGuiComboFlags_NoPreview),
    HNIMGUI_CONST(ImGuiComboFlags_WidthFitPreview),
};

// ImGuiTabBarFlags_, imgui.h:1478, flags. Not exported: _FittingPolicyMask_,
// _FittingPolicyDefault_ (imgui.h:1493-1494).
constexpr Constant kTabBarFlagsList[] = {
    HNIMGUI_CONST(ImGuiTabBarFlags_None),
    HNIMGUI_CONST(ImGuiTabBarFlags_Reorderable),
    HNIMGUI_CONST(ImGuiTabBarFlags_AutoSelectNewTabs),
    HNIMGUI_CONST(ImGuiTabBarFlags_TabListPopupButton),
    HNIMGUI_CONST(ImGuiTabBarFlags_NoCloseWithMiddleMouseButton),
    HNIMGUI_CONST(ImGuiTabBarFlags_NoTabListScrollingButtons),
    HNIMGUI_CONST(ImGuiTabBarFlags_NoTooltip),
    HNIMGUI_CONST(ImGuiTabBarFlags_DrawSelectedOverline),
    HNIMGUI_CONST(ImGuiTabBarFlags_FittingPolicyMixed),
    HNIMGUI_CONST(ImGuiTabBarFlags_FittingPolicyShrink),
    HNIMGUI_CONST(ImGuiTabBarFlags_FittingPolicyScroll),
};

// ImGuiTabItemFlags_, imgui.h:1502, flags.
constexpr Constant kTabItemFlagsList[] = {
    HNIMGUI_CONST(ImGuiTabItemFlags_None),
    HNIMGUI_CONST(ImGuiTabItemFlags_UnsavedDocument),
    HNIMGUI_CONST(ImGuiTabItemFlags_SetSelected),
    HNIMGUI_CONST(ImGuiTabItemFlags_NoCloseWithMiddleMouseButton),
    HNIMGUI_CONST(ImGuiTabItemFlags_NoPushId),
    HNIMGUI_CONST(ImGuiTabItemFlags_NoTooltip),
    HNIMGUI_CONST(ImGuiTabItemFlags_NoReorder),
    HNIMGUI_CONST(ImGuiTabItemFlags_Leading),
    HNIMGUI_CONST(ImGuiTabItemFlags_Trailing),
    HNIMGUI_CONST(ImGuiTabItemFlags_NoAssumedClosure),
};

// ImGuiFocusedFlags_, imgui.h:1517, flags.
constexpr Constant kFocusedFlagsList[] = {
    HNIMGUI_CONST(ImGuiFocusedFlags_None),
    HNIMGUI_CONST(ImGuiFocusedFlags_ChildWindows),
    HNIMGUI_CONST(ImGuiFocusedFlags_RootWindow),
    HNIMGUI_CONST(ImGuiFocusedFlags_AnyWindow),
    HNIMGUI_CONST(ImGuiFocusedFlags_NoPopupHierarchy),
    HNIMGUI_CONST(ImGuiFocusedFlags_DockHierarchy),
    HNIMGUI_CONST(ImGuiFocusedFlags_RootAndChildWindows),
};

// ImGuiHoveredFlags_, imgui.h:1531, flags.
constexpr Constant kHoveredFlagsList[] = {
    HNIMGUI_CONST(ImGuiHoveredFlags_None),
    HNIMGUI_CONST(ImGuiHoveredFlags_ChildWindows),
    HNIMGUI_CONST(ImGuiHoveredFlags_RootWindow),
    HNIMGUI_CONST(ImGuiHoveredFlags_AnyWindow),
    HNIMGUI_CONST(ImGuiHoveredFlags_NoPopupHierarchy),
    HNIMGUI_CONST(ImGuiHoveredFlags_DockHierarchy),
    HNIMGUI_CONST(ImGuiHoveredFlags_AllowWhenBlockedByPopup),
    HNIMGUI_CONST(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
    HNIMGUI_CONST(ImGuiHoveredFlags_AllowWhenOverlappedByItem),
    HNIMGUI_CONST(ImGuiHoveredFlags_AllowWhenOverlappedByWindow),
    HNIMGUI_CONST(ImGuiHoveredFlags_AllowWhenDisabled),
    HNIMGUI_CONST(ImGuiHoveredFlags_NoNavOverride),
    HNIMGUI_CONST(ImGuiHoveredFlags_AllowWhenOverlapped),
    HNIMGUI_CONST(ImGuiHoveredFlags_RectOnly),
    HNIMGUI_CONST(ImGuiHoveredFlags_RootAndChildWindows),
    HNIMGUI_CONST(ImGuiHoveredFlags_ForTooltip),
    HNIMGUI_CONST(ImGuiHoveredFlags_Stationary),
    HNIMGUI_CONST(ImGuiHoveredFlags_DelayNone),
    HNIMGUI_CONST(ImGuiHoveredFlags_DelayShort),
    HNIMGUI_CONST(ImGuiHoveredFlags_DelayNormal),
    HNIMGUI_CONST(ImGuiHoveredFlags_NoSharedDelay),
};

// ImGuiDockNodeFlags_, imgui.h:1571, flags.
constexpr Constant kDockNodeFlagsList[] = {
    HNIMGUI_CONST(ImGuiDockNodeFlags_None),
    HNIMGUI_CONST(ImGuiDockNodeFlags_KeepAliveOnly),
    HNIMGUI_CONST(ImGuiDockNodeFlags_NoDockingOverCentralNode),
    HNIMGUI_CONST(ImGuiDockNodeFlags_PassthruCentralNode),
    HNIMGUI_CONST(ImGuiDockNodeFlags_NoDockingSplit),
    HNIMGUI_CONST(ImGuiDockNodeFlags_NoResize),
    HNIMGUI_CONST(ImGuiDockNodeFlags_AutoHideTabBar),
    HNIMGUI_CONST(ImGuiDockNodeFlags_NoUndocking),
};

// ImGuiDir, imgui.h:1637, enum.
constexpr Constant kDirList[] = {
    HNIMGUI_CONST(ImGuiDir_None),
    HNIMGUI_CONST(ImGuiDir_Left),
    HNIMGUI_CONST(ImGuiDir_Right),
    HNIMGUI_CONST(ImGuiDir_Up),
    HNIMGUI_CONST(ImGuiDir_Down),
    HNIMGUI_CONST(ImGuiDir_COUNT),
};

// ImGuiKey, imgui.h:1661, enum. Not exported: the four _ReservedForMod*
// (imgui.h:1756, marked [Internal]), ImGuiMod_Mask_ (1777). The ImGuiMod_*
// enumerators are members of this enum.
constexpr Constant kKeyList[] = {
    HNIMGUI_CONST(ImGuiKey_None),
    HNIMGUI_CONST(ImGuiKey_NamedKey_BEGIN),
    HNIMGUI_CONST(ImGuiKey_Tab),
    HNIMGUI_CONST(ImGuiKey_LeftArrow),
    HNIMGUI_CONST(ImGuiKey_RightArrow),
    HNIMGUI_CONST(ImGuiKey_UpArrow),
    HNIMGUI_CONST(ImGuiKey_DownArrow),
    HNIMGUI_CONST(ImGuiKey_PageUp),
    HNIMGUI_CONST(ImGuiKey_PageDown),
    HNIMGUI_CONST(ImGuiKey_Home),
    HNIMGUI_CONST(ImGuiKey_End),
    HNIMGUI_CONST(ImGuiKey_Insert),
    HNIMGUI_CONST(ImGuiKey_Delete),
    HNIMGUI_CONST(ImGuiKey_Backspace),
    HNIMGUI_CONST(ImGuiKey_Space),
    HNIMGUI_CONST(ImGuiKey_Enter),
    HNIMGUI_CONST(ImGuiKey_Escape),
    HNIMGUI_CONST(ImGuiKey_LeftCtrl),
    HNIMGUI_CONST(ImGuiKey_LeftShift),
    HNIMGUI_CONST(ImGuiKey_LeftAlt),
    HNIMGUI_CONST(ImGuiKey_LeftSuper),
    HNIMGUI_CONST(ImGuiKey_RightCtrl),
    HNIMGUI_CONST(ImGuiKey_RightShift),
    HNIMGUI_CONST(ImGuiKey_RightAlt),
    HNIMGUI_CONST(ImGuiKey_RightSuper),
    HNIMGUI_CONST(ImGuiKey_Menu),
    HNIMGUI_CONST(ImGuiKey_0),
    HNIMGUI_CONST(ImGuiKey_1),
    HNIMGUI_CONST(ImGuiKey_2),
    HNIMGUI_CONST(ImGuiKey_3),
    HNIMGUI_CONST(ImGuiKey_4),
    HNIMGUI_CONST(ImGuiKey_5),
    HNIMGUI_CONST(ImGuiKey_6),
    HNIMGUI_CONST(ImGuiKey_7),
    HNIMGUI_CONST(ImGuiKey_8),
    HNIMGUI_CONST(ImGuiKey_9),
    HNIMGUI_CONST(ImGuiKey_A),
    HNIMGUI_CONST(ImGuiKey_B),
    HNIMGUI_CONST(ImGuiKey_C),
    HNIMGUI_CONST(ImGuiKey_D),
    HNIMGUI_CONST(ImGuiKey_E),
    HNIMGUI_CONST(ImGuiKey_F),
    HNIMGUI_CONST(ImGuiKey_G),
    HNIMGUI_CONST(ImGuiKey_H),
    HNIMGUI_CONST(ImGuiKey_I),
    HNIMGUI_CONST(ImGuiKey_J),
    HNIMGUI_CONST(ImGuiKey_K),
    HNIMGUI_CONST(ImGuiKey_L),
    HNIMGUI_CONST(ImGuiKey_M),
    HNIMGUI_CONST(ImGuiKey_N),
    HNIMGUI_CONST(ImGuiKey_O),
    HNIMGUI_CONST(ImGuiKey_P),
    HNIMGUI_CONST(ImGuiKey_Q),
    HNIMGUI_CONST(ImGuiKey_R),
    HNIMGUI_CONST(ImGuiKey_S),
    HNIMGUI_CONST(ImGuiKey_T),
    HNIMGUI_CONST(ImGuiKey_U),
    HNIMGUI_CONST(ImGuiKey_V),
    HNIMGUI_CONST(ImGuiKey_W),
    HNIMGUI_CONST(ImGuiKey_X),
    HNIMGUI_CONST(ImGuiKey_Y),
    HNIMGUI_CONST(ImGuiKey_Z),
    HNIMGUI_CONST(ImGuiKey_F1),
    HNIMGUI_CONST(ImGuiKey_F2),
    HNIMGUI_CONST(ImGuiKey_F3),
    HNIMGUI_CONST(ImGuiKey_F4),
    HNIMGUI_CONST(ImGuiKey_F5),
    HNIMGUI_CONST(ImGuiKey_F6),
    HNIMGUI_CONST(ImGuiKey_F7),
    HNIMGUI_CONST(ImGuiKey_F8),
    HNIMGUI_CONST(ImGuiKey_F9),
    HNIMGUI_CONST(ImGuiKey_F10),
    HNIMGUI_CONST(ImGuiKey_F11),
    HNIMGUI_CONST(ImGuiKey_F12),
    HNIMGUI_CONST(ImGuiKey_F13),
    HNIMGUI_CONST(ImGuiKey_F14),
    HNIMGUI_CONST(ImGuiKey_F15),
    HNIMGUI_CONST(ImGuiKey_F16),
    HNIMGUI_CONST(ImGuiKey_F17),
    HNIMGUI_CONST(ImGuiKey_F18),
    HNIMGUI_CONST(ImGuiKey_F19),
    HNIMGUI_CONST(ImGuiKey_F20),
    HNIMGUI_CONST(ImGuiKey_F21),
    HNIMGUI_CONST(ImGuiKey_F22),
    HNIMGUI_CONST(ImGuiKey_F23),
    HNIMGUI_CONST(ImGuiKey_F24),
    HNIMGUI_CONST(ImGuiKey_Apostrophe),
    HNIMGUI_CONST(ImGuiKey_Comma),
    HNIMGUI_CONST(ImGuiKey_Minus),
    HNIMGUI_CONST(ImGuiKey_Period),
    HNIMGUI_CONST(ImGuiKey_Slash),
    HNIMGUI_CONST(ImGuiKey_Semicolon),
    HNIMGUI_CONST(ImGuiKey_Equal),
    HNIMGUI_CONST(ImGuiKey_LeftBracket),
    HNIMGUI_CONST(ImGuiKey_Backslash),
    HNIMGUI_CONST(ImGuiKey_RightBracket),
    HNIMGUI_CONST(ImGuiKey_GraveAccent),
    HNIMGUI_CONST(ImGuiKey_CapsLock),
    HNIMGUI_CONST(ImGuiKey_ScrollLock),
    HNIMGUI_CONST(ImGuiKey_NumLock),
    HNIMGUI_CONST(ImGuiKey_PrintScreen),
    HNIMGUI_CONST(ImGuiKey_Pause),
    HNIMGUI_CONST(ImGuiKey_Keypad0),
    HNIMGUI_CONST(ImGuiKey_Keypad1),
    HNIMGUI_CONST(ImGuiKey_Keypad2),
    HNIMGUI_CONST(ImGuiKey_Keypad3),
    HNIMGUI_CONST(ImGuiKey_Keypad4),
    HNIMGUI_CONST(ImGuiKey_Keypad5),
    HNIMGUI_CONST(ImGuiKey_Keypad6),
    HNIMGUI_CONST(ImGuiKey_Keypad7),
    HNIMGUI_CONST(ImGuiKey_Keypad8),
    HNIMGUI_CONST(ImGuiKey_Keypad9),
    HNIMGUI_CONST(ImGuiKey_KeypadDecimal),
    HNIMGUI_CONST(ImGuiKey_KeypadDivide),
    HNIMGUI_CONST(ImGuiKey_KeypadMultiply),
    HNIMGUI_CONST(ImGuiKey_KeypadSubtract),
    HNIMGUI_CONST(ImGuiKey_KeypadAdd),
    HNIMGUI_CONST(ImGuiKey_KeypadEnter),
    HNIMGUI_CONST(ImGuiKey_KeypadEqual),
    HNIMGUI_CONST(ImGuiKey_AppBack),
    HNIMGUI_CONST(ImGuiKey_AppForward),
    HNIMGUI_CONST(ImGuiKey_Oem102),
    HNIMGUI_CONST(ImGuiKey_GamepadStart),
    HNIMGUI_CONST(ImGuiKey_GamepadBack),
    HNIMGUI_CONST(ImGuiKey_GamepadFaceLeft),
    HNIMGUI_CONST(ImGuiKey_GamepadFaceRight),
    HNIMGUI_CONST(ImGuiKey_GamepadFaceUp),
    HNIMGUI_CONST(ImGuiKey_GamepadFaceDown),
    HNIMGUI_CONST(ImGuiKey_GamepadDpadLeft),
    HNIMGUI_CONST(ImGuiKey_GamepadDpadRight),
    HNIMGUI_CONST(ImGuiKey_GamepadDpadUp),
    HNIMGUI_CONST(ImGuiKey_GamepadDpadDown),
    HNIMGUI_CONST(ImGuiKey_GamepadL1),
    HNIMGUI_CONST(ImGuiKey_GamepadR1),
    HNIMGUI_CONST(ImGuiKey_GamepadL2),
    HNIMGUI_CONST(ImGuiKey_GamepadR2),
    HNIMGUI_CONST(ImGuiKey_GamepadL3),
    HNIMGUI_CONST(ImGuiKey_GamepadR3),
    HNIMGUI_CONST(ImGuiKey_GamepadLStickLeft),
    HNIMGUI_CONST(ImGuiKey_GamepadLStickRight),
    HNIMGUI_CONST(ImGuiKey_GamepadLStickUp),
    HNIMGUI_CONST(ImGuiKey_GamepadLStickDown),
    HNIMGUI_CONST(ImGuiKey_GamepadRStickLeft),
    HNIMGUI_CONST(ImGuiKey_GamepadRStickRight),
    HNIMGUI_CONST(ImGuiKey_GamepadRStickUp),
    HNIMGUI_CONST(ImGuiKey_GamepadRStickDown),
    HNIMGUI_CONST(ImGuiKey_MouseLeft),
    HNIMGUI_CONST(ImGuiKey_MouseRight),
    HNIMGUI_CONST(ImGuiKey_MouseMiddle),
    HNIMGUI_CONST(ImGuiKey_MouseX1),
    HNIMGUI_CONST(ImGuiKey_MouseX2),
    HNIMGUI_CONST(ImGuiKey_MouseWheelX),
    HNIMGUI_CONST(ImGuiKey_MouseWheelY),
    HNIMGUI_CONST(ImGuiKey_NamedKey_END),
    HNIMGUI_CONST(ImGuiKey_NamedKey_COUNT),
    HNIMGUI_CONST(ImGuiMod_None),
    HNIMGUI_CONST(ImGuiMod_Ctrl),
    HNIMGUI_CONST(ImGuiMod_Shift),
    HNIMGUI_CONST(ImGuiMod_Alt),
    HNIMGUI_CONST(ImGuiMod_Super),
};

// ImGuiCol_, imgui.h:1859, enum.
constexpr Constant kColList[] = {
    HNIMGUI_CONST(ImGuiCol_Text),
    HNIMGUI_CONST(ImGuiCol_TextDisabled),
    HNIMGUI_CONST(ImGuiCol_WindowBg),
    HNIMGUI_CONST(ImGuiCol_ChildBg),
    HNIMGUI_CONST(ImGuiCol_PopupBg),
    HNIMGUI_CONST(ImGuiCol_Border),
    HNIMGUI_CONST(ImGuiCol_BorderShadow),
    HNIMGUI_CONST(ImGuiCol_FrameBg),
    HNIMGUI_CONST(ImGuiCol_FrameBgHovered),
    HNIMGUI_CONST(ImGuiCol_FrameBgActive),
    HNIMGUI_CONST(ImGuiCol_TitleBg),
    HNIMGUI_CONST(ImGuiCol_TitleBgActive),
    HNIMGUI_CONST(ImGuiCol_TitleBgCollapsed),
    HNIMGUI_CONST(ImGuiCol_MenuBarBg),
    HNIMGUI_CONST(ImGuiCol_ScrollbarBg),
    HNIMGUI_CONST(ImGuiCol_ScrollbarGrab),
    HNIMGUI_CONST(ImGuiCol_ScrollbarGrabHovered),
    HNIMGUI_CONST(ImGuiCol_ScrollbarGrabActive),
    HNIMGUI_CONST(ImGuiCol_CheckMark),
    HNIMGUI_CONST(ImGuiCol_CheckboxSelectedBg),
    HNIMGUI_CONST(ImGuiCol_SliderGrab),
    HNIMGUI_CONST(ImGuiCol_SliderGrabActive),
    HNIMGUI_CONST(ImGuiCol_Button),
    HNIMGUI_CONST(ImGuiCol_ButtonHovered),
    HNIMGUI_CONST(ImGuiCol_ButtonActive),
    HNIMGUI_CONST(ImGuiCol_Header),
    HNIMGUI_CONST(ImGuiCol_HeaderHovered),
    HNIMGUI_CONST(ImGuiCol_HeaderActive),
    HNIMGUI_CONST(ImGuiCol_Separator),
    HNIMGUI_CONST(ImGuiCol_SeparatorHovered),
    HNIMGUI_CONST(ImGuiCol_SeparatorActive),
    HNIMGUI_CONST(ImGuiCol_ResizeGrip),
    HNIMGUI_CONST(ImGuiCol_ResizeGripHovered),
    HNIMGUI_CONST(ImGuiCol_ResizeGripActive),
    HNIMGUI_CONST(ImGuiCol_InputTextCursor),
    HNIMGUI_CONST(ImGuiCol_TabHovered),
    HNIMGUI_CONST(ImGuiCol_Tab),
    HNIMGUI_CONST(ImGuiCol_TabSelected),
    HNIMGUI_CONST(ImGuiCol_TabSelectedOverline),
    HNIMGUI_CONST(ImGuiCol_TabDimmed),
    HNIMGUI_CONST(ImGuiCol_TabDimmedSelected),
    HNIMGUI_CONST(ImGuiCol_TabDimmedSelectedOverline),
    HNIMGUI_CONST(ImGuiCol_DockingPreview),
    HNIMGUI_CONST(ImGuiCol_DockingEmptyBg),
    HNIMGUI_CONST(ImGuiCol_PlotLines),
    HNIMGUI_CONST(ImGuiCol_PlotLinesHovered),
    HNIMGUI_CONST(ImGuiCol_PlotHistogram),
    HNIMGUI_CONST(ImGuiCol_PlotHistogramHovered),
    HNIMGUI_CONST(ImGuiCol_TableHeaderBg),
    HNIMGUI_CONST(ImGuiCol_TableBorderStrong),
    HNIMGUI_CONST(ImGuiCol_TableBorderLight),
    HNIMGUI_CONST(ImGuiCol_TableRowBg),
    HNIMGUI_CONST(ImGuiCol_TableRowBgAlt),
    HNIMGUI_CONST(ImGuiCol_TextLink),
    HNIMGUI_CONST(ImGuiCol_TextSelectedBg),
    HNIMGUI_CONST(ImGuiCol_TreeLines),
    HNIMGUI_CONST(ImGuiCol_DragDropTarget),
    HNIMGUI_CONST(ImGuiCol_DragDropTargetBg),
    HNIMGUI_CONST(ImGuiCol_UnsavedMarker),
    HNIMGUI_CONST(ImGuiCol_NavCursor),
    HNIMGUI_CONST(ImGuiCol_NavWindowingHighlight),
    HNIMGUI_CONST(ImGuiCol_NavWindowingDimBg),
    HNIMGUI_CONST(ImGuiCol_ModalWindowDimBg),
    HNIMGUI_CONST(ImGuiCol_COUNT),
};

// ImGuiStyleVar_, imgui.h:1942, enum.
constexpr Constant kStyleVarList[] = {
    HNIMGUI_CONST(ImGuiStyleVar_Alpha),
    HNIMGUI_CONST(ImGuiStyleVar_DisabledAlpha),
    HNIMGUI_CONST(ImGuiStyleVar_WindowPadding),
    HNIMGUI_CONST(ImGuiStyleVar_WindowRounding),
    HNIMGUI_CONST(ImGuiStyleVar_WindowBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_WindowMinSize),
    HNIMGUI_CONST(ImGuiStyleVar_WindowTitleAlign),
    HNIMGUI_CONST(ImGuiStyleVar_ChildRounding),
    HNIMGUI_CONST(ImGuiStyleVar_ChildBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_PopupRounding),
    HNIMGUI_CONST(ImGuiStyleVar_PopupBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_FramePadding),
    HNIMGUI_CONST(ImGuiStyleVar_FrameRounding),
    HNIMGUI_CONST(ImGuiStyleVar_FrameBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_ItemSpacing),
    HNIMGUI_CONST(ImGuiStyleVar_ItemInnerSpacing),
    HNIMGUI_CONST(ImGuiStyleVar_IndentSpacing),
    HNIMGUI_CONST(ImGuiStyleVar_CellPadding),
    HNIMGUI_CONST(ImGuiStyleVar_ScrollbarSize),
    HNIMGUI_CONST(ImGuiStyleVar_ScrollbarRounding),
    HNIMGUI_CONST(ImGuiStyleVar_ScrollbarPadding),
    HNIMGUI_CONST(ImGuiStyleVar_GrabMinSize),
    HNIMGUI_CONST(ImGuiStyleVar_GrabRounding),
    HNIMGUI_CONST(ImGuiStyleVar_ImageRounding),
    HNIMGUI_CONST(ImGuiStyleVar_ImageBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_TabRounding),
    HNIMGUI_CONST(ImGuiStyleVar_TabBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_TabMinWidthBase),
    HNIMGUI_CONST(ImGuiStyleVar_TabMinWidthShrink),
    HNIMGUI_CONST(ImGuiStyleVar_TabBarBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_TabBarOverlineSize),
    HNIMGUI_CONST(ImGuiStyleVar_TableAngledHeadersAngle),
    HNIMGUI_CONST(ImGuiStyleVar_TableAngledHeadersTextAlign),
    HNIMGUI_CONST(ImGuiStyleVar_TreeLinesSize),
    HNIMGUI_CONST(ImGuiStyleVar_TreeLinesRounding),
    HNIMGUI_CONST(ImGuiStyleVar_MenuItemRounding),
    HNIMGUI_CONST(ImGuiStyleVar_SelectableRounding),
    HNIMGUI_CONST(ImGuiStyleVar_DragDropTargetRounding),
    HNIMGUI_CONST(ImGuiStyleVar_ButtonTextAlign),
    HNIMGUI_CONST(ImGuiStyleVar_SelectableTextAlign),
    HNIMGUI_CONST(ImGuiStyleVar_SeparatorSize),
    HNIMGUI_CONST(ImGuiStyleVar_SeparatorTextBorderSize),
    HNIMGUI_CONST(ImGuiStyleVar_SeparatorTextAlign),
    HNIMGUI_CONST(ImGuiStyleVar_SeparatorTextPadding),
    HNIMGUI_CONST(ImGuiStyleVar_DockingSeparatorSize),
    HNIMGUI_CONST(ImGuiStyleVar_COUNT),
};

// ImGuiButtonFlags_, imgui.h:1994, flags. Not exported: _MouseButtonMask_
// (imgui.h:2000).
constexpr Constant kButtonFlagsList[] = {
    HNIMGUI_CONST(ImGuiButtonFlags_None),
    HNIMGUI_CONST(ImGuiButtonFlags_MouseButtonLeft),
    HNIMGUI_CONST(ImGuiButtonFlags_MouseButtonRight),
    HNIMGUI_CONST(ImGuiButtonFlags_MouseButtonMiddle),
    HNIMGUI_CONST(ImGuiButtonFlags_EnableNav),
    HNIMGUI_CONST(ImGuiButtonFlags_AllowOverlap),
};

// ImGuiColorEditFlags_, imgui.h:2006, flags. Not exported: _DefaultOptions_
// and the five masks (imgui.h:2047, 2050-2054).
constexpr Constant kColorEditFlagsList[] = {
    HNIMGUI_CONST(ImGuiColorEditFlags_None),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoAlpha),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoPicker),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoOptions),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoSmallPreview),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoInputs),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoTooltip),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoLabel),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoSidePreview),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoDragDrop),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoBorder),
    HNIMGUI_CONST(ImGuiColorEditFlags_NoColorMarkers),
    HNIMGUI_CONST(ImGuiColorEditFlags_AlphaOpaque),
    HNIMGUI_CONST(ImGuiColorEditFlags_AlphaNoBg),
    HNIMGUI_CONST(ImGuiColorEditFlags_AlphaPreviewHalf),
    HNIMGUI_CONST(ImGuiColorEditFlags_AlphaBar),
    HNIMGUI_CONST(ImGuiColorEditFlags_HDR),
    HNIMGUI_CONST(ImGuiColorEditFlags_DisplayRGB),
    HNIMGUI_CONST(ImGuiColorEditFlags_DisplayHSV),
    HNIMGUI_CONST(ImGuiColorEditFlags_DisplayHex),
    HNIMGUI_CONST(ImGuiColorEditFlags_Uint8),
    HNIMGUI_CONST(ImGuiColorEditFlags_Float),
    HNIMGUI_CONST(ImGuiColorEditFlags_PickerHueBar),
    HNIMGUI_CONST(ImGuiColorEditFlags_PickerHueWheel),
    HNIMGUI_CONST(ImGuiColorEditFlags_PickerNoRotate),
    HNIMGUI_CONST(ImGuiColorEditFlags_InputRGB),
    HNIMGUI_CONST(ImGuiColorEditFlags_InputHSV),
};

// ImGuiSliderFlags_, imgui.h:2066, flags. Not exported: _InvalidMask_
// (imgui.h:2080).
constexpr Constant kSliderFlagsList[] = {
    HNIMGUI_CONST(ImGuiSliderFlags_None),
    HNIMGUI_CONST(ImGuiSliderFlags_Logarithmic),
    HNIMGUI_CONST(ImGuiSliderFlags_NoRoundToFormat),
    HNIMGUI_CONST(ImGuiSliderFlags_NoInput),
    HNIMGUI_CONST(ImGuiSliderFlags_WrapAround),
    HNIMGUI_CONST(ImGuiSliderFlags_ClampOnInput),
    HNIMGUI_CONST(ImGuiSliderFlags_ClampZeroRange),
    HNIMGUI_CONST(ImGuiSliderFlags_NoSpeedTweaks),
    HNIMGUI_CONST(ImGuiSliderFlags_ColorMarkers),
    HNIMGUI_CONST(ImGuiSliderFlags_AlwaysClamp),
};

// ImGuiMouseButton_, imgui.h:2085, enum.
constexpr Constant kMouseButtonList[] = {
    HNIMGUI_CONST(ImGuiMouseButton_Left),
    HNIMGUI_CONST(ImGuiMouseButton_Right),
    HNIMGUI_CONST(ImGuiMouseButton_Middle),
    HNIMGUI_CONST(ImGuiMouseButton_COUNT),
};

// ImGuiCond_, imgui.h:2127, enum.
constexpr Constant kCondList[] = {
    HNIMGUI_CONST(ImGuiCond_None),
    HNIMGUI_CONST(ImGuiCond_Always),
    HNIMGUI_CONST(ImGuiCond_Once),
    HNIMGUI_CONST(ImGuiCond_FirstUseEver),
    HNIMGUI_CONST(ImGuiCond_Appearing),
};

// ImGuiTableFlags_, imgui.h:2162, flags. Not exported: _SizingMask_
// (imgui.h:2211).
constexpr Constant kTableFlagsList[] = {
    HNIMGUI_CONST(ImGuiTableFlags_None),
    HNIMGUI_CONST(ImGuiTableFlags_Resizable),
    HNIMGUI_CONST(ImGuiTableFlags_Reorderable),
    HNIMGUI_CONST(ImGuiTableFlags_Hideable),
    HNIMGUI_CONST(ImGuiTableFlags_Sortable),
    HNIMGUI_CONST(ImGuiTableFlags_NoSavedSettings),
    HNIMGUI_CONST(ImGuiTableFlags_ContextMenuInBody),
    HNIMGUI_CONST(ImGuiTableFlags_RowBg),
    HNIMGUI_CONST(ImGuiTableFlags_BordersInnerH),
    HNIMGUI_CONST(ImGuiTableFlags_BordersOuterH),
    HNIMGUI_CONST(ImGuiTableFlags_BordersInnerV),
    HNIMGUI_CONST(ImGuiTableFlags_BordersOuterV),
    HNIMGUI_CONST(ImGuiTableFlags_BordersH),
    HNIMGUI_CONST(ImGuiTableFlags_BordersV),
    HNIMGUI_CONST(ImGuiTableFlags_BordersInner),
    HNIMGUI_CONST(ImGuiTableFlags_BordersOuter),
    HNIMGUI_CONST(ImGuiTableFlags_Borders),
    HNIMGUI_CONST(ImGuiTableFlags_NoBordersInBody),
    HNIMGUI_CONST(ImGuiTableFlags_NoBordersInBodyUntilResize),
    HNIMGUI_CONST(ImGuiTableFlags_SizingFixedFit),
    HNIMGUI_CONST(ImGuiTableFlags_SizingFixedSame),
    HNIMGUI_CONST(ImGuiTableFlags_SizingStretchProp),
    HNIMGUI_CONST(ImGuiTableFlags_SizingStretchSame),
    HNIMGUI_CONST(ImGuiTableFlags_NoHostExtendX),
    HNIMGUI_CONST(ImGuiTableFlags_NoHostExtendY),
    HNIMGUI_CONST(ImGuiTableFlags_NoKeepColumnsVisible),
    HNIMGUI_CONST(ImGuiTableFlags_PreciseWidths),
    HNIMGUI_CONST(ImGuiTableFlags_NoClip),
    HNIMGUI_CONST(ImGuiTableFlags_PadOuterX),
    HNIMGUI_CONST(ImGuiTableFlags_NoPadOuterX),
    HNIMGUI_CONST(ImGuiTableFlags_NoPadInnerX),
    HNIMGUI_CONST(ImGuiTableFlags_ScrollX),
    HNIMGUI_CONST(ImGuiTableFlags_ScrollY),
    HNIMGUI_CONST(ImGuiTableFlags_SortMulti),
    HNIMGUI_CONST(ImGuiTableFlags_SortTristate),
    HNIMGUI_CONST(ImGuiTableFlags_HighlightHoveredColumn),
};

// ImGuiTableColumnFlags_, imgui.h:2215, flags. Not exported: the four
// trailing-underscore ones (imgui.h:2246-2249).
constexpr Constant kTableColumnFlagsList[] = {
    HNIMGUI_CONST(ImGuiTableColumnFlags_None),
    HNIMGUI_CONST(ImGuiTableColumnFlags_Disabled),
    HNIMGUI_CONST(ImGuiTableColumnFlags_DefaultHide),
    HNIMGUI_CONST(ImGuiTableColumnFlags_DefaultSort),
    HNIMGUI_CONST(ImGuiTableColumnFlags_WidthStretch),
    HNIMGUI_CONST(ImGuiTableColumnFlags_WidthFixed),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoResize),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoReorder),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoHide),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoClip),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoSort),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoSortAscending),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoSortDescending),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoHeaderLabel),
    HNIMGUI_CONST(ImGuiTableColumnFlags_NoHeaderWidth),
    HNIMGUI_CONST(ImGuiTableColumnFlags_PreferSortAscending),
    HNIMGUI_CONST(ImGuiTableColumnFlags_PreferSortDescending),
    HNIMGUI_CONST(ImGuiTableColumnFlags_IndentEnable),
    HNIMGUI_CONST(ImGuiTableColumnFlags_IndentDisable),
    HNIMGUI_CONST(ImGuiTableColumnFlags_AngledHeader),
    HNIMGUI_CONST(ImGuiTableColumnFlags_IsEnabled),
    HNIMGUI_CONST(ImGuiTableColumnFlags_IsVisible),
    HNIMGUI_CONST(ImGuiTableColumnFlags_IsSorted),
    HNIMGUI_CONST(ImGuiTableColumnFlags_IsHovered),
};

// ImGuiTableRowFlags_, imgui.h:2253, flags.
constexpr Constant kTableRowFlagsList[] = {
    HNIMGUI_CONST(ImGuiTableRowFlags_None),
    HNIMGUI_CONST(ImGuiTableRowFlags_Headers),
};

// ImDrawFlags_, imgui.h:3450, flags. Not exported: the six trailing-
// underscore ones (imgui.h:3505, 3508-3512).
constexpr Constant kDrawFlagsList[] = {
    HNIMGUI_CONST(ImDrawFlags_None),
    HNIMGUI_CONST(ImDrawFlags_RoundTopLeft),
    HNIMGUI_CONST(ImDrawFlags_RoundTopRight),
    HNIMGUI_CONST(ImDrawFlags_RoundBottomLeft),
    HNIMGUI_CONST(ImDrawFlags_RoundBottomRight),
    HNIMGUI_CONST(ImDrawFlags_RoundNone),
    HNIMGUI_CONST(ImDrawFlags_RoundAll),
    HNIMGUI_CONST(ImDrawFlags_RoundTop),
    HNIMGUI_CONST(ImDrawFlags_RoundBottom),
    HNIMGUI_CONST(ImDrawFlags_RoundLeft),
    HNIMGUI_CONST(ImDrawFlags_RoundRight),
    HNIMGUI_CONST(ImDrawFlags_Closed),
    HNIMGUI_CONST(ImDrawFlags_JoinMiter),
    HNIMGUI_CONST(ImDrawFlags_CapSquare),
    HNIMGUI_CONST(ImDrawFlags_AAFill),
    HNIMGUI_CONST(ImDrawFlags_AALines),
    HNIMGUI_CONST(ImDrawFlags_AALineEnds),
    HNIMGUI_CONST(ImDrawFlags_StrokeInside),
    HNIMGUI_CONST(ImDrawFlags_StrokeCenter),
    HNIMGUI_CONST(ImDrawFlags_StrokeCenterBiased),
    HNIMGUI_CONST(ImDrawFlags_StrokeOutside),
    HNIMGUI_CONST(ImDrawFlags_StrokeLegacy),
    HNIMGUI_CONST(ImDrawFlags_TextNoPixelSnap),
    HNIMGUI_CONST(ImDrawFlags_UseTexForRoundCorners),
    HNIMGUI_CONST(ImDrawFlags_UseVtxOffset),
};
/// A flags type's valid mask: every bit some exported enumerator sets.
template <size_t N>
constexpr int maskOf(const Constant (&list)[N]) {
  unsigned mask = 0;
  for (size_t k = 0; k < N; ++k)
    mask |= static_cast<unsigned>(list[k].value);
  return static_cast<int>(mask);
}

/// An enum type's value list, in the order of its constant list.
template <size_t N>
struct Values {
  int v[N];
};
template <size_t N>
constexpr Values<N> valuesOf(const Constant (&list)[N]) {
  Values<N> out{};
  for (size_t k = 0; k < N; ++k)
    out.v[k] = list[k].value;
  return out;
}

constexpr auto kDirValues = valuesOf(kDirList);
constexpr auto kKeyValues = valuesOf(kKeyList);
constexpr auto kColValues = valuesOf(kColList);
constexpr auto kStyleVarValues = valuesOf(kStyleVarList);
constexpr auto kMouseButtonValues = valuesOf(kMouseButtonList);
constexpr auto kCondValues = valuesOf(kCondList);

/// One type's constants, for registerConstants().
struct List {
  const Constant *items;
  size_t count;
};

#define HNIMGUI_LIST(T)               \
  List {                              \
    k##T##List, std::size(k##T##List) \
  }

const List kLists[] = {
    HNIMGUI_LIST(WindowFlags),
    HNIMGUI_LIST(ChildFlags),
    HNIMGUI_LIST(InputTextFlags),
    HNIMGUI_LIST(TreeNodeFlags),
    HNIMGUI_LIST(PopupFlags),
    HNIMGUI_LIST(SelectableFlags),
    HNIMGUI_LIST(ComboFlags),
    HNIMGUI_LIST(TabBarFlags),
    HNIMGUI_LIST(TabItemFlags),
    HNIMGUI_LIST(FocusedFlags),
    HNIMGUI_LIST(HoveredFlags),
    HNIMGUI_LIST(DockNodeFlags),
    HNIMGUI_LIST(Dir),
    HNIMGUI_LIST(Key),
    HNIMGUI_LIST(Col),
    HNIMGUI_LIST(StyleVar),
    HNIMGUI_LIST(ButtonFlags),
    HNIMGUI_LIST(ColorEditFlags),
    HNIMGUI_LIST(SliderFlags),
    HNIMGUI_LIST(MouseButton),
    HNIMGUI_LIST(Cond),
    HNIMGUI_LIST(TableFlags),
    HNIMGUI_LIST(TableColumnFlags),
    HNIMGUI_LIST(TableRowFlags),
    HNIMGUI_LIST(DrawFlags),
};

/// The JavaScript name of a C++ enumerator: minus `ImGui`, else minus `Im`.
const char *jsName(const char *cName) {
  if (std::strncmp(cName, "ImGui", 5) == 0)
    return cName + 5;
  if (std::strncmp(cName, "Im", 2) == 0)
    return cName + 2;
  return cName;
}

bool setNumber(napi_env env, napi_value target, const char *name, double v) {
  napi_value value = nullptr;
  return napi_create_double(env, v, &value) == napi_ok &&
      napi_set_named_property(env, target, name, value) == napi_ok;
}

} // namespace

// The EnumInfo of each type; `name` is the JavaScript prefix the messages
// use ("flags has bits set that no WindowFlags_ constant has").
#define HNIMGUI_FLAGS_INFO(T) \
  const EnumInfo k##T = {#T "_", true, maskOf(k##T##List), nullptr, 0};
#define HNIMGUI_ENUM_INFO(T) \
  const EnumInfo k##T = {    \
      #T "_",                \
      false,                 \
      0,                     \
      k##T##Values.v,        \
      static_cast<int>(std::size(k##T##List))};

HNIMGUI_FLAGS_INFO(WindowFlags)
HNIMGUI_FLAGS_INFO(ChildFlags)
HNIMGUI_FLAGS_INFO(InputTextFlags)
HNIMGUI_FLAGS_INFO(TreeNodeFlags)
HNIMGUI_FLAGS_INFO(PopupFlags)
HNIMGUI_FLAGS_INFO(SelectableFlags)
HNIMGUI_FLAGS_INFO(ComboFlags)
HNIMGUI_FLAGS_INFO(TabBarFlags)
HNIMGUI_FLAGS_INFO(TabItemFlags)
HNIMGUI_FLAGS_INFO(FocusedFlags)
HNIMGUI_FLAGS_INFO(HoveredFlags)
HNIMGUI_FLAGS_INFO(DockNodeFlags)
HNIMGUI_ENUM_INFO(Dir)
HNIMGUI_ENUM_INFO(Key)
HNIMGUI_ENUM_INFO(Col)
HNIMGUI_ENUM_INFO(StyleVar)
HNIMGUI_FLAGS_INFO(ButtonFlags)
HNIMGUI_FLAGS_INFO(ColorEditFlags)
HNIMGUI_FLAGS_INFO(SliderFlags)
HNIMGUI_ENUM_INFO(MouseButton)
HNIMGUI_ENUM_INFO(Cond)
HNIMGUI_FLAGS_INFO(TableFlags)
HNIMGUI_FLAGS_INFO(TableColumnFlags)
HNIMGUI_FLAGS_INFO(TableRowFlags)
HNIMGUI_FLAGS_INFO(DrawFlags)

bool registerConstants(napi_env env, napi_value exports) {
  for (const List &list : kLists) {
    for (size_t k = 0; k < list.count; ++k) {
      if (!setNumber(
              env, exports, jsName(list.items[k].cName), list.items[k].value))
        return false;
    }
  }
  // G4: the C float limits, as the doubles they convert to exactly.
  return setNumber(env, exports, "FLT_MIN", static_cast<double>(FLT_MIN)) &&
      setNumber(env, exports, "FLT_MAX", static_cast<double>(FLT_MAX));
}

} // namespace bind
} // namespace hnimgui
