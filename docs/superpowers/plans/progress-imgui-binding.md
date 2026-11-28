# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-11-27-imgui-binding.md`
(implementation plan), its surface table
`docs/superpowers/plans/2026-11-27-imgui-binding-surface.md`, and the design
`docs/superpowers/specs/2026-11-27-imgui-binding-design.md`.

## Status

| Task | Description | Status |
|------|-------------|--------|
| Task 1 | Binding core, the error model, and the Frame group | done (GL) |
| Task 2 | Constants, and flag and enum checking | done (GL) |
| Task 3 | Windows, Layout, IDs, Text, Item and input queries | done (GL) |
| Task 4 | Widgets | done (GL) |
| Task 5 | Trees, tabs, menus, popups and tooltips | done (GL) |
| Task 6 | Tables | done (GL) |
| Task 7 | Style, plots, draw list, list clipper | done (GL) |
| Task 8 | Docking and settings | done (GL) |
| Task 9 | The application | done (GL) |
| Task 10 | Documentation and the full suite | done (GL) |
| Codex review | Codex's code review, round 1 (C1-C8, L1-L3) | done (GL) |

## Mutations

Each test must be shown able to fail: the change made, and what it did.
Every mutation was restored, and the addon rebuilt, afterwards.

| Task | Test | Mutation | Result |
|------|------|----------|--------|
| 1 | the three new tests | (RED) run against the part A build, before any binding code existed | `imgui-bind-surface.js`: `MISSING begin beginChild ...` (29 names); `ui-imgui-bind-args.js`: `FAILED TypeError: undefined is not a function` at the first new call; `ui-imgui-bind-errors.js`: `WRONGCODE undefined (want ERR_IMGUI_USAGE)` |
| 1 | `ui-imgui-bind-errors.js` | `guard::configureContext()` not called from `ensureContext` | ImGui aborted in the first `newFrame()`: `imgui.cpp:11858 ... Assertion ... g.ErrorCallback != __null failed` (beginFrame() still turns the three error outputs off, so with no callback nothing reports); test failed |
| 1 | `ui-imgui-bind-errors.js` | the usage-error check in `render()` moved after `hnui_frame_target` | `LEFT_OPEN WRONG code=ERR_IMGUI_USAGE frameOpen=false published=1->2 next=true`: the throwing render() had taken the target and the tick published; test failed |
| 1 | `ui-imgui-bind-args.js` | the re-entrancy flag never set (`gInsideCall = true` removed from `Call`'s constructor) | the accessor's `render()` ended the frame, then `checkbox` threw `imgui.checkbox(): no frame is open`, uncaught; no `REENTRANT` line, test failed |
| 1 | `ui-imgui-bind-args.js`, `ui-imgui-bind-errors.js` | call counting moved from `enterImGui()` to `Call`'s constructor (before the argument reads) | args: `CALLED begin ERR_IMGUI_NO_FRAME: ...` (the first "not called" check, NOFRAME); errors: `CALLED endGroup: imgui.endGroup(): EndGroup() with no group open`; both failed |
| 1 | `ui-imgui-bind-errors.js` | `canEndGroup()` returning nullptr always | ImGui aborted in GUARD_GROUP: `imgui.cpp:12719: void ImGui::EndGroup(): Assertion g.GroupStack.Size > 0 failed`; test failed |
| 1 | `ui-imgui-bind-errors.js` | `canEndChild()` also refusing when the child's `SkipItems` is set (that is, requiring a true `beginChild`) | the clipped child's `endChild()` threw `imgui.endChild(): EndChild() outside a child window`, uncaught in GUARD_CHILD; test failed |
| 1 | `imgui-bind-surface.js` | `popID` removed from `bind-groups.js` | `EXTRA popID`; test failed |
| 1 | `imgui-bind-surface.js` | `getID` (not bound yet) added to `bind-groups.js` | `MISSING getID`; test failed |
| 1 | `ui-imgui-bind-errors.js` | a fourth guard condition `canDummy:never` registered and never refused | `UNREFUSED canDummy:never`; test failed |
| 1 | `ui-imgui-bind-args.js` | `ImGui::ShowMetricsWindow()` removed, `enterImGui()` kept (Frame group: an ImGui call removed) | `FRAME WRONG ...` with no `Dear ImGui Metrics/Debugger` in `windows`; `COVERAGE Frame ok` still printed, as it must (the counter moved); test failed |
| 1 | `ui-imgui-bind-args.js` | `enterImGui()` removed from `getDeltaTime` (Frame group: coverage) | `UNCOVERED Frame getDeltaTime`; test failed |
| 1 | `ui-imgui-bind-args.js` | `showDemoWindow`'s write-back of `pOpen` removed (Frame group: a write-back) | `FRAME WRONG ... writes={"demo":[],"metrics":[true]}`; test failed |
| 1 | `ui-imgui-bind-args.js` | the element-type check of `refBool` removed (an argument check) | `NOTHROW ERR_INVALID_ARG_TYPE` at `checkbox('a', [1])` in REFARRAY; test failed |
| 1 | `ui-imgui-bind-errors.js` | `io.ConfigErrorRecoveryEnableDebugLog = false` removed from `setRecoveryFlags()` | PASS, as expected: no test can see it (see "Behaviours no test detects") |
| 1 (review) | `ui-imgui-bind-errors.js` | `canEndChild()` requiring a true `beginChild` again, after GUARD_CHILD was changed to count only the frame whose `beginChild()` returned false and to catch the refusal | the assertion itself now fails: `SETUP child clipped`, then `GUARD_CHILD not accepted`; test failed |
| 1 (review) | `ui-imgui-bind-errors.js` | every usage error traced twice (`throwUsage()` also tracing `usage error: <msg>`) | the ONCE run failed: `ONCE-NEXT: is not on the line after the previous match` at the second line, `Calling End() too many times!` appearing twice; test failed |
| 2 | `ui-imgui-bind-constants.js`, `imgui-bind-surface.js` | (RED) run against Task 1's build, with the test and `bind-groups.js` written and no constants exported | constants: `COUNT WindowFlags 0 want 24` and one such line per type; surface: `MISSING WindowFlags_NoTitleBar ... FLT_MAX FLT_MIN` (every sample) |
| 2 | `ui-imgui-bind-constants.js`, `imgui-bind-surface.js` | the `HNIMGUI_CONST(ImGuiCond_Appearing)` line deleted | constants: `COUNT Cond 4 want 5`, `WRONG Cond_Appearing=undefined want 8`; surface: `MISSING Cond_Appearing`; both failed |
| 2 | `ui-imgui-bind-constants.js` | `HNIMGUI_CONST(ImGuiWindowFlags_ChildWindow)` (marked [Internal]) added to the window-flags list | `COUNT WindowFlags 25 want 24`, `EXCLUDED WindowFlags_ChildWindow`; test failed |
| 2 | `ui-imgui-bind-constants.js` | the mask check removed from `Call::flags` (`if (false && bad != 0)`) | `NOTHROW ERR_OUT_OF_RANGE` at `begin('w', null, 1 << 30)`, after `CONSTANTS`, `FLT`, `HIGHBIT`; test failed |
| 2 | `ui-imgui-bind-constants.js` | the value check removed from `Call::enumValue` (every value accepted) | `setNextWindowPos([0, 0], 3)` reached ImGui, which aborted: `imgui.cpp:9433 ... Assertion cond == 0 \|\| ImIsPowerOfTwo(cond) failed`; no `REJECT` line, test failed |
| 2 | `ui-imgui-bind-constants.js` | `pushStyleColor`'s `Col_COUNT` refusal removed | `NOTHROW ERR_OUT_OF_RANGE` at `pushStyleColor(imgui.Col_COUNT, 0)`; test failed |
| 2 | `ui-imgui-bind-constants.js` | `FLT_MIN` exported as `-FLT_MIN` | `WRONG FLT_MIN=-1.1754943508222875e-38 want 1.1754943508222875e-38`; test failed |
| 2 | `imgui-bind-surface.js` | the `Cond` entry removed from `bind-groups.js`'s `constantTypes` | `EXTRA Cond_Always Cond_Appearing Cond_FirstUseEver Cond_None Cond_Once`; test failed |
| 2 (review) | `imgui-bind-surface.js` (no display) | the `HNIMGUI_CONST(ImGuiCond_Appearing)` line deleted, after the exact name lists were added | `MISSING Cond_Appearing`; test failed |
| 2 (review) | `imgui-bind-surface.js`, `ui-imgui-bind-constants.js` | `HNIMGUI_CONST(ImGuiWindowFlags_NoDocking)` replaced by `HNIMGUI_CONST(ImGuiWindowFlags_ChildWindow)` (count unchanged) | surface: `EXTRA WindowFlags_ChildWindow`, `MISSING WindowFlags_NoDocking`; constants: `EXCLUDED WindowFlags_ChildWindow`, `WRONG WindowFlags_NoDocking=undefined want 524288`; both failed |
| 3 | `ui-imgui-bind-windows.js`, `ui-imgui-bind-layout.js` | (RED) none recorded: the five sections were implemented before the two tests were first run (see the Task 3 notes) | - |
| 3 | `ui-imgui-bind-windows.js` | `ImGui::SetNextWindowBgAlpha()` removed, `enterImGui()` kept (Windows: an ImGui call removed) | `BGALPHA WRONG 200,175=19,19,87 120,120=19,19,87 280,230=19,19,87` (the 0.25-alpha blue blended over the clear); `COVERAGE Windows ok` still printed; test failed |
| 3 | `ui-imgui-bind-windows.js` | `enterImGui()` removed from `getScrollMaxY` (Windows: coverage) | `UNCOVERED Windows getScrollMaxY` (SCROLL still passed); test failed |
| 3 | `ui-imgui-bind-windows.js` | `begin()`'s `write(pOpen)` removed (Windows: a write-back) | `CLOSE WRONG open=true active=true at=329,60`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `setScrollHereY`'s 0..1 check removed (Windows: an argument check) | ImGui aborted in REJECTED: `imgui.cpp:12979 ... Assertion center_y_ratio >= 0.0f && center_y_ratio <= 1.0f failed`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `begin()`'s empty-name check removed | aborted in REJECTED: `imgui.cpp:8003 ... Assertion name != __null && name[0] != '\0' failed`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `beginChild`'s `WindowFlags_AlwaysAutoResize` check removed | aborted: `imgui.cpp:6904 ... Cannot specify ImGuiWindowFlags_AlwaysAutoResize for BeginChild()`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `beginChild`'s `ChildFlags_AlwaysAutoResize` with `_ResizeX`/`_ResizeY` check removed | aborted: `imgui.cpp:6907 ... Cannot use ImGuiChildFlags_ResizeX or ImGuiChildFlags_ResizeY with ImGuiChildFlags_AlwaysAutoResize!`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `beginChild`'s "`AlwaysAutoResize` needs `AutoResizeX`/`Y`" check removed | aborted: `imgui.cpp:6908 ... Must use ImGuiChildFlags_AutoResizeX or ImGuiChildFlags_AutoResizeY with ImGuiChildFlags_AlwaysAutoResize!`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `getWindowPos(out)` returning a new array instead of `out` (`resultVec2` given an argument index past argc) | `POS WRONG pos=40,50 size=300,200 out=/300,200 same=false/true`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::Indent()` removed, `enterImGui()` kept (Layout: an ImGui call removed) | `INDENT WRONG 28 28 8` (the unindent then moved the next item left of the margin); test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::PopItemWidth()` removed | `WIDTH WRONG 390,123,90,90,90`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `enterImGui()` removed from `getFrameHeight` (Layout: coverage) | `UNCOVERED Layout getFrameHeight`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `pushID(number)` dispatched to the string overload (`PushID(std::to_string(id).c_str())`) | `IDS WRONG {... "int7":1753019048,"str7":1753019048 ...}`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `getID(number)` dispatched to the string overload | `IDS WRONG {... "getInt":3327332898,"getStr":3327332898}`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::PopID()` removed (IDs: an ImGui call removed) | the layout window's `end()` threw `imgui.end(): Missing PopID()`, uncaught, after `GREEN ok`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::TextColored()` replaced by `TextUnformatted()` (Text: an ImGui call removed) | `GREEN WRONG 0 0`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::IsKeyDown()` replaced by `false`, `enterImGui()` kept (Queries: an ImGui call removed) | `TIMEOUT KEY down`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::SetKeyboardFocusHere()` removed | `FOCUSHERE WRONG before={"here":false,"other":true} after={"here":false,"other":true}`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `isMouseDoubleClicked` always false (ImGui call result discarded), against the first version of DOUBLE (retry loop) | `DOUBLE WRONG never seen` after five attempts; test failed |
| 3 | `ui-imgui-bind-layout.js` | `getMouseDragDelta(..., out)` returning a new array (Queries: the out write-back) | `DRAG WRONG 40,30 ` (the value right, `out` left empty); test failed |
| 3 | `ui-imgui-bind-layout.js` | the mouse-button range check removed (`kLastMouseButton` = `INT32_MAX`; Queries: an argument check) | aborted in REJECTED: `imgui.cpp:10766 ... IsMouseClicked ... Assertion button >= 0 && button < ... failed`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `readKey`'s named-key-or-single-modifier check removed, the enum check kept | aborted in REJECTED: `imgui.cpp:10286 ... GetKeyData ... Assertion IsNamedKey(key) ... failed`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `setKeyboardFocusHere`'s `offset >= -1` check removed | aborted in REJECTED: `imgui.cpp:9687 ... Assertion offset >= -1 failed`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `IsWindowHovered()`'s result replaced by false | `HOVER WRONG inside=false outside=false`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `IsWindowFocused()`'s result replaced by false | `FOCUS WRONG afterB={"a":false,"b":false} afterA={"a":false,"b":false}`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `ImGui::SetScrollHereY()` removed | `SCROLL WRONG before=0,452 after=0/452`; test failed |
| 3 | `ui-imgui-bind-windows.js` | `GetContentRegionAvail()` replaced by `GetWindowSize()` | `AVAIL WRONG 300,200 out=300,200`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::SameLine()` removed | `SAMELINE WRONG` (the second button at y 70, the first at 47); test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::BeginGroup()` removed | GROUP's `endGroup()` refused by its guard, `imgui.endGroup(): EndGroup() with no group open`, uncaught, after `INDENT ok`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `Dummy()` given its size transposed | `DUMMY WRONG {"min":[28,47],"max":[68,77]}`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::SetCursorScreenPos()` removed | `CURSOR WRONG 28,47 28,47 28,47`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::Spacing()` removed | `SPACING WRONG` with `"plain":4,"spacing":4`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `calcTextSize` passing false for `hideTextAfterDoubleHash` | `TEXTSIZE WRONG` with `"hidden":[42,13]` where `"size":[21,13]`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `ImGui::AlignTextToFramePadding()` removed | `HEIGHTS WRONG` with `"cursorY":64,"alignedY":64`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `textDisabled` passing its text as the format (`TextDisabled(s.c_str(), 1234567)`) | `TEXTS WRONG empty= percent=49/14` (`%d` formatted); test failed |
| 3 | `ui-imgui-bind-layout.js` | `IsItemHovered()`'s result replaced by false | `HOVERED WRONG false false`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `IsItemClicked()`'s result replaced by false | `CLICK WRONG [true,false,true,true,false]`; test failed |
| 3 | `ui-imgui-bind-layout.js` | `GetMousePos()` replaced by `(0, 0)` | `MOUSEPOS WRONG 0,0 0,0`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `ImGui::SetItemDefaultFocus()` removed | `TIMEOUT DEFAULTFOCUS`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `ImGui::TextWrapped()` removed | `TEXTS WRONG textWrapped:not-below textWrapped:one-line`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `ImGui::BulletText()` removed | `TEXTS WRONG bulletText:not-below`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `ImGui::LabelText()` removed | `TEXTS WRONG labelText:not-below`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `ImGui::IsMouseDoubleClicked()` removed (`v = false`), against the hook-based DOUBLE | `DOUBLE WRONG afterFirst=0 doubles=0`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `enterImGui()` removed from `popID` (IDs: coverage) | `UNCOVERED IDs popID`; test failed. (Removing it from one of `getID`'s two paths was tried first and PASSED: the other path still counts, so that mutation does not test coverage.) |
| 3 (review) | `ui-imgui-bind-layout.js` | `enterImGui()` removed from `labelText` (Text: coverage) | `UNCOVERED Text labelText`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `enterImGui()` removed from `isItemActive` (Queries: coverage) | `UNCOVERED Queries isItemActive`; test failed |
| 3 (review) | `ui-imgui-bind-layout.js` | `__test.setDoubleClickTime()`'s two assignments removed (informational) | PASS: the clicks fell within 0.30 s in that run; see the Task 3 note on DOUBLE |
| 4 | the four new or extended tests | (RED) run against the Task 3 build, before any Task 4 binding code | `imgui-bind-surface.js`: `MISSING arrowButton beginCombo ... smallButton` (25 names); `ui-imgui-bind-args.js`: `WRONGCODE undefined (want ERR_IMGUI_NO_FRAME): undefined is not a function` (NOFRAME, the first new sample); `ui-imgui-bind-widgets.js` and `ui-imgui-bind-input-text.js`: `TypeError: undefined is not a function` at the first `smallButton` / `inputText` call; `ui-imgui-bind-errors.js`: GUARD_LABELID aborted on `imgui.cpp:12319 ... Assertion id != window->ID ... failed` (`button('')` reached ImGui) |
| 4 | `ui-imgui-bind-widgets.js` | `ImGui::SmallButton()` replaced by `false`, `enterImGui()` kept | `BUTTONS WRONG smallButton {... "smallButton":{"count":0,...}}`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `enterImGui()` removed from `bullet` | `UNCOVERED Widgets bullet`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `radioButton(label, v, vButton)`'s write-back of `v` removed | `RADIO WRONG 1 1 0 1` (pressed once, `v[0]` still 0); test failed |
| 4 | `ui-imgui-bind-widgets.js` | the `SliderFlags_WrapAround` check removed (`sliderFlags()`, every slider) | ImGui aborted: `imgui_widgets.cpp:3310 ... (flags & ImGuiSliderFlags_WrapAround) == 0 failed`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `sliderFloat2..4` reading their flags as a drag's (WrapAround accepted; Task 1's owed case for `sliderFloat3`) | the same abort at `imgui_widgets.cpp:3310`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | the format check removed (`validFormat()` skipped in `Call::format`) | `NOTHROW ERR_INVALID_ARG_VALUE`: `sliderFloat('f', [0], 0, 1, '%s')` reached ImGui, which executed `%s` with a float argument and did not crash in that run (undefined behaviour; no ASAN report); test failed |
| 4 | `ui-imgui-bind-widgets.js` | `sliderFloat2..4`'s format read with `string()` instead of `format()` (Task 1's owed case for `sliderFloat3`) | `NOTHROW ERR_INVALID_ARG_VALUE` (`sliderFloat2`, '%s'); test failed |
| 4 | `ui-imgui-bind-widgets.js` | the `ColorEditFlags_Display*` exclusivity check removed | ImGui aborted: `imgui_widgets.cpp:5960 ... ImIsPowerOfTwo(flags & ImGuiColorEditFlags_DisplayMask_) failed`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `beginCombo`'s `ComboFlags_NoPreview` check removed | ImGui aborted: `imgui_widgets.cpp:1959 ... failed`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `arrowButton`'s `Dir_None` / `Dir_COUNT` check removed | ImGui aborted: `imgui_draw.cpp:7595 RenderArrow ... Assertion 0 failed`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `selectable(label, p)`'s write-back removed | `SELREF WRONG false false 2`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `progressBar`'s `size` replaced by the default | `PROGRESS WRONG [{"min":[28,47],"max":[612,66]},...]`; test failed |
| 4 | `ui-imgui-bind-widgets.js` | `colorButton`'s colour passed with R and G swapped | `COLORBTN WRONG 1 0,255,0 ...`; test failed |
| 4 | `ui-imgui-bind-input-text.js` | `inputText` writing back only when the widget returned true | `TIMEOUT typeText "x"` in ENTER (with `EnterReturnsTrue` the keystrokes never reach the string); test failed |
| 4 | `ui-imgui-bind-input-text.js` | the buffer compared with the truncated string instead of the original (`original.resize(n)` in `refString`) | `TRUNCATE WRONG ["abcdef","aé","x","fits",0]` and `SURROGATE WRONG "a\ud800bc"`; test failed |
| 4 | `ui-imgui-bind-input-text.js`, `ui-imgui-bind-errors.js` | `labelIdIsWindowId()` returning false always | both aborted in ImGui: `imgui.cpp:12319 ... Assertion id != window->ID ... failed` (Debug build); both failed |
| 4 | `ui-imgui-bind-input-text.js` | `colorButton`'s `needLabelId()` removed (one function's application of the check) | the same abort at `imgui.cpp:12319` in STRINGS; test failed |
| 4 | `ui-imgui-bind-errors.js` | `beginChild`'s `needLabelId()` removed | `NOTHROW ERR_IMGUI_USAGE` (BeginChild itself does not assert; the ItemAdd that would is in EndChild); test failed |
| 4 | `ui-imgui-bind-errors.js` | GUARD_LABELID's two refusals not made (test edited) | `UNREFUSED labelId:window-id`; test failed |
| 4 | `ui-imgui-bind-input-text.js` | the `InputTextFlags_Callback*` check removed | `NOTHROW ERR_OUT_OF_RANGE` (ImGui asserts only when the callback's event occurs, so nothing aborted in that frame); test failed |
| 4 | `ui-imgui-bind-input-text.js` | the capacity's lower bound made 0 | `AddressSanitizer: SEGV on unknown address 0x000000000000` (a zero-byte buffer); test failed |
| 4 | `ui-imgui-bind-input-text.js` | `inputTextMultiline`'s write-back removed | `TIMEOUT pressKey 257` in MULTILINE; test failed |
| 4 | `ui-imgui-bind-input-text.js` | `ImGui::SetKeyboardFocusHere()` removed, `enterImGui()` kept (Task 3's owed text-field half) | `TIMEOUT FOCUS`; test failed |
| 4 | `ui-imgui-bind-input-text.js` | `ImGui::IsItemDeactivatedAfterEdit()` replaced by `false` (Task 3's owed case) | `DEACTIVATE WRONG 0 0`; test failed |
| 5 | the three new tests, `ui-imgui-bind-errors.js`, `ui-imgui-bind-args.js`, `imgui-bind-surface.js` | (RED) run against the Task 4 build, with the tests and `bind-groups.js` written and no Task 5 code | surface: `MISSING beginMainMenuBar beginMenu ... treeNodeEx treePop` (26 names); trees-tabs, menus and popups: `TypeError: undefined is not a function` at the first new call (`treeNode`, `beginMainMenuBar`, `beginPopup`); args: `WRONGCODE undefined (want ERR_IMGUI_NO_FRAME): undefined is not a function` (NOFRAME, the first new sample); errors: after `GUARD_CHILD ok`, `WRONGCODE undefined (want ERR_IMGUI_USAGE): undefined is not a function` (GUARD_LABELID's `treeNode('')`) |
| 5 | `ui-imgui-bind-trees-tabs.js` | `ImGui::TreeNode()` replaced by `false`, `enterImGui()` kept | `TIMEOUT TREENODE open`; test failed |
| 5 | `ui-imgui-bind-trees-tabs.js` | `enterImGui()` removed from `setNextItemOpen` | `UNCOVERED TreesTabs setNextItemOpen`; test failed |
| 5 | `ui-imgui-bind-trees-tabs.js` | `collapsingHeader(label, pVisible)`'s write-back removed | `TIMEOUT HEADER hidden`; test failed |
| 5 | `ui-imgui-bind-trees-tabs.js` | `beginTabItem`'s write-back of `pOpen` removed | `TIMEOUT TABCLOSE closed`; test failed |
| 5 | `ui-imgui-bind-trees-tabs.js` | `beginTabItem`'s `TabItemFlags_Leading` with `_Trailing` check removed | ImGui aborted: `imgui_widgets.cpp:10852 ... TabItemEx ... Assertion ... failed`; test failed |
| 5 | `ui-imgui-bind-trees-tabs.js` | `treeNodeEx` passing 0 for its flags | `TREEEX WRONG false 0 false none`; test failed |
| 5 | `ui-imgui-bind-trees-tabs.js` | `ImGui::EndTabBar()` removed, `enterImGui()` kept | `Error: imgui.end(): Missing EndTabBar()`, uncaught; test failed |
| 5 | `ui-imgui-bind-menus.js` | `ImGui::BeginMenu()` replaced by `false`, `enterImGui()` kept | `TIMEOUT MAINBAR open`; test failed |
| 5 | `ui-imgui-bind-menus.js` | `enterImGui()` removed from `endMainMenuBar` | `UNCOVERED Menus endMainMenuBar`; test failed |
| 5 | `ui-imgui-bind-menus.js` | `menuItem(label, shortcut, p)`'s write-back removed | `TIMEOUT MENUREF checked`; test failed |
| 5 | `ui-imgui-bind-menus.js` | `ImGui::BeginMenuBar()` replaced by `false` | `MENUBAR WRONG false 0 false`, then `UNCOVERED Menus endMenuBar`; test failed |
| 5 | `ui-imgui-bind-args.js` | `menuItem`'s dispatch TypeError removed (any third argument the first form) | `NOTHROW ERR_INVALID_ARG_TYPE` (ARGTYPE, `menuItem('m', null, 1)`); test failed |
| 5 | `ui-imgui-bind-popups.js` | `ImGui::CloseCurrentPopup()` removed, `enterImGui()` kept | `TIMEOUT POPUP closed`; test failed |
| 5 | `ui-imgui-bind-popups.js` | `enterImGui()` removed from `endPopup` | `UNCOVERED Popups endPopup`; test failed |
| 5 | `ui-imgui-bind-popups.js` | `ImGui::SetItemTooltip()` removed | `TIMEOUT TOOLTIPS item`; test failed |
| 5 | `ui-imgui-bind-popups.js` | `ImGui::SetTooltip()` removed | `TIMEOUT TOOLTIPS set`; test failed |
| 5 | `ui-imgui-bind-popups.js` | `beginPopupModal`'s write-back of `pOpen` removed | `MODAL WRONG 0 1 false true`; test failed |
| 5 | `ui-imgui-bind-popups.js` | `beginPopupModal`'s empty-name check removed | `NOTHROW ERR_INVALID_ARG_VALUE` (ImGui's own assertion is in `Begin()`, reached only when the popup is open, so nothing aborted); test failed |
| 5 | `ui-imgui-bind-popups.js` | `beginPopupContextItem` passing NULL for every strId | ImGui aborted: `imgui.cpp:13617 ... BeginPopupContextItem ... Assertion id != 0 failed` (the `'tc'` call after `text`); test failed |
| 5 | `ui-imgui-bind-popups.js` | `beginPopupModal` calling `BeginPopup()` instead | `TIMEOUT MODAL open`; test failed |
| 5 | `ui-imgui-bind-popups.js` | `__test.setTooltipDelay()`'s two assignments removed (informational) | PASS: with the default 0.15 s + 0.15 s the tooltip still appeared within the 600-frame bound; see "Behaviours no test detects" |
| 5 | `ui-imgui-bind-errors.js` | `canTreePop()` returning nullptr always | ImGui aborted in GUARD_TREE's first call: `imgui_widgets.cpp:7426 ... TreePop ... Assertion window->IDStack.Size > 1 failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canEndTabItem()` returning nullptr always | aborted in GUARD_TABITEM: `imgui_widgets.cpp:10794 ... Assertion tab_bar->LastTabItemIdx >= 0 failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canEndMenuBar()` returning nullptr always | aborted in GUARD_MENUBAR: `imgui_widgets.cpp:9362 ... Assertion window->Flags & ImGuiWindowFlags_MenuBar failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canBeginMenuBar()` returning nullptr always | aborted in GUARD_BEGINMENUBAR: `imgui_widgets.cpp:9331 ... Assertion !window->DC.MenuBarAppending failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canEndTooltip()` returning nullptr always | aborted in GUARD_TOOLTIP: `imgui.cpp:13088 ... EndTooltip ... Assertion ... ImGuiWindowFlags_Tooltip failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canEndListBox()` returning nullptr always | aborted in GUARD_LISTBOX: `imgui_widgets.cpp:8997 ... EndListBox ... Assertion ... failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canBeginPopupContextItem()` returning nullptr always | aborted in GUARD_CONTEXTITEM: `imgui.cpp:13617 ... Assertion id != 0 failed`; test failed |
| 5 | `ui-imgui-bind-errors.js` | `canEndCombo()` returning nullptr always | no abort: ImGui's own check reported it, `CALLED endCombo: imgui.endCombo(): Calling EndCombo() in wrong window!` (GUARD_COMBO, the unmatched call); test failed |
| 5 | `ui-imgui-bind-errors.js` | the reset of `g.BeginComboDepth` removed from `guard::beginFrame()` | `COMBO_LEFT_OPEN WRONG ERR_IMGUI_USAGE true true ["##Combo_01"]`; test failed |
| 4 (review) | `ui-imgui-bind-widgets.js` | the 30-byte limit on a conversion specification removed (`formatProblem`, `bind_util.cpp`) | FORMATLEN: the 31-byte float format was accepted, and the drag that followed aborted ImGui: `imgui_widgets.cpp:3714: ImParseFormatSanitizeForPrinting ... Assertion (size_t)(fmt_end - fmt_in + 1) < fmt_out_size failed`; test failed |
| 4 (review) | `ui-imgui-bind-widgets.js` | `i` removed from the int conversions | `TypeError: imgui.sliderInt(): format must hold exactly one integer conversion (such as %d); got '%i'`, uncaught in FORMAT's frame; test failed |
| 4 (review) | `ui-imgui-bind-widgets.js` | `%%` counted as a conversion instead of a literal | `TypeError: imgui.sliderFloat(): ... got '%%%.1f'`, uncaught in FORMAT's frame; test failed |
| 4 (review) | `ui-imgui-bind-widgets.js` | `l` accepted as a length modifier | `NOTHROW ERR_INVALID_ARG_VALUE` (`'%ld'`); test failed |
| 5 (review) | `ui-imgui-bind-errors.js` | (before the fix) `beginMainMenuBar(); endGroup(); endMainMenuBar();`, and the same with the implicit window's group underneath, run by hand | aborted: `imgui.h:2377 ... ImVector<ImGuiGroupData>::back() ... Assertion Size > 0 failed`, and `imgui.cpp:12722 ... EndGroup ... Assertion group_data.WindowID == window->ID failed` |
| 5 (review) | `ui-imgui-bind-errors.js` | `canEndMainMenuBar()` returning nullptr always | aborted in GUARD_MAINMENUBAR: `imgui.h:2377 ... back() ... Assertion Size > 0 failed`; test failed |
| 5 (review) | `ui-imgui-bind-errors.js` | the wrong-window condition of the shared `groupOpenIn()` removed (every group-ending guard at once) | aborted in GUARD_GROUP, the first wrong-window case: `imgui.cpp:12722 ... Assertion group_data.WindowID == window->ID failed`; test failed |
| 5 (review) | `ui-imgui-bind-menus.js` | `ImGui::EndMenu()` removed, `enterImGui()` kept | the menu stays current, so the next `endMainMenuBar()` throws `imgui.endMainMenuBar(): Calling EndMainMenuBar() not from a menu-bar!` (ImGui's own report of the missing closer) in the first frame with the menu open, before CLOSERS prints; test failed |
| 5 (review) | `ui-imgui-bind-menus.js` | `ImGui::EndMainMenuBar()` removed, `enterImGui()` kept | the first frame's `render()` throws `imgui.render(): Missing End()` (the bar left open), before CLOSERS prints; test failed |
| 5 (review) | `ui-imgui-bind-popups.js` | `__test.setTooltipDelay()` returning the new values instead of the previous ones | `TOOLTIPS WRONG saved:[0,0]`; test failed |
| 6 | `ui-imgui-bind-tables.js`, `imgui-bind-surface.js` | (RED) run with the test and `bind-groups.js` written and no Task 6 code | surface: `MISSING beginTable endTable tableHeadersRow tableNextColumn tableNextRow tableSetColumnIndex tableSetupColumn tableSetupScrollFreeze`; tables: `TypeError: undefined is not a function` at the first `beginTable` |
| 6 | `ui-imgui-bind-errors.js` | (RED) the TABLE_* cases written, the binding built with only the surface table's guards (`canTableRowCall`, `canTableSetColumnIndex`, `canTableSetupScrollFreeze`) | every case through TABLE_WRONG_WINDOW printed `ok`; TABLE_CURSOR aborted: `imgui_tables.cpp:1493 ... EndTable ... Assertion table->RowPosY2 == inner_window->DC.CursorPos.y failed` |
| 6 | `ui-imgui-bind-errors.js` | (RED) with `canEndTable` added, before `canTableSetupColumn` | TABLE_NAMES: `NOTHROW ERR_IMGUI_USAGE` (the label past 32767 bytes accepted; the abort it leads to comes at the header row) |
| 6 | `ui-imgui-bind-errors.js` | (RED) with `canTableSetupColumn` added, before the table-region conditions | TABLE_REGION aborted: `imgui.h:2377 ... ImVector<ImVec4>::back() ... Assertion Size > 0 failed` (`endChild()` reached ImGui) |
| 6 | `ui-imgui-bind-tables.js` | `ImGui::TableHeadersRow()` removed, `enterImGui()` kept | `HEADERS WRONG header 83,20=55,182,64 ...` (no header row: the last item is the window's), then `TIMEOUT SCROLL body moved`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `enterImGui()` removed from `tableHeadersRow` | `UNCOVERED Tables tableHeadersRow`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `enterImGui()` removed from `tableSetupScrollFreeze` | `NOTCALLED tableSetupScrollFreeze` (REJECTED's accepted edge, before COVERAGE); test failed |
| 6 | `ui-imgui-bind-tables.js` | `tableNextColumn` returning false always (no write-back exists in this group: no Tables function takes a reference array, so the return value stands in for one) | `NEXTCOLUMN WRONG {"results":[false,false,false,false],...}`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `beginTable`'s column-count check removed (read with `int32`) | ImGui aborted in REJECTED: `imgui_tables.cpp:329 ... Assertion columns_count > 0 && columns_count < 512 failed`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `beginTable`'s negative-`innerWidth`-with-`ScrollX` check removed | aborted: `imgui_tables.cpp:331 ... Assertion inner_width >= 0.0f failed`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `tableSetupColumn`'s status-flag check removed | aborted: `imgui_tables.cpp:1742 ... Illegal to pass StatusMask values ... failed`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `tableSetupColumn`'s `WidthStretch` with `WidthFixed` check removed | aborted: `imgui_tables.cpp:802 ... ImIsPowerOfTwo(flags & ImGuiTableColumnFlags_WidthMask_) failed`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `tableSetupScrollFreeze`'s `cols` check removed, then (separately) its `rows` check | aborted: `imgui_tables.cpp:1859 ... columns >= 0 && columns < 512`, and `imgui_tables.cpp:1860 ... rows >= 0 && rows < 128`; test failed both times |
| 6 | `ui-imgui-bind-tables.js` | `tableSetupColumn` passing 0 for its width | `COLUMNS WRONG {"first":[33,46,59],"xs":[33,49,65]}`, `SETCOLUMN WRONG ...`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `ImGui::TableSetupScrollFreeze()` removed | `SCROLL WRONG {"header0":{"min":[28,28],...},"headerRect":{"min":[28,-37],...}}` (the header scrolled away); test failed |
| 6 | `ui-imgui-bind-tables.js` | `tableSetColumnIndex` passing 0 for every index | `COLUMNS WRONG {"first":[33,33,33],...}`, `NEXTCOLUMN WRONG`, `SETCOLUMN WRONG`; test failed |
| 6 | `ui-imgui-bind-tables.js` | `ImGui::EndTable()` removed, `enterImGui()` kept | `Error: imgui.end(): Missing EndTable()`, uncaught, in COLUMNS' first frame; test failed |
| 6 | `ui-imgui-bind-tables.js` | `ImGui::TableNextRow()` removed, `enterImGui()` kept | `Error: imgui.tableSetColumnIndex(): TableSetColumnIndex() before TableNextRow()`, uncaught; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canTableRowCall()` returning nullptr always | `AddressSanitizer: SEGV on unknown address 0x000000000235` in TABLE_OUTSIDE_GUARDED (`tableNextRow()` with no table, the NULL dereference at tables:2064); test failed |
| 6 | `ui-imgui-bind-errors.js` | only `canTableRowCall`'s wrong-window condition removed | aborted in TABLE_WRONG_WINDOW: `imgui_tables.cpp:2126 ... TableEndRow ... Assertion window == table->InnerWindow failed`; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canTableSetColumnIndex()` returning nullptr always | `NOTHROW ERR_IMGUI_USAGE` in TABLE_OUTSIDE_GUARDED (ImGui returns false with no table); test failed |
| 6 | `ui-imgui-bind-errors.js` | only `canTableSetColumnIndex`'s no-row condition removed | aborted in TABLE_NO_ROW: `imgui_draw.cpp:3483 ... SetCurrentChannel ... Assertion idx >= 0 && idx < _Count failed`; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canTableSetupScrollFreeze()` never refusing | aborted in TABLE_FREEZE_LATE: `imgui_tables.cpp:1858 ... Assertion table->IsLayoutLocked == false ... failed`; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canEndTable()` never refusing | aborted in TABLE_CURSOR: `imgui_tables.cpp:1493 ... failed`; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canTableSetupColumn()` never refusing | `NOTHROW ERR_IMGUI_USAGE` in TABLE_NAMES; test failed. The abort the guard prevents was measured by hand (below): `imgui.h:2369 ... ImVector<char>::operator[] ... Assertion i >= 0 && i < Size failed`, from `tableHeadersRow()` |
| 6 | `ui-imgui-bind-errors.js` | `canEnd()` never refusing | aborted in TABLE_REGION (`end()` after the refused `endChild()`): `imgui.h:2377 ... ImVector<ImVec4>::back() ... Size > 0 failed`; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canEndChild`'s table-region condition removed | the same abort, at TABLE_REGION's `endChild()`; test failed |
| 6 | `ui-imgui-bind-errors.js` | `canEndListBox`'s table-region condition removed | the same abort, at TABLE_REGION's `endListBox()` (through its `EndChild()`); test failed |
| 6 | `ui-imgui-bind-errors.js` | a guard condition `canTableDummy:never` registered and never refused | `UNREFUSED canTableDummy:never`; test failed |
| 7 | the four new tests, `imgui-bind-surface.js`, `ui-imgui-bind-args.js`, `ui-imgui-bind-errors.js` | (RED) run with the tests and `bind-groups.js` written and no Task 7 code | surface: `MISSING colorConvertFloat4ToU32 drawList.addBezierCubic ... styleColorsLight` (16 names); args: `TypeError: undefined is not a function` (EITHER's `styleColorsDark()`); style: `WRONGCODE undefined (want ERR_IMGUI_NO_WINDOW)`; drawlist: `TypeError: undefined is not a function` (NOWINDOW); plots, clipper: the same TypeError, uncaught in the first frame callback; errors: `WRONGCODE undefined (want ERR_IMGUI_USAGE)` at CLIPPER_WRONG_WINDOW |
| 7 | `ui-imgui-bind-style.js` | (RED, after the first GREEN run) no case called `colorConvertFloat4ToU32` | `UNCOVERED Style colorConvertFloat4ToU32`; GETCOLOR gained its three conversions |
| 7 | `ui-imgui-bind-drawlist.js` | `AddCircle()` removed, `enterImGui()` kept | `SHAPES WRONG circle-east@375,300=0,64,0 circle-west@264,300=0,64,0 circle-south@320,355=0,64,0 circle-north@320,244=0,64,0`; test failed |
| 7 | `ui-imgui-bind-drawlist.js` | `enterImGui()` removed from `addTriangleFilled` | `UNCOVERED DrawList drawList.addTriangleFilled`; test failed |
| 7 | `ui-imgui-bind-drawlist.js`, `ui-imgui-bind-style.js` | `getColorU32` returning 0 (the stand-in for a write-back: no Style, Plots or DrawList function writes a reference array) | drawlist: `COLOR WRONG 0 4278190335`; style: `THEMES WRONG {... "light":{"u32":0,"pixel":[240,240,240]}...}`, `COLOR WRONG`, `GETCOLOR WRONG`, `ALPHA WRONG 0 0`; both failed |
| 7 | `ui-imgui-bind-drawlist.js` | `addPolyline`'s 16383-point cap removed | `NOTHROW ERR_OUT_OF_RANGE` in LIMITS; test failed |
| 7 | `ui-imgui-bind-drawlist.js` | `addPolyline` passing one point fewer than given | `POLYLINE WRONG left@100,200=0,64,0 bottom@200,300=0,64,0 centre@200,200=255,128,0`; test failed |
| 7 | `ui-imgui-bind-style.js` | `ImGui::StyleColorsLight()` removed, `enterImGui()` kept | `THEMES WRONG` (light and dark both `[46,46,46]`); test failed |
| 7 | `ui-imgui-bind-style.js` | `enterImGui()` removed from `colorConvertFloat4ToU32` | `UNCOVERED Style colorConvertFloat4ToU32`; test failed |
| 7 | `ui-imgui-bind-style.js` | `getColorU32`'s `Col_COUNT` refusal removed | `NOTHROW ERR_OUT_OF_RANGE` in GETCOLOR (the read past `Style.Colors[]` stays inside `ImGuiStyle`, so ASAN does not see it); test failed |
| 7 | `ui-imgui-bind-style.js` | `pushStyleVar`'s float overload passing 1 instead of the value | `ALPHA WRONG 4278190335 4278190335`; test failed |
| 7 | `ui-imgui-bind-plots.js` | `ImGui::PlotHistogram()` removed, `enterImGui()` kept | `RECTS WRONG hist {"min":[20,20],"max":[620,20]} ...`, `HISTOGRAM WRONG bar1@96,70=0,255,0`, `EMPTY WRONG`; test failed |
| 7 | `ui-imgui-bind-plots.js` | `enterImGui()` removed from `plotLines` | `NOTCALLED plotLines` (REJECTED's accepted edge, before COVERAGE); test failed |
| 7 | `ui-imgui-bind-plots.js` | `valuesOffset`'s upper bound removed | `NOTHROW ERR_OUT_OF_RANGE` in REJECTED; test failed |
| 7 | `ui-imgui-bind-plots.js` | `valuesOffset` passed as 0 | `OFFSET WRONG differs`; test failed |
| 7 | `ui-imgui-bind-plots.js` | the graph size passed as `ImVec2(0, 0)` | `RECTS WRONG hist {"min":[28,28],"max":[452,49]} ...`, `HISTOGRAM WRONG`, `EMPTY WRONG`; test failed |
| 7 | `ui-imgui-bind-args.js` | `needFrame()` removed from `drawList.addText` | NOFRAME: `AddressSanitizer: SEGV ... in ImGui::GetCurrentWindow()` (outside a frame `g.CurrentWindow` is NULL, and `GetWindowDrawList()` dereferences it); test failed |
| 7 | `ui-imgui-bind-clipper.js` | the scope comparison dropping the table | `SCOPE_ENDTABLE-ret WRONG {"error":null,...}`, `SCOPE_OPENTABLE-ret WRONG {"error":null,...}`: the loop went on and nothing aborted in the Debug build (the throwing variants still propagate their own error); test failed |
| 7 | `ui-imgui-bind-clipper.js` | `ItemsCount` not set to -1 on abandon | first run: PASS (nothing observed the seek); the SCOPE_* cases then gained the cursor check, and with it: `SCOPE_ENDCHILD-ret WRONG {... "inside":[28,251],"after":[28,1747] ...}`, likewise for SCOPE_OPENCHILD and SCOPE_ENDTABLE, both variants (End() seeked the now-current window's cursor to the clipper's end); test failed |
| 7 | `ui-imgui-bind-clipper.js` | the callback depth not raised (`++gCallbackDepth` removed from `CallbackScope`) | FRAMEFNS: `render()` from the callback ran and ended the frame, so the scope changed and `listClipper` threw its abandon `ERR_IMGUI_USAGE`, uncaught; test failed |
| 7 | `ui-imgui-bind-clipper.js` | the callback not called (the ImGui loop kept) | `RANGES WRONG {"r":[],"texts":0,"lo":15,"hi":20}`, `TIMEOUT END last range at 10000`; test failed |
| 7 | `ui-imgui-bind-clipper.js` | the callback given `(start, start)` (its arguments stand in for a write-back) | `RANGES WRONG {"r":[[0,0]],...}`; test failed |
| 7 | `ui-imgui-bind-clipper.js` | `enterImGui()` removed from `listClipper` | `NOTCALLED listClipper` (REJECTED's count-0 call); test failed |
| 7 | `ui-imgui-bind-clipper.js` | `itemHeight`'s check removed | `WRONGCODE ERR_IMGUI_USAGE (want ERR_OUT_OF_RANGE): ... ImGuiListClipper: Failed to calculate item height! ...` (0 reaches ImGui's measuring mode); test failed |
| 7 | `ui-imgui-bind-clipper.js` | the pending-exception check removed (Step() called again after a throw) | PASS: not detected (see "Behaviours no test detects") |
| 7 (review) | `ui-imgui-bind-plots.js` | the detached-Float32Array check removed from `Call::floats` | ASAN `SEGV on unknown address 0x000000000000 ... bind_util.cpp:719 in Call::floats` (the finite-check loop reading the null data); test failed |
| 7 (review) | `ui-imgui-bind-plots.js` | the Float32Array length cap removed from `Call::floats` | `NOTHROW ERR_OUT_OF_RANGE` (4097 values); test failed |
| 7 (review) | `ui-imgui-bind-drawlist.js` | the Array length cap removed from `Call::floats` | `NOTHROW ERR_OUT_OF_RANGE` at the first over-long Array (16384 points read and drawn) |
| 7 (review) | `ui-imgui-bind-drawlist.js` | the Array length cap moved after the copy and the element reads (the shape before the fix) | `new Array(2 ** 32 - 1)`: a 16 GiB vector value-initialised, then `WRONGCODE ERR_INVALID_ARG_TYPE (want ERR_OUT_OF_RANGE): ... points[0] must be a number` (the hole read before the cap); the run finished inside its time limit; test failed |
| 7 (review) | `ui-imgui-bind-plots.js` | `graphSize[0]`'s 2^31 check removed | the process hung in the REJECTED frame (the `plotLines` with a 3e9-wide frame) until `with-display.py`'s limit: exit 124; test failed |
| 7 (review) | by hand, `hermes-node-ui` on a scratch script | the plot cap raised to 100000, then `plotHistogram` of 4096 and of 5921 zeros in a frame of that width | 4096: survived; 5921: `imgui_widgets.cpp:9150 ... Assertion v1_idx >= 0 && v1_idx < values_count failed`, as the float simulation predicted |
| 7 (review) | `ui-imgui-bind-clipper.js` | `BeginCount` dropped from the scope comparison | `SCOPE_REBEGINWINDOW-ret WRONG {"error":null,...,"after":[28,1747]...}` and `-throw WRONG` (the clipper seeked the reopened window); test failed |
| 7 (review) | `ui-imgui-bind-clipper.js` | `InstanceCurrent` dropped from the scope comparison | `SCOPE_REBEGINTABLE-ret WRONG {"error":null,...}`, `-throw WRONG`; test failed |
| 7 (review) | `ui-imgui-bind-clipper.js` | the itemHeight floor back to `> 0` | `NOTHROW ERR_OUT_OF_RANGE` (0.5); test failed |
| 7 (review) | `ui-imgui-bind-clipper.js` | the 2^30 list-height check removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 7 (review) | `ui-imgui-bind-clipper.js` | the abandon trace written twice | the ABANDON run counts 24 lines, wants 12; test failed |
| 7 (review) | `ui-imgui-bind-clipper.js` | the pending-exception check removed, with FROZEN's second range present | PASS: still not detected (Hermes refuses the call, below) |
| 7 | `ui-imgui-bind-errors.js` | `canListClipper()` never refusing | aborted in CLIPPER_WRONG_WINDOW: `imgui_tables.cpp:2126: ... TableEndRow ... Assertion window == table->InnerWindow failed`; test failed |
| 6 (review) | `ui-imgui-bind-errors.js` | (before the fix) `beginTable('s', 1, ScrollY, [0, 100]); tableNextRow(); tableNextColumn(); beginTable('n', 1);` then `endChild()`, `end()` or `endListBox()` (the last with a group open outside), run by hand | each aborted: `imgui.h:2377 ... ImVector<ImVec4>::back() ... Assertion Size > 0 failed` |
| 6 (review) | `ui-imgui-bind-errors.js` | `inTableRegion()` reverted to the top table only (the walk stops after the first plain table) | aborted in TABLE_REGION's nested case: `imgui.h:2377 ... Size > 0 failed`; test failed |
| 6 (review) | `ui-imgui-bind-errors.js` | `canTableSetupColumn`'s all-disabled refusal removed | `NOTHROW ERR_IMGUI_USAGE` in TABLE_DISABLED; test failed. The abort it prevents was measured by hand in Task 6: `imgui_tables.cpp:1005 ... Assertion table->LeftMostEnabledColumn >= 0 && ... failed` |
| 6 (review) | `ui-imgui-bind-errors.js` | the all-disabled check reading only `Columns[i].Flags` (the queued reconcile requests ignored) | `NOTHROW ERR_IMGUI_USAGE` at TABLE_DISABLED's second frame, whose first column is queued for reconciliation; test failed |
| 8 (widths) | `ui-imgui-bind-layout.js`, `ui-imgui-bind-windows.js`, `ui-imgui-bind-input-text.js` | (RED) run against Task 7's build, with ITEMWIDTH, SIZES and MLSIZE written and no bound | each `NOTHROW ERR_OUT_OF_RANGE` at its first case (`setNextItemWidth(2147483648)`, `setNextWindowSize([2147483648, 100])`, `inputTextMultiline(..., [2147483648, 100])`); all three failed |
| 8 (widths) | `ui-imgui-bind-layout.js` | `setNextItemWidth`'s bound removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 8 (widths) | `ui-imgui-bind-layout.js` | `pushItemWidth`'s bound removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 8 (widths) | `ui-imgui-bind-layout.js` | `Call::belowIntLimit` bounding below 2147483520 instead of 2^31 (the largest accepted value refused) | `RangeError: imgui.setNextItemWidth(): itemWidth must be less than 2147483648; got 2.14748e+09`, uncaught, after `WIDTH ok`; test failed |
| 8 (widths) | `ui-imgui-bind-windows.js` | `setNextWindowSize`'s `size[1]` bound removed | `NOTHROW ERR_OUT_OF_RANGE` at `[100, 2147483648]`; test failed |
| 8 (widths) | `ui-imgui-bind-windows.js` | `beginChild`'s two size bounds removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 8 (widths) | `ui-imgui-bind-input-text.js` | `inputTextMultiline`'s `size[1]` bound removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 8 | `ui-imgui-bind-docking.js`, `ui-imgui-bind-errors.js`, `ui-imgui-bind-args.js`, `imgui-bind-surface.js` | (RED) run against the width commit's build, with the tests and `bind-groups.js` written and no docking code | docking: `FAILED TypeError: undefined is not a function` at the first `saveIniSettingsToMemory()`; errors: `WRONGCODE undefined (want ERR_IMGUI_USAGE): undefined is not a function` after `CLIPPER_WRONG_WINDOW ok`; args: `FAILED TypeError: undefined is not a function` in EITHER; surface: `MISSING dockSpaceOverViewport loadIniSettingsFromMemory saveIniSettingsToMemory` |
| 8 | `ui-imgui-bind-docking.js` | `ImGui::DockSpaceOverViewport()` removed, `enterImGui()` kept (an ImGui call removed) | `FIRST WRONG 0 ...` with both windows `docked: false`, then `TIMEOUT FIRST tiled`; `COVERAGE` not reached; test failed |
| 8 | `ui-imgui-bind-docking.js` | `ImGui::LoadIniSettingsFromMemory()` removed, `enterImGui()` kept | `FIRST WRONG 146626941 ...` with both windows floating at 60,60, 32x35, then `TIMEOUT FIRST tiled`; test failed |
| 8 | `ui-imgui-bind-docking.js` | `enterImGui()` removed from `saveIniSettingsToMemory` (coverage) | every case ok, then `UNCOVERED Docking saveIniSettingsToMemory`; test failed |
| 8 | `ui-imgui-bind-docking.js` | `needNoFrame()` removed from `loadIniSettingsFromMemory` | `NOTHROW ERR_IMGUI_USAGE` in INFRAME; test failed |
| 8 | `ui-imgui-bind-docking.js` | the returned ID replaced by the argument (`resultU32(id)`; the result, this group's write-back) | `FIRST WRONG 0 ...`, `STABLE WRONG 0,0,0 146626941`; test failed |
| 8 | `ui-imgui-bind-docking.js` | the flags check of `dockSpaceOverViewport` replaced by a plain `int32` (an argument check removed) | REJECTED's `dockSpaceOverViewport(0, 1 << 10)` reached ImGui: `imgui.cpp:20919 ... Assertion (flags & ImGuiDockNodeFlags_DockSpace) == 0 failed`; test failed |
| 8 | `ui-imgui-bind-docking.js` (NOWINDOW) | `needContext()` removed from `saveIniSettingsToMemory` | ASAN `SEGV ... imgui.cpp:16707:26 in ImGui::SaveIniSettingsToMemory` (no context); test failed |
| 8 | `ui-imgui-bind-errors.js` | `canDockSpaceOverViewport`'s twice condition off | aborted in DOCKSPACE_TWICE: `imgui.cpp:20939 ... Assertion node->IsDockSpace() == false && "Cannot call DockSpace() twice a frame with the same ID" failed`; test failed |
| 8 | `ui-imgui-bind-errors.js` | `canDockSpaceOverViewport`'s not-root condition off | aborted in DOCKSPACE_NOT_ROOT: `imgui.cpp:20985 ... Assertion node->IsRootNode() failed`; test failed |
| 8 | `ui-imgui-bind-errors.js` | the guard's default ID computed from `"Dockspace"` instead of `"DockSpace"` | aborted in DOCKSPACE_TWICE's second frame (the explicit ID, then 0): `imgui.cpp:20939 ... twice a frame ...`; test failed |
| 8 | `ui-imgui-bind-errors.js`, `ui-imgui-bind-docking.js` | `needNoFrame()`'s trace line removed | errors: the ONCE chain broke at the `must be called outside a frame` line; docking: the TRACE run's positive line missing; both failed |
| 8 | by hand, `hermes-node-ui` on a scratch script | three malformed settings strings, loaded between frames: a self-parented node; a window `DockId` naming no node; a chain of 300 nodes, then saved | the first never returned (the run ended at its 60 s limit, status 124); the second aborted, `imgui.cpp:18618 ... DockContextBuildAddWindowsToNodes ... Assertion node != __null failed`; the third loaded, and the save aborted, `imgui.cpp:21927 ... Assertion depth < (1 << (sizeof(node_settings.Depth) << 3)) failed`. Loaded before the first frame, the first and third were pruned and did nothing ("Not covered" 3g) |
| 9 (harness) | `ui-imgui-adopt.js` | (RED) run against the committed harness, whose `adopt()` called `resync()` first | nothing printed; the run ended at its 120 s limit (`EXITCODE 124`): the capture request `resync()` makes is counted per presented frame and nothing had been presented. This is also the test's mutation (`resync(h)` back in `adopt()`) |
| 9 | `imgui-app-viewer.js`, `ui-imgui-app.js` | (RED) run before `examples/imgui-app/` existed | viewer: `Cannot find module` for `panels/viewer.js`, FileCheck `'-' is empty`; app: `EXITCODE 1` (the same, from `app.js`) |
| 9 | `imgui-app-viewer.js` | the viewer reading with `fs.readFile()` and slicing | `CAP WRONG readFile used: fs.open,handle.stat,fs.readFile,handle.close`; test failed |
| 9 | `imgui-app-viewer.js` | the read loop not stopping at the cap (`while (true)`, 64 KiB requests) | `CAP WRONG a read past the cap: {"length":65536,"position":1048576,"returned":1}`; test failed |
| 9 | `imgui-app-viewer.js` | `close()` moved out of `finally`, after the reads | `CLOSE_ON_ERROR WRONG {"status":"error","code":"EIO","closes":0,...}`; test failed |
| 9 | `imgui-app-viewer.js` | the request counter's check removed | `SUPERSEDE WRONG older-last {"path":"first","status":"text","lines":["first content"]}`; test failed |
| 9 | `imgui-app-viewer.js` | the read position advanced by the requested length instead of the returned one | `SHORT_READ WRONG position 65536 after 1000 bytes returned`; test failed |
| 9 | `imgui-app-viewer.js` | text decided by searching the decoded text for U+FFFD instead of `isUtf8()` | `CLASSIFY WRONG fffd.txt={"status":"hex","rows":4,"truncated":false}`; test failed |
| 9 | `imgui-app-viewer.js` | the trailing incomplete-sequence trim removed | `CLASSIFY WRONG straddle={"status":"hex","rows":65536,"truncated":true}`; test failed |
| 9 | `ui-imgui-app.js` | the Reset Layout load moved after `newFrame()` | after `RESET floating`: `Error: imgui.loadIniSettingsFromMemory(): must be called outside a frame (before newFrame())`, uncaught, `EXITCODE 1`; test failed |
| 9 | `ui-imgui-app.js` | the orbiting circle drawn at 0.8 of the orbit radius (`orbitPosition()` unchanged) | `CANVAS WRONG orbit(1)=21,162,162 want 0,255,255` (the trail); test failed |
| 9 | `ui-imgui-app.js` | `CallbackScope`'s destructor not lowering the callback depth (`--gCallbackDepth` removed) | fails before THROW is reached: the main run's first frame, once Log's clipper has run, throws `Error: imgui.render(): not allowed inside a list clipper's callback`; `EXITCODE 1` after `SCALE 1`; test failed |
| 9 | `ui-imgui-app.js` (`throw`) | the callback scope left raised only when the callback threw (a heap `CallbackScope`, deleted only with no exception pending): the brief's mutation confined to the throw path | after `THROW caught ...`: `Error: imgui.newFrame(): not allowed inside a list clipper's callback`, `EXITCODE 1`, no `THROW ok`; test failed. Before the `process.emit` fix below, the same mutation hung instead (status 124, nothing printed): the second uncaught exception was swallowed |
| 9 (emit) | `test-process-emit-return.js` | (RED) run before the fix | `EMIT true true true true false` (node v24.13.1: `true false true false false`); the second timer's throw swallowed and `STILL ALIVE` printed, exit 0; test failed |
| 8 (review) | `ui-imgui-bind-windows.js`, `ui-imgui-bind-widgets.js` | (RED) POSITION and LBSIZE written, no bound yet | windows: `NOTHROW ERR_OUT_OF_RANGE` at `setNextWindowPos([2147483648, 0])`; widgets: `NOTHROW ERR_OUT_OF_RANGE` at `beginListBox('##big', [2147483648, 100])`; both failed |
| 8 (review) | `ui-imgui-bind-windows.js` | `setNextWindowPos`'s position bound removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 8 (review) | `ui-imgui-bind-windows.js` | `Call::inIntRange` one-sided (`v < 2^31` only) | `NOTHROW ERR_OUT_OF_RANGE` at the first negative case; test failed |
| 8 (review) | `ui-imgui-bind-windows.js` | `Call::inIntRange`'s lower end exclusive (`v > -2^31`) | `RangeError: imgui.setNextWindowPos(): pos[0] must be at least -2147483648 and less than 2147483648; got -2.14748e+09`, uncaught (the accepted -2^31 refused); test failed |
| 8 (review) | `ui-imgui-bind-widgets.js` | `beginListBox`'s size bound removed | `NOTHROW ERR_OUT_OF_RANGE`; test failed |
| 8 (review) | `ui-imgui-bind-docking.js` | NOWINDOW's script ending in `process.exit(1)` after its output | the new CODE check of the NOWINDOW run: `CODE: expected string not found in input`; test failed (before the fix that run checked no status) |
| 9 (review) | `ui-imgui-app.js` | the Files list clipper removed (every row drawn) | `BIG WRONG calls per frame: big 40242, small 636`; test failed |
| 9 (review) | `ui-imgui-app.js` | `app.rects` not emptied at the start of each frame | `COLLAPSE WRONG still recorded: .../dir1/deep .../dir1/b.js`; test failed |
| 9 (review) | `ui-imgui-app.js` | `resetLayout()` not reopening the panels | `RESET WRONG Canvas={"name":"Canvas","active":false,"docked":false}`; test failed |
| 9 (review) | `ui-imgui-adopt.js` (`late`) | `adopt()`'s `frameCount` check removed | the late run printed `ADOPT ok`, `RUN ok`, `CAPTURE ok`, then `RESYNC WRONG 3 want 2` (the count it started from was already wrong); LATE's check failed |
| 9 (review) | `imgui-app-viewer.js` | truncation decided by `stat.size > CAP`, the probe removed | `CAP WRONG 0 truncation probes, want 1`; test failed |
| 9 (review) | `imgui-app-viewer.js` | the probe kept but `&& stat.size > CAP` added | `GROWING WRONG {"status":"text","truncated":false}`; test failed |
| 9 (review) | `imgui-app-viewer.js` | the lead-byte test widened back to `b >= 0xc0` | `CLASSIFY WRONG invalid-lead={"status":"text","lines":23549,"truncated":true}`; test failed |
| 9 (review) | `test-process-emit-return.js` | the emit fix reverted (`if (!list) return false;`) | `exception`: `EMIT true true true true false`, `STILL ALIVE`, `EXITCODE 0`; `rejection` (run by hand): only `LISTENER first`, no report of the second rejection; test failed |
| 10 | `ui-imgui-bind-widgets.js` (FORMATLEN, the int candidates alone) | the 30-byte limit removed (`formatProblem`'s `TooLong` return disabled), with FORMATLEN's two float candidates temporarily removed from the test so that the float abort (the Task 4 review row) cannot come first; both restored, the addon rebuilt, the test passing again | the 31-byte `%...1d` format was accepted by `dragInt`, and its Ctrl+click text entry aborted ImGui: `imgui_widgets.cpp:3730: ImParseFormatSanitizeForScanning ... Assertion (size_t)(fmt_end - fmt_in + 1) < fmt_out_size failed`, exit 134; test failed. The int-specific half of the bound is therefore shown, separately from the float half |
| 10 (review) | `ui-imgui-app.js` (NAMES) | the Files panel's label sanitising removed (`treeRow` passing the name to `treeNodeEx` as it is) | `NAMES WRONG row height {"name":"line\nbreak.txt","min":[34,199],"max":[372,225]} want 13`; test failed |
| 10 (review) | `ui-imgui-app.js` (NAMES) | only the control-character replacement removed | the same `NAMES WRONG row height` line; test failed |
| 10 (review) | `ui-imgui-app.js` (NAMES) | only the "##" path removed (`asText` always false) | `NAMES WRONG no whole-name text drawn for "hash##tag.txt"`; test failed |
| final review | `ui-imgui-bind-recovery.js` | (RED) the new test run before the fix (hook only) | CALL aborted: `imgui.cpp:8992: void ImGui::End(): Assertion ... "Calling End() too many times!" failed`, `EXITCODE 134`; stdout empty (ImGui's `[imgui-error]` lines are lost to the buffer, as Task 1's note says) |
| final review | `ui-imgui-bind-recovery.js` | the re-assert removed from `Call::enterImGui()` | the same abort in CALL, `EXITCODE 134`; test failed |
| final review | `ui-imgui-bind-recovery.js` | the re-assert removed from `newFrame()`'s discard path | DISCARD aborted: `imgui.cpp:12004: ErrorRecoveryTryToRecoverState ... Assertion (0) && "Missing End()" failed`, `EXITCODE 134` after `CALL ok`, `RENDER ok`; test failed |
| final review | `ui-imgui-bind-recovery.js` | the re-assert removed from `CallbackScope`'s destructor | `CLIPPER WRONG inside=false,false,false after=true,true,true drew=true`, `EXITCODE 1`; test failed |
| final review | `ui-imgui-bind-errors.js` | (RED) CLIPPER_WRONG_NOROW written, the guard unchanged | `NOTHROW ERR_IMGUI_USAGE` after `CLIPPER_WRONG_WINDOW ok`, `EXITCODE 1`; test failed. By hand on the same build, the finding's sequence: `endTable()` threw `EndTable() with the cursor moved before the table's first row`, and the frame-end recovery then aborted at `imgui_tables.cpp:1493` (`table->RowPosY2 == inner_window->DC.CursorPos.y`), exit 134 |
| final review | `ui-imgui-bind-errors.js` | the `IsInsideRow` term put back into `canListClipper()` | `NOTHROW ERR_IMGUI_USAGE` in CLIPPER_WRONG_NOROW; test failed |
| final review | `ui-imgui-bind-errors.js` | `canListClipper()` returning nullptr always | CLIPPER_WRONG_WINDOW aborted: `imgui_tables.cpp:2126: TableEndRow ... Assertion window == table->InnerWindow failed`, `EXITCODE 134`; test failed |
| final review | `ui-imgui-bind-tables.js` | (RED) REJECTED's outerSize / innerWidth cases written, no bound yet | `NOTHROW ERR_OUT_OF_RANGE` after `SCROLL ok` (`beginTable('r', 2, ScrollX, [2147483648, 100], 0)` accepted), `EXITCODE 1`; test failed |
| final review | `ui-imgui-bind-tables.js` | `beginTable`'s `innerWidth` bound alone removed (the two `outerSize` bounds kept) | `NOTHROW ERR_OUT_OF_RANGE` at the innerWidth case, `EXITCODE 1`; test failed |
| final review | `ui-imgui-bind-widgets.js` (FORMAT) | (RED) the width / precision cases written, the validator unchanged | `NOTHROW ERR_INVALID_ARG_VALUE` after `TEXTFIELDS ok` (`'%100f'` accepted); test failed |
| final review | `ui-imgui-bind-widgets.js` (FORMAT) | `formatProblem()` never returning `FieldTooWide` | `NOTHROW ERR_INVALID_ARG_VALUE`, `EXITCODE 1`; test failed |
| final review | `ui-imgui-bind-widgets.js` (FORMAT) | only the precision's digit check removed (the width's kept) | `NOTHROW ERR_INVALID_ARG_VALUE`, `EXITCODE 1`; test failed. The line does not name the format; the first case, `'%100f'`, is a width and still refused, so the failure is at a precision case (`'%.100f'` is next) |
| final review | `ui-imgui-bind-widgets.js` (FORMATLEN) | the 30-byte limit removed, with FORMATLEN's float candidates now long by `0` flags (`'%' + 25 zeros + '1.1f'`) rather than by a 27-digit precision, which the new digit cap refuses first | `FORMAT ok`, then the 31-byte float format accepted and dragged: `imgui_widgets.cpp:3714: ImParseFormatSanitizeForPrinting ... Assertion ... < fmt_out_size failed`, `EXITCODE 134`; test failed. So FORMATLEN still shows the float half of the 30-byte limit |
| final review | `ui-imgui-bind-layout.js` (REJECTED) | (RED, and the mutation: the bound back at `INT32_MAX`, which is the code before the fix) `setKeyboardFocusHere(2147483647)` | `NOTHROW ERR_OUT_OF_RANGE`, test failed |
| final review | `ui-imgui-bind-args.js`, `ui-imgui-bind-layout.js` | call counting gated the wrong way round (`if (!testHooksEnabled())`) | args: `UNCOVERED Frame newFrame render showDemoWindow ...`; layout: `REJECTED edges not accepted: 0`; both `EXITCODE 1`, both failed |
| final review | `imgui-app-viewer.js` | (RED) NOT_REGULAR written, the viewer unchanged | `NOT_REGULAR WRONG fifo={"status":"text","rec":["fs.open","handle.read","handle.close"],...}` and the same for char, socket, block and dir: an open() issued for each; test failed |
| final review | `imgui-app-viewer.js` | the regular-file check removed (the stat kept, its answer ignored) | `NOT_REGULAR WRONG fifo={"status":"text","rec":["fs.stat","fs.open","handle.read","handle.close"],...}`; test failed. (Removing the stat call as well fails SUPERSEDE first, whose hand-settled step is now the stat.) |
| final review | `ui-imgui-app.js` (SPECIAL) | (RED) run against the unfixed `files.js` and `viewer.js` | `SPECIAL WRONG the FIFO listed as {"kind":"file"}`; test failed |
| final review | `ui-imgui-app.js` (SPECIAL) | the unfixed `viewer.js` alone (the tree's labelling kept) | `TIMEOUT idle SPECIAL link` after the link to the FIFO was clicked: the open() blocked a threadpool thread and app.idle() never resolved; the run ended in 34 s, bounded by the test's own wait, not a hang; test failed |
| codex | `ui-imgui-bind-style.js` (VARS) | (RED) VARS written, `pushStyleVar` unchanged | `NOTHROW ERR_OUT_OF_RANGE` after `MISMATCH message ...`, `EXITCODE 1`: Codex's `ScrollbarSize -1` accepted. By hand on the same build, Codex's sequence: `imgui_widgets.cpp:989: GetWindowScrollbarRect ... Assertion scrollbar_size >= 0.0f failed`, exit 134 |
| codex | `ui-imgui-bind-widgets.js` (COLORDOMAIN) | (RED) COLORDOMAIN written, the editors unchanged | `NOTHROW ERR_OUT_OF_RANGE` after `REJECTED ok` (Codex's `[1e10, 0, 0]` accepted), `EXITCODE 1` |
| codex | `ui-imgui-bind-tables.js` (REJECTED) | (RED) the width cases written, `tableSetupColumn` unchanged | `NOTHROW ERR_OUT_OF_RANGE` after `SCROLL ok`, `EXITCODE 1` |
| codex | `ui-imgui-bind-drawlist.js` (LENGTHS) | (RED) LENGTHS written, the draw list unchanged | `NOTHROW ERR_OUT_OF_RANGE` after `LIMITS message ...` (Codex's `addCircleFilled` radius `1e10` accepted), `EXITCODE 1` |
| codex | `ui-imgui-bind-plots.js` (HISTSCALE) | (RED) HISTSCALE written, `plotHistogram` unchanged | `NOTHROW ERR_OUT_OF_RANGE` after `REJECTED message ...`, `EXITCODE 1` |
| codex | `ui-imgui-bind-style.js` (VARS) | `varValueOk()` returning true | `NOTHROW ERR_OUT_OF_RANGE` at Codex's `ScrollbarSize -1`, `EXITCODE 1`; test failed |
| codex | `ui-imgui-bind-style.js` (VARS) | `Call::length()`'s not-negative half removed (`lo` always `-limit`) | `NOTHROW ERR_OUT_OF_RANGE` (at `ScrollbarSize -1`, the first refusal it reopens); test failed |
| codex | `ui-imgui-bind-style.js` (VARS) | the Unit domain checked as a length | `NOTHROW ERR_OUT_OF_RANGE` (at `Alpha -0.5`); test failed |
| codex | `ui-imgui-bind-style.js` (VARS) | the MinSize domain checked as a length | `NOTHROW ERR_OUT_OF_RANGE` (at `WindowMinSize [0.5, 10]`); test failed |
| codex | `ui-imgui-bind-widgets.js` (COLORDOMAIN) | `colorComponentsOk()` returning true | `NOTHROW ERR_OUT_OF_RANGE` at Codex's `[1e10, 0, 0]`; test failed |
| codex | `ui-imgui-bind-widgets.js` (COLORDOMAIN) | the HSV domain removed (`hsv` always false) | `NOTHROW ERR_OUT_OF_RANGE` (at `[2048.5, 0, 0]` with `InputHSV`); test failed |
| codex | `ui-imgui-bind-tables.js` (REJECTED) | `tableSetupColumn`'s `initWidthOrWeight` bound removed | `NOTHROW ERR_OUT_OF_RANGE` after `SCROLL ok`; test failed |
| codex | `ui-imgui-bind-drawlist.js` (LENGTHS) | `radiusOk()` returning true | `NOTHROW ERR_OUT_OF_RANGE` at Codex's `addCircleFilled([100, 100], 1e10, ...)`; test failed |
| codex | `ui-imgui-bind-drawlist.js` (LENGTHS) | `addLine`'s thickness check removed | `NOTHROW ERR_OUT_OF_RANGE` (at Codex's `addLine` thickness `1e10` with `StrokeCenterBiased`); test failed |
| codex | `ui-imgui-bind-drawlist.js` (LENGTHS) | `cornerOk()` returning true | `NOTHROW ERR_OUT_OF_RANGE` (at the first corner of `2^30 + 128`); test failed |
| codex | `ui-imgui-bind-drawlist.js` (LENGTHS) | `addText`'s position check removed | `NOTHROW ERR_OUT_OF_RANGE` (at `[2^31, 0]`); test failed |
| codex | `ui-imgui-bind-plots.js` (HISTSCALE) | `histogramOk()` returning true | `NOTHROW ERR_OUT_OF_RANGE` at `[0, FLT_MAX]`; test failed |
| codex | `ui-imgui-bind-plots.js` (HISTSCALE) | only the reciprocal check removed | `NOTHROW ERR_OUT_OF_RANGE` (at `[0, 1e-45]`); test failed |
| codex | `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | (RED) DRAGDOMAIN written, `dragInt` unchanged | `NOTHROW ERR_OUT_OF_RANGE` after `COLORDOMAIN message ...` (Codex's `vSpeed` `1e10` accepted), `EXITCODE 1`. By hand before the fix, Codex's drag (`dragInt('n', [0], 1e10)`, dragged 100 units right): `v 0`, where `(int)` of the accumulator gave INT_MIN twice (cvttss2si's value on x86-64) and their sum wrapped |
| codex | `ui-imgui-input.js` (CLAMP) | (RED) CLAMP written, the platform half unchanged | `CLAMP mouse=1000000000,-1000000000 wheel=1000000000`; FileCheck failed |
| codex | `ui-imgui-input.js` (CLAMP), `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | the clamp removed (`clampedInput()` returning its argument) | input: `CLAMP mouse=1000000000,-1000000000 wheel=1000000000`, FileCheck failed; widgets: `TIMEOUT DRAGDOMAIN far move` (the position never reads back as 65536); both failed |
| codex | `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | `dragIntDomainOk()` returning true | `NOTHROW ERR_OUT_OF_RANGE` at Codex's `vSpeed` `1e10`; test failed |
| codex | `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | the automatic-speed check removed | `NOTHROW ERR_OUT_OF_RANGE` (at bounds 0 .. 80001 with `vSpeed` 0, the first refusal it reopens); test failed |
| codex | `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | the `v[0]` check removed | `NOTHROW ERR_OUT_OF_RANGE` (at Codex's `v` `[2147483647]`); test failed |
| codex | `ui-imgui-bind-clipper.js` (POOLGROW) | (RED, and the mutation) the table compared by address again, as before the fix (the address kept in the snapshot's index field and compared with the current one) | `POOLGROW WRONG {"error":"imgui.listClipper(): the callback changed the window scope (...); the clipper was abandoned","before":8,"after":18,"nested":9,...}`; test failed. The fixed build: `POOLGROW ok`, the capacity grown from 8 to 18 |
| codex | `ui-imgui-app.js` (LOGROWS) | (RED, and the mutation) LOGROWS written, the Log's `add()` storing the text as it is (the code before the fix; the rectangles recorded) | `LOGROWS WRONG bottom row 9 height 26 want 13`: an entry with a line break is two lines tall; test failed |
| codex | `imgui-app-viewer.js` (CLASSIFY) | (RED) the new cases written, `completeLength()` unchanged | `CLASSIFY WRONG E0-80={"status":"text",...,"truncated":true} ED-A0=... F4-90=... F0-80=... F0-80-80=...`: each invalid suffix trimmed and the file classified as text; the four new valid splits passed; test failed |
| codex | `imgui-app-viewer.js` (CLASSIFY) | `secondByteOk()` giving every lead the default range 0x80..0xBF (the same outcome as removing the check) | the same five `CLASSIFY WRONG ... "status":"text"` entries; test failed |
| codex (write-back) | `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | both write-back clamps removed (dragInt's and the colour editors') | `DRAGDOMAIN WRONG top {"first":1126141823,...,"refused":"imgui.dragInt(): v[0] must be from -1073741823 to 1073741823; got 1126141823"...} hdr {"first":-256.643...,"refused":"imgui.colorEdit3(): col[0] must be from 0 to 8388608; got -256.643127"...}`; test failed |
| codex (write-back) | `ui-imgui-bind-widgets.js` (DRAGDOMAIN) | the automatic speed not capped (vSpeed 0 passed through) | `DRAGDOMAIN WRONG capped {"v":[0],"want":53644500,...}`: the accumulator overflowed int (INT_MIN on x86-64) and ImGui's own wrap test clamped to the lower bound; test failed |
| codex r2 | `ui-imgui-bind-widgets.js` (SAVEDHUE) | (RED, and the mutation) `guard::sanitizeColorEditState()` not called | `SAVEDHUE WRONG large {"saved":{"held":true,"hue":10000000000,"saturation":0},"after":{"held":true,"hue":10000000000,...}}`: ImGui kept and restored the typed hue (and converted it at widgets:5980, silently on x86-64); test failed |
| codex r2 | `ui-imgui-bind-widgets.js` (COLORDOMAIN) | (RED, and the mutation) `colorButton`'s domain check removed | `NOTHROW ERR_OUT_OF_RANGE` after `REJECTED ok` (Codex's `[-0.1, FLT_MAX, 0, 1]` with InputHSV accepted); test failed |
| codex r2 | `ui-imgui-bind-style.js` (PICKER) | (RED, and the mutation) `framePaddingYLimit()` returning the length limit | `NOTHROW ERR_OUT_OF_RANGE` (FramePadding [4, Y + 1] accepted); test failed. By hand before the fix, Codex's padding [4, 100000] with the picker open drew 1106303 vertices, arcs of about 91,700 segments each, each one primitive past 16-bit indices (no abort: Render()'s check sees only the last offset, draw:3554) |
| codex r2 | `ui-imgui-bind-widgets.js` (FLOATWB) | (RED, and the mutation) `Call::write()`'s infinity rule removed | `FLOATWB WRONG inputFloat {"seen":[null,null],"threw":"imgui.inputFloat(): v[0] must be a finite number within the range of a float; got inf"} dragFloat {"seen":[3e+38,null,null],...,"threw":"... got inf"}` (JSON null is Infinity); test failed |
| codex r2 | `ui-imgui-bind-widgets.js` (FLOATWB) | the drag accumulator reset removed | `FLOATWB WRONG dragFloat {"seen":[3.0000000054977558e+38,3.4028234663852886e+38,-3.4028234663852886e+38],...}`: the third move flipped the value to -FLT_MAX; test failed |
| codex r3 | `ui-imgui-bind-widgets.js` (TRANSITION) | (RED, and the mutation) `guard::resetDragAccumulatorForInt()` not called | `TRANSITION WRONG {"integer":[-1073741823],"floating":[1.0000000150474662e+30],...}`: the 5e11 accumulator converted to int (INT_MIN on x86-64, undefined) and the write-back clamped it; test failed |
| codex r4 | `ui-imgui-bind-widgets.js` (SHAREDACCUM, LOGDRAG) | (RED, and the mutation) the round-3 rule restored: reset unless below 1 before every dragInt, without the active-ID condition or the logarithmic case | `SHAREDACCUM WRONG {"alone":{"f":16777234},"beside":{"f":16777224}}` (the unrelated dragInt discarded the float drag's remainder every frame) and `LOGDRAG WRONG {"afterRight":100,"v":[1]}` (the legitimate remainder discarded, so the move back dropped to 1); test failed |
| codex r4 | `ui-imgui-bind-widgets.js` (TRANSITION) | `guard::beforeIntDrag()` not called | `TRANSITION WRONG {"integer":[-1073741823],...}`; test failed |

## Behaviours no test detects

Properties that hold, by reading or by a check made by hand, and that no
automated test would notice losing. Each task appends its own.

- **Task 1: ImGui's error output to stdout is off.** `setRecoveryFlags()`
  (`addons/imgui/bind/bind_guards.cpp`) sets
  `io.ConfigErrorRecoveryEnableDebugLog = false`. With it on, ImGui
  `printf`s `[<frame>] [imgui-error] In window '<name>': <msg>` to stdout
  for every usage error. A test cannot see it: lit's stdout is a file, so
  stdio is fully buffered, and every windowed process ends in `_exit()`,
  which discards that buffer. Observed on 2026-11-27 with the line removed,
  under a pty: `[00001] [imgui-error] (current settings: Assert=0, Log=1,
  Tooltip=0)`, then one line per error (`... In window 'Debug##Default':
  Calling End() too many times!`, `... In window 'left open': Missing
  End()`, ...); with it restored, none. To re-check: `script -qc "python3
  utils/with-display.py 100 env HERMES_NODE_UI_TEST_HOOKS=1
  cmake-build-asan/bin/hermes-node-ui test/ui-imgui-bind-errors.js main
  /tmp/x.ppm" /dev/null | grep -c imgui-error` prints 0.
- **Task 1: an N-API failure never returns silently.** Every `napi_*`
  status the readers, writers and result helpers in
  `addons/imgui/bind/bind_util.cpp` check goes through `Call::napiOk()`,
  which throws `imgui.<name>(): internal error <action> <what>` when no
  exception is already pending, so a binding function cannot return
  `undefined` with nothing thrown. Those statuses fail only on engine
  errors no JavaScript value produces (a wrong-type value is caught by the
  `napi_typeof` check before), so the path is unreachable from a test.
  (`typeOf()` and `isArray()` do not go through it: a failure there reads
  as `undefined` / not an array, which then throws the ordinary
  `ERR_INVALID_ARG_TYPE`.)
- **Task 3: `__test.setDoubleClickTime()` takes effect.** With its two
  assignments removed, DOUBLE still passed in the one run tried, because
  the two clicks happened to fall within ImGui's default 0.30 s. The hook
  exists to remove that dependency on timing, so no test can see it lost.
  It was checked by reading: the function writes
  `io.MouseDoubleClickTime` and `io.MouseSingleClickDelay` and returns the
  previous time.
- **Task 5: `__test.setTooltipDelay()` takes effect.** With its two
  assignments removed, TOOLTIPS still passed: at the default 0.15 s +
  0.15 s the tooltip appears well inside `harness.until`'s 600-frame
  bound. The hook exists to remove that dependency on frame time, so no
  test can see it lost; it was checked by reading (it writes
  `style.HoverStationaryDelay` and `style.HoverDelayShort` and returns
  the previous values of both, `[stationary, delayShort]`; corrected in
  the final review, this note first said it returned the previous
  `HoverDelayShort` alone).
- **Task 4 (review): `Call::write(RefString)` measures the buffer with
  `strnlen` bounded by its capacity**, not `strlen`. The buffer is always
  terminated (the binding terminates it and ImGui never writes its last
  byte with anything else), so no test can tell the two apart; the bound
  is a defence. Its neighbour, invalid UTF-8 coming back from ImGui, was
  checked by reading and is not reachable: when the buffer is full ImGui
  inserts only whole code points (`STB_TEXTEDIT_INSERTCHARS`,
  imgui_widgets.cpp:4311-4313), which FULL in
  `ui-imgui-bind-input-text.js` exercises; and Hermes's
  `napi_create_string_utf8` (`hermes/API/napi/hermes_napi_string.cpp`)
  replaces an invalid sequence with U+FFFD rather than failing, so even
  such a buffer would come back as a valid string and never throw.
- **Task 7: a `Float32Array` is used in place, not copied.** `plotLines`,
  `plotHistogram` and `drawList.addPolyline` pass a Float32Array's own
  data to ImGui (`Call::floats`). A copy would draw the same picture, so
  no test can tell the two apart; checked by reading.
- **Task 7: `listClipper` never steps a clipper again after the callback
  threw.** With the pending-exception check removed, the loop calls
  `Step()` again with the exception pending. A clipper with a second range
  exists and is tested (FROZEN: a scrolled table with a frozen header row,
  imgui.cpp:3571-3578, imgui_tables.cpp:1866), and the mutation still
  passes: Hermes's `napi_call_function` begins with `NAPI_PREAMBLE`, which
  returns `napi_pending_exception` without calling while an exception is
  pending (hermes/API/napi/hermes_napi_function.cpp:272,
  hermes/API/napi/hermes_napi_impl.h:597-604), so the callback is not run
  again, and the remaining `Step()`s end the clipper where `End()` would.
  The check stays: it is what the design requires, and an engine that did
  call would run the callback with an exception pending.
- **Task 8: a settings string's size stays inside ImGui's `int`
  arithmetic** (`Buf.resize((int)ini_size + 1)`, imgui.cpp:16622) only
  because Hermes caps a string at 2^28 UTF-16 units
  (hermes/include/hermes/VM/StringPrimitive.h:136), at most 805,306,368
  bytes once encoded. The binding does not check it, and no test makes a
  string that size; checked by reading.
- **Task 7 (review): `listClipper` takes ImGui's usage errors after
  `Begin()` and after each `Step()`** (`bind_clipper.cpp`), so none is
  dropped by the callback's first binding call. None is reachable with an
  itemHeight of at least 1 (the one recoverable check is in the measuring
  branch, imgui.cpp:3620), so no test reaches the two lines.
- **The length limit's division by the pixel density** (`lengthLimit()`,
  `bind/bind_util.cpp`; Codex review). Every window test opens its window
  without `highDpi`, at density 1, where the limit is 2^20 units either
  way: a mutation that drops the division (or the cap at 2^20 units for a
  density below 1) leaves every test green. Read: at density D the draw
  list multiplies each length by D before the conversions the limit
  protects (imgui_draw.cpp:666, 873, 2412-2413, 2598).

## Notes

### Task 1

- **What `ErrorLog()` does with the callback set and asserts off**
  (imgui.cpp:12099-12142, read before writing `bind_guards.cpp`). Every
  `IM_ASSERT_USER_ERROR` / `_RET` / `_RETV` (internal:2374-2376) calls
  `ErrorLog(msg)` and asserts only if it returns true. `ErrorLog` (1)
  writes `[imgui-error] In window '<name>': <msg>` to ImGui's debug log
  when `io.ConfigErrorRecoveryEnableDebugLog` is set, and the context's
  default `DebugLogFlags` include `ImGuiDebugLogFlags_OutputToTTY`
  (imgui.cpp:4596), so `DebugLogV` also `printf`s it to stdout
  (imgui.cpp:24182); (2) opens the error tooltip when
  `io.ConfigErrorRecoveryEnableTooltip` is set; (3) calls
  `g.ErrorCallback(&g, g.ErrorCallbackUserData, msg)` when set
  (imgui.cpp:12137-12138); and (4) returns
  `io.ConfigErrorRecoveryEnableAssert`. With asserts off it returns false,
  so the macro does not assert: the plain form goes on after the report,
  the `_RET` forms return. `ErrorRecoveryTryToRecoverState` (imgui.cpp:
  11977, run by `EndFrame()` against the stack sizes stored at
  `NewFrame()`) ends every window left open, reporting "Missing End()" /
  "Missing EndChild()" / "Missing EndTable()", and then runs
  `ErrorRecoveryTryToRecoverWindowState` (imgui.cpp:12015, also run by
  every `End()` against the sizes stored at that window's `Begin()`),
  which closes, in this order and each with its own "Missing ...()"
  report, tables, tab bars, multi-selects, the menu bar, tree nodes,
  groups, IDs, disabled blocks, style colours, item flags, style
  variables, fonts and focus scopes. `ErrorCheckNewFrameSanityChecks`
  asserts plainly that at least one of the four outputs is on
  (imgui.cpp:11857-11858); the callback is the one left on.
- **Departure: ImGui's debug log is turned off too.** The plan names two
  flags; `setRecoveryFlags()` also sets
  `io.ConfigErrorRecoveryEnableDebugLog = false`, because with it on every
  usage error is `printf`ed to stdout, unconditionally (point 1 above):
  output the addon did not decide to print, outside the tracing rule. The
  callback's trace replaces it. Verified by hand, not by a test; what was
  observed and how to re-check it are under "Behaviours no test detects".
- **Every usage error is traced once.** ImGui's through the callback; a
  guard's refusal by the guard itself (`refuse()` in `bind_guards.cpp`),
  both as `usage error: <msg>`. `throwUsage()` does not trace, so nothing
  is traced twice. `ui-imgui-bind-errors.js`'s ONCE and CLOSINGONCE runs
  pin it: the usage-error and discard lines, between `BEGIN` and `END`,
  as one `-NEXT` chain.
- **`newFrame()` calls `leaveImGui()` after `ImGui::NewFrame()`**, like
  every bound function. NewFrame()'s own body (imgui.cpp:5867-6174) has no
  `IM_ASSERT_USER_ERROR`; if one is reached through a function it calls,
  the frame is open and the error is thrown. The discard path still only
  traces.
- **Checks implemented in Task 1 that still need their test cases:**
  - ~~`begin('')` -> `ERR_INVALID_ARG_VALUE`~~: tested in Task 3,
    `test/ui-imgui-bind-windows.js` REJECTED.
  - ~~`beginChild`'s three flag combinations -> `ERR_OUT_OF_RANGE`~~:
    tested in Task 3, `test/ui-imgui-bind-windows.js` REJECTED, with a
    mutation for each.
  - ~~`sliderFloat3`'s `SliderFlags_WrapAround` rejection and its `format`
    check~~: tested in Task 4, `test/ui-imgui-bind-widgets.js` REJECTED and
    FORMAT, with a mutation for each.
- **`enterImGui()` drops a stale usage error** (one reported outside any
  binding call, already traced), so it is not blamed on the next call.
  Nothing in this task produces one; it is a defence.
- **`endGroup` has two guard conditions**, `canEndGroup:no-group` and
  `canEndGroup:wrong-window`, as its surface entry says
  (imgui.cpp:12719 and 12722). The brief's GUARD_GROUP refuses only the
  first (twice); the case also begins a group in the implicit window and
  calls `endGroup()` inside another window, so the second condition has a
  refusal for `checkGuardCoverage`. `endChild` has one,
  `canEndChild:not-child`.
- **Only what the pilot functions use is declared in `bind_guards.h`**
  (`canEndGroup`, `canEndChild`, the registry, the window list), as the
  ledger rules. `labelIdIsWindowId` is Task 4's (its STRINGS case and
  mutation are there) and `snapshotScope` / `sameScope` Task 7's (the
  clipper). Until Task 4, `button('')` or `beginChild('')` directly in a
  window still reaches ImGui's plain assertion (imgui.cpp:12319) and
  aborts a Debug build.
- **Pilot functions live in their groups' files**, not in
  `bind_frame.cpp`: `bind_windows.cpp`, `bind_layout.cpp` (Layout and
  IDs: the File Structure has no file for IDs), `bind_text.cpp`,
  `bind_widgets.cpp`, `bind_queries.cpp`, `bind_style.cpp`, each with its
  `register<Group>()` declared in `bind_util.h`. The tasks that "create"
  those files extend them instead. `newFrame()` and `render()` stay in
  `imgui_module.cpp` (the frame protocol); `showDemoWindow` moved from
  there to `bind_frame.cpp`.
- **`bind_util.h` beyond the plan's interface**: `Call::outArray(i,
  what)`, which checks a getter's optional output array before the ImGui
  call (so a wrong `out` is a rejected call, not counted);
  `Call::typeOf`, `isNull`, `isArray`, `name`; `Call::napiOk()` (see
  "Behaviours no test detects"); `bind::bindCalls()` for the test hook.
  Every reader of the plan's interface is implemented, including
  those no pilot uses yet (`refInt`, `refString`, `floats`, `flags`,
  `enumValue`, `stringOrNull`): they are untested until their tasks.
- **Until Task 2**, every flags and enum argument was read with `int32`
  under a `// Task 2: EnumInfo` comment, except the two indices that index
  an array without a check: `pushStyleColor`'s and `pushStyleVar`'s `idx`
  were read with `intIn(0, <type>_COUNT - 1)`. Task 2 replaced all of
  them (see its notes).
- **Dispatched arguments name both forms** in their `TypeError`:
  `pushStyleColor`'s "col must be a number or an array of 4 numbers",
  `pushStyleVar`'s "val must be a number or an array of 2 numbers",
  `pushID`'s "strId must be a string, or intId a number" (the surface
  entry's parameter names); `ui-imgui-bind-args.js` ARGTYPE checks the
  text.
- **`begin('')`** throws `ERR_INVALID_ARG_VALUE` (its "Rejected" line names
  the case and not the code; an empty string is a wrong value, not a wrong
  type).
- **The Frame group's test is in `ui-imgui-bind-args.js`** (EITHER, FRAME,
  and `checkGroupCoverage(h, 'Frame')`), since the plan gives that group no
  test file of its own. `checkGroupCoverage` skips names that are not
  functions on the module, which is `version`; its value is checked by
  `test/imgui-load.js`.
