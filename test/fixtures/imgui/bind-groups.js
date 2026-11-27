// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The checked-in surface of `hermes-node:imgui`: the bound names by group,
// as the surface table (docs/superpowers/plans/2026-11-27-imgui-binding-
// surface.md) sections them, and a sample of constants with their values.
//
// test/imgui-bind-surface.js compares all() with the module's own property
// names, so an added or removed export fails there; each group's test ends
// with harness.checkGroupCoverage(h, '<group>'), which fails unless every
// function of the group has reached ImGui. Each task appends what it binds.
// A group listed here can be incomplete until the task that completes it:
// only then is its coverage checked.

'use strict';

const groups = {
  Frame: [
    'newFrame',
    'render',
    'showDemoWindow',
    'version', // a string property, not a function
    'showMetricsWindow',
    'getTime',
    'getFrameRate',
    'getDeltaTime',
    'getDisplaySize',
    'wantCaptureMouse',
    'wantCaptureKeyboard',
  ],
  Windows: [
    'begin',
    'end',
    'beginChild',
    'endChild',
    'setNextWindowPos',
    'setNextWindowSize',
    'setNextWindowBgAlpha',
    'getWindowPos',
    'getWindowSize',
    'getContentRegionAvail',
    'isWindowFocused',
    'isWindowHovered',
    'getScrollY',
    'getScrollMaxY',
    'setScrollHereY',
  ],
  Layout: [
    'sameLine',
    'separator',
    'separatorText',
    'spacing',
    'dummy',
    'newLine',
    'indent',
    'unindent',
    'beginGroup',
    'endGroup',
    'setNextItemWidth',
    'pushItemWidth',
    'popItemWidth',
    'getCursorScreenPos',
    'setCursorScreenPos',
    'getTextLineHeight',
    'getTextLineHeightWithSpacing',
    'getFrameHeight',
    'calcTextSize',
    'alignTextToFramePadding',
  ],
  IDs: [
    'pushID',
    'popID',
    'getID',
  ],
  Text: [
    'text',
    'textColored',
    'textDisabled',
    'textWrapped',
    'bulletText',
    'labelText',
  ],
  Widgets: [
    'button',
    'smallButton',
    'invisibleButton',
    'arrowButton',
    'checkbox',
    'radioButton',
    'progressBar',
    'bullet',
    'sliderFloat',
    'sliderInt',
    'sliderFloat2',
    'sliderFloat3',
    'sliderFloat4',
    'dragFloat',
    'dragInt',
    'inputText',
    'inputTextMultiline',
    'inputTextWithHint',
    'inputFloat',
    'inputInt',
    'beginCombo',
    'endCombo',
    'selectable',
    'colorEdit3',
    'colorEdit4',
    'colorButton',
    'beginListBox',
    'endListBox',
  ],
  TreesTabs: [
    'treeNode',
    'treeNodeEx',
    'treePop',
    'collapsingHeader',
    'setNextItemOpen',
    'beginTabBar',
    'endTabBar',
    'beginTabItem',
    'endTabItem',
  ],
  Menus: [
    'beginMainMenuBar',
    'endMainMenuBar',
    'beginMenuBar',
    'endMenuBar',
    'beginMenu',
    'endMenu',
    'menuItem',
  ],
  Popups: [
    'openPopup',
    'beginPopup',
    'beginPopupModal',
    'beginPopupContextItem',
    'endPopup',
    'closeCurrentPopup',
    'setTooltip',
    'setItemTooltip',
    'beginTooltip',
    'endTooltip',
  ],
  Queries: [
    'isItemHovered',
    'isItemClicked',
    'isItemActive',
    'isItemDeactivatedAfterEdit',
    'getItemRectMin',
    'getItemRectMax',
    'setItemDefaultFocus',
    'setKeyboardFocusHere',
    'isKeyPressed',
    'isKeyDown',
    'isMouseDown',
    'isMouseClicked',
    'isMouseDoubleClicked',
    'getMousePos',
    'getMouseDragDelta',
  ],
  // Incomplete until Task 7.
  Style: ['pushStyleColor', 'popStyleColor', 'pushStyleVar', 'popStyleVar'],
};

