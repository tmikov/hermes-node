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
  // Incomplete until Task 4.
  Widgets: ['button', 'checkbox', 'sliderFloat3'],
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
// minus `ImGui` / `Im`, up to and including the first `_`) -> how many
// are exported, the kind (G1: a flags type accepts any union of its
// enumerators' bits, an enum type one enumerator's value), and the names
// the table excludes (and its obsolete, uncompiled ones), none of which may
// be exported. The `ImGuiMod_*` enumerators are members of `ImGuiKey`
// (imgui.h:1772-1777), so `Mod_` names count under `Key`.
const constantTypes = {
  WindowFlags: {count: 24, kind: 'flags', excluded: [
    // "// [Internal]" (imgui.h:1252), 1253-1258.
    'WindowFlags_DockNodeHost', 'WindowFlags_ChildWindow',
    'WindowFlags_Tooltip', 'WindowFlags_Popup', 'WindowFlags_Modal',
    'WindowFlags_ChildMenu',
  ]},
  ChildFlags: {count: 10, kind: 'flags', excluded: []},
  InputTextFlags: {count: 26, kind: 'flags', excluded: []},
  TreeNodeFlags: {count: 22, kind: 'flags', excluded: [
    // Obsolete (imgui.h:1417), not compiled.
    'TreeNodeFlags_NavLeftJumpsBackHere',
  ]},
  PopupFlags: {count: 10, kind: 'flags', excluded: [
    // imgui.h:1439-1441.
    'PopupFlags_MouseButtonShift_', 'PopupFlags_MouseButtonMask_',
    'PopupFlags_InvalidMask_',
  ]},
  SelectableFlags: {count: 8, kind: 'flags', excluded: [
    'SelectableFlags_DontClosePopups', // obsolete (imgui.h:1457)
  ]},
  ComboFlags: {count: 9, kind: 'flags', excluded: [
    'ComboFlags_HeightMask_', // imgui.h:1474
  ]},
  TabBarFlags: {count: 11, kind: 'flags', excluded: [
    // imgui.h:1493, 1494; obsolete 1497.
    'TabBarFlags_FittingPolicyMask_', 'TabBarFlags_FittingPolicyDefault_',
    'TabBarFlags_FittingPolicyResizeDown',
  ]},
  TabItemFlags: {count: 10, kind: 'flags', excluded: []},
  FocusedFlags: {count: 7, kind: 'flags', excluded: []},
  HoveredFlags: {count: 21, kind: 'flags', excluded: []},
  DockNodeFlags: {count: 8, kind: 'flags', excluded: [
    // Obsolete (imgui.h:1584, 1585).
    'DockNodeFlags_NoSplit', 'DockNodeFlags_NoDockingInCentralNode',
  ]},
  Dir: {count: 6, kind: 'enum', excluded: []},
  Key: {count: 160, kind: 'enum', excluded: [
    // "// [Internal] Reserved for mod storage" (imgui.h:1755), 1756.
    'Key_ReservedForModCtrl', 'Key_ReservedForModShift',
    'Key_ReservedForModAlt', 'Key_ReservedForModSuper',
    'Mod_Mask_', // imgui.h:1777
    'Key_COUNT', // obsolete (imgui.h:1780)
    'Mod_Shortcut', // obsolete (imgui.h:1781)
  ]},
  Col: {count: 64, kind: 'enum', excluded: [
    // Obsolete (imgui.h:1927-1930).
    'Col_TabActive', 'Col_TabUnfocused', 'Col_TabUnfocusedActive',
    'Col_NavHighlight',
  ]},
  StyleVar: {count: 46, kind: 'enum', excluded: []},
  ButtonFlags: {count: 6, kind: 'flags', excluded: [
    'ButtonFlags_MouseButtonMask_', // imgui.h:2000
  ]},
  ColorEditFlags: {count: 27, kind: 'flags', excluded: [
    // imgui.h:2047, 2050-2054; obsolete 2058.
    'ColorEditFlags_DefaultOptions_', 'ColorEditFlags_AlphaMask_',
    'ColorEditFlags_DisplayMask_', 'ColorEditFlags_DataTypeMask_',
    'ColorEditFlags_PickerMask_', 'ColorEditFlags_InputMask_',
    'ColorEditFlags_AlphaPreview',
  ]},
  SliderFlags: {count: 10, kind: 'flags', excluded: [
    'SliderFlags_InvalidMask_', // imgui.h:2080
  ]},
  MouseButton: {count: 4, kind: 'enum', excluded: []},
  Cond: {count: 5, kind: 'enum', excluded: []},
  TableFlags: {count: 36, kind: 'flags', excluded: [
    'TableFlags_SizingMask_', // imgui.h:2211
  ]},
  TableColumnFlags: {count: 24, kind: 'flags', excluded: [
    // imgui.h:2246-2249.
    'TableColumnFlags_WidthMask_', 'TableColumnFlags_IndentMask_',
    'TableColumnFlags_StatusMask_', 'TableColumnFlags_NoDirectResize_',
  ]},
  TableRowFlags: {count: 2, kind: 'flags', excluded: []},
  DrawFlags: {count: 25, kind: 'flags', excluded: [
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
 * The constants are counted per type instead (`constantTypes`), since
 * listing 583 of them here would be typing them by hand a second time.
 */
function all() {
  const names = [];
  for (const g of Object.keys(groups))
    names.push(...groups[g]);
  return names.sort();
}

module.exports = {groups, constantTypes, constants, constantType, all};