- **`test/imgui-load.js` no longer prints the whole export list**: its
  `KEYS` line became `TEST <bool>` (whether `__test` is exported), which
  is what that line guarded. The exact list is
  `test/imgui-bind-surface.js`'s now, and would otherwise have to be
  edited in two places by every task.
- `state().windows` reads `WasActive` inside a frame and `Active`
  outside one, since `NewFrame()` copies the one into the other
  (imgui.cpp:6069-6070); `docked` is `DockIsActive`.
- No disagreement between the surface table and ImGui's source was found
  in the entries bound here.

### Task 2

- **The "Constants" table agrees with `imgui.h`**, checked by parsing every
  one of the 25 enum blocks with the obsolete `#ifndef
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS` sections set aside: each type's compiled
  count, excluded names and obsolete names are the table's, and the
  exported counts sum to 581. No correction to the table was needed.
- **`addons/imgui/bind/bind_constants.cpp`** holds one `constexpr Constant
  k<Type>List[]` per type, one `HNIMGUI_CONST(<C++ enumerator>)` line per
  constant, so no value is typed. The JS name is the C++ spelling minus
  `ImGui` / `Im`. Each `EnumInfo` is computed from the same list:
  `maskOf()` (the union of every value's bits) for a flags type,
  `valuesOf()` for an enum type; so a constant added to or removed from a
  list changes what is exported and what is accepted together. The lists
  were generated from the header once and checked against the table; they
  are maintained by hand from now on, like any source.
- **Every flags or enum parameter bound so far** now goes through
  `Call::flags` / `Call::enumValue`: `begin` (`kWindowFlags`), `beginChild`
  (`kChildFlags`, `kWindowFlags`), `setNextWindowPos` and
  `setNextWindowSize` (`kCond`), `sliderFloat3` (`kSliderFlags`),
  `pushStyleColor` (`kCol`, then `Col_COUNT` refused by name) and
  `pushStyleVar` (`kStyleVar`, then `StyleVar_COUNT`). The two indices
  keep Task 1's behaviour: the accepted set is still 0 .. `_COUNT - 1`,
  now as "an exported value that is not `_COUNT`".
- **Messages** (`Call::flags`, `Call::enumValue`): `<arg> has bits set that
  no <Prefix>_ constant has: 0x<bits>` and `<arg> must equal one of the
  <Prefix>_ constants; got <n>`, where `EnumInfo::name` is the JavaScript
  prefix (`"WindowFlags_"`). `ui-imgui-bind-constants.js` pins both and the
  `_COUNT` one.
- **No exported flags enumerator has bit 31** in this version (the highest
  is `ColorEditFlags_InputHSV`, `1 << 29`), so the brief's "a negative value
  produced by `|` with a high public bit is accepted where such a bit
  exists" has no case to run. The test prints `HIGHBIT none` (computed from
  the module's constants, so a future bit 31 changes the line and someone
  has to write the accepting case), and checks the other half: a negative
  value made with `|` passes the integer reader and is refused by the mask
  (`... 0x80000000`).