// The exported constants, per enum type, as the surface table's
// "Constants" section gives them: the JavaScript prefix (the C++ name
// minus `ImGui` / `Im`, up to the first `_`) -> the kind (G1: a flags
// type accepts any union of its enumerators' bits, an enum type one
// enumerator's value), the exact sorted list of exported names (its
// length is the table's "Exported" count; test/imgui-bind-surface.js
// compares it with the module exactly), and the names the table excludes
// (and its obsolete, uncompiled ones), none of which may be exported. The `ImGuiMod_*` enumerators are members of `ImGuiKey`
// (imgui.h:1772-1777), so `Mod_` names count under `Key`.
const constantTypes = {
  WindowFlags: {kind: 'flags', names: [
    'WindowFlags_AlwaysAutoResize', 'WindowFlags_AlwaysHorizontalScrollbar',
    'WindowFlags_AlwaysVerticalScrollbar',
    'WindowFlags_HorizontalScrollbar', 'WindowFlags_MenuBar',
    'WindowFlags_NoBackground', 'WindowFlags_NoBringToFrontOnFocus',
    'WindowFlags_NoCollapse', 'WindowFlags_NoDecoration',
    'WindowFlags_NoDocking', 'WindowFlags_NoFocusOnAppearing',
    'WindowFlags_NoInputs', 'WindowFlags_NoMouseInputs',
    'WindowFlags_NoMove', 'WindowFlags_NoNav', 'WindowFlags_NoNavFocus',
    'WindowFlags_NoNavInputs', 'WindowFlags_NoResize',
    'WindowFlags_NoSavedSettings', 'WindowFlags_NoScrollWithMouse',
    'WindowFlags_NoScrollbar', 'WindowFlags_NoTitleBar', 'WindowFlags_None',
    'WindowFlags_UnsavedDocument',
  ], excluded: [
    // "// [Internal]" (imgui.h:1252), 1253-1258.
    'WindowFlags_DockNodeHost', 'WindowFlags_ChildWindow',
    'WindowFlags_Tooltip', 'WindowFlags_Popup', 'WindowFlags_Modal',
    'WindowFlags_ChildMenu',
  ]},
  ChildFlags: {kind: 'flags', names: [
    'ChildFlags_AlwaysAutoResize', 'ChildFlags_AlwaysUseWindowPadding',
    'ChildFlags_AutoResizeX', 'ChildFlags_AutoResizeY',
    'ChildFlags_Borders', 'ChildFlags_FrameStyle',
    'ChildFlags_NavFlattened', 'ChildFlags_None', 'ChildFlags_ResizeX',
    'ChildFlags_ResizeY',
  ], excluded: []},
  InputTextFlags: {kind: 'flags', names: [
    'InputTextFlags_AllowTabInput', 'InputTextFlags_AlwaysOverwrite',
    'InputTextFlags_AutoSelectAll', 'InputTextFlags_CallbackAlways',
    'InputTextFlags_CallbackCharFilter',
    'InputTextFlags_CallbackCompletion', 'InputTextFlags_CallbackEdit',
    'InputTextFlags_CallbackHistory', 'InputTextFlags_CallbackResize',
    'InputTextFlags_CharsDecimal', 'InputTextFlags_CharsHexadecimal',
    'InputTextFlags_CharsNoBlank', 'InputTextFlags_CharsScientific',
    'InputTextFlags_CharsUppercase', 'InputTextFlags_CtrlEnterForNewLine',
    'InputTextFlags_DisplayEmptyRefVal', 'InputTextFlags_ElideLeft',
    'InputTextFlags_EnterReturnsTrue', 'InputTextFlags_EscapeClearsAll',
    'InputTextFlags_NoHorizontalScroll', 'InputTextFlags_NoUndoRedo',
    'InputTextFlags_None', 'InputTextFlags_ParseEmptyRefVal',
    'InputTextFlags_Password', 'InputTextFlags_ReadOnly',
    'InputTextFlags_WordWrap',
  ], excluded: []},
  TreeNodeFlags: {kind: 'flags', names: [
    'TreeNodeFlags_AllowOverlap', 'TreeNodeFlags_Bullet',
    'TreeNodeFlags_CollapsingHeader', 'TreeNodeFlags_DefaultOpen',
    'TreeNodeFlags_DrawLinesFull', 'TreeNodeFlags_DrawLinesNone',
    'TreeNodeFlags_DrawLinesToNodes', 'TreeNodeFlags_FramePadding',
    'TreeNodeFlags_Framed', 'TreeNodeFlags_LabelSpanAllColumns',
    'TreeNodeFlags_Leaf', 'TreeNodeFlags_NavLeftJumpsToParent',
    'TreeNodeFlags_NoAutoOpenOnLog', 'TreeNodeFlags_NoTreePushOnOpen',
    'TreeNodeFlags_None', 'TreeNodeFlags_OpenOnArrow',
    'TreeNodeFlags_OpenOnDoubleClick', 'TreeNodeFlags_Selected',
    'TreeNodeFlags_SpanAllColumns', 'TreeNodeFlags_SpanAvailWidth',
    'TreeNodeFlags_SpanFullWidth', 'TreeNodeFlags_SpanLabelWidth',
  ], excluded: [
    // Obsolete (imgui.h:1417), not compiled.
    'TreeNodeFlags_NavLeftJumpsBackHere',
  ]},
  PopupFlags: {kind: 'flags', names: [
    'PopupFlags_AnyPopup', 'PopupFlags_AnyPopupId',
    'PopupFlags_AnyPopupLevel', 'PopupFlags_MouseButtonLeft',
    'PopupFlags_MouseButtonMiddle', 'PopupFlags_MouseButtonRight',
    'PopupFlags_NoOpenOverExistingPopup', 'PopupFlags_NoOpenOverItems',
    'PopupFlags_NoReopen', 'PopupFlags_None',
  ], excluded: [
    // imgui.h:1439-1441.
    'PopupFlags_MouseButtonShift_', 'PopupFlags_MouseButtonMask_',
    'PopupFlags_InvalidMask_',
  ]},
  SelectableFlags: {kind: 'flags', names: [
    'SelectableFlags_AllowDoubleClick', 'SelectableFlags_AllowOverlap',
    'SelectableFlags_Disabled', 'SelectableFlags_Highlight',
    'SelectableFlags_NoAutoClosePopups', 'SelectableFlags_None',
    'SelectableFlags_SelectOnNav', 'SelectableFlags_SpanAllColumns',
  ], excluded: [
    'SelectableFlags_DontClosePopups', // obsolete (imgui.h:1457)
  ]},
  ComboFlags: {kind: 'flags', names: [
    'ComboFlags_HeightLarge', 'ComboFlags_HeightLargest',
    'ComboFlags_HeightRegular', 'ComboFlags_HeightSmall',
    'ComboFlags_NoArrowButton', 'ComboFlags_NoPreview', 'ComboFlags_None',
    'ComboFlags_PopupAlignLeft', 'ComboFlags_WidthFitPreview',
  ], excluded: [
    'ComboFlags_HeightMask_', // imgui.h:1474
  ]},
  TabBarFlags: {kind: 'flags', names: [
    'TabBarFlags_AutoSelectNewTabs', 'TabBarFlags_DrawSelectedOverline',
    'TabBarFlags_FittingPolicyMixed', 'TabBarFlags_FittingPolicyScroll',
    'TabBarFlags_FittingPolicyShrink',
    'TabBarFlags_NoCloseWithMiddleMouseButton',
    'TabBarFlags_NoTabListScrollingButtons', 'TabBarFlags_NoTooltip',
    'TabBarFlags_None', 'TabBarFlags_Reorderable',
    'TabBarFlags_TabListPopupButton',
  ], excluded: [
    // imgui.h:1493, 1494; obsolete 1497.
    'TabBarFlags_FittingPolicyMask_', 'TabBarFlags_FittingPolicyDefault_',
    'TabBarFlags_FittingPolicyResizeDown',
  ]},
  TabItemFlags: {kind: 'flags', names: [
    'TabItemFlags_Leading', 'TabItemFlags_NoAssumedClosure',
    'TabItemFlags_NoCloseWithMiddleMouseButton', 'TabItemFlags_NoPushId',
    'TabItemFlags_NoReorder', 'TabItemFlags_NoTooltip', 'TabItemFlags_None',
    'TabItemFlags_SetSelected', 'TabItemFlags_Trailing',
    'TabItemFlags_UnsavedDocument',
  ], excluded: []},
  FocusedFlags: {kind: 'flags', names: [
    'FocusedFlags_AnyWindow', 'FocusedFlags_ChildWindows',
    'FocusedFlags_DockHierarchy', 'FocusedFlags_NoPopupHierarchy',
    'FocusedFlags_None', 'FocusedFlags_RootAndChildWindows',
    'FocusedFlags_RootWindow',
  ], excluded: []},
  HoveredFlags: {kind: 'flags', names: [
    'HoveredFlags_AllowWhenBlockedByActiveItem',
    'HoveredFlags_AllowWhenBlockedByPopup',
    'HoveredFlags_AllowWhenDisabled', 'HoveredFlags_AllowWhenOverlapped',
    'HoveredFlags_AllowWhenOverlappedByItem',
    'HoveredFlags_AllowWhenOverlappedByWindow', 'HoveredFlags_AnyWindow',
    'HoveredFlags_ChildWindows', 'HoveredFlags_DelayNone',
    'HoveredFlags_DelayNormal', 'HoveredFlags_DelayShort',
    'HoveredFlags_DockHierarchy', 'HoveredFlags_ForTooltip',
    'HoveredFlags_NoNavOverride', 'HoveredFlags_NoPopupHierarchy',
    'HoveredFlags_NoSharedDelay', 'HoveredFlags_None',
    'HoveredFlags_RectOnly', 'HoveredFlags_RootAndChildWindows',
    'HoveredFlags_RootWindow', 'HoveredFlags_Stationary',
  ], excluded: []},
  DockNodeFlags: {kind: 'flags', names: [
    'DockNodeFlags_AutoHideTabBar', 'DockNodeFlags_KeepAliveOnly',
    'DockNodeFlags_NoDockingOverCentralNode',
    'DockNodeFlags_NoDockingSplit', 'DockNodeFlags_NoResize',
    'DockNodeFlags_NoUndocking', 'DockNodeFlags_None',
    'DockNodeFlags_PassthruCentralNode',
  ], excluded: [
    // Obsolete (imgui.h:1584, 1585).
    'DockNodeFlags_NoSplit', 'DockNodeFlags_NoDockingInCentralNode',
  ]},
  Dir: {kind: 'enum', names: [
    'Dir_COUNT', 'Dir_Down', 'Dir_Left', 'Dir_None', 'Dir_Right', 'Dir_Up',
  ], excluded: []},
  Key: {kind: 'enum', names: [
    'Key_0', 'Key_1', 'Key_2', 'Key_3', 'Key_4', 'Key_5', 'Key_6', 'Key_7',
    'Key_8', 'Key_9', 'Key_A', 'Key_Apostrophe', 'Key_AppBack',
    'Key_AppForward', 'Key_B', 'Key_Backslash', 'Key_Backspace', 'Key_C',
    'Key_CapsLock', 'Key_Comma', 'Key_D', 'Key_Delete', 'Key_DownArrow',
    'Key_E', 'Key_End', 'Key_Enter', 'Key_Equal', 'Key_Escape', 'Key_F',
    'Key_F1', 'Key_F10', 'Key_F11', 'Key_F12', 'Key_F13', 'Key_F14',
    'Key_F15', 'Key_F16', 'Key_F17', 'Key_F18', 'Key_F19', 'Key_F2',
    'Key_F20', 'Key_F21', 'Key_F22', 'Key_F23', 'Key_F24', 'Key_F3',
    'Key_F4', 'Key_F5', 'Key_F6', 'Key_F7', 'Key_F8', 'Key_F9', 'Key_G',
    'Key_GamepadBack', 'Key_GamepadDpadDown', 'Key_GamepadDpadLeft',
    'Key_GamepadDpadRight', 'Key_GamepadDpadUp', 'Key_GamepadFaceDown',
    'Key_GamepadFaceLeft', 'Key_GamepadFaceRight', 'Key_GamepadFaceUp',
    'Key_GamepadL1', 'Key_GamepadL2', 'Key_GamepadL3',
    'Key_GamepadLStickDown', 'Key_GamepadLStickLeft',
    'Key_GamepadLStickRight', 'Key_GamepadLStickUp', 'Key_GamepadR1',
    'Key_GamepadR2', 'Key_GamepadR3', 'Key_GamepadRStickDown',
    'Key_GamepadRStickLeft', 'Key_GamepadRStickRight',
    'Key_GamepadRStickUp', 'Key_GamepadStart', 'Key_GraveAccent', 'Key_H',
    'Key_Home', 'Key_I', 'Key_Insert', 'Key_J', 'Key_K', 'Key_Keypad0',
    'Key_Keypad1', 'Key_Keypad2', 'Key_Keypad3', 'Key_Keypad4',
    'Key_Keypad5', 'Key_Keypad6', 'Key_Keypad7', 'Key_Keypad8',
    'Key_Keypad9', 'Key_KeypadAdd', 'Key_KeypadDecimal', 'Key_KeypadDivide',
    'Key_KeypadEnter', 'Key_KeypadEqual', 'Key_KeypadMultiply',
    'Key_KeypadSubtract', 'Key_L', 'Key_LeftAlt', 'Key_LeftArrow',
    'Key_LeftBracket', 'Key_LeftCtrl', 'Key_LeftShift', 'Key_LeftSuper',
    'Key_M', 'Key_Menu', 'Key_Minus', 'Key_MouseLeft', 'Key_MouseMiddle',
    'Key_MouseRight', 'Key_MouseWheelX', 'Key_MouseWheelY', 'Key_MouseX1',
    'Key_MouseX2', 'Key_N', 'Key_NamedKey_BEGIN', 'Key_NamedKey_COUNT',
    'Key_NamedKey_END', 'Key_None', 'Key_NumLock', 'Key_O', 'Key_Oem102',
    'Key_P', 'Key_PageDown', 'Key_PageUp', 'Key_Pause', 'Key_Period',
    'Key_PrintScreen', 'Key_Q', 'Key_R', 'Key_RightAlt', 'Key_RightArrow',
    'Key_RightBracket', 'Key_RightCtrl', 'Key_RightShift', 'Key_RightSuper',
    'Key_S', 'Key_ScrollLock', 'Key_Semicolon', 'Key_Slash', 'Key_Space',
    'Key_T', 'Key_Tab', 'Key_U', 'Key_UpArrow', 'Key_V', 'Key_W', 'Key_X',
    'Key_Y', 'Key_Z', 'Mod_Alt', 'Mod_Ctrl', 'Mod_None', 'Mod_Shift',
    'Mod_Super',
  ], excluded: [
    // "// [Internal] Reserved for mod storage" (imgui.h:1755), 1756.
    'Key_ReservedForModCtrl', 'Key_ReservedForModShift',
    'Key_ReservedForModAlt', 'Key_ReservedForModSuper',
    'Mod_Mask_', // imgui.h:1777
    'Key_COUNT', // obsolete (imgui.h:1780)
    'Mod_Shortcut', // obsolete (imgui.h:1781)
  ]},
  Col: {kind: 'enum', names: [
    'Col_Border', 'Col_BorderShadow', 'Col_Button', 'Col_ButtonActive',
    'Col_ButtonHovered', 'Col_COUNT', 'Col_CheckMark',
    'Col_CheckboxSelectedBg', 'Col_ChildBg', 'Col_DockingEmptyBg',
    'Col_DockingPreview', 'Col_DragDropTarget', 'Col_DragDropTargetBg',
    'Col_FrameBg', 'Col_FrameBgActive', 'Col_FrameBgHovered', 'Col_Header',
    'Col_HeaderActive', 'Col_HeaderHovered', 'Col_InputTextCursor',
    'Col_MenuBarBg', 'Col_ModalWindowDimBg', 'Col_NavCursor',
    'Col_NavWindowingDimBg', 'Col_NavWindowingHighlight',
    'Col_PlotHistogram', 'Col_PlotHistogramHovered', 'Col_PlotLines',
    'Col_PlotLinesHovered', 'Col_PopupBg', 'Col_ResizeGrip',
    'Col_ResizeGripActive', 'Col_ResizeGripHovered', 'Col_ScrollbarBg',
    'Col_ScrollbarGrab', 'Col_ScrollbarGrabActive',
    'Col_ScrollbarGrabHovered', 'Col_Separator', 'Col_SeparatorActive',
    'Col_SeparatorHovered', 'Col_SliderGrab', 'Col_SliderGrabActive',
    'Col_Tab', 'Col_TabDimmed', 'Col_TabDimmedSelected',
    'Col_TabDimmedSelectedOverline', 'Col_TabHovered', 'Col_TabSelected',
    'Col_TabSelectedOverline', 'Col_TableBorderLight',
    'Col_TableBorderStrong', 'Col_TableHeaderBg', 'Col_TableRowBg',
    'Col_TableRowBgAlt', 'Col_Text', 'Col_TextDisabled', 'Col_TextLink',
    'Col_TextSelectedBg', 'Col_TitleBg', 'Col_TitleBgActive',
    'Col_TitleBgCollapsed', 'Col_TreeLines', 'Col_UnsavedMarker',
    'Col_WindowBg',
  ], excluded: [
    // Obsolete (imgui.h:1927-1930).
    'Col_TabActive', 'Col_TabUnfocused', 'Col_TabUnfocusedActive',
    'Col_NavHighlight',
  ]},
  StyleVar: {kind: 'enum', names: [
    'StyleVar_Alpha', 'StyleVar_ButtonTextAlign', 'StyleVar_COUNT',
    'StyleVar_CellPadding', 'StyleVar_ChildBorderSize',
    'StyleVar_ChildRounding', 'StyleVar_DisabledAlpha',
    'StyleVar_DockingSeparatorSize', 'StyleVar_DragDropTargetRounding',
    'StyleVar_FrameBorderSize', 'StyleVar_FramePadding',
    'StyleVar_FrameRounding', 'StyleVar_GrabMinSize',
    'StyleVar_GrabRounding', 'StyleVar_ImageBorderSize',
    'StyleVar_ImageRounding', 'StyleVar_IndentSpacing',
    'StyleVar_ItemInnerSpacing', 'StyleVar_ItemSpacing',
    'StyleVar_MenuItemRounding', 'StyleVar_PopupBorderSize',
    'StyleVar_PopupRounding', 'StyleVar_ScrollbarPadding',
    'StyleVar_ScrollbarRounding', 'StyleVar_ScrollbarSize',
    'StyleVar_SelectableRounding', 'StyleVar_SelectableTextAlign',
    'StyleVar_SeparatorSize', 'StyleVar_SeparatorTextAlign',
    'StyleVar_SeparatorTextBorderSize', 'StyleVar_SeparatorTextPadding',
    'StyleVar_TabBarBorderSize', 'StyleVar_TabBarOverlineSize',
    'StyleVar_TabBorderSize', 'StyleVar_TabMinWidthBase',
    'StyleVar_TabMinWidthShrink', 'StyleVar_TabRounding',
    'StyleVar_TableAngledHeadersAngle',
    'StyleVar_TableAngledHeadersTextAlign', 'StyleVar_TreeLinesRounding',
    'StyleVar_TreeLinesSize', 'StyleVar_WindowBorderSize',
    'StyleVar_WindowMinSize', 'StyleVar_WindowPadding',
    'StyleVar_WindowRounding', 'StyleVar_WindowTitleAlign',
  ], excluded: []},
  ButtonFlags: {kind: 'flags', names: [
    'ButtonFlags_AllowOverlap', 'ButtonFlags_EnableNav',
    'ButtonFlags_MouseButtonLeft', 'ButtonFlags_MouseButtonMiddle',
    'ButtonFlags_MouseButtonRight', 'ButtonFlags_None',
  ], excluded: [
    'ButtonFlags_MouseButtonMask_', // imgui.h:2000
  ]},
  ColorEditFlags: {kind: 'flags', names: [
    'ColorEditFlags_AlphaBar', 'ColorEditFlags_AlphaNoBg',
    'ColorEditFlags_AlphaOpaque', 'ColorEditFlags_AlphaPreviewHalf',
    'ColorEditFlags_DisplayHSV', 'ColorEditFlags_DisplayHex',
    'ColorEditFlags_DisplayRGB', 'ColorEditFlags_Float',
    'ColorEditFlags_HDR', 'ColorEditFlags_InputHSV',
    'ColorEditFlags_InputRGB', 'ColorEditFlags_NoAlpha',
    'ColorEditFlags_NoBorder', 'ColorEditFlags_NoColorMarkers',
    'ColorEditFlags_NoDragDrop', 'ColorEditFlags_NoInputs',
    'ColorEditFlags_NoLabel', 'ColorEditFlags_NoOptions',
    'ColorEditFlags_NoPicker', 'ColorEditFlags_NoSidePreview',
    'ColorEditFlags_NoSmallPreview', 'ColorEditFlags_NoTooltip',
    'ColorEditFlags_None', 'ColorEditFlags_PickerHueBar',
    'ColorEditFlags_PickerHueWheel', 'ColorEditFlags_PickerNoRotate',
    'ColorEditFlags_Uint8',
  ], excluded: [
    // imgui.h:2047, 2050-2054; obsolete 2058.
    'ColorEditFlags_DefaultOptions_', 'ColorEditFlags_AlphaMask_',
    'ColorEditFlags_DisplayMask_', 'ColorEditFlags_DataTypeMask_',
    'ColorEditFlags_PickerMask_', 'ColorEditFlags_InputMask_',
    'ColorEditFlags_AlphaPreview',
  ]},
  SliderFlags: {kind: 'flags', names: [
    'SliderFlags_AlwaysClamp', 'SliderFlags_ClampOnInput',
    'SliderFlags_ClampZeroRange', 'SliderFlags_ColorMarkers',
    'SliderFlags_Logarithmic', 'SliderFlags_NoInput',
    'SliderFlags_NoRoundToFormat', 'SliderFlags_NoSpeedTweaks',
    'SliderFlags_None', 'SliderFlags_WrapAround',
  ], excluded: [
    'SliderFlags_InvalidMask_', // imgui.h:2080
  ]},
  MouseButton: {kind: 'enum', names: [
    'MouseButton_COUNT', 'MouseButton_Left', 'MouseButton_Middle',
    'MouseButton_Right',
  ], excluded: []},
  Cond: {kind: 'enum', names: [
    'Cond_Always', 'Cond_Appearing', 'Cond_FirstUseEver', 'Cond_None',
    'Cond_Once',
  ], excluded: []},
  TableFlags: {kind: 'flags', names: [
    'TableFlags_Borders', 'TableFlags_BordersH', 'TableFlags_BordersInner',
    'TableFlags_BordersInnerH', 'TableFlags_BordersInnerV',
    'TableFlags_BordersOuter', 'TableFlags_BordersOuterH',
    'TableFlags_BordersOuterV', 'TableFlags_BordersV',
    'TableFlags_ContextMenuInBody', 'TableFlags_Hideable',
    'TableFlags_HighlightHoveredColumn', 'TableFlags_NoBordersInBody',
    'TableFlags_NoBordersInBodyUntilResize', 'TableFlags_NoClip',
    'TableFlags_NoHostExtendX', 'TableFlags_NoHostExtendY',
    'TableFlags_NoKeepColumnsVisible', 'TableFlags_NoPadInnerX',
    'TableFlags_NoPadOuterX', 'TableFlags_NoSavedSettings',
    'TableFlags_None', 'TableFlags_PadOuterX', 'TableFlags_PreciseWidths',
    'TableFlags_Reorderable', 'TableFlags_Resizable', 'TableFlags_RowBg',
    'TableFlags_ScrollX', 'TableFlags_ScrollY', 'TableFlags_SizingFixedFit',
    'TableFlags_SizingFixedSame', 'TableFlags_SizingStretchProp',
    'TableFlags_SizingStretchSame', 'TableFlags_SortMulti',
    'TableFlags_SortTristate', 'TableFlags_Sortable',
  ], excluded: [
    'TableFlags_SizingMask_', // imgui.h:2211
  ]},
  TableColumnFlags: {kind: 'flags', names: [
    'TableColumnFlags_AngledHeader', 'TableColumnFlags_DefaultHide',
    'TableColumnFlags_DefaultSort', 'TableColumnFlags_Disabled',
    'TableColumnFlags_IndentDisable', 'TableColumnFlags_IndentEnable',
    'TableColumnFlags_IsEnabled', 'TableColumnFlags_IsHovered',
    'TableColumnFlags_IsSorted', 'TableColumnFlags_IsVisible',
    'TableColumnFlags_NoClip', 'TableColumnFlags_NoHeaderLabel',
    'TableColumnFlags_NoHeaderWidth', 'TableColumnFlags_NoHide',
    'TableColumnFlags_NoReorder', 'TableColumnFlags_NoResize',
    'TableColumnFlags_NoSort', 'TableColumnFlags_NoSortAscending',
    'TableColumnFlags_NoSortDescending', 'TableColumnFlags_None',
    'TableColumnFlags_PreferSortAscending',
    'TableColumnFlags_PreferSortDescending', 'TableColumnFlags_WidthFixed',
    'TableColumnFlags_WidthStretch',
  ], excluded: [
    // imgui.h:2246-2249.
    'TableColumnFlags_WidthMask_', 'TableColumnFlags_IndentMask_',
    'TableColumnFlags_StatusMask_', 'TableColumnFlags_NoDirectResize_',
  ]},
  TableRowFlags: {kind: 'flags', names: [
    'TableRowFlags_Headers', 'TableRowFlags_None',
  ], excluded: []},
  DrawFlags: {kind: 'flags', names: [
    'DrawFlags_AAFill', 'DrawFlags_AALineEnds', 'DrawFlags_AALines',
    'DrawFlags_CapSquare', 'DrawFlags_Closed', 'DrawFlags_JoinMiter',
    'DrawFlags_None', 'DrawFlags_RoundAll', 'DrawFlags_RoundBottom',
    'DrawFlags_RoundBottomLeft', 'DrawFlags_RoundBottomRight',
    'DrawFlags_RoundLeft', 'DrawFlags_RoundNone', 'DrawFlags_RoundRight',
    'DrawFlags_RoundTop', 'DrawFlags_RoundTopLeft',
    'DrawFlags_RoundTopRight', 'DrawFlags_StrokeCenter',
    'DrawFlags_StrokeCenterBiased', 'DrawFlags_StrokeInside',
    'DrawFlags_StrokeLegacy', 'DrawFlags_StrokeOutside',
    'DrawFlags_TextNoPixelSnap', 'DrawFlags_UseTexForRoundCorners',
    'DrawFlags_UseVtxOffset',
  ], excluded: [
    // imgui.h:3505, 3508-3512; obsolete 3517-3526 (one named here).
    'DrawFlags_AllowTexForRoundCorners_', 'DrawFlags_RoundMask_',
    'DrawFlags_AllowInPushScope_', 'DrawFlags_AllowInFrameScope_',
    'DrawFlags_StrokeMask_', 'DrawFlags_InvalidMask_',
    'DrawFlags_RoundCornersAll',
  ]},
};

