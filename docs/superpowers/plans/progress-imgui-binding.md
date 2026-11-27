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
| Task 4 | Widgets | |
| Task 5 | Trees, tabs, menus, popups and tooltips | |
| Task 6 | Tables | |
| Task 7 | Style, plots, draw list, list clipper | |
| Task 8 | Docking and settings | |
| Task 9 | The application | |
| Task 10 | Documentation and the full suite | |

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
| 3 | `ui-imgui-bind-layout.js` | `isMouseDoubleClicked` always false (ImGui call result discarded) | `DOUBLE WRONG never seen` after five attempts; test failed |
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
  - `sliderFloat3`'s `SliderFlags_WrapAround` rejection and its `format`
    check: `sliderFloat3()` in `addons/imgui/bind/bind_widgets.cpp`
    (the format check itself is `Call::format()` / `validFormat()` in
    `addons/imgui/bind/bind_util.cpp`).
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
  "Behaviours no test detects"); `bind::bindCalls()` for the test hook. Every reader of the plan's interface is implemented, including
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
  case of both tests has at least one that breaks it).
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
- **`setItemDefaultFocus` has no behavioural assertion, and none could be
  written with this surface.** It applies only while its window is
  appearing and ImGui has a navigation init request for it
  (imgui.cpp:9716-9730), and its effect is the navigation id of the
  window, which nothing bound reads (`IsItemFocused` is not bound) and
  which is not drawn without keyboard navigation
  (`ImGuiConfigFlags_NavEnableKeyboard` is not set by the addon). It is
  called in FOCUSHERE so that its count is non-zero.
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
    item; `textDisabled('%d')` is as wide as `'xx'`, so no `%` is
    interpreted), and `calcTextSize`'s `hideTextAfterDoubleHash` and
    `wrapWidth`.
- **DOUBLE depends on ImGui's time, which is the frame clock.**
  `isMouseDoubleClicked` is true only when two clicks fall within
  `io.MouseDoubleClickTime` (0.30 s) of ImGui's own time, which advances by
  `io.DeltaTime`, measured from the real clock (imgui_platform.cpp). The
  four button events are injected together, so ImGui's input trickling
  spreads them over four consecutive frames; and the case tries up to five
  times, each attempt a fresh pair (a failed attempt means the gap
  exceeded the double-click time, so the next one starts a new count). A
  run whose frames all took over ~75 ms would still fail it. The
  assertion is otherwise a read-back, like every other.
- **`beginChild`'s label-id guard (its `Label-id: yes`) is still Task 4's**,
  as Task 1's notes rule (`labelIdIsWindowId` comes with Task 4's STRINGS
  case and mutation); until then `beginChild('')` directly in a window
  reaches ImGui's plain assertion.
- **`isWindowHovered` and `isItemHovered` with flags outside their allowed
  masks** (kind (c), imgui.cpp:9225 and 5114) throw `ERR_IMGUI_USAGE`
  after the call; both tests' REJECTED pin the message and that the call
  was counted.
- **`check-hermes-node-unit` was red once** before this commit, with
  `SurfaceTableTest.DisplacementRacesCompletionUnderLoad` the only failure
  (in two of three runs); it passed 5/5 alone and the third suite run was
  460/460. That is the open load-dependency issue in `dz/` for that test,
  not this task: nothing here touches the compositor.