- **`bind-groups.js` pins every constant by name**: `constantTypes`
  (prefix -> kind, the exact sorted `names` exported, and the excluded
  names including the obsolete ones), generated once from the built module
  and checked against the header parse and the table's per-type counts
  (581 names, plus `FLT_MIN` / `FLT_MAX`); `allConstants()` lists them.
  `constants` holds three hand-copied samples per type, each with its
  `imgui.h` line, plus the float limits. `all()` lists the groups' names
  only, because `ui-imgui-bind-args.js` NOFRAME iterates it.
  `imgui-bind-surface.js` (no display) compares the module's names with
  `all()` plus `allConstants()` exactly, so an added, removed or swapped
  constant is `EXTRA` / `MISSING` there. `ui-imgui-bind-constants.js`
  checks the per-type counts (from `names.length`), the excluded names and
  the sample values. (Review fix: the first version counted constants per
  type only, in the window test, so without a display a changed constant
  failed nothing unless it was a sample, and a swap kept every count.)
- **Out-of-range flags spellings**: `Call::int32` accepts only integers in
  -2^31 .. 2^31-1, so a flags argument of 2^31 or 2^32-1 (bit 31 written
  unsigned) is `ERR_OUT_OF_RANGE` before the mask is consulted; REJECT has
  both cases.