// Exported constant -> its value, copied by hand from imgui.h (the line in
// the comment): three per type of `constantTypes` (`Key` and `Mod`
// together), and the two float limits (G4), whose values are the C
// `float` FLT_MAX and FLT_MIN written as the doubles they convert to.
// Every one is also an exported name, so a sample that is not exported is
// `MISSING` in test/imgui-bind-surface.js.
const constants = {
  WindowFlags_NoTitleBar: 1, // imgui.h:1228
  WindowFlags_NoDocking: 524288, // 1 << 19, imgui.h:1247
  WindowFlags_NoInputs: 197120, // (1 << 9) | (1 << 16) | (1 << 17), 1250
  ChildFlags_Borders: 1, // imgui.h:1279
  ChildFlags_FrameStyle: 128, // imgui.h:1286
  ChildFlags_NavFlattened: 256, // imgui.h:1287
  InputTextFlags_CharsDecimal: 1, // imgui.h:1339
  InputTextFlags_Password: 1024, // imgui.h:1353
  InputTextFlags_CallbackResize: 4194304, // 1 << 22, imgui.h:1370
  TreeNodeFlags_Selected: 1, // imgui.h:1390
  TreeNodeFlags_CollapsingHeader: 26, // 2 | 8 | 16, imgui.h:1408
  TreeNodeFlags_DrawLinesToNodes: 1048576, // 1 << 20, imgui.h:1414
  PopupFlags_MouseButtonRight: 8, // 2 << 2, imgui.h:1430
  PopupFlags_NoReopen: 32, // imgui.h:1432
  PopupFlags_AnyPopup: 3072, // (1 << 10) | (1 << 11), imgui.h:1438
  SelectableFlags_NoAutoClosePopups: 1, // imgui.h:1448
  SelectableFlags_SpanAllColumns: 2, // imgui.h:1449
  SelectableFlags_Highlight: 32, // imgui.h:1453
  ComboFlags_PopupAlignLeft: 1, // imgui.h:1466
  ComboFlags_HeightLargest: 16, // imgui.h:1470
  ComboFlags_WidthFitPreview: 128, // imgui.h:1473
  TabBarFlags_Reorderable: 1, // imgui.h:1481
  TabBarFlags_DrawSelectedOverline: 64, // imgui.h:1487
  TabBarFlags_FittingPolicyScroll: 512, // imgui.h:1492
  TabItemFlags_UnsavedDocument: 1, // imgui.h:1505
  TabItemFlags_Leading: 64, // imgui.h:1511
  TabItemFlags_NoAssumedClosure: 256, // imgui.h:1513
  FocusedFlags_ChildWindows: 1, // imgui.h:1520
  FocusedFlags_DockHierarchy: 16, // imgui.h:1524
  FocusedFlags_RootAndChildWindows: 3, // 2 | 1, imgui.h:1525
  HoveredFlags_ChildWindows: 1, // imgui.h:1534
  HoveredFlags_ForTooltip: 4096, // 1 << 12, imgui.h:1556
  HoveredFlags_NoSharedDelay: 131072, // 1 << 17, imgui.h:1565
  DockNodeFlags_KeepAliveOnly: 1, // imgui.h:1574
  DockNodeFlags_PassthruCentralNode: 8, // imgui.h:1577
  DockNodeFlags_AutoHideTabBar: 64, // imgui.h:1580
  Dir_None: -1, // imgui.h:1639, the one negative value
  Dir_Up: 2, // imgui.h:1642
  Dir_COUNT: 4, // imgui.h:1644
  Key_NamedKey_BEGIN: 512, // imgui.h:1665
  Key_LeftArrow: 513, // imgui.h:1668, after Key_Tab = 512
  Mod_Ctrl: 4096, // 1 << 12, imgui.h:1773
  Col_Text: 0, // imgui.h:1861
  Col_WindowBg: 2, // imgui.h:1863
  Col_COUNT: 63, // imgui.h:1924, after 63 colours from 1861
  StyleVar_Alpha: 0, // imgui.h:1945
  StyleVar_WindowPadding: 2, // imgui.h:1947
  StyleVar_COUNT: 45, // imgui.h:1990, after 45 variables from 1945
  ButtonFlags_MouseButtonLeft: 1, // imgui.h:1997
  ButtonFlags_MouseButtonMiddle: 4, // imgui.h:1999
  ButtonFlags_EnableNav: 8, // imgui.h:2001
  ColorEditFlags_NoAlpha: 2, // imgui.h:2009
  ColorEditFlags_AlphaBar: 262144, // 1 << 18, imgui.h:2031
  ColorEditFlags_InputHSV: 536870912, // 1 << 29, imgui.h:2042
  SliderFlags_Logarithmic: 32, // imgui.h:2069
  SliderFlags_WrapAround: 256, // imgui.h:2072
  SliderFlags_ColorMarkers: 4096, // 1 << 12, imgui.h:2076
  MouseButton_Left: 0, // imgui.h:2087
  MouseButton_Middle: 2, // imgui.h:2089
  MouseButton_COUNT: 5, // imgui.h:2090
  Cond_Always: 1, // imgui.h:2130
  Cond_FirstUseEver: 4, // imgui.h:2132
  Cond_Appearing: 8, // imgui.h:2133
  TableFlags_Resizable: 1, // imgui.h:2166
  TableFlags_SizingStretchSame: 32768, // 4 << 13, imgui.h:2189
  TableFlags_HighlightHoveredColumn: 268435456, // 1 << 28, imgui.h:2208
  TableColumnFlags_DefaultHide: 2, // imgui.h:2220
  TableColumnFlags_AngledHeader: 262144, // 1 << 18, imgui.h:2237
  TableColumnFlags_IsHovered: 134217728, // 1 << 27, imgui.h:2243
  TableRowFlags_None: 0, // imgui.h:2255
  TableRowFlags_Headers: 1, // imgui.h:2256
  DrawFlags_RoundAll: 240, // 16 | 32 | 64 | 128, imgui.h:3468
  DrawFlags_Closed: 512, // 1 << 9, imgui.h:3475
  DrawFlags_StrokeLegacy: 917504, // 7 << 17, imgui.h:3499
  FLT_MAX: 3.4028234663852886e+38, // <float.h>, (2 - 2^-23) * 2^127
  FLT_MIN: 1.1754943508222875e-38, // <float.h>, 2^-126
};

/**
 * The `constantTypes` key of an exported constant's name, 'FLT' for the
 * two float limits, or undefined when `name` is not a constant's.
 */
function constantType(name) {
  if (name === 'FLT_MIN' || name === 'FLT_MAX')
    return 'FLT';
  const u = name.indexOf('_');
  if (u <= 0)
    return undefined;
  const prefix = name.slice(0, u) === 'Mod' ? 'Key' : name.slice(0, u);
  return Object.prototype.hasOwnProperty.call(constantTypes, prefix) ?
      prefix :
      undefined;
}

/**
 * Every exported name that is not a constant, sorted: the groups' names.
 * The constants are allConstants().
 */
function all() {
  const names = [];
  for (const g of Object.keys(groups))
    names.push(...groups[g]);
  return names.sort();
}

/**
 * Every exported constant's name, sorted: each type's `names` and the two
 * float limits.
 */
function allConstants() {
  const names = ['FLT_MAX', 'FLT_MIN'];
  for (const t of Object.keys(constantTypes))
    names.push(...constantTypes[t].names);
  return names.sort();
}

module.exports = {
  groups,
  constantTypes,
  constants,
  constantType,
  all,
  allConstants,
};