- `ImGuiMouseButton` has `kMouseButton` like every type, but a mouse-button
  argument will be read with `intIn(0, 4)` (G1); no function bound so far
  takes one.

### Task 3

- **The five sections are bound as their entries say**, in the files
  Task 1 created for them: `bind_windows.cpp` (Windows), `bind_layout.cpp`
  (Layout and IDs), `bind_text.cpp` (Text), `bind_queries.cpp` (Item and
  input queries). The brief's "Create" for those four files is out of
  date, and `addons/imgui/CMakeLists.txt` needed no change: the files were
  already listed. No (b) guard was added, since none of these entries has
  one beyond Task 1's `canEndGroup` and `canEndChild`, so
  `ui-imgui-bind-errors.js` is unchanged and `GUARDS ok` still holds.
- **No RED run was recorded.** The four bind files were extended before
  the two new tests were first run, so the first run was already green;
  each test's ability to fail is shown by the mutations instead (every
  case of both tests has at least one that breaks it). The "(RED)" row of
  the Mutations table records that absence and is not a mutation: Task 3
  has 38 mutation rows, plus 9 from review.
- **No disagreement between the surface table and ImGui's source** was
  found in these five sections; no entry was changed.
- **Two keys of the argument rules, implemented once each**:
  `kLastMouseButton` (4) for every mouse-button argument (`intIn(0, 4)`,
  G1's exception), and `readKey()` in `bind_queries.cpp` for `isKeyDown` /
  `isKeyPressed`: `enumValue(kKey)` first (G1), then GetKeyData's own
  condition (imgui.cpp:10283-10286), a named key or exactly one of the
  four `Mod_` keys, which refuses `Key_None` / `Mod_None`,
  `Key_NamedKey_END` and `Key_NamedKey_COUNT` with `key must be a named key
  or a single Mod_ constant; got <n>`.
- **`getID` dispatches like `pushID`**, with the same two-form TypeError;
  `ui-imgui-bind-args.js` ARGTYPE checks it, and NOFRAME has a sample for
  every new function.
- **Behaviour asserted in Task 4, called here only for coverage:**
  `isItemDeactivatedAfterEdit` (called every frame of the queries case;
  a button never edits, so it is always false here) and the text-field
  half of `setKeyboardFocusHere` (activation of an inputable item). The
  button half is asserted here: FOCUSHERE.
- **`setItemDefaultFocus` is asserted by DEFAULTFOCUS** (corrected in
  review; the first version of these notes said nothing bound could
  observe it, which was wrong). A window submitted for the first time
  appears and takes focus, and Begin() then calls `FocusWindow` and
  `NavInitWindow(window, false)` (imgui.cpp:8799-8802), which sets
  `g.NavInitRequest` (imgui.cpp:14688). That passes SetItemDefaultFocus's
  early checks, and when the item lies outside the clip rectangle it calls
  `ScrollToRectEx` (imgui.cpp:9716-9732). So a new 300x200 window holding
  `dummy([10, 600])`, `button('t')` and `setItemDefaultFocus()` scrolls:
  `getScrollY() > 0`, waited for with `harness.until`. With the ImGui call
  removed it never scrolls (`TIMEOUT DEFAULTFOCUS`).
- **The two windows tests follow the brief with these choices, each the
  strongest true assertion found:**
  - CLOSE locates the close button from `getWindowPos()`,
    `getWindowSize()`, `getTextLineHeight()` and a pushed
    `StyleVar_FramePadding` of `[4, 4]`, following
    RenderWindowTitleBarContents (imgui.cpp:7865), rather than from
    default style values.
  - SCROLL reads the scroll inside the parent window, right after its
    `begin()` (where ImGui applies a scroll request), and runs up to ten
    frames for a new window's content size to settle before reading
    `getScrollMaxY() > 0`. `getScrollY() === getScrollMaxY()` held exactly.
  - AVAIL: the brief names no assertion for `getContentRegionAvail`; it is
    exactly `[280, 180]` at the start of a 300x200 window with no title
    bar and a 10-unit WindowPadding.
  - BGALPHA pushes `Col_WindowBg` as `[0, 0, 1, 0.25]` and checks the
    interior is `(0, 0, 255)` exactly: without `setNextWindowBgAlpha(1)`
    the window is the blend `(19, 19, 87)` (the mutation).
  - WIDTH: no single-value slider is bound before Task 4, so the slider is
    `sliderFloat3` with a hidden label (`'##next'`), whose item is the
    group of its three fields and is exactly the item width
    (PushMultiItemsWidths). `popItemWidth()` is checked by the next
    slider returning to the default width.
  - GREEN scans every pixel of the `textColored` item's rectangle for
    exact `(0, 255, 0)`, and the same text drawn with `text()` beside it
    for none. Some glyph pixels do come out exactly `(0, 255, 0)` at scale
    1 (observed: the case passes, and with `TextColored` replaced it counts
    0); why the font's coverage is fully opaque there was not looked into.
    This is the one assertion on glyph pixels, as the brief asks, rather
    than on a flat area 3 px from any edge, which a glyph does not have.
  - HEIGHTS: "positive and ordered" is asserted as
    `getTextLineHeight() > 0`, equal to `text('abc')`'s height, and less
    than both other heights; `alignTextToFramePadding` lowers the next text
    by `(frameHeight - lineHeight) / 2`.
  - Beyond the brief: GROUP (`beginGroup`/`endGroup`'s item is the union
    of its two buttons), SPACING (`spacing`, `newLine`, `separator`,
    `separatorText` geometry), TEXTS (each text variant is a non-empty
    item starting at or below the previous one's bottom, since `rect()`
    reads the last item and a variant that drew nothing would otherwise
    be read as its predecessor; a long `textWrapped` is several lines and
    narrower than the window; `bulletText` is wider than its text and
    `labelText` wider than its value; `textDisabled('%d')` is as wide as
    `'xx'`, so no `%` is interpreted), and `calcTextSize`'s `hideTextAfterDoubleHash` and
    `wrapWidth`.
- **DOUBLE does not depend on time: `__test.setDoubleClickTime()` exists
  for it** (added in review, `addons/imgui/imgui_test_hooks.cpp`, test hooks
  only). ImGui counts a click as a repeat only within
  `io.MouseDoubleClickTime` (0.30 s) of the previous one, measured in its
  own time, which advances by `io.DeltaTime` from the real clock
  (imgui_platform.cpp), so a double click made through frames that each
  wait on a read-back passes or fails with machine load. The first version
  of the case injected four events at once and retried up to five times,
  which still depended on elapsed time. The hook sets the time to 1000 s
  for the case, and the default back afterwards. It also moves
  `io.MouseSingleClickDelay` to `seconds + 0.2`, ImGui's default gap,
  because NewFrame asserts plainly that the delay is the larger
  (imgui.cpp:11854). The case then clicks once far from the button, so
  that CLICK's earlier click does not count (a repeat must also be within
  `MouseDoubleClickMaxDist`, imgui.cpp:11154-11163), and clicks the
  button twice with `harness.clickAt`: no double through the first click,
  exactly one frame with a double in the second. The mutation that matters
  is on the binding (`isMouseDoubleClicked` always false: `DOUBLE WRONG
  afterFirst=0 doubles=0`). With the hook's effect removed, the case still
  passed in the one run tried: the clicks were within 0.30 s. That is
  exactly the luck the hook removes, so no test can detect losing it.
- ~~**`beginChild`'s label-id guard (its `Label-id: yes`) is still Task
  4's**, as Task 1's notes rule; until then `beginChild('')` directly in a
  window reaches ImGui's plain assertion.~~ Done in Task 4 (GUARD_LABELID
  in `ui-imgui-bind-errors.js`, STRINGS in `ui-imgui-bind-input-text.js`).
- **`isWindowHovered` and `isItemHovered` with flags outside their allowed
  masks** (kind (c), imgui.cpp:9225 and 5114) throw `ERR_IMGUI_USAGE`
  after the call; both tests' REJECTED pin the message and that the call
  was counted.
- **`check-hermes-node-unit` was red once** before this commit, with
  `SurfaceTableTest.DisplacementRacesCompletionUnderLoad` the only failure
  (in two of three runs); it passed 5/5 alone and the third suite run was
  460/460. That is the open load-dependency issue in `dz/` for that test,
  not this task: nothing here touches the compositor.

### Task 4

- **The Widgets section is bound as its entries say**, in
  `addons/imgui/bind/bind_widgets.cpp`, which Task 1 had created (the
  brief's "Create" is out of date; `addons/imgui/CMakeLists.txt` needed no
  change). No disagreement between the section and ImGui's source was
  found; no entry was changed.
- **The label-id check** is `guard::labelIdIsWindowId(label)`
  (`bind_guards.cpp`): `ImGui::GetID(label) == g.CurrentWindow->ID`,
  counted under the guard condition `labelId:window-id` and traced as
  `usage error: an item ID equal to the window's own ID`. The binding
  functions reach it through `Call::needLabelId(what, label)`
  (`bind_util.h`), after `needFrame()`, whose message names the argument
  (`label`, `strId`, `descId`). It is applied to every entry bound so far
  that says `Label-id: yes`: the 19 in Widgets and `beginChild` (Windows).
  No entry of Layout, IDs, Text or Queries has one. The four bound
  entries the section exempts (`sliderFloat2..4`, `colorEdit3/4`) and
  `progressBar` / `bullet` (item ID 0) do not run it. STRINGS gives `''`
  to every one of the 20 and checks each is refused, uncalled.
- **endCombo's and endListBox's state guards are not here.** Both are
  bound, but `canEndCombo` and `canEndListBox` (their (b) preconditions)
  are Task 5's in the plan ("Produces: ... guards ... `canEndListBox`,
  `canEndCombo`, and the `BeginComboDepth` reset", with GUARD_LISTBOX and
  GUARD_COMBO), and `canEndCombo` rests on the `BeginComboDepth` facts
  Task 5 verifies first. Until then a misplaced `endListBox()` reaches
  ImGui's plain assertion (widgets:8997) and a misplaced `endCombo()`
  ImGui's recoverable one (widgets:2096) after it has decremented the
  counter.
- **The brief's "the Callback* bits are not exported"** is out of date:
  Task 2 exports all six (`bind-groups.js`, `constantTypes`). FLAGS names
  them and also pins their values against `1 << 18 .. 1 << 23`
  (imgui.h:1366-1371).
- **SURROGATE uses a truncation to force the write-back.** Hermes encodes
  an unpaired surrogate as U+FFFD when the binding reads the string, so
  the buffer equals the original string's encoding and, by the design's
  rule, an untouched field writes nothing back. `'a\ud800bc'` at capacity 5
  is cut after the replacement character, and comes back as `'a\ufffd'`.
- **Choices made where the brief leaves the assertion open**, each the
  strongest true one found:
  - BUTTONS asserts "the frame of the release" as: exactly one frame
    returned true, and `isMouseDown(0)` was false in it.
  - SLIDERS drags each slider's last field (a hidden label, so the item
    rectangle is the frames) from 4 units inside its right end to 60
    beyond it: that element is exactly the maximum and the others are
    unchanged, which also checks that all N elements are written back.
  - DRAGS asserts `v[0] > 0` after a 40-unit drag, not a value: ImGui's
    drag threshold and speed make the exact amount an implementation
    detail.
  - COLOREDIT starts from components of 0 and 1, which survive ImGui's
    8-bit round trip exactly (ColorEdit4 rewrites every component when one
    changes), so the untouched ones are compared exactly.
  - COLORBTN moves the mouse away before the capture, so that the
    tooltip does not cover the button; the 40x40 button's centre is exactly
    (255, 0, 0).
  - DEACTIVATE counts `isItemDeactivatedAfterEdit()`: 0 frames while the
    field is edited, exactly 1 after a click on the window's background.
  - FOCUS waits until `wantTextInput` is false with no focus request,
    calls `setKeyboardFocusHere()` for one frame, then waits for it to be
    true.
  - STRINGS: `text()` of a 1 MiB string is an item wider than the window
    (the renderer has `RendererHasVtxOffset`, so the reservation of 4 Mi
    vertices does not overflow 16-bit indices).
- **No assertion depends on time or a frame count**; nothing needed a new
  test hook (no double click or key repeat is involved).
- `ui-imgui-bind-args.js` gained a NOFRAME sample for every new function,
  the dispatch messages of `radioButton` and `selectable` (both forms
  named), and the argument types of the dispatched forms, `inputText`'s
  buffer, `beginCombo`'s preview and `progressBar`'s overlay.
- `test/ui-imgui-bind-layout.js`'s comments that pointed at Task 4 now
  name the cases in `ui-imgui-bind-input-text.js`.

### Task 5

- **Step 1, the combo facts, verified from the source before the reset
  was added.** `g.BeginComboDepth` appears in four places and no others
  (grep of `external/imgui/imgui/`): declared at internal:2808; set to 0
  in the context's constructor (imgui.cpp:4549); incremented in
  `BeginComboPopup()` only after the popup window's `Begin()` returned
  true (widgets:2083); decremented in `EndCombo()` (widgets:2091), before
  its "Calling EndCombo() in wrong window!" check (widgets:2096), which
  returns with the counter already lowered. The two other reads format
  the popup's name `"##Combo_%02d"` (widgets:2055, 2094). No combo can be
  open across a frame boundary: `EndFrame()` runs
  `ErrorRecoveryTryToRecoverState()` (imgui.cpp:6432-6433), which ends
  every window above the depth stored at `NewFrame()` (imgui.cpp:
  11981-12007, `StackSizesInNewFrame` stored at imgui.cpp:6162), then
  `EndFrame()` ends the fallback window itself (imgui.cpp:6456), so the
  window stack is empty when the frame ends; `EndFrame()` cannot be
  skipped with windows open (its early return is for a frame already
  ended, imgui.cpp:6425-6426); `NewFrame()` asserts that the last frame
  was ended (imgui.cpp:11839); and the binding's `newFrame()` ends a frame
  it finds open before starting the next one (`imgui_module.cpp`). The
  recovery ends a combo's popup with `End()`, never `EndCombo()`, so the
  counter stays one too high after a combo left open; at `newFrame()`
  zero is the correct value. The reset was therefore added
  (`guard::beginFrame()`, called by `newFrame()` before
  `ImGui::NewFrame()`); nothing went to the tracker. COMBO_LEFT_OPEN
  shows it: without the reset the next frame's combo is `##Combo_01`.
- **Guards added, one name per condition** (all in
  `addons/imgui/bind/bind_guards.cpp`, each with a refusal in
  `ui-imgui-bind-errors.js`): `canTreePop:no-node`, `:id-mismatch`,
  `:id-stack`; `canEndTabItem:no-tab-item`; `canBeginMenuBar:already-open`;
  `canEndMenuBar:no-menu-bar-flag`, `:not-open`, `:no-group`,
  `:wrong-window`; `canEndTooltip:not-tooltip`; `canEndListBox:not-child`,
  `:no-group`, `:wrong-window`; `canEndCombo:no-combo`, `:wrong-window`;
  `canBeginPopupContextItem:no-id`. Where ImGui returns before its
  assertion (a window with `SkipItems`, a missing tab bar that
  `EndTabItem()` reports recoverably, `BeginMenuBar()` in a window without
  `WindowFlags_MenuBar`), the guard returns nullptr, so it never refuses
  a call ImGui would accept. `endCombo` and `endListBox` in
  `bind_widgets.cpp` gained only their guard calls (and the file comment
  that said they were owed).
- **Cases beyond the brief's list, each because a registered condition
  needs a refusal of its own**: GUARD_TREE has a third case, a node whose
  own ID was popped with `popID()` (`canTreePop:id-stack`, widgets:7426);
  GUARD_MENUBAR has four (`endMenuBar()` without the flag, with no menu
  bar begun, and after the menu bar's own group was ended with
  `endGroup()`, once with no group left and once with another window's
  on top); GUARD_LISTBOX has three (the clipped list box, which refuses as
  not-child since its parent is a top-level window, then a plain child
  with no group in its parent, then one whose parent's top group is the
  implicit window's). The pushID case of GUARD_TREE needs a node that
  keeps stack data, since ImGui compares IDs only then (widgets:7406-7410):
  `TreeNodeFlags_DrawLinesFull` makes it (widgets:7097-7100); with
  default flags the unpopped ID is popped in the node's place and the
  window's `end()` reports "Missing PopID()".
- **The brief's GUARD_COMBO and COMBO_LEFT_OPEN, as built.** The combo is
  opened by a click (the errors test had no clicks before; it uses
  `harness.clickAt`), `state().windows` shows it as `##Combo_00`
  (printed, `SETUP combo ##Combo_00`). One frame then calls `endCombo()`
  before the combo (no-combo) and from a child window begun inside it
  (wrong-window, the child being `##Combo_00/in combo_<id>`); after that
  frame the only active combo popup window is still `##Combo_00`. A child
  of the popup also starts with `##Combo_`, so the read-back matches the
  name exactly (`/^##Combo_-?[0-9]+$/`). Without `canEndCombo` the
  unmatched call reaches ImGui, which reports "Calling EndCombo() in
  wrong window!" itself, so that mutation fails as `CALLED endCombo`
  rather than as an abort.
- **ENDMENU, ENDPOPUP, ENDTABBAR** are one frame, each line printed with
  ImGui's message, and each call counted (it reaches ImGui, which
  recovers).
- **Surface table corrected** (the source wins): `endMenuBar`'s entry said
  the group condition fails "when a `beginGroup()` inside the menu bar was
  left open"; such a group belongs to the window, so the condition holds
  and `EndMenuBar()` ends it in place of the menu bar's own group (checked
  by hand on 2026-11-27: `endMenuBar()` accepted, then `end()` threw
  "Missing EndGroup()"). What fails it is an `endGroup()` inside the menu
  bar (GUARD_MENUBAR).
  `endCombo`'s entry gained the `BeginComboDepth > 0` condition (an
  unmatched call would compare with `"##Combo_-1"`). The internal-state
  table lists `beginPopupContextItem` as a reader of `SkipItems`.
- **A tooltip delay made deterministic, through a new test hook.**
  `setItemTooltip()`'s hover check uses `style.HoverFlagsForTooltipMouse`,
  `Stationary | DelayShort` (imgui.cpp:1660, 13115): the item must have
  been hovered with the mouse still for `HoverStationaryDelay` and then
  for `HoverDelayShort` (0.15 s each, imgui.cpp:1657-1658), measured in
  ImGui's time, which advances by the real frame time (imgui.cpp:
  5199-5204, 5991-6004, 11135). `__test.setTooltipDelay(seconds)`
  (`imgui_test_hooks.cpp`, documented in the README) sets both; the
  popups test sets 0, so the tooltip shows one or two frames after the
  mouse arrives however long frames take. `setTooltip()` and
  `beginTooltip()` make no hover check of their own and are called while
  `isItemHovered()` (no delay with default flags) is true. Menus opened
  by a click in a menu bar, and context popups, have no delay.
- **Window names the read-backs use**, from the source: `##MainMenuBar`
  (widgets:9466), a menu `<label>###Menu_<depth %02d>` (imgui.cpp:13466),
  a popup `##Popup_<id %08x>` with the id `getID(strId)` where it is
  begun, or the item's for a context popup with no strId (imgui.cpp:
  13438, 13616), a modal its own name (imgui.cpp:13510), a tooltip
  `##Tooltip_<nn>` (imgui.cpp:13075-13077).
- **Close buttons are located, not guessed**: the header's and the tab's
  from the item rectangle, a pushed `StyleVar_FramePadding` of `[4, 4]`
  and `getTextLineHeight()` (the font size), following CollapsingHeader
  (widgets:7494-7495) and TabItemLabelAndCloseButton (widgets:11227-11228;
  the tab bar takes its padding from the style at `BeginTabBar()`,
  widgets:10041). A selected tab's close button is always shown
  (`TabCloseButtonMinWidthSelected` -1, imgui.cpp:1620).
- **Group names in `bind-groups.js`**: `TreesTabs`, `Menus`, `Popups`.
  NOFRAME has a sample for every new function; ARGTYPE has the dispatch
  messages of `collapsingHeader` ("flags must be a number, or pVisible an
  Array or null") and `menuItem` ("selected must be a boolean, or
  pSelected an Array"), and the dispatched forms' own arguments.
- **One usage error in a group test.** TREEEX's refused `treePop()` after
  `TreeNodeFlags_NoTreePushOnOpen` is traced by its guard, so the trees
  test cannot forbid usage errors with an `--implicit-check-not`; it reads
  them in a run of its own (`USAGE`, one `-NEXT` chain between `BEGIN` and
  `END`) and allows exactly that one.
- `treeNode('')` was added to GUARD_LABELID, to show the label-id check
  applied in the new groups as well.

### Task 4, review fixes

- **Format strings are bounded.** A conversion specification longer than
  30 bytes is `ERR_INVALID_ARG_VALUE` (`formatProblem()`,
  `kMaxFormatSpec`, `addons/imgui/bind/bind_util.cpp`), with its own
  message. Every fixed-size copy of the format in the widgets bound was
  checked (`char fmt`, `fmt_buf`, `ImParseFormat*` in imgui_widgets.cpp):
  the three 32-byte ones set the bound; every whole-string use is a
  bounded `vsnprintf`. The surface table's new rule G5 holds the line
  numbers. FORMATLEN drives an accepted 30-byte format through each
  assertion that guards those copies (a float slider whose value changes;
  Ctrl+click text entry on `dragInt`) and checks that 31 bytes is refused.
- **The validator's spellings are pinned**: FORMAT rejects `%ld`,
  `%hhd`, `%ls`, `%n`, `%*d`, `%5$d`, `%.*f`, `%lf`, `%Lf` and accepts
  `%%%.1f`, `%i`, `%04x`. `%i` was already accepted (the int set was
  `diuxXo` from Task 1); the review's finding was a misreading, and the
  case now pins it. ImGui's scanner keeps `i` (ImParseFormatSanitizeForScanning
  strips only width, precision and flags), so typed text in an `%i`
  field is read by `sscanf("%i")`, where a leading 0 means octal: ImGui's
  behaviour, not checked here. `%%` is a literal for both the validator
  and `ImParseFormatFindStart`, so the two find the same conversion.
- `sliderFloatN` selects its ImGui function with `if constexpr`.
- The widgets test's header no longer lists its mutations; it points at
  the Mutations table.
- **Review fix: `endMainMenuBar` guarded.** `EndMainMenuBar()` checks
  only that a menu bar is open, recoverably (widgets:9483), and then its
  `EndMenuBar()` ends the group on top (widgets:9401, 9405), so
  `beginMainMenuBar(); endGroup(); endMainMenuBar();` aborted (reproduced,
  both variants; see the Mutations table). `canEndMainMenuBar` applies
  `endMenuBar`'s two group conditions when the menu bar is open and the
  window does not skip items, as `canEndMainMenuBar:no-group` and
  `:wrong-window`; GUARD_MAINMENUBAR refuses each. The group-stack pair of
  checks is now one helper, `groupOpenIn()` in `bind_guards.cpp`, used by
  `canEndGroup`, `canEndListBox`, `canEndMenuBar` and `canEndMainMenuBar`
  with their own names and messages.
- **Review fix: the other closers re-checked for the same pattern**
  (a closer that calls another `End*` or pops what ImGui pushed):
  `endMenu` -> `EndPopup()` -> `End()`, after ImGui's own recoverable
  check of the `Popup | ChildMenu` flags (widgets:9755); `endPopup` ->
  `End()` after its recoverable check (imgui.cpp:13525); `endCombo` ->
  `EndPopup()`, its window the combo popup once `canEndCombo` passed;
  `endTooltip` -> `End()` once `canEndTooltip` passed; `endListBox` ->
  `EndChild()` and `EndGroup()`, both covered by `canEndListBox`;
  `endTabBar` -> `TabBarLayout()`, `PopID()` (recoverable) and
  `g.CurrentTabBarStack.pop_back()`, non-empty whenever `g.CurrentTabBar`
  is set; `endTabItem` -> `PopID()` (recoverable). None of these has a
  plain assertion that a sequence of bound calls reaches in the closer's
  own code. What every window-ending closer does share is `End()`'s
  window recovery (imgui.cpp:9039), which closes left-open scopes by
  calling their closers itself; a sequence that damages one of those
  scopes with another pair's closer makes that recovery abort. Besides
  the menu-bar case already in the surface table's "Not covered" 3b, one
  more was reproduced: `begin('w'); treeNodeEx('n',
  TreeNodeFlags_DefaultOpen); popID(); end();` aborts in the recovery's
  `TreePop()` (widgets:7426). It is added to 3b, not guarded: the call
  that reaches the assertion is ImGui's, and preventing it needs the
  binding to track which scopes its caller opened (part C).
- **Review fix: `__test.setTooltipDelay(stationary[, delayShort])`
  returns both previous values**, `[stationary, delayShort]`, and the
  popups test restores exactly what it returned, then checks that the
  saved values were ImGui's defaults (`Math.fround(0.15)`, imgui.cpp:
  1657-1658) and that a second set returns them unchanged.
- **Review fix: `endMenu` and `endMainMenuBar` have a behavioural
  assertion**, CLOSERS in the menus test: `getWindowPos()` after
  `endMenu()` is the main menu bar's, and after `endMainMenuBar()` the
  window's current before `beginMainMenuBar()`. With either ImGui call
  removed the run fails before CLOSERS prints, on ImGui's own report of the
  closer left out (Mutations table).
- **`endListBox()`'s accepted path is still exercised**:
  `test/ui-imgui-bind-widgets.js` LISTBOX calls it after a `beginListBox`
  that returned true, and `COVERAGE Widgets ok` counts only calls that
  reached ImGui (a refused call is not counted), with usage errors
  excluded from that test's trace.

### Task 6

- **The Tables section is bound** in `addons/imgui/bind/bind_tables.cpp`,
  registered after Menus (`registerTables`, `bind_util.h`;
  `imgui_module.cpp`; `addons/imgui/CMakeLists.txt`), and listed as the
  `Tables` group in `bind-groups.js`. No Tables entry has a write-back:
  none takes a reference array, so the "write-back removed" mutation the
  Global Constraints ask for is replaced by `tableNextColumn`'s return
  value, and said so in its row.
- **Step 1b, the wrong-window condition: reachable, kept.** A window
  appended to with `begin()` of its own name from inside a cell of a
  scrolling table becomes current (imgui.cpp:8882) with the table current,
  because `BeginTableEx()` set the table's index on the outer window as
  well as on the inner one (imgui_tables.cpp:551-553) and
  `SetCurrentWindow()` takes the current table from the window's index
  (imgui.cpp:4890), while the table's inner window is the child. TABLE_WRONG_WINDOW
  first prints `SETUP wrong window: table current, current window wrong
  window, inner window wrong window/s_<id>` from the new test hook
  `__test.tableScope()` (README), then shows the condition applied where
  ImGui asserts it: `endTable()` always (tables:1476); `tableNextRow()`,
  `tableHeadersRow()` and a `tableNextColumn()` that wraps only with a
  row open (`TableEndRow()`, tables:2126). A `tableNextColumn()` inside
  the row is accepted there. With no row open, the three row functions
  were each run unguarded in that state by hand and nothing asserted, so
  the guard does not refuse them (nothing stricter). Removing the
  condition aborts at tables:2126 (Mutations). The surface table's
  claim that an `endChild()` in a scrolling table leads to this state was
  wrong: that `endChild()` aborts (next bullet).
- **Guards beyond the surface table's, each found by walking the entry
  paths and measured as an abort before it was added** (all in
  `bind_guards.cpp`, each with its refusal in `ui-imgui-bind-errors.js`):
  `canEndTable:cursor-moved` (tables:1493: `setCursorScreenPos()` before
  the first row, which the skipped cell items do not prevent);
  `canTableSetupColumn:name-offset` (labels are stored at an `ImS16`
  offset into one buffer, tables:1747-1750; past 32767 bytes the offset
  wraps and `tableHeadersRow()` indexes the buffer with it,
  tables:1909, imgui.h:2369); and `canEnd:table-region`,
  `canEndChild:table-region`, `canEndListBox:table-region`: `end()`,
  `endChild()` or `endListBox()` in a scrolling table's own region make
  `End()`'s window recovery call `EndTable()`, which ends the same child
  again from inside that `End()` (imgui.cpp:9038-9039, 12019-12023;
  tables:1612-1616), popping the clip rectangle stack past empty
  (imgui.cpp:6296-6297, imgui.h:2377). `end` thereby gains its first
  guard (`canEnd`, `bind_windows.cpp`), outside this task's group, because
  the hazard is the table's; its surface entry, `endChild`'s and
  `endListBox`'s say so. `endPopup`, `endMenu`, `endMainMenuBar`,
  `endTooltip` and `endCombo` were checked for the same path: each
  refuses or returns (recoverably) before ending a window that is not its
  own kind, so none reaches it.
- **`canTableRowCall` takes the call** (`guard::TableRowCall`:
  `NextRow`, `NextColumn`, `HeadersRow`, `EndTable`), where the plan's
  `bind_guards.h` sketch has no parameter: the four functions reach the
  two conditions differently (only `TableNextRow()` dereferences a
  missing table; only `EndTable()` asserts the window with no row open;
  `TableNextColumn()` ends the row only when it is full), and the
  messages name the function.
- **Not covered, recorded in the surface table** (3c, 3d, 3e), each
  measured as an abort or a hang with the binding as committed: a table
  that is not the innermost one begun made current by re-entering a
  window (an ended scrolling table's region through `beginChild()` with
  its `strId`; a window holding an open table re-entered while a later
  table is open), whose every exit aborts or hangs so that a guard on the
  table calls could not help; every column disabled through visibility (tables:1005; the `TableColumnFlags_Disabled` variant is guarded since the review, below); a
  sortable table with 64 or more sorted columns (tables:3154); a tree node
  keeping stack data left open in a scrolling table's cell (widgets:7410,
  from inside `endTable()`). Note 4 there gains the `treePop()` that
  passes inside a non-scrolling table (the table raises the tree depth)
  and loses the `endChild()` in a scrolling table, now refused.
- **Choices where the brief leaves the assertion open:** COLUMNS asserts
  that the two residuals (step minus width) are equal and positive; they
  are 9 at scale 1 (`CellPadding.x` 4 on each side plus the 1-unit inner
  border, CellSpacingX1), printed by hand, not asserted as a constant.
  HEADERS samples the header row's middle line, 8 units from its edges,
  at each column's centre (50 to 100 units from any text or border), and
  the same x at the middle of the first body row; Header, HeaderHovered
  and HeaderActive are pushed to the same blue as TableHeaderBg and
  TableRowBg/Alt to the window's green, so a hover could not change the
  answer. SCROLL waits on the first body item's `min[1]` moving up (the
  wheel reaches the table's child through ImGui's hovered window), then
  compares the header's rectangle exactly. REJECTED's accepted edges
  (511 columns, `tableSetupScrollFreeze(511, 127)`, a null label, a
  negative `innerWidth` without ScrollX) show the checks are not
  stricter than ImGui's limits.
- **No disagreement in the entries' "Rejected" lines** with the source;
  the corrections to the Tables section are its introduction (the
  wrong-window route), the conditions' scope (with a row open), and the
  added guards above.
- `ui-imgui-bind-args.js` gained a NOFRAME sample for each of the eight
  functions (its `NOSAMPLE beginTable` was the first full-suite failure).

### Task 7

- **The Style, Plots, Draw list and Clipper sections are bound**: Style
  completed in `bind_style.cpp`, the others in `bind_plots.cpp`,
  `bind_drawlist.cpp` and `bind_clipper.cpp` (`registerPlots`,
  `registerDrawList`, `registerClipper` in `bind_util.h`;
  `imgui_module.cpp`; `addons/imgui/CMakeLists.txt`), and listed as the
  `Style`, `Plots`, `DrawList` and `Clipper` groups in `bind-groups.js`.
  `imgui.drawList` is a plain object of nine functions; each `Call` is
  named `drawList.<name>`, so its messages say `imgui.drawList.addLine():`
  and its `bindCalls()` key is the name the surface test and
  `checkGroupCoverage` already used.
- **The clipper** (`bind_clipper.cpp`): arguments, frame and the new guard
  `canListClipper:wrong-window` (the surface table's (b) precondition:
  `Begin()` ends an open row, `TableEndRow()` asserts the window,
  tables:2126; refused in `ui-imgui-bind-errors.js` CLIPPER_WRONG_WINDOW,
  added after TABLE_REGION, with its line in the ONCE chain), then
  `guard::snapshotScope()`, `ImGuiListClipper` on the stack, `Begin()`,
  and `Step()` in a loop calling the callback inside a `CallbackScope`.
  After each callback the scope is compared first: a mismatch abandons
  (`ItemsCount = -1; End()`, traced `listClipper: abandoned, the callback
  changed the window scope`) and propagates the callback's exception or
  throws `ERR_IMGUI_USAGE`; otherwise a pending exception ends the
  clipper normally and propagates. `Step()` is never called after either.
  `snapshotScope()` / `sameScope()` read `g.CurrentWindow`,
  `g.CurrentWindowStack.Size` and `g.CurrentTable`, already in the surface
  table's internal-state list.
- **Facts read from the source that shaped the tests.** In this ImGui
  `AddRect` and `AddCircle` stroke inside the outline by default
  (`ImDrawFlags_StrokeInside`, imgui_draw.cpp:2359, 2710), `AddLine`,
  paths and so `AddPolyline` and `AddBezierCubic` on it
  (`ImDrawFlags_StrokeCenter`, imgui_draw.cpp:1021, 1999), so each
  stroke's samples are placed accordingly, at pixel centres 3.5-4.5 units
  from both edges; centred strokes run on half-integer coordinates so that
  their 9 units cover whole pixels. The anti-aliasing flags were not
  touched: every sample is in a flat interior, and every one matched
  exactly with the default style. `addText` in the default font has
  pixels exactly the given colour inside its `calcTextSize` box, so TEXT
  asserts that and none outside. The histogram's bar n has the n-th
  value's height (PlotEx draws from `tp0`, imgui_widgets.cpp:9146-9175).
  The dark theme's opaque window background measured `[46, 46, 46]`; the
  tests compare with `getColorU32(Col_WindowBg)`'s bytes rather than a
  constant.
- **The scope cases check the cursor.** The first run of the "ItemsCount
  not set to -1" mutation passed: nothing observed `End()`'s seek. Each
  SCOPE_* case now also requires the current window's cursor after
  `listClipper` returns to be where the callback left it, which that
  mutation breaks (Mutations). The scope comparison without the table
  does not abort the Debug build; the returning variants of SCOPE_ENDTABLE
  and SCOPE_OPENTABLE fail on the missing throw.
- **No disagreement between the entries and the source.** Two notes on
  the binding's choices within them: `listClipper` rejects a callback that
  is not a function with `ERR_INVALID_ARG_TYPE` (a type rule, not a
  "Rejected" line); `itemHeight` NaN is refused by the float reader
  (`... must be a finite number ...`) before the "greater than 0" check.
- `ui-imgui-bind-args.js`: NOFRAME resolves `drawList.<name>` and has a
  sample for every new function; the three that need no frame are
  skipped there and called in EITHER (`styleColorsDark()`,
  `colorConvertFloat4ToU32()`); ARGTYPE gains `getColorU32`'s dispatch
  message and the Float32Array-or-Array and callback type checks.
- **Review fixes (Task 7).** `Call::floats` takes a maximum count, checked
  on the length of an Array or a Float32Array before anything is allocated
  or read, and refuses a detached Float32Array (Hermes reports its old
  length with null data, hermes_napi_typedarray.cpp:221-232) with
  `ERR_INVALID_ARG_VALUE`; the detached array comes from a new test hook,
  `__test.detachArrayBuffer()` (README), since this runtime has no
  `ArrayBuffer.prototype.transfer` or `structuredClone`, and
  `hermes-node-ui` takes neither `--vm=` nor `HERMES_NODE_VM_OPTIONS`
  (both tried: `unknown option '--vm=-Xhermes-internal-test-methods'`, and
  `HermesInternal.detachArrayBuffer` undefined with the variable set,
  where plain `hermes-node` defines it). The plot cap is 4096 values, not
  2^30: PlotEx's own float arithmetic breaks its plain assertion at
  widgets:9150 from 5921 values in a frame as wide (surface entry, and the
  by-hand row in Mutations); `graphSize[0]` is bounded below 2^31 for
  PlotEx's `(int)` of the width. `listClipper` requires itemHeight >= 1
  and count * itemHeight <= 2^30 (bind_clipper.cpp and the surface entry
  give the source lines), compares the window's `BeginCount` and the
  table's `InstanceCurrent` too (SCOPE_REBEGINWINDOW, SCOPE_REBEGINTABLE),
  and takes usage errors after `Begin()` and each `Step()`. The clipper
  test counts the abandon trace lines (ABANDON).
- **Found, not fixed (outside Task 7's groups):** `setNextItemWidth` and
  `pushItemWidth` accept any float, and ImGui's `CalcItemWidth` converts
  the item width with `IM_TRUNC` (`(int)`, imgui.cpp:12601), undefined at
  2^31 or more, for every widget sized by it (a plot with `graphSize[0]`
  0 included). Reported to the controller.
- Task 1's list of checks still needing test cases had no Style entry
  left (`Col_COUNT` / `StyleVar_COUNT` were Task 2's); `getColorU32`'s own
  `Col_COUNT` refusal is GETCOLOR's.

### Task 6 (review)

- **The table-region guard walks the recovery's table chain.** With a
  plain table nested in a scrolling table's cell, `g.CurrentTable` is the
  plain one, whose inner and outer window are both the region, so the
  first version (top table only) passed; `End()`'s recovery then ends the
  plain table, `EndTable()` makes current the table of the slot below
  because that slot was begun in the same window (tables:470, 1670-1671),
  and the loop (imgui.cpp:12019-12023) ends the scrolling table from
  inside `End()`. Reproduced first, as the review asked: all three closers
  aborted (Mutations). `inTableRegion()` now walks `g.TablesTempData`
  from `g.TablesTempDataStacked - 1` exactly as that loop would, and
  refuses at the first table in the run with a region of its own. The
  O(1) alternative (the parent window's table index) was not used: it
  answers for one specific shape, while the walk is the recovery's own
  sequence of reads and so holds at any depth and stops where the
  recovery stops. TABLE_REGION now covers one nested plain table under
  each closer, two under `endChild()`, and four accepted counterparts.
- **The wrong-window state with no row open is tested** (TABLE_WRONG_NOROW):
  each of `tableNextRow()`, `tableNextColumn()` (returning true) and
  `tableHeadersRow()` is accepted, and `tableSetColumnIndex(1)` is then
  accepted, which reads back that the call opened a row. No usage error
  is traced. It replaces the "measured by hand" statement.
- **`canTableSetupColumn:all-disabled`.** The review was right that the
  `TableColumnFlags_Disabled` variant of "every column disabled" can be
  read before the layout. It is guarded where it becomes inevitable, at
  the `tableSetupColumn` that would declare the last column with every
  column disabled: refusing a row function instead would not help, since
  `EndTable()` and the frame-end recovery run the same layout. A declared
  column's flags are read from the queued reconcile request when there is
  one (TABLE_DISABLED's second frame, and its mutation). Visibility from
  `DefaultHide` or settings stays in "Not covered" 3d, reworded.

### Task 8

- **The item width and four sizes are bounded below 2^31** (its own
  commit, before the docking group). ImGui converts them to `int` as they
  are, which is undefined from 2^31 up: the item width in
  `CalcItemWidth` (`IM_TRUNC`, imgui.cpp:12601; also a tab's width,
  widgets:10876, 1875, 1914), a window's size in `SetWindowSize`
  (imgui.cpp:9361, 9365), reached by `setNextWindowSize`, by
  `beginChild`'s size through `CalcItemSize` and `SetNextWindowSize`
  (imgui.cpp:12615-12622, 6961), and by `inputTextMultiline`'s size
  through its child window (widgets:4821, 4856). Only positive values are
  converted as they are; negatives become `max(1 or 4, avail + v)` first,
  so they are accepted at any magnitude (`Call::belowIntLimit`,
  `bind_util.h`). The surface entries say so. ITEMWIDTH
  (`ui-imgui-bind-layout.js`), SIZES (`-windows.js`) and MLSIZE
  (`-input-text.js`) reject 2^31 and 3e38, accept 2147483520 (the
  largest float below 2^31) and read back the item or window size drawn
  with it; ITEMWIDTH also draws a slider 1 wide with -3e38.
- **The survey of the other size and position arguments** found the same
  conversion only as a sum: `ItemSize`'s next-line cursor (imgui.cpp:
  12406-12407), `SetWindowPos` (9311, with a pivot 8426), the window's
  inner rectangles (8649-8652, 8716-8717) and every drawn text's position
  (imgui_draw.cpp:7353-7354). `setNextWindowPos`, `setCursorScreenPos`,
  `indent`, `sameLine`, `dummy` and `progressBar`'s size reach them, but
  no bound on the argument alone prevents it, so they are listed as "Not
  covered" 3f rather than bounded. `plotLines`/`plotHistogram`'s
  `graphSize[0]` was already bounded (Task 7). No sanitizer in the test
  builds reports float-to-int overflow, so the accepted cases show only
  that the call is made and draws, not that nothing downstream overflows.
- **The Docking and settings section is bound** (`bind_docking.cpp`,
  `registerDocking` in `bind_util.h`, `imgui_module.cpp`,
  `addons/imgui/CMakeLists.txt`; the `Docking` group in `bind-groups.js`).
  `dockSpaceOverViewport` reads the ID as `ImGuiID` (G3) and the flags
  against `DockNodeFlags_` (G1, which keeps out the private `DockSpace`
  and `CentralNode` bits). `loadIniSettingsFromMemory` passes the encoded
  length and needs a context, then no frame; `saveIniSettingsToMemory`
  returns ImGui's buffer as a string, inside or outside a frame.
- **Two guards, `canDockSpaceOverViewport:twice` and `:not-root`**
  (`bind_guards.cpp`; refused in `ui-imgui-bind-errors.js` DOCKSPACE_TWICE
  and DOCKSPACE_NOT_ROOT, with their lines in the ONCE chain). Both look
  the node up by the ID the call will use, and for a zero argument the
  guard computes the default as ImGui does: `ImHashStr("DockSpace", 0,
  ImHashStr("WindowOverViewport_11111111"))`, `0x08BD597D` (computed
  first in Python from the CRC32c table at imgui.cpp:2569-2584 and then
  read back as the return value; DOCKSPACE_TWICE's second frame fails
  if the guard's computation is wrong, Mutations). `:not-root` is
  stricter than ImGui where ImGui accepts the call and marks a split node
  as a dock space; the surface entry says why. The new internal reads are
  in the surface table's list.
- **`needNoFrame()` now traces its refusal** (`usage error: must be called
  outside a frame (before newFrame())`), as the design's errors test asks
  for `loadIniSettingsFromMemory` inside a frame (LOADINI_INFRAME, and
  INFRAME in the docking test). It had no caller before this task.
- **Disagreements with the brief, from measurement.** Loaded before the
  first frame, the two windows are docked in the first frame but do not
  tile until the second: the split lays out only nodes visible in the
  previous frame (imgui.cpp:20615-20618), and in the first frame the
  central node's window reads back the whole display. FIRST and REDOCK
  therefore assert `docked: true` in the first frame and wait for the
  tiling with `harness.until`, rather than asserting it in a fixed frame.
  The brief's "four out-of-frame functions" are tested as all ten
  functions that work outside a frame and need a context (NOWINDOW), with
  `dockSpaceOverViewport()` there too (ERR_IMGUI_NO_FRAME).
- **How the docked layout string was produced (for Task 9's default
  layout).** No window was docked by injected drags. Instead:
  1. Write the string from the format the handlers parse
     (`WindowSettingsHandler_ReadLine`, imgui.cpp:16807-16823;
     `DockSettingsHandler_ReadLine`, imgui.cpp:21883-21922): one
     `[Window][<name>]` section per window with `DockId=0x<node>,0`; then
     `[Docking][Data]`, a line `DockSpace ID=0x08BD597D Pos=0,0
     Size=<display w>,<h> Split=X` (or `Y`), and per child `  DockNode
     ID=0x<node> Parent=0x08BD597D SizeRef=<w>,<h>`, one of them with
     `CentralNode=1`. Node IDs are free (1, 2, ...); the dock space's must
     be the one `dockSpaceOverViewport()` returns (0x08BD597D for the main
     viewport, whatever the window size). Nested splits are a child line
     with its own `Split=` and children indented under it, each naming
     its parent.
  2. Run a scratch script under `HERMES_NODE_UI_TEST_HOOKS=1 python3
     utils/with-display.py 100 cmake-build-asan/bin/hermes-node-ui
     <script>` that opens the window through
     `test/fixtures/imgui/harness.js` (`harness.open()`, 640x480 here),
     calls `imgui.loadIniSettingsFromMemory(<string>)` before the first
     `newFrame()`, then runs a few frames of exactly what the program
     draws (the dock space first, then each window by name), printing
     each window's `getWindowPos()`/`getWindowSize()` and
     `__test.state().windows`, and finally
     `JSON.stringify(imgui.saveIniSettingsToMemory())`.
  3. Check the frames: every window `docked: true`, the rectangles
     tiling the dock space from the second frame. Paste the saved
     string, less the `[Window][Debug##Default]` section (ImGui's
     fallback window) and the `LastUsed=` lines (the date of the run).
     ImGui adds each window's `Pos`/`Size`/`Collapsed`, the host window
     `[Window][WindowOverViewport_11111111]`, `Window=0x1BBC0F80` (the
     host's ID) on the `DockSpace` line and `Selected=` (the tab ID) on
     each node; the hand-written form docks the same.
  A main menu bar moves the viewport's work area down by its height, so
  an application with one gets a different `Pos`/`Size` on the
  `DockSpace` line; the round trip in step 2, run with the menu bar
  drawn, gives the right ones.


### Task 9

- **The harness's `adopt()` never resolved** (its own commit, first). It
  called `resync()`, which learns the publication count from a capture
  request that gives up after a number of presented frames; the host
  counts those frames only as it presents them, so before the first
  publication the request never settles. An adopted handle now starts at
  `published` 0, as `open()`'s does, and `adopt()` must be called before
  the application draws (`driven: true`). `test/ui-imgui-adopt.js`.
- **A runtime defect, fixed in its own commit**: `process.emit()`
  (`libjs/process-events.js`) returned `true` for an event whose last
  listener had been removed (the emptied array stayed behind), and
  `process._fatalException` decides from that answer whether a listener
  took an uncaught exception. So after a `process.once('uncaughtException')`
  listener had handled one exception, every later uncaught exception was
  swallowed and the process exited 0; node v24.13.1 exits 1. Found by the
  THROW mutation, whose run hung instead of failing. Not specific to the
  UI: the same with two throwing timers under plain `hermes-node`.
  `test/test-process-emit-return.js`, its expected answers taken from node.
- **Runtime facts checked first** (2026-11-27, `cmake-build-asan`
  `hermes-node` against node v24.13.1, one script, identical output except
  where said): `require('buffer').isUtf8` exists and answers as Node does
  for valid UTF-8 with U+FFFD (true), `ff 41` (false), a truncated `e2 82`
  (false), empty (true), a NUL (true), a surrogate `ed a0 80` (false) and a
  `Uint8Array` (true). `fs.promises.open` / `FileHandle.stat` /
  `FileHandle.read(buffer, offset, length, position)` with explicit
  positions read the bytes at those positions (`ghij` at 6, `abc` into
  offset 1 from 0, 0 bytes past the end) and resolve with the same buffer;
  `close` works; `readdir(..., {withFileTypes: true})` gives `Dirent`s;
  missing and unreadable paths reject with `ENOENT` and `EACCES`.
  `performance` is undefined here (as the constraints say). And one
  divergence the Monitor shows: `process.memoryUsage()` reports
  `heapUsed`, `heapTotal`, `external` and `arrayBuffers` as 0 (only `rss`
  is real), so two of its plots are flat; the README says so.
- **No binding gap and no binding bug.** Every panel is written with the
  bound surface as it is; nothing was added to the binding.
- **The default layout** was produced as Task 8's notes describe: a
  hand-written string for the sketch's three columns of two, loaded
  before the first frame of a scratch run of the application itself
  (`createApp` driven, `harness.adopt`), every panel `docked: true` from
  frame 0, then `saveIniSettingsToMemory()` after six frames, less
  `[Window][Debug##Default]` and the `LastUsed=` lines. Loaded again and
  saved again, it came back identical. With the main menu bar the dock
  space is at `Pos=0,19 Size=1280,781`. The steps are in
  `examples/imgui-app/README.md`.
- **Departures from the brief, each the strongest true form.**
  - THROW's throwing frame is registered with `ui.requestAnimationFrame`
    directly, not run with `harness.rawTick`: `rawTick` catches what its
    function throws (it rejects with it), so the `uncaughtException`
    listener the brief asks to see the error would never be reached. The
    host's own path is what the case is about.
  - THROW selects the Widgets panel's Table tab first (`selectTab`, read
    back as `visibleTab`), because the clipper inside a table inside a
    child window lives in that tab; `throwNextFrame` alone would wait for
    a tab nobody shows. Selection by `TabItemFlags_SetSelected` takes
    effect a frame later (widgets:10910, `TabBarQueueFocus`).
  - The hex line is `APP opened <path> <bytes> hex=<rows>`; the design
    gave only the text form (`lines=`).
  - The design's test mode "selected by an environment variable" is the
    brief's `createApp({root, clock, driven})`; `driven` is `ctx.testMode`
    and turns the `APP` lines on.
  - RESET's floating layout is a window-only section for `Log` without a
    `DockId` (the shape Task 8's UNDOCK loaded safely), hand-written in
    the test, not a string `saveIniSettingsToMemory()` produced; the
    application itself loads only its saved `DEFAULT`. (Superseded by
    the review fixes below: that section clears the dock data, so all six
    panels float, and RESET now loads ImGui's own saved text.)
  - The literal `CallbackScope` mutation fails earlier than THROW (every
    frame's clipper leaves the depth raised), so a second mutation
    confined to the throw path shows THROW itself failing (Mutations).
  - `app.lastFrameMs` is added to the brief's app object: the JS time of
    the last whole frame, which the Monitor plots and the measurement
    reads.
- **The discarded frame's trace (THROW)**, pinned exactly: `Missing
  EndTable()`, `Missing EndChild()` twice (the scrolling table's inner
  child, then the panel's child), `Missing EndTabBar()`, `Missing PopID()`
  (the tab item's pushed ID, which recovery does not pop), then `frame
  discarded`; after the next frame, no usage error.
- **TINY**: under Xvfb the window manager grants 1x1 (`TINY size 1x1`;
  the host starts a 1x1 generation). Nothing throws, ImGui reports no
  usage error, and all six panels are docked again at 1280x800.
- **Not exercised by a test** (each looked at once in a capture, or not
  at all): the Open Folder and About modals, the View toggles, Dark /
  Light, the ImGui Demo and Metrics toggles, File > Quit through the menu
  (QUIT calls `app.quit()`), the 5000-entry cap and its "... N more" row,
  a symbolic link in the tree, the Log's filter and its follow-the-bottom
  rule, the canvas's drag, speed and pause. A capture of the whole window
  (scale 1, GL) was looked at by the implementer and every panel drew as
  designed; nobody has looked at it on the Mac.
- **Measurement** (spec goal 5; `test/fixtures/imgui/app-measure.js`,
  2026-11-27, Linux, Intel(R) Xeon(R) E-2288G CPU @ 3.70GHz, 16 threads,
  under Xvfb at scale 1; the application driven, all six panels open, the
  viewer showing `dir1/b.js` of `test/fixtures/imgui/app-tree`, the
  Widgets panel on its Basic tab; 600 frames after 10 warm-up; JS time is
  `app.frame()`'s own `process.hrtime.bigint()` measurement of its body,
  `newFrame()` to `render()` and the draw calls included; two runs each):

  | Build | JS time per frame, mean | max | binding calls per frame |
  |---|---|---|---|
  | `cmake-build-asan` (Debug, ASAN, UI on) | 12.196 ms, 12.158 ms | 198.6 ms, 197.4 ms | 254.1 |
  | `cmake-build-noasan` (Release, UI on) | 1.590 ms, 1.603 ms | 2.28 ms, 2.30 ms | 254.0 |

  The Debug maximum is one frame in about 63 (152 ms at frame 193 rising
  to 212 ms at frame 589 in a run that printed them); Release has no such
  frame. Its cause was not measured (it could be garbage collection while
  the heap grows; nothing here shows that).
  Not a leak: a Release run of 16000 frames had rss 147 MiB at the start,
  a peak of 172 MiB near frame 6000, and 152.5 MiB from frame 10000 to the
  end.
- **Review fixes (Task 8).** `setNextWindowPos` is bounded to -2^31 ..
  < 2^31 on both components (`Call::inIntRange`): with the default zero
  pivot Begin() hands the position to `SetWindowPos` as it is
  (imgui.cpp:8155-8169), which truncates it (9311), so the earlier
  survey's "only a sum" was true only for a non-zero pivot (8426).
  Re-reading the other sized and positioned arguments found one more
  converted as it is: `beginListBox`'s size (`ImTrunc`, widgets:8965),
  now bounded below 2^31. `setCursorScreenPos`, `dummy`, `sameLine`,
  `indent`/`unindent`, and the size of `button`, `invisibleButton`,
  `selectable`, `progressBar` and `colorButton` reach a conversion only
  as part of a sum; the surface table's "Not covered" 3f now has the
  table. `inputTextMultiline`'s and `beginListBox`'s children are begun
  with `fl(cursor + size) - cursor`, which can round past an accepted
  size; documented, not bounded lower, since the cursor is unbounded.
  Found by reading in that pass, not a conversion and not measured:
  `colorButton`'s alpha checkerboard is unclipped and emits about
  `(size.x / size.y) * 9` rectangles (3f). The `canDockSpaceOverViewport`
  comment now says the host's SkipItems makes both conditions stricter
  than ImGui, and the NOWINDOW run of the docking test checks its exit
  status. POSITION (`ui-imgui-bind-windows.js`) rejects 2^31, the float
  below -2^31 and +-3e38 on each component, and begins 32 x 32 windows at
  -2^31 and at 2147483520, reading the positions back; LBSIZE
  (`ui-imgui-bind-widgets.js`) rejects 2^31 and draws a list box
  2147483520 wide.


### Task 9, review fixes

- **Files clips** (Important). The visible tree is one array of rows
  (`flatten()` in `panels/files.js`), rebuilt only when a directory opens,
  closes or finishes loading, and drawn through `listClipper` with the
  row height `getTextLineHeightWithSpacing()` (every row is one line of
  text: a frameless tree node or a text item) and `indent()` per depth.
  The open state moved from ImGui's storage to the node (`node.open`,
  passed with `setNextItemOpen(..., Cond_Always)` on every drawn row),
  because a row the clipper skips is not drawn and ImGui would not keep
  its state. IDs, the click behaviour and the `APP` lines are unchanged.
  Checked by a temporary counter (reverted): 3 rebuilds in a whole
  measurement run, none during the 600 measured frames of either state.
  BIG in `ui-imgui-app.js`: one frame's binding calls with big/ (5001
  entries) open and with small/ (50) open differ by at most 60 and stay
  under 1500 (measured 2026-11-28 under ASAN, the test's `BIG calls per
  frame` line: big 331, small 327); with the clipper removed they were
  40242 against 636.
- **`app.rects` only when driven, and exactly what the last frame drew**:
  `recordRect` is a no-op unless `driven`, and the map is emptied at the
  start of every frame (COLLAPSE). Widgets records each tab under
  `tab:<name>`, a directory's "... N more" row under `more:<path>`.
- **Viewer**: truncation is decided by a 1-byte read at position CAP
  (GROWING, a handle whose `stat()` says 0); the lead-byte test is exactly
  0xC2..0xF4; new cases for every cut position of 2-, 3- and 4-byte
  characters, an invalid last byte, stray continuation bytes and a failed
  `stat()` (which still closes).
- **Reset Layout reopens every panel**, and RESET closes Canvas first.
  RESET's floating layout is now ImGui's own saved text (the test's header
  says how it was produced). Loading Task 8's window-only section for one
  window clears the dock data, so all six were floating in that save, not
  only Log as the first version of this note said: the first RESET
  checked only Log.
- **`harness.adopt()` refuses** an application whose `frameCount` is not 0
  (`ADOPT refused: ...`, exit 1); `app.frameCount` is new.
- **No test controls in Widgets but one**: `selectTab`/`visibleTab` are
  gone (THROW clicks the Table tab's recorded rectangle and waits for the
  tab's child window, `Widgets/system...`, to be active in
  `state().windows`); `throwNextFrame` stays, honoured only in test mode,
  since nothing a test can inject makes application code throw.
- **Monitor**: rss is plotted; the heap figures only once the runtime
  reports them, with a one-line note otherwise.
- **For Task 10 to file in the tracker** (runtime limitations, not fixed
  here): (1) `process.memoryUsage()` returns `heapUsed`, `heapTotal`,
  `external` and `arrayBuffers` as 0 (Node returns V8's figures; only `rss`
  is real here). (2) An unhandled rejection whose reason is an `Error` is
  reported wrapped in `UnhandledPromiseRejection`, where Node reports the
  Error itself: the vendored `isErrorLike()`
  (`libjs-node/internal/process/promises.js`) requires an own `stack`
  property, and Hermes defines `stack` as an accessor on
  `Error.prototype`. (3) Unhandled rejections are processed late: with a
  `process.once('unhandledRejection')` listener, `Promise.reject()` at
  start-up and a 200 ms timer, the timer ran before the listener; Node
  runs the listener first. Both are recorded in
  `test/test-process-emit-return.js`'s header and not asserted there.
- **Measurement again** (2026-11-28, same machine: Intel(R) Xeon(R)
  E-2288G CPU @ 3.70GHz, 16 threads, Xvfb, scale 1). The script now builds
  its own root (`b.js`, shown in the viewer, and `big/` with 5000 files)
  and measures two states, 600 frames each after 10 warm-up: `default`
  (big/ collapsed) and `big` (big/ opened by a click on its row, listing
  loaded, the panel at its top). The numbers include the two calls per
  drawn row that recording `app.rects` costs in driven mode. Two runs
  each:

  | Build | State | JS time per frame, mean | max | binding calls per frame |
  |---|---|---|---|---|
  | `cmake-build-asan` | default | 12.454, 12.552 ms | 207.2, 225.1 ms | 250.1 |
  | `cmake-build-asan` | big | 23.358, 24.058 ms | 429.6, 463.3 ms | 402.1 |
  | `cmake-build-noasan` | default | 1.173, 1.383 ms | 2.01, 2.10 ms | 250.0 |
  | `cmake-build-noasan` | big | 1.534, 1.528 ms | 2.63, 2.20 ms | 402.0 |

  The extra 152 calls are the panel filling with file rows (about 20
  visible rows of 8 calls) where the default view has 2. Under ASAN the
  mean nearly doubles with big/ open and the occasional slow frame
  doubles too, although the row list is not rebuilt (above). The cause of
  the ASAN increase is not established: nothing measured garbage
  collection, and the per-call cost does not account for it either way
  (the default view's ~0.05 ms per call under ASAN predicts about +7.6 ms
  for the 152 extra calls, against about +11 ms measured; in Release about
  +0.8 ms is predicted against about +0.25 ms measured). The Release cost
  of the open directory is about 0.2-0.4 ms a frame. (Corrected in Task 10:
  this note first said the increase had the shape of garbage collection
  over a larger live heap.)

### Task 10

- **Documentation.** `addons/imgui/README.md` describes the binding as
  built: the frame protocol (part A's "JS surface", kept where still
  true), the calling convention as `bind_util.cpp` reads it, the
  exceptions, the error codes, the functions that work outside a frame,
  the surface by group (generated from `bind-groups.js`, named there as
  the authority), the audited recoverable errors, every guard condition
  `bind_guards.cpp` registers, the limits, the trace lines and test hooks
  the binding added (`usage error:`, `listClipper: abandoned`;
  `bindCalls()`, `guards()`, `state().windows`), the tests, "Verified, and
  not", and the `imgui_internal.h` audit list under "Bumping ImGui". The
  audit list adds what the surface table's "Internal state read by
  bind_guards.cpp" omits: the test hook's reads of each window's
  `Active`, `WasActive` and `DockIsActive` and of `g.WithinFrameScope`.
  `CLAUDE.md` gains a paragraph on the binding and `examples/imgui-app/`
  in the list of examples without npm packages. The README's example was
  run (a copy capped at 30 frames: `FRAMES 30 drew=true show=true`).
- **Tracker**, filed 2026-11-28: 01a21f89-a69d (preconditions not
  prevented, pointing to the surface table's "Not covered"),
  01a21f89-a6f1 (behaviours no test detects), 01a21f89-a743 (the draw
  list's clip rectangle functions, left to part C), 01a21f89-a794
  (`process.memoryUsage()` heap fields 0), 01a21f89-a7e7 (`hermes-node-ui`
  and VM options), 01a21f89-a836 (the two unhandled-rejection differences
  from node), 01a21f89-a889 (Linux/GL at scale 1 only; the Mac round).
  Each was measured on 2026-11-28 where it states a behaviour. Found while
  measuring 01a21f89-a7e7, beyond what Task 9 recorded: a container built
  with `--vm=-enable-eval=false` (locked) runs with eval **on** under
  `hermes-node-ui --bundle`, its baked options ignored. Part A's
  01a21c59-bd05 and 01a21c59-bd54 are unchanged: nothing in part B
  touched the renderer paths the first lists, or packaging.
- **Test headers reconciled with the Mutations table** (comments only):
  `ui-imgui-bind-layout.js` now points to the table, as
  `-widgets.js` does; `-menus.js` says the closer mutations kill the run
  before CLOSERS; `-popups.js` (the empty-name check: `NOTHROW`, no abort),
  `-plots.js` and `-tables.js` (the coverage mutation fails REJECTED's
  `NOTCALLED` first), `-args.js` (the first "not called" check),
  `-errors.js` (the abort is in the first `newFrame()`), `ui-imgui-app.js`
  (no `THROW ok`, not PANELS) and `imgui-app-viewer.js` (the two stat.size
  mutations fail CAP and GROWING) say what was measured. Long comment
  lines wrapped in `bind-groups.js`, `ui-imgui-bind-constants.js` and this
  file's Task 1 notes.
- **The int half of the format bound** is now a recorded mutation (the
  Mutations table, Task 10), so it is not added to "Behaviours no test
  detects".
- **The full suite**, 2026-11-28, one configuration at a time:

  | Build | Command | JS (lit) | Unit |
  |---|---|---|---|
  | `cmake-build-asan` (Debug, ASAN, UI on) | `cmake --build cmake-build-asan --target check-hermes-node` | 270 passed, 10 unsupported, 0 failed | 460 passed |
  | `cmake-build-noasan` (Release, UI on) | `cmake --build cmake-build-noasan`, then `--target check-hermes-node` (the whole suite) | 269 passed, 11 unsupported, 0 failed | 460 passed |
  | `cmake-build-asan-noui` (Debug, ASAN, UI off) | `cmake --build cmake-build-asan-noui --target check-hermes-node` | 200 passed, 80 unsupported, 0 failed | 336 passed |

  No flake appeared, so nothing was rerun. With the UI off every binding
  test reports UNSUPPORTED except `imgui-app-viewer.js`, which needs no
  window and passes (checked by running every `imgui-*.js` and
  `ui-imgui-*.js` there: 1 passed, 33 unsupported). After the suite run in `cmake-build-asan`, the
  twelve tests whose comments this task edited were run again there
  (12 passed).

### Task 10, review fixes

- README: the Limits section says which code each limit throws (a format
  too long is `ERR_INVALID_ARG_VALUE`, the rest `ERR_OUT_OF_RANGE`); the
  slider bounds are stated as the two one-sided checks they are; a vector
  with too few elements is in the `ERR_OUT_OF_RANGE` row; the error
  callback is installed once, at context creation, and `newFrame()`
  re-sets only the flags and the combo depth; the audit list names
  `__test.tableScope()`'s reads and `ImGuiWindowStackData::Window`.
  `CLAUDE.md`'s verification bullet now says it is part A's.
- 01a21f89-a7e7's container command gained its entry script and was
  re-run as written: eval off under `hermes-node --bundle`, on under
  `hermes-node-ui --bundle`, as the issue says.
- **Files labels** (`examples/imgui-app/panels/files.js`, `treeRow`): a
  control character in a name is shown as `?`, so a newline cannot make a
  row two lines tall and throw the clipper's uniform height off; a name
  holding `##`, which ImGui takes as the end of the displayed label
  (FindRenderedTextEnd, imgui.cpp:4037-4046), is drawn as a node labelled
  `##row` followed by `sameLine(0, 0)` and `text()` of the name, which
  ImGui shows whole (TextEx does not hide after `##`). The row's ID was
  already `pushID(node.path)`. Only such names take the second path, so
  the measured call counts of ordinary trees are unchanged. NAMES in
  `ui-imgui-app.js` (the root now lists 10 entries, `odd/` added): both
  names one-line rows, evenly spaced, the `##` name's text as wide as
  `calcTextSize()` of the whole name, each opening when clicked.
- The two measurement notes no longer attribute the ASAN slow frames or
  the ASAN increase with big/ open to garbage collection: nothing measured
  it, and the per-call arithmetic does not account for the increase.
- The application README notes the indent by text line height (no style
  accessor is bound) and the label handling.
- Suite after these fixes, `cmake --build cmake-build-asan --target
  check-hermes-node` (2026-11-28): JS 270 passed, 10 unsupported, 0
  failed; unit 460 passed.

### Final review

The whole-branch review's ten findings (F1-F10), each checked against
the code and ImGui's source before it was changed.

- **F1, the error outputs turned back on** (Important). Confirmed:
  `imgui_demo.cpp:626-627`, inside Configuration > Configuration, sets
  `ConfigErrorRecoveryEnableAssert`, `...DebugLog` and `...Tooltip` all
  true whenever all three are false, every frame; `ErrorLog()` returns the
  assert flag (imgui.cpp:12141), so `IM_ASSERT_USER_ERROR` then asserts.
  The flags were set only at context creation and in `guard::beginFrame()`.
  Now `guard::restoreRecoveryFlags()` sets them before every call into
  ImGui that can report an error: in `Call::enterImGui()` (so `render()`'s
  `Render()` too), before the discard path's `EndFrame()`, and in
  `CallbackScope`'s destructor, since the list clipper calls `Step()` and
  `End()` after a callback without another `enterImGui()`. It does nothing
  with no context (`colorConvertFloat4ToU32()` reaches `enterImGui()`
  without one). The demo's node cannot be opened from the binding, so the
  test uses a new hook, `__test.setErrorOutputs(on)`, which does what the
  demo does and returns what was in force: `test/ui-imgui-bind-recovery.js`
  (CALL, RENDER, DISCARD, CLIPPER). Release cannot see DISCARD's mutation
  (its read-back follows `NewFrame()`, which re-asserts anyway, and a
  Release assert is a no-op); the Debug build the suite runs aborts. The
  stdout half stays as Task 1's "Behaviours no test detects" note says:
  ImGui's `printf` is lost to the stdio buffer at `_exit()`, so the test
  reads the flags back instead.
- **F2, the clipper outside its table's window** (Important). Confirmed
  by reading (the seek sets `table->RowPosY2` from the current window's
  cursor whether or not a row is open, imgui.cpp:3461-3472) and by running
  the finding's sequence on the unfixed build: `endTable()` refused as
  `cursor-moved`, then the frame-end recovery's `EndTable()` aborted at
  tables:1493. `canListClipper()` now refuses whenever a table is current
  and the current window is not its inner window. Checked for a valid use
  it refuses: none. A window's first Begin() in a frame resets its
  `DC.CurrentTableIdx` (imgui.cpp:8228), so a table is current in a window
  other than its inner one only when the outer window of a scrolling table
  is appended to again; a child, popup or other window begun in a cell has
  no table current, and a plain table's inner window is its outer one.
  CLIPPER_WRONG_NOROW in `ui-imgui-bind-errors.js`; the message no longer
  says "inside a table row", and the `bind_guards.h` comment no longer says
  the later seeks are covered by the scope comparison (they are covered
  because a state that passes the guard is not one the clipper changes).
- **F4, `beginTable`'s sizes** (Minor). Confirmed: `ImTrunc(CalcItemSize(
  outer_size, ...))` (tables:342), and `inner_width` with ScrollX reaches
  `SetNextWindowContentSize`'s `ImTrunc` (tables:419-422,
  imgui.cpp:9469). Both `outerSize` components and `innerWidth` are now
  bounded below 2^31 with `Call::belowIntLimit`. `innerWidth` is bounded
  whether or not ScrollX is set (without it the value is stored and not
  used; the stricter rule is the simpler one to state). The cursor plus the
  outer size (tables:343) is a sum and stays unbounded, as for the other
  sized items. A ScrollX table 2147483520 wide with that inner width is
  drawn without an assertion (REJECTED's accepted edge).
- **F5, a format's width and precision** (Minor). Confirmed by reading:
  the validator skipped any number of digits. Now each is at most two
  digits, which is ImGui's own ceiling on a precision
  (`ImParseFormatPrecision` falls back to its default above 99,
  widgets:3772); it also keeps that function's `ImAtoi<int>` from the
  signed overflow ten digits would cause (`'%.99999999999f'` fits in 30
  bytes). A `0` right after `%` is a flag, so `'%099d'` is width 99.
  FORMATLEN's float candidates had their length from a 27-digit
  precision, which the cap now refuses before the length check; they are
  now long by repeated `0` flags, and the length mutation was re-run to
  show they still exercise the 30-byte copy. The reviewer's "seconds a
  call" was not re-measured here.
- **F6, `setKeyboardFocusHere`'s offset** (Minor). Confirmed:
  `g.NavTabbingCounter = offset + 1` (imgui.cpp:9712); the counter is only
  decremented elsewhere (imgui.cpp:14496), so `INT32_MAX - 1` is the bound.
- **F8, the clipper's height bound** (Minor). Confirmed: `Step()`'s
  quotient (imgui.cpp:3692-3693) has the cursor in it, and the cursor is
  unbounded. The `kMinItemHeight` comment and the surface entry now say
  the bound holds only for a cursor the layout placed; 3f lists the
  clipper among the sums, with the two `beginTable` rows.
  (The first suite run before this commit failed one unit test,
  `SurfaceTableTest.DisplacementRacesCompletionUnderLoad`, tracker issue
  01a0d7d4-b666, a wall-clock throughput floor that fails under load: 6/6
  passing alone, and the whole suite passed when rerun.)
- **F7, call counting under the test hooks only** (Minor). Confirmed:
  `++gCallCounts[name_]` (a `std::string` built from the name and a
  `std::map` lookup) ran on every binding call. It is now gated on
  `testHooksEnabled()`, which reads the environment once; it stays
  immediately before the ImGui call. Nothing observable changes with the
  hooks on, so there is no RED; the coverage tests pass, and the gate
  inverted (counting only without the hooks) fails them: Mutations.
  **The measurement was re-run** (2026-11-28, the same machine, Intel(R)
  Xeon(R) E-2288G CPU @ 3.70GHz, Xvfb, scale 1; `test/fixtures/imgui/
  app-measure.js` by hand under `utils/with-display.py 300`, exactly as
  Task 9's review notes ran it, `cmake-build-noasan` rebuilt first, two
  runs each, nothing else building or running lit). The earlier numbers
  are the Task 9 review's table above; this build also has F1-F6's
  changes, `restoreRecoveryFlags()` in every `enterImGui()` among them.

  | Build | State | JS time per frame, mean: before / after | max: before / after | calls per frame: before / after |
  |---|---|---|---|---|
  | `cmake-build-asan` | default | 12.454, 12.552 / 12.935, 12.919 ms | 207.2, 225.1 / 213.6, 214.6 ms | 250.1 / 250.1 |
  | `cmake-build-asan` | big | 23.358, 24.058 / 24.442, 23.812 ms | 429.6, 463.3 / 425.9, 421.6 ms | 402.1 / 402.1 |
  | `cmake-build-noasan` | default | 1.173, 1.383 / 1.182, 1.178 ms | 2.01, 2.10 / 2.22, 1.92 ms | 250.0 / 250.0 |
  | `cmake-build-noasan` | big | 1.534, 1.528 / 1.262, 1.562 ms | 2.63, 2.20 / 2.45, 2.77 ms | 402.0 / 402.0 |

  Plainly: the call counts did not change. Release times are within the
  spread of the earlier runs. The ASAN default mean is about 0.4 ms (3%)
  higher in both runs, and the ASAN big mean within the earlier spread;
  the cause of the ASAN difference was not measured. **This measurement
  cannot show F7's change**: the script reads `__test.bindCalls()`, so it
  runs with `HERMES_NODE_UI_TEST_HOOKS=1`, under which the counting still
  happens. A run without the hooks has no way to count calls, and the
  harness needs the hooks too; the saving, if any, is in runs nobody here
  measures.
- **F3, the viewer opening anything** (Important). Confirmed: `kindOf`
  in `panels/files.js` made every non-directory, non-link entry a 'file',
  and `readCapped` opened with no type check. Measured with the unfixed
  viewer in the window test: a click on a link to a FIFO left
  `app.idle()` unresolved (`TIMEOUT idle SPECIAL link`). Now the viewer
  stats the path (through the injected `fs`, following a link) before
  opening, and anything but a regular file is the error
  `NOT_A_REGULAR_FILE`, its message naming the kind ("not a regular file:
  a FIFO"), logged, and an `APP error` line when driven; a dangling link
  is the stat's own ENOENT. The tree labels a FIFO, socket or device from
  its Dirent (`pipe (fifo)`, kind 'special') and does not offer it; an
  entry whose Dirent reports no type stays a 'file', since the viewer's
  stat refuses it anyway, so a filesystem without `d_type` keeps its
  files clickable. **Choices**: the code is the application's own name,
  not an errno, so it cannot be mistaken for one; a path replaced by a
  FIFO between the stat and the open is not caught (opening with
  `O_NONBLOCK` and checking the handle's stat would close that, at the
  cost of an `fs` interface beyond the one the finding prescribes).
  `imgui-app-viewer.js` NOT_REGULAR (a fake fs for FIFO, character and
  block devices, socket and directory: no open() recorded; a rejecting
  stat; a regular file still opened; the real `/dev/null`); the fakes of
  GROWING and SUPERSEDE gained a `stat`, and SUPERSEDE's hand-settled
  step moved from open() to stat(), the chain's first call now.
  `ui-imgui-app.js` SPECIAL, with a real FIFO from `mkfifo` (via
  `child_process.spawnSync`; `SKIP fifo` without one); the root now lists
  11 entries.
- **F10, the Files listing is read once** (Minor). The application
  README and the `files.js` header now say so: collapsing and expanding
  does not re-read; Open Folder starts over.
- **Observed, not changed**: `imgui-app-viewer.js` run by hand under
  `cmake-build-asan/bin/hermes-node` spends minutes of CPU in CLASSIFY
  (timed out at 150 s), where under lit the whole test takes about 20 s;
  the unmodified test from before this fix behaves the same, so it is
  not this change. Not investigated.
- **F9, documentation.** The README said "Four rows ... cannot be
  reached" and listed three, counting the draw list's two flag rows as
  one, and also named `endCombo` under "Raised by a call", which its guard
  makes unreachable: the list now names the two draw-list rows and
  `endCombo` is only in the unreachable list. The Task 5 note on
  `__test.setTooltipDelay()` now says it returns both previous delays.
- Each commit of this round ran `./utils/format.sh -f` and then
  `cmake --build cmake-build-asan --target check-hermes-node`, green
  (JS 271 passed, 10 unsupported; unit 460) before it was made.

### Codex code review, round 1

Codex's findings, verbatim, are in
`.superpowers/sdd/2026-11-27-imgui-binding/codex-code-r1.md`; the ruling
on each in `codex-findings.md` beside it. Mutations are the rows tagged
"codex".

- **C1 (style variable values)** verified and reproduced by hand before
  the fix, on `cmake-build-asan`: Codex's sequence aborts at
  `imgui_widgets.cpp:989`. Every exported variable now has a domain
  (`styleVarInfo()` in `bind_style.cpp`, with the source lines of each):
  0..1 for `Alpha`, `DisabledAlpha` and four alignments; 1..limit for
  `WindowMinSize`; 0..limit for `ScrollbarSize` (the assertion),
  `ScrollbarPadding` and the four window/frame border sizes (a negative one
  grows the scrollbar's rectangle, which can reach ScrollbarEx's plain
  assertion, widgets:1060-1064); the length limit either way for the rest;
  the two angled-header variables unbounded (read only by the unbound
  `TableAngledHeadersRow()`). A pushed value cannot reach the next
  `NewFrame()`'s style assertions: the frame-end recovery pops it first
  (imgui.cpp:6432-6433), and `pushStyleVar` needs a frame. The check
  applies only to the overload of the variable's own type; the other is
  ImGui's recoverable error, which pushes nothing (MISMATCH unchanged).
- **The length limit** (`lengthLimit()`, `bind_util.cpp`), shared by C1
  and C4: 2^20 screen pixels at the window's pixel density, at most 2^20
  units. The binding reads the density from the current window's draw list
  (`guard::pixelDensity()`, `_InvFringeScale`, an "[internal]" member of
  `imgui.h`), so the bound is exact rather than assuming a framebuffer
  scale; the alternative considered, clamping the framebuffer scale in the
  platform half so the bound could be a constant, was not taken because it
  would change rendering.
- **C2 (colour editors)** verified by reading, and the conversions
  demonstrated with ImGui's own expressions in a standalone program built
  with `-fsanitize=float-cast-overflow` (`IM_F32_TO_INT8_UNBOUND(1e10)`:
  "2.55e+12 is outside the range of representable values of type 'int'").
  On x86-64 without the sanitizer the conversion is silent (cvttss2si's
  0x80000000), which is why the window test can only show the refusal.
  The domain depends on the input mode: RGB 0..2^23 (non-negative, because
  the HSV display, selectable from the editor's own options menu during
  the call, divides by the largest component); HSV -2048..2048, so that
  the HSV-to-RGB products stay below 2^23. HDR values above 1 stay
  accepted.
- **C3 (column widths)** verified by reading (tables:1689, 1103). Bound
  below 2^31 whatever the flags, since with neither width flag the table's
  sizing policy decides (tables:791-798). A stretch weight is converted
  only through sums and products with content widths: 3f.
- **C4 (draw-list radii and thicknesses)** verified by reading and by the
  same standalone program: the fallback segment formula at a radius of
  4e8 (below `INT_MAX`) converts `inf`. Radius, thickness and rounding are
  bounded by the length limit, derived from the segment count's fallback
  (safe to `e * 2^24` = 5033164 screen pixels at e = 0.30) and the
  centre-biased offset's cast.
- **The sweep** (the table is "Not covered" 3f's, now every numeric
  argument): beyond Codex's four, three conversions of arguments as they
  are were found and bounded: `addRect`'s and `addRectFilled`'s corners
  (`IM_IS_TRUNCATED4`, a plain `(int)` on ARM and in a Debug build),
  `addText`'s position (`IM_TRUNC`), and a histogram's values and scales
  (NaN corners from an overflowing scale, then the same conversion).
  Classified "sum" and left in 3f: a stretch weight, `tableNextRow`'s
  `minRowHeight`, the colour picker's grab positions. A new "Not covered"
  item 3h (a dock splitter with `DockingSeparatorSize` above the node's
  size), from reading, not measured; the tracker issue gained it.
- Every new case's RED was recorded by running the new tests against the
  unfixed addon (the fix set aside and the addon rebuilt), then GREEN.
- **C5 (dragInt)** verified by reading and reproduced before the fix:
  `dragInt('n', [0], 1e10)` dragged 100 units right ended at 0, the
  accumulator's `(int)` giving INT_MIN on x86-64 twice and the sum
  wrapping (undefined behaviour twice over). Fixed by a domain, option
  (a) of the ruling: the platform half clamps mouse coordinates (and wheel
  deltas, which become a scroll target converted to an `ImS64`,
  imgui.cpp:12815-12817; found in the same reading) to +-2^16, a frame's
  delta is then at most 2^17; the value and the bounds are within
  +-(2^30 - 1) (so `v_max - v_min + 1` fits, for wrapping); the speed,
  given or automatic, within +-800, so the accumulator stays below
  1 + 10 * 2^17 * 800 < 2^30. The bound on the bounds is applied whatever
  the flags (the alternative, applying it only when ImGui computes the
  range, was not taken: simpler, and the same rule as `sliderInt`'s).
  `dragFloat` is float arithmetic only; `sliderInt`'s and `sliderFloat`'s
  bounds are ImGui's asserted ones (Task 4) and the value is clamped;
  `inputInt`'s steps saturate (`ImAddClampOverflow`). Not covered, from
  reading: a keyboard-driven drag (3i), whose delta is bounded by the
  frame's length, and typed text (3j, `sscanf`). The colour editors' own
  `DragInt` on `255 * component` keeps its headroom under the same mouse
  bound. `test/ui-imgui-input.js` and part A's other input tests pass with
  the clamp.
- The first full-suite run with DRAGDOMAIN failed it once, under the
  16-way load: `TIMEOUT DRAGDOMAIN far move` in its second drag, which
  presses where the first one did. The cause taken (not proved): a press
  within ImGui's double-click time of the last one opens a drag's text
  input instead of dragging, and ImGui's time advances with the real
  clock. The case now sets the double-click time to 1 us for its drags
  (`__test.setDoubleClickTime`), as `ui-imgui-bind-layout.js` DOUBLE
  widens it; two more parallel runs and the full suite then passed.
- **C6 (the clipper's table by address)** verified by reading: `g.Tables`
  is an `ImPool` whose storage is one `ImVector`, and `BeginTable` of a
  new ID adds to it (imgui_tables.cpp:358; imgui_internal.h:811-830), so
  the outer table's address changes while its index does not, and
  `EndTable` restores the outer table by index (imgui_tables.cpp:1671).
  Reproduced by POOLGROW with the old comparison: 9 nested tables grew the
  capacity from 8 to 18 and the clipper was abandoned. The snapshot now
  keeps the table's pool index and ID (with its instance, as before). The
  test reads the growth back through a new field of `__test.tableScope()`,
  `poolCapacity`. **Audit of the other ImGui pointers the addon keeps
  across a call that can run JavaScript**: the snapshot's window address
  is stable (each window is its own allocation, imgui.cpp:7153, freed only
  at shutdown, 4744); the clipper's `TempData` points into
  `g.ClipperTempData`, an `ImVector` a nested clipper can grow, and
  `ImGuiListClipper::End()` re-points the outer clipper's `TempData` when
  the nested one ends (imgui.cpp:3526-3530), and the binding ends every
  clipper it began before returning; the guards and the test hooks hold
  ImGui pointers only within one call, with no JavaScript in between. No
  other case was found.
- **C7 (Log rows)** verified and reproduced by LOGROWS before the fix: an
  entry holding a line break was 26 units tall against a 13-unit line.
  `display-text.js` (new) holds the control-character replacement the
  Files tree used; the Log applies it in `add()`, and records each drawn
  row (`log:<index>`, test mode only) so the test reads the geometry back
  at the bottom and after a wheel step up.
- **C8 (UTF-8 trim)** verified and reproduced by the new CLASSIFY cases
  before the fix (all five invalid suffixes classified as text). The trim
  now also requires the second byte to be in the range its lead allows
  (RFC 3629 section 4); four valid splits at those ranges' edges are
  covered too.
- **L1** the README's recovery paragraph states the cost (a context test
  and four `bool` stores per binding call); **L2** the application README
  says the stat-then-open window is not closed and what closing it would
  need; **L3** `readCapped`'s comment is back above `readCapped`, and
  `kindOf()`'s fallback reads "an unknown kind of file".
- **Coordinator's ruling after the fix wave: values written back are
  clamped** (rule G6). dragInt clamps to +-(2^30 - 1) and the colour
  editors to their input mode's domain before writing back; user
  interaction never leads to a refusal. The speed bound is now the exact
  819 (1 + 10 * 2^17 * 819 = 1073479681 < 2^30), and an automatic speed
  above it is passed as 819 instead of refusing the range (a wide range
  drags at 819 per unit). DRAGDOMAIN now checks the clamped write-back
  (value at the bound, the next calls accepted, a second drag leaves it
  there; an HDR red component dragged below 0 ends at 0) and the capped
  speed's exact float product. The input test's CLAMP checks that a
  negative coordinate inside the bound reaches ImGui unchanged.

### Codex code review, round 2

Codex's text: `.superpowers/sdd/2026-11-27-imgui-binding/codex-code-r2.md`.

- **D1 (saved hue)** verified by reading (widgets:5902-5917, 5978, 5980,
  6124-6128) and reproduced: with the forgetting removed, SAVEDHUE reads
  hue 1e10 still held after the next call, so ImGui restored and
  converted it. Fixed by forgetting a saved state outside 0 .. 1
  (`SavedID = 0`) before each editor call; in-domain state (everything an
  ordinary drag or the picker saves) is kept, and SAVEDHUE shows hue 0.5
  kept. Invalidation was chosen over clamping: it fabricates no hue. The
  picker's nested editors produce and consume such a value inside one
  ImGui call, which the binding cannot get between: "Not covered" 3k.
- **D2 (colorButton NaN)** verified by reading (imgui.cpp:3046-3055:
  a negative hue gives f < 0 and `s * (1 - f)` overflows; times v = 0,
  NaN). The sweep row's "never NaN" was wrong (it assumed f >= 0); the
  row is split and corrected. colorButton now takes the editors' domain
  (one helper). The other float-colour arguments were re-derived: they
  meet only finite factors and saturating packing, so they overflow to
  infinity at worst, never NaN.
- **D3 (picker tessellation)** reproduced by hand: Codex's
  [4, 100000] with the hue-wheel picker open drew 1106303 vertices, arcs
  of about 91,700 segments, each one unsplit primitive past 16-bit
  indices; no abort (Render()'s plain check at draw:3554 sees only the
  vertex count after the last offset move), so wrapped indices and the
  work were the observable harm. The renderer uses 16-bit indices and
  sets `RendererHasVtxOffset`. Fixed by FramePadding.y <=
  floor((32767 - font size) / 2) and ItemInnerSpacing.x >= -2^14, derived
  from the wheel's explicit segment count; the font size is read, and is
  not settable through the binding. The sweep of every primitive a style
  length scales is the table in the surface entry of `pushStyleVar`;
  nothing else needed a bound. PICKER draws the open picker at the bound.
- **D4 (float write-back)** verified (widgets:2367, 2631; bind_util's
  f32 check). `Call::write(RefFloat&)` now writes +-FLT_MAX for an
  infinity and the element's previous value for a NaN, for every
  float-writing function; each accepts any finite value, and the colour
  editors clamp to their domain first. One retained value re-produced the
  overflow: the drag accumulator left at -inf, which flipped the value to
  -FLT_MAX at the next move (FLOATWB, the reset removed). The binding now
  resets a non-finite accumulator before each dragFloat (a new write to
  ImGui's internal state, listed). The README's "refused at the next
  call" sentence for the colour editors is corrected (in the D1/D2
  commit); the earlier progress notes that say so are history, superseded
  by the write-back ruling.

### Codex code review, round 3

- **E1 (accumulator carried across a type change)** verified (widgets:
  2648 keeps a delta too small to change the float whole; 2700-2727
  dispatch on the new type; 2631 converts) and reproduced by TRANSITION
  with the fix removed: the integer became -1073741823 (INT_MIN clamped).
  Fixed by resetting `g.DragCurrentAccum` to 0 before dragInt and the
  colour editors when its magnitude is 1 or more: the integer path never
  leaves 1 or more between calls, so the rule is exactly "a float drag's
  remainder". The colour editors' internal DragInt/DragFloat run inside
  one ImGui call, but their type can change only between calls (the
  flags, or the options menu, which a held left drag cannot reach), so the
  reset before the call covers them. The siblings are "Not covered" 3l,
  each read and none needing a reset. The first run of TRANSITION failed
  on a double click (it presses where FLOATWB did); it sets the 1 us
  double-click time as DRAGDOMAIN does.
- **E2** 3k reworded: no single threshold, the component conversion
  (8421505 or more as converted) and the hue-bar cursor (geometry
  dependent, about 1.0e7 at the default picker) stated apart; the tracker
  issue's line too.

### Codex code review, round 4

- **F1** verified (widgets:2716-2717: an inactive item never reads the
  accumulator) and reproduced by SHAREDACCUM under the round-3 rule: a
  float drag at 16777216 beside an unrelated dragInt ended 10 units short.
- **F2** verified: the round-3 note above ("the integer path never leaves
  1 or more between calls") holds for the linear path only; a
  logarithmic integer drag keeps a parametric remainder above 1
  (LOGDRAG). The grab offset and "finite or NaN" statements of 3l were
  wrong too, and are corrected.
- **The rule now**: each accumulator write is conditioned on the drag
  about to run being the active item (its `ImGui::GetID(label)` equals
  `g.ActiveId`) and on what its own consuming path can take: a float or
  logarithmic integer drag any finite value, a linear integer drag below
  1. No type memory: each condition is the consuming path's requirement
  and is inert for that path's own remainders, so it is sufficient
  whatever type left the value, and remembering the type would add
  nothing (the coordinator's rule 3 alone, shown sufficient; rule 2 not
  implemented, with its mutation therefore not applicable). The colour
  editors write nothing: their component IDs are theirs alone and both of
  their drag types leave below 1 within the colour domain; COLORMODE
  switches Float to Uint8 under a held drag.

