# Dear ImGui JS binding: the bound surface

This is the table of bound functions that the "Metadata" section of the
design (`docs/superpowers/specs/2026-11-27-imgui-binding-design.md`) says
the plan contains. It accompanies the plan
`docs/superpowers/plans/2026-11-27-imgui-binding.md`. Every entry was
written from the vendored source in `external/imgui/imgui/` (Dear ImGui
v1.93.0-docking, unmodified), built with the addon's configuration
(`addons/imgui/imgui_config.h`: `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`,
`IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`, `IMGUI_USE_WCHAR32`). Line numbers
are those of the files on disk. Nothing here is from memory of other ImGui
versions: this version differs from older ones in several bound signatures
(for example `ImDrawList::AddRect` and `AddPolyline` take `thickness`
before `flags`, and `ImDrawFlags_Closed` is `1 << 9`, not 1).

## How to read an entry

- **JS**: the JavaScript signature. `ref<bool>` is a one-element reference
  array, `ref<float>[3]` a reference array of at least three elements,
  `ref<string>` a one-element reference array holding a string, `vec2` /
  `vec4` an `Array` of 2 / 4 numbers, `out?: vec2` the optional output
  array of a getter. `= x` is a default; the parameter may be omitted or
  `undefined`.
- **C++**: the declaration bound, copied from `imgui.h`, with its line.
- **Overload / Nullable / Omitted / Frame / Rejected**: the design's
  metadata kinds. `Frame: open` means the function needs an open frame and
  throws `ERR_IMGUI_NO_FRAME` otherwise; `Frame: either` means it works
  inside or outside a frame, creating the context on demand, and throws
  `ERR_IMGUI_NO_WINDOW` with no window. "Length" is one element for every
  pointer not written `v[N]`, and is not repeated. "Rejected" lists only
  what a function rejects beyond the general argument rules below.
- **Label-id**: present (`Label-id: yes`) on the functions that run the
  label-id guard described below. Absent means the guard does not apply.
- **Preconditions**: what ImGui asserts with a plain `IM_ASSERT`, or relies
  on without checking, on the entry path, that JavaScript could violate.
  Each is tagged:
  - **(a)** an argument check the binding makes;
  - **(b)** a state check the binding makes by reading ImGui's state, with
    the internal field named; a failed one throws `ERR_IMGUI_USAGE` without
    calling ImGui;
  - **(c)** already a recoverable `IM_ASSERT_USER_ERROR` in ImGui (the
    binding does nothing; the call throws `ERR_IMGUI_USAGE` after ImGui
    has recovered).
- **Notes**: anything else. A question the rulings did not settle is
  marked `QUESTION:` and numbered under "Open questions" (`[O1]`).

File names are abbreviated: `imgui.cpp`, `widgets` =
`imgui_widgets.cpp`, `tables` = `imgui_tables.cpp`, `draw` =
`imgui_draw.cpp`, `internal` = `imgui_internal.h`.

## General argument rules

These hold for every bound function and are not repeated in the entries.
They were decided after the first version of this table; "Rulings
applied" at the end lists them.

- **G1, flags and enums.** A flags or enum argument is a number that is
  an integer in the signed 32-bit range (-2^31 .. 2^31-1), so the result
  of JavaScript's `a | b` always fits. It is then rejected with
  `ERR_OUT_OF_RANGE` unless it is made of exported enumerators of the
  parameter's type ("Constants" gives the exported set per type):
  - a **flags** type (every type whose name ends in `Flags_`): no bit may
    be set that is not set in some exported enumerator of that type;
  - an **enum** type (`ImGuiDir`, `ImGuiKey`, `ImGuiCol_`,
    `ImGuiStyleVar_`, `ImGuiCond_`): the value must
    equal an exported enumerator.
  - **`ImGuiMouseButton`** is the one exception: ImGui has five buttons
    and names three (`_Left`, `_Right`, `_Middle`; `_COUNT` is 5,
    imgui.h:2087-2090), and its mouse functions accept 0..4. A
    mouse-button argument is an integer from 0 to 4. `ImGuiCond_` is in this group because
    the header says "Treat as a regular enum! Do NOT combine multiple
    values using binary operators!" (imgui.h:2126).

  This alone makes the following source checks unreachable, so the
  entries do not list them under "Rejected":
  - the six internal window flags (imgui.h:1253-1258), which reach plain
    assertions and unchecked indexing in `Begin()` (imgui.cpp:8032, 8093,
    8411, 8417, 8913) and in `BeginPopupEx` (imgui.cpp:13436);
  - child-flag bits outside `ImGuiChildFlags_SupportedMask_`
    (imgui.cpp:6901, 6903): the mask is exactly the nine non-zero
    exported enumerators;
  - `ImGuiSliderFlags_InvalidMask_` bits, `0x7000000F` (widgets:2696,
    3309): the exported slider enumerators use bits 5..12 only;
  - the private input-text bits `1 << 26` .. `1 << 28` (internal:1125-1127;
    widgets:4084, 4095);
  - `ImGuiTabItemFlags_Button`, `1 << 21` (internal:3161;
    widgets:10774), and the private tab-bar bits `1 << 20` .. `1 << 22`
    (internal:3151-3153; `_DockNode` would make `BeginTabBarEx` skip its
    ID push, widgets:10006-10007);
  - the private dock-node bits `1 << 10` and `1 << 11`
    (internal:2083-2084; imgui.cpp:20919-20920);
  - popup-flag bits 0 and 1 (`ImGuiPopupFlags_InvalidMask_`,
    imgui.h:1441; imgui.cpp:13548);
  - `ImDrawFlags_InvalidMask_` bits, `~0x7FFFFFF0` (imgui.h:3512; the
    recoverable checks at draw:981 and draw:2353);
  - a combined `ImGuiCond` (imgui.cpp:9433, 9444);
  - the private bits of `ImGuiButtonFlags`, `ImGuiSelectableFlags` and
    `ImGuiTreeNodeFlags` (internal:1131-1154, 1164-1174, 1177-1184), for
    which no assertion was found but whose effect on a public call is not
    defined.

  The rule tests bits, not meanings: an exported enumerator that a
  particular function must not receive (`Col_COUNT`, `Dir_None`, the
  `InputTextFlags_Callback*` bits, the table column status flags) is
  rejected by that function's own "Rejected" line.
- **G2, strings.** A string argument containing U+0000 is rejected with a
  `TypeError` whose `code` is `ERR_INVALID_ARG_VALUE`, everywhere,
  `loadIniSettingsFromMemory` included: ImGui reads a `const char*` up to
  the first NUL, so such a string would be seen truncated.
- **G3, 32-bit values.** An `ImU32` or `ImGuiID` argument accepts an
  integer from -2^31 to 2^32-1 and is taken modulo 2^32, so a colour
  assembled with JavaScript's signed bitwise operators (`0xFF000000 |
  rgb`) is accepted. Such values are returned as numbers 0 .. 2^32-1. An
  `int` argument accepts the signed 32-bit range, then the function's own
  limits.
- **G4, float limits.** The module exports `imgui.FLT_MIN` and
  `imgui.FLT_MAX`, numbers exactly equal to the C `float` values
  `FLT_MIN` and `FLT_MAX` (`<float.h>`, included at imgui.h:79). ImGui
  gives them meaning in bound signatures: `-FLT_MIN` as a width or size
  component means "align to the right" (imgui.h:559, 561, 661), and
  `FLT_MAX` is the plots' "compute from the data" (imgui.h:812, 814).
  JavaScript's own `-Number.MIN_VALUE` converts to `-0.0f` and must not
  be used for the first.
- **G5, formats.** A `format` argument (sliders, drags, `inputFloat`)
  must hold exactly one printf conversion of the value's kind (`f F e E
  g G a A` for a float, `d i u x X o` for an int), with flags (`-+ #0`),
  a width and a precision allowed and nothing else: no `*`, no `$`, no
  length modifier, no `%n`; `%%` is a literal, as in ImGui's own
  `ImParseFormatFindStart` (widgets:3659-3669). Anything else is
  `ERR_INVALID_ARG_VALUE`. The conversion specification, from its `%` to
  its conversion character inclusive, is also at most **30 bytes**:
  `ImParseFormatSanitizeForPrinting` and `ImParseFormatSanitizeForScanning`
  copy it into a `char[32]` and check only with a plain assertion that its
  length plus the terminator is below 32 (widgets:3713-3714, 3728-3729).
  Those copies are `fmt_sanitized[32]` in `RoundScalarWithFormatT`
  (widgets:2516-2517), reached from `DragBehaviorT` and `SliderBehaviorT`
  when a float value changes (widgets:2636, 3250, 3272), and
  `format_sanitized[32]` in `DataTypeApplyFromText` (widgets:2402-2409),
  reached from an int widget's text entry; `TempInputScalar`'s
  `fmt_buf[32]` is filled by a bounded copy that would truncate instead
  (widgets:3823-3825, 3704). The whole string has no limit: everywhere
  else it is used it formats into a fixed buffer through `ImFormatString`,
  which is `vsnprintf` bounded by that buffer (imgui.cpp:2466-2478):
  `value_buf[64]` in `DragScalar` / `SliderScalar` (widgets:2838-2839,
  3450), `buf[64]` in `InputScalar` (widgets:3887-3891), `v_str[64]` in
  `RoundScalarWithFormatT` (widgets:2521-2522), `data_buf[32]` in
  `TempInputScalar` (widgets:3826-3828). A long decoration is truncated on
  screen, never overflowed.

## Shared preconditions

- **S1, current window.** Every in-frame function dereferences
  `g.CurrentWindow` without checking. Inside a frame it is never NULL:
  `NewFrame()` begins the fallback window (`Begin("Debug##Default")`,
  imgui.cpp:6157) and `End()` refuses to pop it (imgui.cpp:8990-8993). So
  the frame check covers it, and it is not repeated per entry.
- **label-id, an empty ID at the root of a window.** `ItemAdd()` asserts
  plainly `id != window->ID` (imgui.cpp:12319, inside
  `#ifndef IMGUI_DISABLE_DEBUG_TOOLS`, which the addon does not define).
  A label hashes to the ID-stack seed when it is `""` or ends with `###`
  (`ImHashStr`, imgui.cpp:2636-2641, 2654-2658), and the seed is
  `window->ID` when nothing has been pushed with `pushID`. So
  `imgui.button('')` directly inside `begin()`/`end()` would abort a
  Debug build. Kind (b): a helper in `bind_guards.cpp` computes the ID
  the label would get under the current ID stack (`ImGui::GetID(label)`,
  imgui.h:627, which is `window->GetID(label)`, imgui.cpp:10164-10168)
  and throws `ERR_IMGUI_USAGE` when it equals `g.CurrentWindow->ID`. The
  functions it applies to carry `Label-id: yes`; each entry says where
  the label reaches `ItemAdd()`. The guard is stricter than ImGui in one
  respect: ImGui's assertion is skipped when the item is clipped (the
  early return at imgui.cpp:12304-12307), the guard is not.

No companion function was found to be required by a named function, so
there is no "Proposed addition" under any group.

## Frame

### `newFrame` (existing, part A)

- JS: `imgui.newFrame() -> boolean`
- C++: `IMGUI_API void NewFrame();` (`imgui.h:417`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: outside only in effect: part A's `newFrame()` discards a frame
  left open rather than throwing. Rejected: none.
- Preconditions: `ErrorCheckNewFrameSanityChecks()` (imgui.cpp:5882)
  asserts plainly on IO and style values (imgui.cpp:11837-11862), among
  them `g.FrameCountEnded == g.FrameCount` (imgui.cpp:11839, the previous
  frame was ended), `DisplaySize >= 0` (11840), `Style.Alpha` in 0..1
  (11843) and that at least one of the error outputs is on or an error
  callback is installed (11858). All are the addon's own responsibility
  (part A's frame protocol and context creation); none is reachable from a
  JavaScript argument, because a style variable left pushed is popped by
  the end-of-frame recovery before the next `NewFrame()`.
- Notes: unchanged from part A except that it sets the two
  `ConfigErrorRecovery*` fields again, takes part in the re-entrancy
  flag (design, "Errors" and "Re-entrancy"), and sets
  `g.BeginComboDepth = 0` before calling `ImGui::NewFrame()` (ruling 19;
  see `endCombo`).

### `render` (existing, part A)

- JS: `imgui.render() -> boolean`
- C++: `IMGUI_API void Render();` (`imgui.h:419`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `Render()` asserts `g.Initialized` (imgui.cpp:6536) and
  calls `EndFrame()` (imgui.cpp:6539), which asserts `g.Initialized`
  (6422) and reports a missing `NewFrame()` recoverably (c)
  (imgui.cpp:6427, "Forgot to call ImGui::NewFrame()?"). The binding's
  frame check comes first, so neither is reached. End-of-frame recovery
  runs here (imgui.cpp:6432-6433); see "Recoverable errors, audited".
- Notes: none.

### `showDemoWindow` (existing, part A)

- JS: `imgui.showDemoWindow(pOpen = null) -> undefined`, `pOpen:
  ref<bool>`
- C++: `IMGUI_API void ShowDemoWindow(bool* p_open = NULL);`
  (`imgui.h:423`)
- Overload: only one. Nullable: `p_open`. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (`IM_ASSERT(ImGui::GetCurrentContext() !=
  NULL ...)` at imgui_demo.cpp:363 is covered by the frame check).
- Notes: part A's function took no argument and called
  `ImGui::ShowDemoWindow()` (`addons/imgui/imgui_module.cpp:169`). Part B
  changes it to the convention's signature; a call with no argument
  behaves as before.

### `version` (existing, part A)

- JS: `imgui.version` (a string property, not a function)
- C++: the macro `IMGUI_VERSION` (`imgui.h:32`, `"1.93.0"`), which is
  what `addons/imgui/imgui_module.cpp:213` uses;
  `IMGUI_API const char* GetVersion();` (`imgui.h:432`) returns the same
  macro (imgui.cpp:5439-5442).
- Frame: either (no context needed). Preconditions: none.
- Notes: none.

### `showMetricsWindow`

- JS: `imgui.showMetricsWindow(pOpen = null) -> undefined`, `pOpen:
  ref<bool>`
- C++: `IMGUI_API void ShowMetricsWindow(bool* p_open = NULL);`
  (`imgui.h:424`)
- Overload: only one. Nullable: `p_open`. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found on the entry path (imgui.cpp:22707-22716).
  The real definition is compiled because the addon does not define
  `IMGUI_DISABLE_DEBUG_TOOLS` (the stub is at imgui.cpp:24673).
- Notes: none.

### `getTime`

- JS: `imgui.getTime() -> number`
- C++: `IMGUI_API double GetTime();` (`imgui.h:1090`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: either (context created on demand; `ERR_IMGUI_NO_WINDOW` with
  no window). Rejected: none.
- Preconditions: none found (`return GImGui->Time;`, imgui.cpp:5478-5481;
  needs a context).
- Notes: none.

### `getFrameRate`

- JS: `imgui.getFrameRate() -> number`
- C++: field `float Framerate;` of `ImGuiIO` (`imgui.h:2725`), read through
  `ImGui::GetIO()` (`imgui.h:414`).
- Frame: either (context created on demand; `ERR_IMGUI_NO_WINDOW` with
  no window). Rejected: none.
- Preconditions: none found (needs a context: `GetIO()`).
- Notes: none.

### `getDeltaTime`

- JS: `imgui.getDeltaTime() -> number`
- C++: field `float DeltaTime;` of `ImGuiIO` (`imgui.h:2551`), read through
  `ImGui::GetIO()` (`imgui.h:414`).
- Frame: either (context created on demand; `ERR_IMGUI_NO_WINDOW` with
  no window). Rejected: none.
- Preconditions: none found (needs a context: `GetIO()`).
- Notes: none.

### `getDisplaySize`

- JS: `imgui.getDisplaySize(out?: vec2) -> vec2`
- C++: field `ImVec2 DisplaySize;` of `ImGuiIO` (`imgui.h:2549`), read through
  `ImGui::GetIO()` (`imgui.h:414`).
- Frame: either (context created on demand; `ERR_IMGUI_NO_WINDOW` with
  no window). Rejected: none.
- Preconditions: none found (needs a context: `GetIO()`).
- Notes: none.

### `wantCaptureMouse`

- JS: `imgui.wantCaptureMouse() -> boolean`
- C++: field `bool WantCaptureMouse;` of `ImGuiIO` (`imgui.h:2718`), read through
  `ImGui::GetIO()` (`imgui.h:414`).
- Frame: either (context created on demand; `ERR_IMGUI_NO_WINDOW` with
  no window). Rejected: none.
- Preconditions: none found (needs a context: `GetIO()`).
- Notes: none.

### `wantCaptureKeyboard`

- JS: `imgui.wantCaptureKeyboard() -> boolean`
- C++: field `bool WantCaptureKeyboard;` of `ImGuiIO` (`imgui.h:2719`), read through
  `ImGui::GetIO()` (`imgui.h:414`).
- Frame: either (context created on demand; `ERR_IMGUI_NO_WINDOW` with
  no window). Rejected: none.
- Preconditions: none found (needs a context: `GetIO()`).
- Notes: none.

## Windows

### `begin`

- JS: `imgui.begin(name, pOpen = null, flags = 0) -> boolean`, `pOpen:
  ref<bool>`
- C++: `IMGUI_API bool Begin(const char* name, bool* p_open = NULL,
  ImGuiWindowFlags flags = 0);` (`imgui.h:451`)
- Overload: only one. Nullable: `p_open`. Omitted: none.
- Frame: open. Rejected: an empty `name`.
- Preconditions:
  - `name != NULL && name[0] != '\0'` (imgui.cpp:8003, plain). (a): `name`
    is a non-empty string.
  - `g.WithinFrameScope` and `g.FrameCountEnded != g.FrameCount`
    (imgui.cpp:8004-8005, plain). Covered by the frame check.
  - the internal window flags (imgui.cpp:8032, 8093, 8411, 8417, 8913):
    not exported, so rejected by G1.
- Notes: `end()` is always called, whatever `begin()` returned.

### `end`

- JS: `imgui.end() -> undefined`
- C++: `IMGUI_API void End();` (`imgui.h:452`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - (c) window stack at the fallback window: "Calling End() too many
    times!" (imgui.cpp:8992), returns without popping.
  - (c) current window is a popup not being ended by `EndPopup()`: "Must
    call EndPopup() and not End()!" (imgui.cpp:8999); the window is ended
    anyway.
  - (c) current window is a child not being ended by `EndChild()`: "Must
    call EndChild() and not End()!" (imgui.cpp:9001); the window is ended
    anyway.
  - `End()` runs `ErrorRecoveryTryToRecoverWindowState()`
    (imgui.cpp:9039), so every "Missing ..." error of the window being
    ended is reported here (c); see "Recoverable errors, audited".
  - (c) `ErrorCheckUsingSetCursorPosToExtendParentBoundaries()`
    (imgui.cpp:9022-9023, message at imgui.cpp:11810) when
    `setCursorScreenPos` moved the cursor past the window's extent and no
    item followed.
  - the current window is not a scrolling table's own region, the child
    window `BeginTable()` begins for `TableFlags_ScrollX`/`_ScrollY`
    (tables:340, 432-433). `End()` runs the window recovery before popping
    the window (imgui.cpp:9038-9039); it finds the table open in the
    window and calls `EndTable()` (imgui.cpp:12019-12023), which ends the
    child itself (tables:1612-1616), so the window is ended twice and the
    clip rectangle stack is popped past empty (imgui.cpp:6296-6297,
    imgui.h:2377, plain; measured). The recovery loops while the current
    table's inner window is the current window, and ending a table without
    a region of its own makes current the table of the temp-data slot
    below when that slot was begun in the same window (tables:470,
    1670-1671): so plain tables nested in the region's cell lead to the
    same abort (measured: one plain table, under `end`, `endChild` and
    `endListBox`). (b): the same chain walked without changing anything,
    from `g.CurrentTable` down `g.TablesTempData` from
    `g.TablesTempDataStacked - 1` while the table's inner window is the
    current window, refusing at a table whose `InnerWindow !=
    OuterWindow` (`canEnd:table-region`, Task 6). The walk was chosen over
    a single read of the parent window's table index because it is the
    recovery's own sequence of reads, whatever the depth; a plain table in
    a child of the region, or plain tables only, stop it and are accepted
    (`ui-imgui-bind-errors.js` TABLE_REGION).
- Notes: the same mistake at the end of the frame is recovered without
  this: `ErrorRecoveryTryToRecoverState()` calls `EndTable()` rather than
  `End()` for such a window (imgui.cpp:11990-11994).

### `beginChild`

- JS: `imgui.beginChild(strId, size = [0, 0], childFlags = 0, windowFlags
  = 0) -> boolean`
- C++: `IMGUI_API bool BeginChild(const char* str_id, const ImVec2& size =
  ImVec2(0, 0), ImGuiChildFlags child_flags = 0, ImGuiWindowFlags
  window_flags = 0);` (`imgui.h:472`)
- Overload: the `const char* str_id` one. NOT bound: `BeginChild(ImGuiID
  id, ...)` (`imgui.h:473`). Nullable: none. Omitted: none.
- Frame: open.
- Rejected: `ImGuiWindowFlags_AlwaysAutoResize` in `windowFlags`;
  `ImGuiChildFlags_AlwaysAutoResize` together with `_ResizeX` or
  `_ResizeY`, or without `_AutoResizeX` or `_AutoResizeY`.
- Label-id: yes.
- Preconditions (all in `BeginChildEx`, plain, kind (a) unless said):
  - `id != 0` (imgui.cpp:6895): the hash of `strId`; not checkable from
    the argument and not zero in practice. See "Not covered".
  - `(child_flags & ~ImGuiChildFlags_SupportedMask_) == 0`
    (imgui.cpp:6903): covered by G1.
  - `(window_flags & ImGuiWindowFlags_AlwaysAutoResize) == 0`
    (imgui.cpp:6904).
  - with `ImGuiChildFlags_AlwaysAutoResize`: no `_ResizeX`/`_ResizeY`
    (imgui.cpp:6907) and at least one of `_AutoResizeX`/`_AutoResizeY`
    (imgui.cpp:6908).
  - label-id: `EndChild()` submits the child as an item with
    `child_window->ChildId` (imgui.cpp:7034, 7046), which is the hash of
    `strId` (imgui.cpp:6993). (b).
- Notes: the `ImGuiID` overload is not bound (ruling 4).

### `endChild`

- JS: `imgui.endChild() -> undefined`
- C++: `IMGUI_API void EndChild();` (`imgui.h:474`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - `child_window->Flags & ImGuiWindowFlags_ChildWindow` (imgui.cpp:7021,
    plain). (b): `g.CurrentWindow->Flags & ImGuiWindowFlags_ChildWindow`.
  - It then calls `End()` (imgui.cpp:7025); see `end`. Its last
    condition, the current window not being a scrolling table's own
    region, applies here too (b): `canEndChild:table-region` (Task 6).
- Notes: the first guard is ImGui's own condition, so it is as weak as
  ImGui's: it also holds inside a `beginListBox` (a child, widgets:8989)
  and inside a sub-menu
  (`ImGuiWindowFlags_ChildWindow` is added at widgets:9534-9535; `End()`
  then reports "Must call EndPopup() and not End()!"). An `endChild()`
  after a `beginChild()` that returned false is valid and passes.

### `setNextWindowPos`

- JS: `imgui.setNextWindowPos(pos, cond = 0, pivot = [0, 0]) ->
  undefined`
- C++: `IMGUI_API void SetNextWindowPos(const ImVec2& pos, ImGuiCond cond
  = 0, const ImVec2& pivot = ImVec2(0, 0));` (`imgui.h:492`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `cond == 0 || ImIsPowerOfTwo(cond)` (imgui.cpp:9433,
  plain). Covered by G1: `ImGuiCond` is an enum type, so a combination of
  conditions is not an exported enumerator.
- Notes: none.

### `setNextWindowSize`

- JS: `imgui.setNextWindowSize(size, cond = 0) -> undefined`
- C++: `IMGUI_API void SetNextWindowSize(const ImVec2& size, ImGuiCond
  cond = 0);` (`imgui.h:493`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `cond == 0 || ImIsPowerOfTwo(cond)` (imgui.cpp:9444,
  plain). Covered by G1: `ImGuiCond` is an enum type, so a combination of
  conditions is not an exported enumerator.
- Notes: none.

### `setNextWindowBgAlpha`

- JS: `imgui.setNextWindowBgAlpha(alpha) -> undefined`
- C++: `IMGUI_API void SetNextWindowBgAlpha(float alpha);` (`imgui.h:499`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:9488-9493).
- Notes: none.

### `getWindowPos`

- JS: `imgui.getWindowPos(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetWindowPos();` (`imgui.h:484`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found beyond S1 (imgui.cpp:9292-9297).
- Notes: none.

### `getWindowSize`

- JS: `imgui.getWindowSize(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetWindowSize();` (`imgui.h:485`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found beyond S1 (imgui.cpp:9335-9339).
- Notes: none.

### `getContentRegionAvail`

- JS: `imgui.getContentRegionAvail(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetContentRegionAvail();` (`imgui.h:586`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found beyond S1 (imgui.cpp:12652-12658).
- Notes: none.

### `isWindowFocused`

- JS: `imgui.isWindowFocused(flags = 0) -> boolean`
- C++: `IMGUI_API bool IsWindowFocused(ImGuiFocusedFlags flags=0);`
  (`imgui.h:480`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `IM_ASSERT(cur_window)` (imgui.cpp:13857, plain): S1.
- Notes: none.

### `isWindowHovered`

- JS: `imgui.isWindowHovered(flags = 0) -> boolean`
- C++: `IMGUI_API bool IsWindowHovered(ImGuiHoveredFlags flags=0);`
  (`imgui.h:481`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) flags outside
  `ImGuiHoveredFlags_AllowedMaskForIsWindowHovered` (internal:1117):
  "Invalid flags for IsWindowHovered()!" (imgui.cpp:9225). It is not a
  `_RET` form: the function continues and returns a value, which the
  binding discards when it throws.
- Notes: none.

### `getScrollY`

- JS: `imgui.getScrollY() -> number`
- C++: `IMGUI_API float GetScrollY();` (`imgui.h:514`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found beyond S1 (imgui.cpp:12915-12919).
- Notes: none.

### `getScrollMaxY`

- JS: `imgui.getScrollMaxY() -> number`
- C++: `IMGUI_API float GetScrollMaxY();` (`imgui.h:518`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found beyond S1 (imgui.cpp:12927-12931).
- Notes: none.

### `setScrollHereY`

- JS: `imgui.setScrollHereY(centerYRatio = 0.5) -> undefined`
- C++: `IMGUI_API void SetScrollHereY(float center_y_ratio = 0.5f);`
  (`imgui.h:520`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: a ratio outside 0..1 (NaN included).
- Preconditions: `center_y_ratio >= 0.0f && center_y_ratio <= 1.0f`
  (imgui.cpp:12979, plain, in `SetScrollFromPosY(ImGuiWindow*, ...)`
  called at imgui.cpp:13017). (a).
- Notes: none.

## Docking and settings

### `dockSpaceOverViewport`

- JS: `imgui.dockSpaceOverViewport(dockspaceId = 0, flags = 0) -> number`
- C++: `IMGUI_API ImGuiID DockSpaceOverViewport(ImGuiID dockspace_id = 0,
  const ImGuiViewport* viewport = NULL, ImGuiDockNodeFlags flags = 0,
  const ImGuiWindowClass* window_class = NULL);` (`imgui.h:1002`)
- Overload: only one. Nullable: none in JavaScript.
- Omitted: `viewport` and `window_class`, passed as NULL.
- Frame: open. Rejected: none.
- Preconditions (in `DockSpace`, reached at imgui.cpp:21047, only when
  docking is enabled, which part A does at
  `addons/imgui/imgui_module.cpp:115`):
  - `(flags & ImGuiDockNodeFlags_DockSpace) == 0` (imgui.cpp:20919,
    plain) and `(flags & ImGuiDockNodeFlags_CentralNode) == 0`
    (imgui.cpp:20920, plain): both bits are private (internal:2083-2084),
    so rejected by G1.
  - `dockspace_id != 0` (imgui.cpp:20922, plain): a zero argument is
    replaced by `GetID("DockSpace")` first (imgui.cpp:21045-21046), so it
    cannot be reached.
- Notes: it begins and ends a host window of its own
  (imgui.cpp:21041-21049), pushing and popping three style variables.
  `dockspaceId` and the return value are `ImGuiID`s (G3).

### `loadIniSettingsFromMemory`

- JS: `imgui.loadIniSettingsFromMemory(iniData) -> undefined`
- C++: `IMGUI_API void LoadIniSettingsFromMemory(const char* ini_data,
  size_t ini_size=0);` (`imgui.h:1182`)
- Overload: only one. Nullable: none.
- Omitted: `ini_size` (design, "Exceptions: Settings"); the binding
  passes the encoded byte length.
- Frame: outside only; inside a frame it throws `ERR_IMGUI_USAGE`
  (design). With no window it throws `ERR_IMGUI_NO_WINDOW`. Rejected:
  none.
- Preconditions: `g.Initialized` (imgui.cpp:16614, plain): satisfied by
  context creation. ImGui's own "not between NewFrame() and EndFrame()"
  assertion is commented out (imgui.cpp:16615), so the outside-only rule
  is the binding's, as the design says.
- Notes: passing the byte length rather than 0 matters: with
  `ini_size == 0` ImGui calls `ImStrlen(ini_data)` (imgui.cpp:16620-16621).
  The data is copied (imgui.cpp:16622-16626), so the encoded buffer need
  not outlive the call.

### `saveIniSettingsToMemory`

- JS: `imgui.saveIniSettingsToMemory() -> string`
- C++: `IMGUI_API const char* SaveIniSettingsToMemory(size_t* out_ini_size
  = NULL);` (`imgui.h:1184`)
- Overload: only one. Nullable: none in JavaScript.
- Omitted: `out_ini_size` (design, "Exceptions: Settings").
- Frame: either. With no window it throws `ERR_IMGUI_NO_WINDOW`.
  Rejected: none.
- Preconditions: none found (imgui.cpp:16704-16715).
- Notes: it also clears `g.SettingsDirtyTimer` (imgui.cpp:16707), a
  side effect with `io.IniFilename == NULL` that nothing reads.

## Menus

### `beginMainMenuBar`

- JS: `imgui.beginMainMenuBar() -> boolean`
- C++: `IMGUI_API bool BeginMainMenuBar();` (`imgui.h:831`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: it begins the window `"##MainMenuBar"` (widgets:9466)
  and then calls `BeginMenuBar()` (widgets:9476), which asserts plainly
  `!window->DC.MenuBarAppending` (widgets:9331). That field is cleared
  only on a window's first `Begin()` of the frame (imgui.cpp:8759), so a
  second `beginMainMenuBar()` before the first one's `endMainMenuBar()`
  asserts. Not guarded: the condition is on a window that is not the
  current one; see "Not covered".
- Notes: `endMainMenuBar()` is called only when this returned true (when
  it returns false it has already called `End()`, widgets:9468-9472).

### `endMainMenuBar`

- JS: `imgui.endMainMenuBar() -> undefined`
- C++: `IMGUI_API void EndMainMenuBar();` (`imgui.h:832`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - (c) `g.CurrentWindow->DC.MenuBarAppending`: "Calling EndMainMenuBar()
    not from a menu-bar!" (widgets:9483).
  - It then calls `EndMenuBar()` (widgets:9485), whose two flag
    assertions (widgets:9362-9363) hold once the check above passed (a
    menu bar is open only in a window with `ImGuiWindowFlags_MenuBar`,
    widgets:9328-9349), and `End()` (widgets:9493).
  - But `EndMenuBar()` also reads `g.GroupStack.back()` and calls
    `EndGroup()` (widgets:9401, 9405), which the check above does not
    cover: after `beginMainMenuBar(); endGroup();` (accepted by
    `canEndGroup`, the menu bar's group belonging to `##MainMenuBar`) the
    stack is empty and `ImVector::back()` asserts (imgui.h:2377); with
    another window's group underneath, `EndGroup()`'s plain assertion does
    (imgui.cpp:12722). Both were reproduced. (b), when
    `g.CurrentWindow->DC.MenuBarAppending` is set and `SkipItems` is not:
    `g.GroupStack.Size > 0` and `g.GroupStack.back().WindowID ==
    g.CurrentWindow->ID`, the conditions of `endMenuBar`'s guard,
    registered as `canEndMainMenuBar:no-group` and `:wrong-window`.
    (Added after the Task 5 review; the first version of this entry said
    `EndMenuBar()`'s assertions held.)
- Notes: ImGui's check does not test that the window is the main menu bar
  (its own comment at widgets:9483), so a call inside any window's
  `beginMenuBar()` scope passes it and ends that window.

### `beginMenuBar`

- JS: `imgui.beginMenuBar() -> boolean`
- C++: `IMGUI_API bool BeginMenuBar();` (`imgui.h:829`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `!window->DC.MenuBarAppending` (widgets:9331, plain),
  reached only when the window is not skipping items and has
  `ImGuiWindowFlags_MenuBar` (widgets:9326-9329; otherwise it returns
  false). (b): `g.CurrentWindow->DC.MenuBarAppending == false`.
- Notes: none.

### `endMenuBar`

- JS: `imgui.endMenuBar() -> undefined`
- C++: `IMGUI_API void EndMenuBar();` (`imgui.h:830`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions (none is evaluated when `window->SkipItems`, because the
  function returns first, widgets:9357-9358):
  - `window->Flags & ImGuiWindowFlags_MenuBar` (widgets:9362, plain). (b):
    `g.CurrentWindow->Flags`.
  - `window->DC.MenuBarAppending` (widgets:9363, plain). (b):
    `g.CurrentWindow->DC.MenuBarAppending`.
  - `g.GroupStack.back()` (widgets:9401) and `EndGroup()` (widgets:9405),
    which need the group `BeginMenuBar()` opened still to be the top of
    the group stack: `g.GroupStack.Size > 0` and
    `g.GroupStack.back().WindowID == window->ID` (imgui.cpp:12719, 12722,
    plain). (b): `g.GroupStack`. It fails when an `endGroup()` inside the
    menu bar ended the menu bar's own group (which belongs to the window,
    so `canEndGroup` lets it through): then no group is left, or the top
    one is another window's. A `beginGroup()` left open inside the menu bar
    does not make it fail: that group also belongs to the window, so
    `EndMenuBar()` ends it in place of its own, and the window's `end()`
    reports the menu bar's group as "Missing EndGroup()". (Corrected in
    Task 5; the first version of this entry named that case.)
  - (c) `PopID()` (widgets:9396).
- Notes: none.

### `beginMenu`

- JS: `imgui.beginMenu(label, enabled = true) -> boolean`
- C++: `IMGUI_API bool BeginMenu(const char* label, bool enabled =
  true);` (`imgui.h:833`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`BeginMenuEx` submits `Selectable("")` under
  `PushID(label)`, widgets:9601). No other plain assertion on user input
  was found in `BeginMenuEx` (widgets:9520-9744).
- Notes: none.

### `endMenu`

- JS: `imgui.endMenu() -> undefined`
- C++: `IMGUI_API void EndMenu();` (`imgui.h:834`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) the current window has both
  `ImGuiWindowFlags_Popup` and `ImGuiWindowFlags_ChildMenu`: "Calling
  EndMenu() in wrong window!" (widgets:9755). It then calls `EndPopup()`
  (widgets:9766).
- Notes: none.

### `menuItem`

- JS, dispatched on the type of the third argument:
  - `imgui.menuItem(label, shortcut = null, selected = false, enabled =
    true) -> boolean` (a boolean, `undefined` or `null`)
  - `imgui.menuItem(label, shortcut, pSelected, enabled = true) ->
    boolean`, `pSelected: ref<bool>` (an `Array`)
- C++:
  - `IMGUI_API bool MenuItem(const char* label, const char* shortcut =
    NULL, bool selected = false, bool enabled = true);` (`imgui.h:835`)
  - `IMGUI_API bool MenuItem(const char* label, const char* shortcut,
    bool* p_selected, bool enabled = true);` (`imgui.h:836`)
- Overload: both. Nullable: `shortcut`. `p_selected` is NULL-able in C++
  (widgets:9866-9869), but a `null` third argument selects the first
  overload, which behaves identically, so the binding never passes NULL.
- Omitted: none. Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`MenuItemEx` submits `Selectable("")` under
  `PushID(label)`, widgets:9805). No other plain assertion found
  (widgets:9769-9858).
- Notes: none.

## Layout

### `sameLine`

- JS: `imgui.sameLine(offsetFromStartX = 0, spacing = -1) -> undefined`
- C++: `IMGUI_API void SameLine(float offset_from_start_x=0.0f, float
  spacing=-1.0f);` (`imgui.h:597`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found beyond S1 (imgui.cpp:12429-12453).
- Notes: none.

### `separator`

- JS: `imgui.separator() -> undefined`
- C++: `IMGUI_API void Separator();` (`imgui.h:596`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none reachable. `SeparatorEx` asserts on its flags and
  `thickness > 0.0f` (widgets:1669-1670), but `Separator()` computes both
  itself, the thickness as `ImMax(g.Style.SeparatorSize, 1.0f)`
  (widgets:1738-1744).
- Notes: none.

### `separatorText`

- JS: `imgui.separatorText(label) -> undefined`
- C++: `IMGUI_API void SeparatorText(const char* label);` (`imgui.h:647`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1797-1810; it submits its item with
  ID 0, widgets:1809, so label-id does not apply).
- Notes: none.

### `spacing`

- JS: `imgui.spacing() -> undefined`
- C++: `IMGUI_API void Spacing();` (`imgui.h:599`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1612-1618).
- Notes: none.

### `dummy`

- JS: `imgui.dummy(size) -> undefined`
- C++: `IMGUI_API void Dummy(const ImVec2& size);` (`imgui.h:600`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1620-1629; item ID 0).
- Notes: none.

### `newLine`

- JS: `imgui.newLine() -> undefined`
- C++: `IMGUI_API void NewLine();` (`imgui.h:598`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1631-1646).
- Notes: none.

### `indent`

- JS: `imgui.indent(indentW = 0) -> undefined`
- C++: `IMGUI_API void Indent(float indent_w = 0.0f);` (`imgui.h:601`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12519-12525).
- Notes: an `indent` without its `unindent` is neither diagnosed nor
  harmful beyond layout; ImGui keeps no stack for it.

### `unindent`

- JS: `imgui.unindent(indentW = 0) -> undefined`
- C++: `IMGUI_API void Unindent(float indent_w = 0.0f);` (`imgui.h:602`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12527-12533).
- Notes: none.

### `beginGroup`

- JS: `imgui.beginGroup() -> undefined`
- C++: `IMGUI_API void BeginGroup();` (`imgui.h:603`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12685-12713).
- Notes: none.

### `endGroup`

- JS: `imgui.endGroup() -> undefined`
- C++: `IMGUI_API void EndGroup();` (`imgui.h:604`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - `g.GroupStack.Size > 0` (imgui.cpp:12719, plain). (b):
    `g.GroupStack.Size`.
  - `group_data.WindowID == window->ID` (imgui.cpp:12722, plain): the
    group on top of the stack was begun in the current window. (b):
    `g.GroupStack.back().WindowID` against `g.CurrentWindow->ID`.
  - (c) `ErrorCheckUsingSetCursorPosToExtendParentBoundaries()`
    (imgui.cpp:12724-12725, message at imgui.cpp:11810).
- Notes: the design names only the stack's size; the second condition is
  a separate plain assertion on the same path and needs the same guard.

### `setNextItemWidth`

- JS: `imgui.setNextItemWidth(itemWidth) -> undefined`
- C++: `IMGUI_API void SetNextItemWidth(float item_width);`
  (`imgui.h:561`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12536-12541).
- Notes: C++ code commonly passes `-FLT_MIN` here: `-imgui.FLT_MIN`
  (G4).

### `pushItemWidth`

- JS: `imgui.pushItemWidth(itemWidth) -> undefined`
- C++: `IMGUI_API void PushItemWidth(float item_width);` (`imgui.h:559`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12544-12551).
- Notes: a push without its pop is not diagnosed (design, "Errors").

### `popItemWidth`

- JS: `imgui.popItemWidth() -> undefined`
- C++: `IMGUI_API void PopItemWidth();` (`imgui.h:560`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) `window->DC.ItemWidthStack.Size <= 0`: "Calling
  PopItemWidth() too many times!" (imgui.cpp:12578).
- Notes: none.

### `getCursorScreenPos`

- JS: `imgui.getCursorScreenPos(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetCursorScreenPos();` (`imgui.h:584`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12455-12459).
- Notes: none.

### `setCursorScreenPos`

- JS: `imgui.setCursorScreenPos(pos) -> undefined`
- C++: `IMGUI_API void SetCursorScreenPos(const ImVec2& pos);`
  (`imgui.h:585`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none at the call (imgui.cpp:12461-12467). It sets
  `window->DC.IsSetPos`, and a later `End()`, `EndGroup()` or table cell
  end reports (c) "Code uses SetCursorPos()/SetCursorScreenPos() to
  extend window/parent boundaries. ..." (imgui.cpp:11810) if the cursor
  is past the window's extent and no item was submitted after it.
- Notes: that error is raised by the later call, not this one, so the
  `ERR_IMGUI_USAGE` is thrown from `end()`, `endGroup()`, a table
  function, or `render()`.

### `getTextLineHeight`

- JS: `imgui.getTextLineHeight() -> number`
- C++: `IMGUI_API float GetTextLineHeight();` (`imgui.h:606`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12628-12632).
- Notes: none.

### `getTextLineHeightWithSpacing`

- JS: `imgui.getTextLineHeightWithSpacing() -> number`
- C++: `IMGUI_API float GetTextLineHeightWithSpacing();` (`imgui.h:607`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12634-12638).
- Notes: none.

### `getFrameHeight`

- JS: `imgui.getFrameHeight() -> number`
- C++: `IMGUI_API float GetFrameHeight();` (`imgui.h:608`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:12640-12644).
- Notes: none.

### `calcTextSize`

- JS: `imgui.calcTextSize(text, hideTextAfterDoubleHash = false, wrapWidth
  = -1, out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 CalcTextSize(const char* text, const char*
  text_end = NULL, bool hide_text_after_double_hash = false, float
  wrap_width = -1.0f);` (`imgui.h:1098`)
- Overload: only one. Nullable: none.
- Omitted: `text_end`, passed as NULL.
- Frame: open. Rejected: none.
- Preconditions: none found on the entry path (imgui.cpp:6608-6622).
- Notes: none.

### `alignTextToFramePadding`

- JS: `imgui.alignTextToFramePadding() -> undefined`
- C++: `IMGUI_API void AlignTextToFramePadding();` (`imgui.h:605`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1648-1657).
- Notes: none.

## IDs

### `pushID`

- JS, dispatched on the type of the argument:
  - `imgui.pushID(strId) -> undefined` (a string)
  - `imgui.pushID(intId) -> undefined` (a number, an integer in `int`
    range)
- C++:
  - `IMGUI_API void PushID(const char* str_id);` (`imgui.h:622`)
  - `IMGUI_API void PushID(int int_id);` (`imgui.h:625`)
- Overload: those two. NOT bound: `PushID(const char* str_id_begin, const
  char* str_id_end)` (`imgui.h:623`) and `PushID(const void* ptr_id)`
  (`imgui.h:624`). Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:10088-10094, 10112-10118).
- Notes: a push without its pop is reported when the window ends, (c)
  "Missing PopID()" (imgui.cpp:12056).

### `popID`

- JS: `imgui.popID() -> undefined`
- C++: `IMGUI_API void PopID();` (`imgui.h:626`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) `window->IDStack.Size > 1`: "Calling PopID() too many
  times!" (imgui.cpp:10160).
- Notes: none.

### `getID`

- JS, dispatched on the type of the argument:
  - `imgui.getID(strId) -> number` (a string)
  - `imgui.getID(intId) -> number` (a number, an integer in `int` range)
- C++:
  - `IMGUI_API ImGuiID GetID(const char* str_id);` (`imgui.h:627`)
  - `IMGUI_API ImGuiID GetID(int int_id);` (`imgui.h:630`)
- Overload: those two. NOT bound: `GetID(const char* str_id_begin, const
  char* str_id_end)` (`imgui.h:628`) and `GetID(const void* ptr_id)`
  (`imgui.h:629`). Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:10164-10168, 10182-10186).
- Notes: returns an `ImGuiID`, a number 0..2^32-1.

## Text

All six pass a finished string and never let ImGui interpret `%`.

### `text`

- JS: `imgui.text(text) -> undefined`
- C++: bound through `IMGUI_API void TextUnformatted(const char* text,
  const char* text_end = NULL);` (`imgui.h:634`), the `Unformatted`
  variant of `IMGUI_API void Text(const char* fmt, ...) IM_FMTARGS(1);`
  (`imgui.h:635`).
- Overload: only one. Nullable: none.
- Omitted: `text_end` (NULL); the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:271-274, 161-172).
- Notes: none.

### `textColored`

- JS: `imgui.textColored(col, text) -> undefined`, `col: vec4`
- C++: `IMGUI_API void TextColored(const ImVec4& col, const char* fmt,
  ...) IM_FMTARGS(2);` (`imgui.h:637`), called as `TextColored(col, "%s",
  text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:295-308: a style colour push, the
  text, and its pop).
- Notes: none.

### `textDisabled`

- JS: `imgui.textDisabled(text) -> undefined`
- C++: `IMGUI_API void TextDisabled(const char* fmt, ...) IM_FMTARGS(1);`
  (`imgui.h:639`), called as `TextDisabled("%s", text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:310-324).
- Notes: none.

### `textWrapped`

- JS: `imgui.textWrapped(text) -> undefined`
- C++: `IMGUI_API void TextWrapped(const char* fmt, ...) IM_FMTARGS(1);`
  (`imgui.h:641`), called as `TextWrapped("%s", text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:326-343).
- Notes: none.

### `bulletText`

- JS: `imgui.bulletText(text) -> undefined`
- C++: `IMGUI_API void BulletText(const char* fmt, ...) IM_FMTARGS(1);`
  (`imgui.h:645`), called as `BulletText("%s", text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:425-445).
- Notes: none.

### `labelText`

- JS: `imgui.labelText(label, text) -> undefined`
- C++: `IMGUI_API void LabelText(const char* label, const char* fmt, ...)
  IM_FMTARGS(2);` (`imgui.h:643`), called as `LabelText(label, "%s",
  text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:387-424; the item is submitted with
  ID 0, widgets:416, so label-id does not apply).
- Notes: none.

## Widgets

### `button`

- JS: `imgui.button(label, size = [0, 0]) -> boolean`
- C++: `IMGUI_API bool Button(const char* label, const ImVec2& size =
  ImVec2(0, 0));` (`imgui.h:652`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(bb, id)`, widgets:807).
- Notes: none.

### `smallButton`

- JS: `imgui.smallButton(label) -> boolean`
- C++: `IMGUI_API bool SmallButton(const char* label);` (`imgui.h:653`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (through `ButtonEx`, widgets:841, 807).
- Notes: none.

### `invisibleButton`

- JS: `imgui.invisibleButton(strId, size, flags = 0) -> boolean`
- C++: `IMGUI_API bool InvisibleButton(const char* str_id, const ImVec2&
  size, ImGuiButtonFlags flags = 0);` (`imgui.h:654`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none (the private `ImGuiButtonFlags` bits,
  which would be forwarded to `ButtonBehavior` unchanged, widgets:865,
  are rejected by G1).
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(bb, id, ...)`, widgets:861). No assertion
  on `size` in this version (a zero component is replaced, widgets:856).
- Notes: none.

### `arrowButton`

- JS: `imgui.arrowButton(strId, dir) -> boolean`
- C++: `IMGUI_API bool ArrowButton(const char* str_id, ImGuiDir dir);`
  (`imgui.h:655`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: `Dir_None` and `Dir_COUNT` (G1 leaves the
  exported enumerators -1 .. 4).
- Label-id: yes.
- Preconditions:
  - `dir` is one of the four directions: `RenderArrow` has
    `IM_ASSERT(0)` for `ImGuiDir_None` and `ImGuiDir_COUNT`
    (draw:7593-7596, plain), reached from widgets:894. Any other value
    falls through the `switch` and draws a degenerate triangle. (a).
  - label-id (`ItemAdd(bb, id)`, widgets:883).
- Notes: none.

### `checkbox`

- JS: `imgui.checkbox(label, v) -> boolean`, `v: ref<bool>`
- C++: `IMGUI_API bool Checkbox(const char* label, bool* v);`
  (`imgui.h:656`)
- Overload: only one. Nullable: none (`*v` is read without a check,
  widgets:1272). Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(total_bb, id)`, widgets:1267).
- Notes: none.

### `radioButton`

- JS, dispatched on the type of the second argument:
  - `imgui.radioButton(label, active) -> boolean` (a boolean)
  - `imgui.radioButton(label, v, vButton) -> boolean`, `v: ref<int>` (an
    `Array`)
- C++:
  - `IMGUI_API bool RadioButton(const char* label, bool active);`
    (`imgui.h:659`)
  - `IMGUI_API bool RadioButton(const char* label, int* v, int
    v_button);` (`imgui.h:660`)
- Overload: both. Nullable: none (`*v` is read without a check,
  widgets:1433). Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(total_bb, id)`, widgets:1392).
- Notes: none.

### `progressBar`

- JS: `imgui.progressBar(fraction, size = [-FLT_MIN, 0], overlay = null)
  -> undefined`
- C++: `IMGUI_API void ProgressBar(float fraction, const ImVec2& size_arg
  = ImVec2(-FLT_MIN, 0), const char* overlay = NULL);` (`imgui.h:661`)
- Overload: only one. Nullable: `overlay`. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1440-1500; item ID 0, widgets:1453).
- Notes: the default of `size` is `ImVec2(-FLT_MIN, 0)`, that is
  `[-imgui.FLT_MIN, 0]` (G4).

### `bullet`

- JS: `imgui.bullet() -> undefined`
- C++: `IMGUI_API void Bullet();` (`imgui.h:662`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:1502-1512).
- Notes: none.

### `sliderFloat`

- JS: `imgui.sliderFloat(label, v, vMin, vMax, format = '%.3f', flags =
  0) -> boolean`, `v: ref<float>`
- C++: `IMGUI_API bool SliderFloat(const char* label, float* v, float
  v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags
  = 0);` (`imgui.h:716`)
- Overload: only one. Nullable: none in JavaScript (C++ accepts a NULL
  `format`, widgets:3394-3395; an omitted argument passes the declared
  default instead). Omitted: none.
- Frame: open.
- Rejected: a `format` that is not exactly one float conversion (design);
  `ImGuiSliderFlags_WrapAround`; bounds outside `-FLT_MAX/2 ..
  FLT_MAX/2`.
- Label-id: yes.
- Preconditions (in `SliderBehavior`, all plain, all (a)):
  - `flags == 1 || (flags & ImGuiSliderFlags_InvalidMask_) == 0`
    (widgets:3309): covered by G1, which rejects the value 1 too.
  - `(flags & ImGuiSliderFlags_WrapAround) == 0` (widgets:3310).
  - `v_min >= -FLT_MAX / 2.0f && v_max <= FLT_MAX / 2.0f` (widgets:3358);
    NaN fails it.
  - label-id (`ItemAdd(total_bb, id, &frame_bb, ...)`, widgets:3390).
- Notes: ImGui executes `format` (design, "Calling convention").

### `sliderInt`

- JS: `imgui.sliderInt(label, v, vMin, vMax, format = '%d', flags = 0) ->
  boolean`, `v: ref<int>`
- C++: `IMGUI_API bool SliderInt(const char* label, int* v, int v_min, int
  v_max, const char* format = "%d", ImGuiSliderFlags flags = 0);`
  (`imgui.h:721`)
- Overload: only one. Nullable: none in JavaScript. Omitted: none.
- Frame: open.
- Rejected: a `format` that is not exactly one integer conversion;
  `ImGuiSliderFlags_WrapAround`; bounds outside `INT_MIN/2 ..
  INT_MAX/2`.
- Label-id: yes.
- Preconditions (in `SliderBehavior`, plain, (a)): widgets:3309 and 3310
  as for `sliderFloat`; `v_min >= IM_S32_MIN / 2 && v_max <= IM_S32_MAX /
  2` (widgets:3346); label-id (widgets:3390).
- Notes: none.

### `sliderFloat2`

- JS: `imgui.sliderFloat2(label, v, vMin, vMax, format = '%.3f', flags =
  0) -> boolean`, `v: ref<float>[2]`
- C++: `IMGUI_API bool SliderFloat2(const char* label, float v[2], float
  v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags
  = 0);` (`imgui.h:717`)
- Overload: only one. Nullable: none in JavaScript. Omitted: none.
- Frame: open. Rejected: as `sliderFloat`.
- Preconditions: as `sliderFloat` (it calls `SliderScalar` once per
  component, widgets:3487), except label-id: `SliderScalarN` pushes `label` and
  the component index on the ID stack first (widgets:3477, 3482), so no
  component's ID equals the window's.
- Notes: it opens and closes a group and pushes item widths of its own
  (widgets:3476-3501).

### `sliderFloat3`

- JS: `imgui.sliderFloat3(label, v, vMin, vMax, format = '%.3f', flags =
  0) -> boolean`, `v: ref<float>[3]`
- C++: `IMGUI_API bool SliderFloat3(const char* label, float v[3], float
  v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags
  = 0);` (`imgui.h:718`)
- Overload: only one. Nullable: none in JavaScript. Omitted: none.
- Frame: open. Rejected: as `sliderFloat`.
- Preconditions: as `sliderFloat2`.
- Notes: none.

### `sliderFloat4`

- JS: `imgui.sliderFloat4(label, v, vMin, vMax, format = '%.3f', flags =
  0) -> boolean`, `v: ref<float>[4]`
- C++: `IMGUI_API bool SliderFloat4(const char* label, float v[4], float
  v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags
  = 0);` (`imgui.h:719`)
- Overload: only one. Nullable: none in JavaScript. Omitted: none.
- Frame: open. Rejected: as `sliderFloat`.
- Preconditions: as `sliderFloat2`.
- Notes: none.

### `dragFloat`

- JS: `imgui.dragFloat(label, v, vSpeed = 1, vMin = 0, vMax = 0, format =
  '%.3f', flags = 0) -> boolean`, `v: ref<float>`
- C++: `IMGUI_API bool DragFloat(const char* label, float* v, float
  v_speed = 1.0f, float v_min = 0.0f, float v_max = 0.0f, const char*
  format = "%.3f", ImGuiSliderFlags flags = 0);` (`imgui.h:697`)
- Overload: only one. Nullable: none in JavaScript (C++ accepts a NULL
  `format`, widgets:2778-2779). Omitted: none.
- Frame: open.
- Rejected: a `format` that is not exactly one float conversion.
- Label-id: yes.
- Preconditions: `flags == 1 || (flags & ImGuiSliderFlags_InvalidMask_)
  == 0` (widgets:2696, plain, in `DragBehavior`): covered by G1. label-id
  (widgets:2774). No assertion on the bounds was found (`v_min >= v_max`
  means unbounded).
- Notes: none.

### `dragInt`

- JS: `imgui.dragInt(label, v, vSpeed = 1, vMin = 0, vMax = 0, format =
  '%d', flags = 0) -> boolean`, `v: ref<int>`
- C++: `IMGUI_API bool DragInt(const char* label, int* v, float v_speed =
  1.0f, int v_min = 0, int v_max = 0, const char* format = "%d",
  ImGuiSliderFlags flags = 0);` (`imgui.h:702`)
- Overload: only one. Nullable: none in JavaScript. Omitted: none.
- Frame: open.
- Rejected: a `format` that is not exactly one integer conversion.
- Label-id: yes.
- Preconditions: widgets:2696 as for `dragFloat` (G1); label-id
  (widgets:2774).
- Notes: none.

### `inputText`

- JS: `imgui.inputText(label, buf, bufSize, flags = 0) -> boolean`, `buf:
  ref<string>`, `bufSize` an integer 1..2^24
- C++: `IMGUI_API bool InputText(const char* label, char* buf, size_t
  buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback
  = NULL, void* user_data = NULL);` (`imgui.h:734`)
- Overload: only one. Nullable: none.
- Omitted: `callback` and `user_data`, passed as NULL.
- Frame: open.
- Rejected: the six `ImGuiInputTextFlags_Callback*` bits (design);
  `ImGuiInputTextFlags_WordWrap` (single-line).
- Label-id: yes.
- Preconditions (all plain, all (a)):
  - `!(flags & ImGuiInputTextFlags_Multiline)` (widgets:4084): a
    private bit (internal:1125), rejected by G1.
  - `buf != NULL && buf_size >= 0` (widgets:4802): the binding owns the
    buffer; `buf_size` is cast to `int` (widgets:4085), which the
    capacity limit of 2^24 keeps positive.
  - `_CallbackCompletion` with `_AllowTabInput` (widgets:4804); and every
    `_Callback*` bit needs a callback: `_CallbackResize` asserts
    (widgets:4904), `_CallbackCompletion` / `_CallbackHistory` /
    `_CallbackEdit` / `_CallbackAlways` assert when the event occurs
    (widgets:5406-5408), and `_CallbackCharFilter` calls through the NULL
    pointer with no assertion (widgets:4619-4633).
  - `_WordWrap` requires multi-line (widgets:4807) and excludes
    `_Password` (widgets:4806).
  - "Is your input buffer properly zero-terminated?" (widgets:4932,
    4954): the binding always terminates the buffer (design, "Buffers").
  - label-id (`ItemAdd(total_bb, id, &frame_bb, ...)`, widgets:4882).
- Notes: none.

### `inputTextMultiline`

- JS: `imgui.inputTextMultiline(label, buf, bufSize, size = [0, 0], flags
  = 0) -> boolean`, `buf: ref<string>`
- C++: `IMGUI_API bool InputTextMultiline(const char* label, char* buf,
  size_t buf_size, const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags
  flags = 0, ImGuiInputTextCallback callback = NULL, void* user_data =
  NULL);` (`imgui.h:735`)
- Overload: only one. Nullable: none.
- Omitted: `callback` and `user_data`, passed as NULL.
- Frame: open.
- Rejected: the six `_Callback*` bits; `ImGuiInputTextFlags_ElideLeft`;
  `_WordWrap` together with `_Password`.
- Label-id: yes.
- Preconditions (plain, (a)): the callback rules of `inputText`;
  `_CallbackHistory` with multi-line (widgets:4803); `_ElideLeft` with
  multi-line (widgets:4805); `_WordWrap` with `_Password`
  (widgets:4806); the buffer rules (widgets:4802, 4932, 4954); label-id
  (widgets:4835).
- Notes: it adds `ImGuiInputTextFlags_Multiline` itself (widgets:4090),
  and it opens a group and a child window of its own.

### `inputTextWithHint`

- JS: `imgui.inputTextWithHint(label, hint, buf, bufSize, flags = 0) ->
  boolean`, `buf: ref<string>`
- C++: `IMGUI_API bool InputTextWithHint(const char* label, const char*
  hint, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0,
  ImGuiInputTextCallback callback = NULL, void* user_data = NULL);`
  (`imgui.h:736`)
- Overload: only one. Nullable: none (the convention makes a `const
  char*` nullable only where the row says so; `InputTextEx` treats a NULL
  hint as "no hint", but then the call is `inputText`).
- Omitted: `callback` and `user_data`, passed as NULL.
- Frame: open. Rejected: as `inputText`.
- Label-id: yes.
- Preconditions: as `inputText`, with the multi-line assertion at
  widgets:4095.
- Notes: none.

### `inputFloat`

- JS: `imgui.inputFloat(label, v, step = 0, stepFast = 0, format =
  '%.3f', flags = 0) -> boolean`, `v: ref<float>`
- C++: `IMGUI_API bool InputFloat(const char* label, float* v, float step
  = 0.0f, float step_fast = 0.0f, const char* format = "%.3f",
  ImGuiInputTextFlags flags = 0);` (`imgui.h:737`)
- Overload: only one. Nullable: none in JavaScript (C++ accepts a NULL
  `format`, widgets:3882-3883). Omitted: none.
- Frame: open.
- Rejected: a `format` that is not exactly one float conversion; the six
  `_Callback*` bits, as `inputText` (the flags are forwarded to
  `InputText` with no callback); `_WordWrap`.
- Label-id: yes.
- Preconditions: those of `inputText` that concern flags, (a). label-id
  applies on both paths of `InputScalar`: without step buttons it calls
  `InputText(label, ...)` (widgets:3913); with them it calls
  `PushID(label)` and `InputText("", ...)` (widgets:3905-3907), whose ID
  is the same hash, so an empty label at the root of a window gives the
  window's ID either way.
- Notes: `format` is executed by `DataTypeFormatString` (widgets:3891).

### `inputInt`

- JS: `imgui.inputInt(label, v, step = 1, stepFast = 100, flags = 0) ->
  boolean`, `v: ref<int>`
- C++: `IMGUI_API bool InputInt(const char* label, int* v, int step = 1,
  int step_fast = 100, ImGuiInputTextFlags flags = 0);` (`imgui.h:741`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open.
- Rejected: the six `_Callback*` bits, as `inputText`; `_WordWrap`.
- Label-id: yes.
- Preconditions: those of `inputText` that concern flags, (a); label-id as
  `inputFloat`.
- Notes: there is no `format` parameter; ImGui picks `"%08X"` or `"%d"`
  from `ImGuiInputTextFlags_CharsHexadecimal` (widgets:4041).

### `beginCombo`

- JS: `imgui.beginCombo(label, previewValue, flags = 0) -> boolean`
- C++: `IMGUI_API bool BeginCombo(const char* label, const char*
  preview_value, ImGuiComboFlags flags = 0);` (`imgui.h:679`)
- Overload: only one. Nullable: `preview_value` (tested against NULL at
  widgets:1966). Omitted: none.
- Frame: open.
- Rejected: `ImGuiComboFlags_NoArrowButton` with `_NoPreview`;
  `_WidthFitPreview` with `_NoPreview`; more than one of the four
  `_Height*` bits.
- Label-id: yes.
- Preconditions (plain, (a)):
  - not both `_NoArrowButton` and `_NoPreview` (widgets:1959).
  - `_WidthFitPreview` excludes `_NoPreview` (widgets:1961).
  - at most one `_Height*` bit: `ImIsPowerOfTwo(flags &
    ImGuiComboFlags_HeightMask_)` (widgets:2040, in `BeginComboPopup`,
    reached only when the popup is open and no size constraint is set).
  - label-id (`ItemAdd(total_bb, id, &bb)`, widgets:1971).
- Notes: `endCombo()` is called only when this returned true.

### `endCombo`

- JS: `imgui.endCombo() -> undefined`
- C++: `IMGUI_API void EndCombo();` (`imgui.h:680`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: the current window is the combo popup that the last
  open `beginCombo()` began. ImGui tests this itself, recoverably
  ("Calling EndCombo() in wrong window!", widgets:2096), but only after
  decrementing `g.BeginComboDepth` (widgets:2091), a counter nothing
  restores (see Notes). So the binding makes ImGui's comparison first.
  (b): `g.BeginComboDepth > 0`, and then `g.CurrentWindow->Name` equals
  `"##Combo_%02d"` formatted with `g.BeginComboDepth - 1` (the name ImGui
  builds at widgets:2094 after its decrement). The first condition is the
  unmatched `endCombo()`: ImGui would take the counter to -1 and compare
  with `"##Combo_-1"`. (Added in Task 5, which registers the two as
  `canEndCombo:no-combo` and `canEndCombo:wrong-window`.) When it does not, `ERR_IMGUI_USAGE` is thrown and ImGui
  is not called, so the counter is not disturbed. ImGui's own message is
  therefore never raised through the binding. `EndCombo()` then calls
  `EndPopup()` (widgets:2097).
- Notes: `g.BeginComboDepth` is touched in three places only: set to 0
  when the context is constructed (imgui.cpp:4549), incremented when a
  combo popup's `Begin()` has succeeded (widgets:2074-2083), decremented
  by `EndCombo()` (widgets:2091). A combo left open is closed by the
  window recovery as a window (`End()`), which does not decrement it, so
  without help the counter would stay one too high for the rest of the
  process. The binding's `newFrame()` therefore sets `g.BeginComboDepth =
  0` before calling `ImGui::NewFrame()`. Zero is always correct there:
  the counter counts combo popup windows currently begun, and no window
  is begun at that point, because `EndFrame()` closes every window left
  on the stack (imgui.cpp:6432-6433, 11981-12007) and `NewFrame()`
  asserts that the previous frame was ended (imgui.cpp:11839). This is
  the binding's one write to ImGui's internal state besides installing
  the error callback; see "Internal state read by bind_guards.cpp".
  Within the frame in which a combo was left open the counter is one too
  high after the recovery, which only changes the popup window name a
  later combo in that frame uses (widgets:2055).

### `selectable`

- JS, dispatched on the type of the second argument:
  - `imgui.selectable(label, selected = false, flags = 0, size = [0, 0])
    -> boolean` (a boolean, `undefined` or `null`)
  - `imgui.selectable(label, pSelected, flags = 0, size = [0, 0]) ->
    boolean`, `pSelected: ref<bool>` (an `Array`)
- C++:
  - `IMGUI_API bool Selectable(const char* label, bool selected = false,
    ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0));`
    (`imgui.h:783`)
  - `IMGUI_API bool Selectable(const char* label, bool* p_selected,
    ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0));`
    (`imgui.h:784`)
- Overload: both. Nullable: none (`*p_selected` is read without a check,
  widgets:7710). Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(bb, id, ...)`, widgets:7568, 7574). No
  plain assertion on arguments was found (widgets:7516-7716).
- Notes: none.

### `colorEdit3`

- JS: `imgui.colorEdit3(label, col, flags = 0) -> boolean`, `col:
  ref<float>[3]`
- C++: `IMGUI_API bool ColorEdit3(const char* label, float col[3],
  ImGuiColorEditFlags flags = 0);` (`imgui.h:752`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open.
- Rejected: more than one of `_DisplayRGB` / `_DisplayHSV` /
  `_DisplayHex`; both `_InputRGB` and `_InputHSV`; both `_PickerHueBar`
  and `_PickerHueWheel` (`ColorEdit4` forwards the picker bits,
  widgets:6096-6100, and `ColorPicker4` asserts plainly that exactly one
  is set when the picker opens, widgets:6237). The same three rules
  apply to `colorEdit4`. They do not apply to `colorButton`, which opens
  no picker and whose entry rejects nothing.
- Preconditions (in `ColorEdit4`, plain, (a)):
  `ImIsPowerOfTwo(flags & ImGuiColorEditFlags_DisplayMask_)`
  (widgets:5960) and `ImIsPowerOfTwo(flags &
  ImGuiColorEditFlags_InputMask_)` (widgets:5961). When the caller sets
  no bit of a mask the bit comes from `io.ConfigColorEditFlags`
  (widgets:5951-5958), which `NewFrame()` has checked
  (imgui.cpp:11848-11851), so only a caller passing two bits can fail.
- Notes: it adds `ImGuiColorEditFlags_NoAlpha` (widgets:5888). It opens
  a group and pushes `label` on the ID stack (widgets:5935-5936), so label-id
  does not apply.

### `colorEdit4`

- JS: `imgui.colorEdit4(label, col, flags = 0) -> boolean`, `col:
  ref<float>[4]`
- C++: `IMGUI_API bool ColorEdit4(const char* label, float col[4],
  ImGuiColorEditFlags flags = 0);` (`imgui.h:753`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: as `colorEdit3`.
- Preconditions: as `colorEdit3` (widgets:5960-5961).
- Notes: none.

### `colorButton`

- JS: `imgui.colorButton(descId, col, flags = 0, size = [0, 0]) ->
  boolean`, `col: vec4`
- C++: `IMGUI_API bool ColorButton(const char* desc_id, const ImVec4& col,
  ImGuiColorEditFlags flags = 0, const ImVec2& size = ImVec2(0, 0));`
  (`imgui.h:756`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(bb, id)`, widgets:6600). No plain
  assertion found (widgets:6588-6680).
- Notes: none.

### `beginListBox`

- JS: `imgui.beginListBox(label, size = [0, 0]) -> boolean`
- C++: `IMGUI_API bool BeginListBox(const char* label, const ImVec2& size
  = ImVec2(0, 0));` (`imgui.h:805`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id, through the child window it begins with
  `BeginChild(id, ...)` (widgets:8989), where `id` is `GetID(label)`
  (widgets:8959); `BeginChildEx` also asserts `id != 0` (imgui.cpp:6895),
  not checkable from the argument.
- Notes: when it returns true it has opened a group (widgets:8980) and a
  child window; `endListBox()` is called only then.

### `endListBox`

- JS: `imgui.endListBox() -> undefined`
- C++: `IMGUI_API void EndListBox();` (`imgui.h:806`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - `window->Flags & ImGuiWindowFlags_ChildWindow` (widgets:8997, plain).
    (b): `g.CurrentWindow->Flags`.
  - It then calls `EndChild()` (widgets:9000) and `EndGroup()`
    (widgets:9001). `EndGroup()` runs in the parent window and asserts
    plainly `g.GroupStack.Size > 0` (imgui.cpp:12719) and that the top
    group belongs to that window (imgui.cpp:12722). (b): `g.GroupStack`,
    and the window below the current one on `g.CurrentWindowStack` (or
    `g.CurrentWindow->ParentWindow`).
  - `EndChild()` ends the current window with `End()`, so `end`'s last
    condition applies: not a scrolling table's own region, which is a
    child window and so passes the first check (b):
    `canEndListBox:table-region` (Task 6), tested before the group.
- Notes: ImGui's first check is satisfied by any child window, so an
  `endListBox()` inside a plain `beginChild()` passes it and then fails
  the group check unless a group happens to be open in the parent; the
  second guard is what makes the call safe.

## Trees and tabs

### `treeNode`

- JS: `imgui.treeNode(label) -> boolean`
- C++: `IMGUI_API bool TreeNode(const char* label);` (`imgui.h:760`)
- Overload: that one. NOT bound: `TreeNode(const char* str_id, const
  char* fmt, ...)` (`imgui.h:761`) and `TreeNode(const void* ptr_id, const
  char* fmt, ...)` (`imgui.h:762`), nor `TreeNodeV` (`imgui.h:763-764`).
  Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (`ItemAdd(interact_bb, id)`, widgets:7079, 7085). No
  plain assertion on arguments was found in `TreeNodeBehavior`
  (widgets:7022-7372).
- Notes: `treePop()` is called only when this returned true.

### `treeNodeEx`

- JS: `imgui.treeNodeEx(label, flags = 0) -> boolean`
- C++: `IMGUI_API bool TreeNodeEx(const char* label, ImGuiTreeNodeFlags
  flags = 0);` (`imgui.h:765`)
- Overload: that one. NOT bound: `TreeNodeEx(const char* str_id,
  ImGuiTreeNodeFlags flags, const char* fmt, ...)` (`imgui.h:766`),
  `TreeNodeEx(const void* ptr_id, ...)` (`imgui.h:767`), nor
  `TreeNodeExV` (`imgui.h:768-769`). Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (widgets:7079, 7085).
- Notes: with `ImGuiTreeNodeFlags_NoTreePushOnOpen` an open node pushes
  nothing (widgets:7122-7123, 7302-7303), so `treePop()` must not follow;
  the `treePop` guard below reads the depth and so gets this right.

### `treePop`

- JS: `imgui.treePop() -> undefined`
- C++: `IMGUI_API void TreePop();` (`imgui.h:772`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - `window->DC.TreeDepth > 0`: relied on without a check; the function
    decrements it and shifts by it (widgets:7405-7406). (b):
    `g.CurrentWindow->DC.TreeDepth`.
  - `window->IDStack.Size > 1` (widgets:7426, plain). (b):
    `g.CurrentWindow->IDStack.Size`.
  - `data->ID == window->IDStack.back()` (widgets:7410, plain), tested
    only when `window->DC.TreeHasStackDataDepthMask` has the bit of the
    depth being popped (widgets:7406-7407). It fails when a `pushID` made
    inside the node was not popped before `treePop()`. (b): under the
    same condition, `g.TreeNodeStack.back().ID ==
    g.CurrentWindow->IDStack.back()`. The mask of a depth is the macro
    `TREE_NODE_GET_DEPTH_MASK` (widgets:7019-7020), which is private to
    `imgui_widgets.cpp`, so `bind_guards.cpp` repeats its expression
    (`depth < 32 ? 1u << depth : 0`) with the depth `TreeDepth - 1`.
  - (c) `PopID()` (widgets:7427) cannot fail once the second condition
    holds.
- Notes: `TreeDepth` is per window and reset on a window's first
  `Begin()` of the frame (imgui.cpp:8761).

### `collapsingHeader`

- JS, dispatched on the type of the second argument:
  - `imgui.collapsingHeader(label, flags = 0) -> boolean` (a number or
    `undefined`)
  - `imgui.collapsingHeader(label, pVisible, flags = 0) -> boolean`,
    `pVisible: ref<bool>` (an `Array`, or `null`)
- C++:
  - `IMGUI_API bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags
    flags = 0);` (`imgui.h:774`)
  - `IMGUI_API bool CollapsingHeader(const char* label, bool* p_visible,
    ImGuiTreeNodeFlags flags = 0);` (`imgui.h:775`)
- Overload: both. Nullable: `p_visible`: `null` as the second argument
  selects the second overload and passes NULL, which C++ accepts
  (widgets:7479) and which then behaves as the first overload.
  Omitted: none.
- Frame: open. Rejected: none.
- Label-id: yes.
- Preconditions: label-id (through `TreeNodeBehavior`, widgets:7466,
  7079, 7085). (b).
- Notes: `undefined` as the second argument is the first overload with
  its default flags; this is the one place where `null` and `undefined`
  select different C++ functions.

### `setNextItemOpen`

- JS: `imgui.setNextItemOpen(isOpen, cond = 0) -> undefined`
- C++: `IMGUI_API void SetNextItemOpen(bool is_open, ImGuiCond cond =
  0);` (`imgui.h:776`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (widgets:7438-7446). Unlike
  `SetNextWindowPos`, there is no power-of-two assertion on `cond`; it is
  stored truncated to `ImU8` (widgets:7445).
- Notes: none.

### `beginTabBar`

- JS: `imgui.beginTabBar(strId, flags = 0) -> boolean`
- C++: `IMGUI_API bool BeginTabBar(const char* str_id, ImGuiTabBarFlags
  flags = 0);` (`imgui.h:973`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `tab_bar->ID != 0` (widgets:10005, plain): the hash of
  `strId`, not checkable from the argument; see "Not covered".
- Notes: `endTabBar()` is called only when this returned true.

### `endTabBar`

- JS: `imgui.endTabBar() -> undefined`
- C++: `IMGUI_API void EndTabBar();` (`imgui.h:974`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) `g.CurrentTabBar != NULL`: "Mismatched
  BeginTabBar()/EndTabBar()!" (widgets:10068), evaluated only when the
  window is not skipping items (widgets:10064-10065). (c) `PopID()`
  (widgets:10090).
- Notes: `g.CurrentTabBar` is global, not per window, so ImGui's check
  also passes in a child window of the tab bar's window.

### `beginTabItem`

- JS: `imgui.beginTabItem(label, pOpen = null, flags = 0) -> boolean`,
  `pOpen: ref<bool>`
- C++: `IMGUI_API bool BeginTabItem(const char* label, bool* p_open =
  NULL, ImGuiTabItemFlags flags = 0);` (`imgui.h:975`)
- Overload: only one. Nullable: `p_open`. Omitted: none.
- Frame: open.
- Rejected: `ImGuiTabItemFlags_Leading` together with `_Trailing`.
- Preconditions:
  - (c) `g.CurrentTabBar != NULL`: "Needs to be called between
    BeginTabBar() and EndTabBar()!" (widgets:10773), returns false.
  - `(flags & ImGuiTabItemFlags_Button) == 0` (widgets:10774, plain): a
    private bit (internal:3161), rejected by G1.
  - not both `_Leading` and `_Trailing` (widgets:10852, plain, in
    `TabItemEx`). (a).
- Notes: `endTabItem()` is called only when this returned true.

### `endTabItem`

- JS: `imgui.endTabItem() -> undefined`
- C++: `IMGUI_API void EndTabItem();` (`imgui.h:976`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions (evaluated only when the window is not skipping items,
  widgets:10789-10790):
  - (c) `g.CurrentTabBar != NULL`: "Needs to be called between
    BeginTabBar() and EndTabBar()!" (widgets:10793).
  - `tab_bar->LastTabItemIdx >= 0` (widgets:10794, plain). (b):
    `g.CurrentTabBar->LastTabItemIdx`, when `g.CurrentTabBar` is not
    NULL.
  - (c) `PopID()` (widgets:10797).
- Notes: the design lists `endTabItem` among the functions that assert
  plainly; in this version only the second condition does. That condition
  is weaker than correct pairing, as the design says: `LastTabItemIdx`
  stays set until `EndTabBar()` (widgets:10088), so a second
  `endTabItem()`, or one after a `beginTabItem()` that returned false,
  passes it and pops an ID that is not the tab's.

## Tables

`g.CurrentTable` follows the current window: `SetCurrentWindow()` sets it
from `window->DC.CurrentTableIdx` (imgui.cpp:4890), and `BeginTable()`
sets that index on both the outer and the inner window (tables:551, 553).
So inside a child window begun in a table cell there is no current table
(a window's first `Begin()` of the frame resets the index,
imgui.cpp:8228). A current table whose inner window is not the current
window is reached by appending to a scrolling table's outer window from
inside a cell: a second `begin()` of the outer window's own name makes it
current (imgui.cpp:8882), and its index names the table, whose inner
window is the child (measured, `ui-imgui-bind-errors.js`
TABLE_WRONG_WINDOW, which reads both facts back through
`__test.tableScope()` first). An `endChild()` in a scrolling table's
cell does not get there: it aborts inside `End()` (see `end`), and is
refused. The wrong-window condition below is therefore kept, applied
exactly where ImGui asserts it: in `EndTable()` always (tables:1476), and
in `TableEndRow()` (tables:2126), that is, in a row function only when a
row is open and the call ends it. In that state with no row open,
`tableNextRow`, `tableNextColumn` and `tableHeadersRow` are each accepted
and open a row (`ui-imgui-bind-errors.js` TABLE_WRONG_NOROW).

### `beginTable`

- JS: `imgui.beginTable(strId, columns, flags = 0, outerSize = [0, 0],
  innerWidth = 0) -> boolean`
- C++: `IMGUI_API bool BeginTable(const char* str_id, int columns,
  ImGuiTableFlags flags = 0, const ImVec2& outer_size = ImVec2(0.0f,
  0.0f), float inner_width = 0.0f);` (`imgui.h:923`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open.
- Rejected: `columns` outside 1..511; a negative (or NaN) `innerWidth`
  with `ImGuiTableFlags_ScrollX`.
- Preconditions (in `BeginTableEx`, plain, after the `SkipItems` return
  at tables:325-326):
  - `columns_count > 0 && columns_count < IMGUI_TABLE_MAX_COLUMNS`
    (tables:329; the limit is 512, internal:3235). (a).
  - with `ImGuiTableFlags_ScrollX`, `inner_width >= 0.0f` (tables:331).
    (a).
  - `table->ColumnsCount == columns_count` for a second `beginTable()` of
    the same ID in one frame (tables:394, "Cannot change columns count
    mid-frame while preserving same ID"). Not guarded; see "Not
    covered".
- Notes: `endTable()` is called only when this returned true.

### `endTable`

- JS: `imgui.endTable() -> undefined`
- C++: `IMGUI_API void EndTable();` (`imgui.h:924`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - (c) `g.CurrentTable != NULL`: "EndTable() call should only be done
    while in BeginTable() scope!" (tables:1461).
  - `inner_window == g.CurrentWindow && inner_window->ID ==
    temp_data->WindowID` (tables:1476, plain). (b): `g.CurrentWindow ==
    g.CurrentTable->InnerWindow` (`canTableRowCall:wrong-window`). The
    second half holds by construction for a table that is begun
    (`temp_data->WindowID` is set from the inner window, tables:470); see
    "Not covered" for one that is not.
  - `table->RowPosY2 == inner_window->DC.CursorPos.y` (tables:1493,
    plain). With a row open `TableEndRow()` makes it true (tables:2141);
    before the first row it is the table's top (tables:533) unless
    `setCursorScreenPos` moved the cursor, which the skipped cell items do
    not stop (measured: an abort). The layout `EndTable()` runs first
    changes neither (tables:1468, 846-1378). (b): `g.CurrentTable->
    IsInsideRow`, `RowPosY2` and `InnerWindow->DC.CursorPos.y`
    (`canEndTable:cursor-moved`), after the wrong-window check.
  - (c) "Mismatching PushID/PopID!" (tables:1591) and "Too many
    PopItemWidth!" (tables:1592), both non-returning forms: the table is
    ended anyway.
- Notes: none.

### `tableSetupColumn`

- JS: `imgui.tableSetupColumn(label, flags = 0, initWidthOrWeight = 0,
  userData = 0) -> undefined`
- C++: `IMGUI_API void TableSetupColumn(const char* label,
  ImGuiTableColumnFlags flags = 0, float init_width_or_weight = 0.0f,
  ImGuiID user_data = 0);` (`imgui.h:938`)
- Overload: only one. Nullable: `label` (tested against NULL at
  tables:1747, 1752; an empty string is equivalent). Omitted: none.
- Frame: open.
- Rejected: the four status bits `ImGuiTableColumnFlags_IsEnabled`,
  `_IsVisible`, `_IsSorted`, `_IsHovered` (`_StatusMask_`, imgui.h:2248);
  `_WidthStretch` together with `_WidthFixed`.
- Preconditions:
  - (c) `g.CurrentTable != NULL`: "Call should only be done while in
    BeginTable() scope!" (tables:1739).
  - (c) "TableSetupColumn(): called too many times!" (tables:1740).
  - (c) "TableSetupColumn(): need to call before first row!"
    (tables:1741).
  - `(flags & ImGuiTableColumnFlags_StatusMask_) == 0` (tables:1742,
    plain). (a).
  - `ImIsPowerOfTwo(flags & ImGuiTableColumnFlags_WidthMask_)` when
    either width bit is set (tables:802, plain, in
    `TableSetupColumnFlags`, called at tables:1728). (a).
  - (c) "TableSetupColumn(): can only specify width/weight if sizing
    policy is set explicitly in either Table or Column." (tables:1715).
    On the reconcile path (tables:1775) this one is raised later, from
    the layout that the first row function runs.
  - a non-empty label is stored at `(ImS16)table->ColumnsNames.size()`
    (tables:1747-1750; `NameOffset` is `ImS16`, internal:3262), and the
    buffer is cleared only when the table begins (tables:632-633). Once
    the names stored so far pass 32767 bytes the offset wraps, and
    `TableGetColumnName()` indexes the buffer with it (tables:1909): a
    negative one fails ImVector's plain range assertion (imgui.h:2369),
    reached from `tableHeadersRow` (tables:3259, 3307; measured). (b):
    `g.CurrentTable->ColumnsNames.size() <= 32767`, applied only when
    ImGui gets as far as storing the name (the three (c) checks above
    pass) (`canTableSetupColumn:name-offset`).
  - the column that completes the declaration with every column holding
    `TableColumnFlags_Disabled`: the layout every table runs (a row
    function, `EndTable()`, or the frame-end recovery's) enables only
    columns without it (tables:940) and asserts that one is enabled
    (tables:1005), so nothing could end the table after this call. (b):
    `DeclColumnsCount == ColumnsCount - 1`, the call's flags, and each
    declared column's flags, from `Columns[i].Flags` (tables:823) or, in
    reconcile mode, from the queued request (tables:1759-1789, 1840-1843)
    (`canTableSetupColumn:all-disabled`).
- Notes: `userData` is an `ImGuiID`, a number 0..2^32-1; it is only
  copied into sort specs, which are a non-goal.

### `tableSetupScrollFreeze`

- JS: `imgui.tableSetupScrollFreeze(cols, rows) -> undefined`
- C++: `IMGUI_API void TableSetupScrollFreeze(int cols, int rows);`
  (`imgui.h:939`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: `cols` outside 0..511; `rows` outside 0..127.
- Preconditions:
  - (c) `g.CurrentTable != NULL`: "Call should only be done while in
    BeginTable() scope!" (tables:1857).
  - `table->IsLayoutLocked == false` (tables:1858, plain): it must come
    before the first row. (b): `g.CurrentTable->IsLayoutLocked`, when
    `g.CurrentTable` is not NULL (`canTableSetupScrollFreeze:
    layout-locked`).
  - `columns >= 0 && columns < IMGUI_TABLE_MAX_COLUMNS` (tables:1859,
    plain). (a).
  - `rows >= 0 && rows < 128` (tables:1860, plain). (a).
- Notes: the same mistake is recoverable in `TableSetupColumn`
  (tables:1741) and a plain assertion here.

### `tableHeadersRow`

- JS: `imgui.tableHeadersRow() -> undefined`
- C++: `IMGUI_API void TableHeadersRow();` (`imgui.h:941`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - (c) `g.CurrentTable != NULL`: "Call should only be done while in
    BeginTable() scope!" (tables:3286).
  - It calls `TableNextRow()` (tables:3295), so `tableNextRow`'s second
    condition applies, when a row is open. (b): `canTableRowCall:
    wrong-window`.
- Notes: none.

### `tableNextRow`

- JS: `imgui.tableNextRow(rowFlags = 0, minRowHeight = 0) -> undefined`
- C++: `IMGUI_API void TableNextRow(ImGuiTableRowFlags row_flags = 0,
  float min_row_height = 0.0f);` (`imgui.h:925`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - `g.CurrentTable != NULL`: dereferenced without a check
    (tables:2062-2064). (b): `g.CurrentTable`.
  - when a row is open, `TableEndRow()` asserts `window ==
    table->InnerWindow` (tables:2126, plain, called at tables:2067). (b):
    `g.CurrentWindow == g.CurrentTable->InnerWindow` when
    `g.CurrentTable->IsInsideRow` (`canTableRowCall:wrong-window`; the
    first condition is `canTableRowCall:no-table`).
- Notes: this is the one table function that neither returns nor reports
  when there is no table.

### `tableNextColumn`

- JS: `imgui.tableNextColumn() -> boolean`
- C++: `IMGUI_API bool TableNextColumn();` (`imgui.h:926`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions:
  - with no current table it returns false (tables:2313-2314): safe, no
    guard needed.
  - when it wraps to a new row it calls `TableNextRow()` (tables:2324),
    so `tableNextRow`'s second condition applies (tables:2126). (b):
    `g.CurrentWindow == g.CurrentTable->InnerWindow`, when
    `g.CurrentTable` is not NULL, a row is open and the row is full
    (`IsInsideRow && CurrentColumn + 1 >= ColumnsCount`, tables:2316):
    inside the row it ends a cell and begins the next on the inner
    window (tables:2318-2320), which does not assert.
- Notes: the design's "table row and column functions outside a table"
  is, for this function and `tableSetColumnIndex`, a `false` return in
  this version, not a crash.

### `tableSetColumnIndex`

- JS: `imgui.tableSetColumnIndex(columnN) -> boolean`
- C++: `IMGUI_API bool TableSetColumnIndex(int column_n);`
  (`imgui.h:927`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: `columnN < 0` (`ERR_OUT_OF_RANGE`). After
  `TableNextRow()` the current column is -1 (tables:2092), so -1 skips
  the `if (table->CurrentColumn != column_n)` block that holds ImGui's
  range check and reaches `table->Columns[-1]` (tables:2295-2305). An
  index at or above the column count stays ImGui's recoverable error.
- Preconditions:
  - a table is current and a row is open: relied on without a check.
    `TableBeginCell()` (tables:2300) switches draw channels with
    `table->DrawSplitter->SetCurrentChannel()` (tables:2374, 2381), which
    asserts plainly `idx >= 0 && idx < _Count` (draw:3483); the channels
    exist only once the layout has run, which the first row function
    does (tables:2064-2065). (b): `g.CurrentTable != NULL` and
    `g.CurrentTable->IsInsideRow` (`canTableSetColumnIndex:no-table`,
    `:no-row`).
  - (c) `column_n < table->ColumnsCount` (the binding has already
    rejected a negative index): "TableSetColumnIndex() invalid column
    index!" (tables:2299), returns false.
- Notes: the guard is stricter than ImGui on purpose (ruling 12). With
  no current table ImGui itself returns false (tables:2292-2293); the
  binding throws `ERR_IMGUI_USAGE` instead, as it does with a table but
  no row, which is the call order ImGui documents (`TableNextRow()`
  first, imgui.h:918-921) and does not check.

## Popups and tooltips

### `openPopup`

- JS: `imgui.openPopup(strId, popupFlags = 0) -> boolean`
- C++: `IMGUI_API bool OpenPopup(const char* str_id, ImGuiPopupFlags
  popup_flags = 0);` (`imgui.h:878`)
- Overload: the `const char* str_id` one. NOT bound: `OpenPopup(ImGuiID
  id, ImGuiPopupFlags popup_flags = 0)` (`imgui.h:879`). Nullable: none.
  Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:13230-13236, 13249-13300).
- Notes: the return type is `bool` in this version. The `ImGuiID`
  overload is not bound (ruling 4).

### `beginPopup`

- JS: `imgui.beginPopup(strId, flags = 0) -> boolean`
- C++: `IMGUI_API bool BeginPopup(const char* str_id, ImGuiWindowFlags
  flags = 0);` (`imgui.h:865`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `(extra_window_flags & ImGuiWindowFlags_ChildMenu) == 0`
  (imgui.cpp:13436, plain, in `BeginPopupEx`, reached only when the popup
  is open): an internal window flag, rejected by G1.
- Notes: `endPopup()` is called only when this returned true (when the
  popup is open but `Begin()` returned false, `BeginPopupEx` has already
  called `EndPopup()`, imgui.cpp:13440-13441).

### `beginPopupModal`

- JS: `imgui.beginPopupModal(name, pOpen = null, flags = 0) -> boolean`,
  `pOpen: ref<bool>`
- C++: `IMGUI_API bool BeginPopupModal(const char* name, bool* p_open =
  NULL, ImGuiWindowFlags flags = 0);` (`imgui.h:866`)
- Overload: only one. Nullable: `p_open`. Omitted: none.
- Frame: open. Rejected: an empty `name`.
- Preconditions: `name` is passed to `Begin()` (imgui.cpp:13510), which
  asserts it is not empty (imgui.cpp:8003, plain). (a): a non-empty
  string. The internal window flags are rejected by G1.
- Notes: `*p_open` is written even when the function returns false: it
  is set to false when the popup is not open (imgui.cpp:13495-13496). The
  write-back rule ("outputs are written after the call") covers this.

### `beginPopupContextItem`

- JS: `imgui.beginPopupContextItem(strId = null, popupFlags = 0) ->
  boolean`
- C++: `IMGUI_API bool BeginPopupContextItem(const char* str_id = NULL,
  ImGuiPopupFlags popup_flags = 0);` (`imgui.h:892`)
- Overload: only one. Nullable: `str_id`. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions (after the `SkipItems` return at imgui.cpp:13614-13615):
  - `id != 0` (imgui.cpp:13617, plain): with a NULL `str_id` the popup's
    ID is the last item's, which is 0 after an item with no ID (`text`,
    `dummy`, ...). (b): when `strId` is null, `g.LastItemData.ID != 0`.
  - `(flags & ImGuiPopupFlags_InvalidMask_) == 0` (imgui.cpp:13548,
    plain, in `GetMouseButtonFromPopupFlags`, which is the branch
    compiled with `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`; called through
    imgui.cpp:13618 and 13558): bits 0 and 1 are in no exported
    enumerator, so rejected by G1.
- Notes: the flags here are `ImGuiPopupFlags`, not window flags. The
  default 0 means the right mouse button (imgui.cpp:13552).

### `endPopup`

- JS: `imgui.endPopup() -> undefined`
- C++: `IMGUI_API void EndPopup();` (`imgui.h:867`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) the current window has `ImGuiWindowFlags_Popup` and
  `g.BeginPopupStack.Size > 0`: "Calling EndPopup() in wrong window!"
  (imgui.cpp:13525). It then calls `End()` (imgui.cpp:13537).
- Notes: none.

### `closeCurrentPopup`

- JS: `imgui.closeCurrentPopup() -> undefined`
- C++: `IMGUI_API void CloseCurrentPopup();` (`imgui.h:881`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none; outside a popup it returns without doing anything
  (imgui.cpp:13398-13400).
- Notes: none.

### `setTooltip`

- JS: `imgui.setTooltip(text) -> undefined`
- C++: `IMGUI_API void SetTooltip(const char* fmt, ...) IM_FMTARGS(1);`
  (`imgui.h:844`), called as `SetTooltip("%s", text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:13092-13106; it begins and ends a
  tooltip window itself).
- Notes: none.

### `setItemTooltip`

- JS: `imgui.setItemTooltip(text) -> undefined`
- C++: `IMGUI_API void SetItemTooltip(const char* fmt, ...)
  IM_FMTARGS(1);` (`imgui.h:852`), called as `SetItemTooltip("%s",
  text)`.
- Overload: only one. Nullable: none. Omitted: the variadic arguments.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:13110-13117).
- Notes: none.

### `beginTooltip`

- JS: `imgui.beginTooltip() -> boolean`
- C++: `IMGUI_API bool BeginTooltip();` (`imgui.h:842`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:13027-13030, 13039-13084).
- Notes: in this version it always returns true (imgui.cpp:13078-13083).

### `endTooltip`

- JS: `imgui.endTooltip() -> undefined`
- C++: `IMGUI_API void EndTooltip();` (`imgui.h:843`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: `GetCurrentWindowRead()->Flags &
  ImGuiWindowFlags_Tooltip` (imgui.cpp:13088, plain). (b):
  `g.CurrentWindow->Flags`. It then calls `End()` (imgui.cpp:13089).
- Notes: none.

## Item and input queries

### `isItemHovered`

- JS: `imgui.isItemHovered(flags = 0) -> boolean`
- C++: `IMGUI_API bool IsItemHovered(ImGuiHoveredFlags flags = 0);`
  (`imgui.h:1057`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) flags outside
  `ImGuiHoveredFlags_AllowedMaskForIsItemHovered` (internal:1118):
  "Invalid flags for IsItemHovered()!" (imgui.cpp:5114), a non-returning
  form.
- Notes: none.

### `isItemClicked`

- JS: `imgui.isItemClicked(mouseButton = 0) -> boolean`
- C++: `IMGUI_API bool IsItemClicked(ImGuiMouseButton mouse_button = 0);`
  (`imgui.h:1060`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Accepted: the integers 0..4 (the exception to G1 for mouse
  buttons, below); anything else, `MouseButton_COUNT` (5) included, is
  `ERR_OUT_OF_RANGE`.
- Preconditions: `button >= 0 && button < IM_COUNTOF(g.IO.MouseDown)`
  (imgui.cpp:10766, plain, in `IsMouseClicked`, called at
  imgui.cpp:6759); the array has 5 elements (imgui.h:2742). (a).
- Notes: none.

### `isItemActive`

- JS: `imgui.isItemActive() -> boolean`
- C++: `IMGUI_API bool IsItemActive();` (`imgui.h:1058`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:6707-6712).
- Notes: none.

### `isItemDeactivatedAfterEdit`

- JS: `imgui.isItemDeactivatedAfterEdit() -> boolean`
- C++: `IMGUI_API bool IsItemDeactivatedAfterEdit();` (`imgui.h:1065`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:6733-6737).
- Notes: none.

### `getItemRectMin`

- JS: `imgui.getItemRectMin(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetItemRectMin();` (`imgui.h:1071`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:6854-6858).
- Notes: none.

### `getItemRectMax`

- JS: `imgui.getItemRectMax(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetItemRectMax();` (`imgui.h:1072`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:6860-6864).
- Notes: none.

### `setItemDefaultFocus`

- JS: `imgui.setItemDefaultFocus() -> undefined`
- C++: `IMGUI_API void SetItemDefaultFocus();` (`imgui.h:1045`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found on the entry path (imgui.cpp:9716-9730).
- Notes: none.

### `setKeyboardFocusHere`

- JS: `imgui.setKeyboardFocusHere(offset = 0) -> undefined`
- C++: `IMGUI_API void SetKeyboardFocusHere(int offset = 0);`
  (`imgui.h:1046`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: an `offset` below -1.
- Preconditions: `offset >= -1` (imgui.cpp:9687, plain). (a).
- Notes: none.

### `isKeyPressed`

- JS: `imgui.isKeyPressed(key, repeat = true) -> boolean`
- C++: `IMGUI_API bool IsKeyPressed(ImGuiKey key, bool repeat = true);`
  (`imgui.h:1111`)
- Overload: only one (the `ImGuiInputFlags` / owner form is internal).
  Nullable: none. Omitted: none.
- Frame: open. Rejected: the exported enumerators that are not keys:
  `Key_None`, `Mod_None` (both 0), `Key_NamedKey_END` and
  `Key_NamedKey_COUNT`.
- Preconditions: `IsNamedKey(key)` after single-modifier conversion
  (imgui.cpp:10283-10286, plain, in `GetKeyData`, called at
  imgui.cpp:10692). (a): `key` is in `Key_NamedKey_BEGIN` (512) ..
  `Key_NamedKey_END - 1`, or exactly one of `Mod_Ctrl`, `Mod_Shift`,
  `Mod_Alt`, `Mod_Super` (`ConvertSingleModFlagToKey`, internal:3879).
  G1 already rejects everything that is not an exported enumerator
  (legacy values below 512, combined modifiers, the four
  `ImGuiKey_ReservedForMod*` values); the "Rejected" line adds the
  exported enumerators that fail the assertion.
- Notes: the default of `repeat` is true.

### `isKeyDown`

- JS: `imgui.isKeyDown(key) -> boolean`
- C++: `IMGUI_API bool IsKeyDown(ImGuiKey key);` (`imgui.h:1110`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: as `isKeyPressed`.
- Preconditions: as `isKeyPressed` (imgui.cpp:10286, through
  imgui.cpp:10676). (a).
- Notes: none.

### `isMouseDown`

- JS: `imgui.isMouseDown(button) -> boolean`
- C++: `IMGUI_API bool IsMouseDown(ImGuiMouseButton button);`
  (`imgui.h:1154`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Accepted: the integers 0..4 (the exception to G1 for mouse
  buttons, below); anything else, `MouseButton_COUNT` (5) included, is
  `ERR_OUT_OF_RANGE`.
- Preconditions: `button >= 0 && button < IM_COUNTOF(g.IO.MouseDown)`
  (imgui.cpp:10747, plain). (a).
- Notes: `MouseButton_COUNT` is 5 (imgui.h:2090) and is not itself a
  valid button.

### `isMouseClicked`

- JS: `imgui.isMouseClicked(button, repeat = false) -> boolean`
- C++: `IMGUI_API bool IsMouseClicked(ImGuiMouseButton button, bool
  repeat = false);` (`imgui.h:1155`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Accepted: the integers 0..4 (the exception to G1 for mouse
  buttons, below); anything else, `MouseButton_COUNT` (5) included, is
  `ERR_OUT_OF_RANGE`.
- Preconditions: imgui.cpp:10766, plain (through imgui.cpp:10760). (a).
- Notes: none.

### `isMouseDoubleClicked`

- JS: `imgui.isMouseDoubleClicked(button) -> boolean`
- C++: `IMGUI_API bool IsMouseDoubleClicked(ImGuiMouseButton button);`
  (`imgui.h:1157`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Accepted: the integers 0..4 (the exception to G1 for mouse
  buttons, below); anything else, `MouseButton_COUNT` (5) included, is
  `ERR_OUT_OF_RANGE`.
- Preconditions: imgui.cpp:10819, plain. (a).
- Notes: none.

### `getMousePos`

- JS: `imgui.getMousePos(out?: vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetMousePos();` (`imgui.h:1163`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (imgui.cpp:10933-10937).
- Notes: with no mouse the value is `(-FLT_MAX, -FLT_MAX)` (the
  convention stated beside `IsMousePosValid`, imgui.h:1161), which
  converts to two finite numbers.

### `getMouseDragDelta`

- JS: `imgui.getMouseDragDelta(button = 0, lockThreshold = -1, out?:
  vec2) -> vec2`
- C++: `IMGUI_API ImVec2 GetMouseDragDelta(ImGuiMouseButton button = 0,
  float lock_threshold = -1.0f);` (`imgui.h:1166`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Accepted: the integers 0..4 (the exception to G1 for mouse
  buttons, below); anything else, `MouseButton_COUNT` (5) included, is
  `ERR_OUT_OF_RANGE`.
- Preconditions: imgui.cpp:10985, plain. (a).
- Notes: none.

## Style

### `styleColorsDark`

- JS: `imgui.styleColorsDark() -> undefined`
- C++: `IMGUI_API void StyleColorsDark(ImGuiStyle* dst = NULL);`
  (`imgui.h:435`)
- Overload: only one. Nullable: none in JavaScript.
- Omitted: `dst`, passed as NULL (the current context's style).
- Frame: either. With no window it throws `ERR_IMGUI_NO_WINDOW`.
  Rejected: none.
- Preconditions: with `dst == NULL` it calls `ImGui::GetStyle()`
  (draw:189), which asserts `GImGui != NULL` (imgui.cpp:3758, plain):
  satisfied by context creation.
- Notes: it overwrites `style.Colors` directly, under any colours pushed
  with `pushStyleColor`; a later `popStyleColor` restores the value saved
  at the push, not the new theme's. That is ImGui's behaviour, the same
  in C++.

### `styleColorsLight`

- JS: `imgui.styleColorsLight() -> undefined`
- C++: `IMGUI_API void StyleColorsLight(ImGuiStyle* dst = NULL);`
  (`imgui.h:436`)
- Overload: only one. Nullable: none in JavaScript.
- Omitted: `dst`, passed as NULL.
- Frame: either. With no window it throws `ERR_IMGUI_NO_WINDOW`.
  Rejected: none.
- Preconditions: as `styleColorsDark` (draw:330).
- Notes: none.

### `pushStyleColor`

- JS, dispatched on the type of the second argument:
  - `imgui.pushStyleColor(idx, col) -> undefined`, `col` a number
    (`ImU32`, G3)
  - `imgui.pushStyleColor(idx, col) -> undefined`, `col: vec4` (an
    `Array`)
- C++:
  - `IMGUI_API void PushStyleColor(ImGuiCol idx, ImU32 col);`
    (`imgui.h:547`)
  - `IMGUI_API void PushStyleColor(ImGuiCol idx, const ImVec4& col);`
    (`imgui.h:548`)
- Overload: both. Nullable: none. Omitted: none.
- Frame: open. Rejected: `Col_COUNT`.
- Preconditions: `idx` indexes `g.Style.Colors[idx]` without a check
  (imgui.cpp:3801, 3812). (a): G1 limits `idx` to the exported
  enumerators, 0 .. `Col_COUNT`, and the function rejects `Col_COUNT`.
- Notes: a push without its pop is reported when the window ends, (c)
  "Missing PopStyleColor()" (imgui.cpp:12073).

### `popStyleColor`

- JS: `imgui.popStyleColor(count = 1) -> undefined`
- C++: `IMGUI_API void PopStyleColor(int count = 1);` (`imgui.h:549`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) `g.ColorStack.Size < count`: "Calling
  PopStyleColor() too many times!" (imgui.cpp:3823); it then pops what
  there is. A zero or negative `count` pops nothing (imgui.cpp:3826).
- Notes: none.

### `pushStyleVar`

- JS, dispatched on the type of the second argument:
  - `imgui.pushStyleVar(idx, val) -> undefined`, `val` a number
  - `imgui.pushStyleVar(idx, val) -> undefined`, `val: vec2` (an `Array`)
- C++:
  - `IMGUI_API void PushStyleVar(ImGuiStyleVar idx, float val);`
    (`imgui.h:550`)
  - `IMGUI_API void PushStyleVar(ImGuiStyleVar idx, const ImVec2& val);`
    (`imgui.h:551`)
- Overload: both. NOT bound: `PushStyleVarX` (`imgui.h:552`) and
  `PushStyleVarY` (`imgui.h:553`). Nullable: none. Omitted: none.
- Frame: open. Rejected: `StyleVar_COUNT`.
- Preconditions:
  - `idx >= 0 && idx < ImGuiStyleVar_COUNT` (imgui.cpp:3891, plain, in
    `GetStyleVarInfo`). (a).
  - (c) the variable's type does not match the overload: "Calling
    PushStyleVar() variant with wrong type!" (imgui.cpp:3900 for the
    `float` overload, 3930 for the `ImVec2` one). Both are `_RET` forms:
    nothing is pushed.
- Notes: because nothing is pushed on a type mismatch, the caller's
  matching `popStyleVar()` then pops something else or reports "too many
  times". Which variables take which type is listed under "Constants".

### `popStyleVar`

- JS: `imgui.popStyleVar(count = 1) -> undefined`
- C++: `IMGUI_API void PopStyleVar(int count = 1);` (`imgui.h:554`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) `g.StyleVarStack.Size < count`: "Calling
  PopStyleVar() too many times!" (imgui.cpp:3941); it then pops what
  there is.
- Notes: none.

### `getColorU32`

- JS, dispatched on the type of the first argument (design,
  "Exceptions"):
  - `imgui.getColorU32(idx, alphaMul = 1) -> number` (a number: the
    `ImGuiCol` overload)
  - `imgui.getColorU32(col) -> number`, `col: vec4` (an `Array`)
- C++:
  - `IMGUI_API ImU32 GetColorU32(ImGuiCol idx, float alpha_mul = 1.0f);`
    (`imgui.h:569`)
  - `IMGUI_API ImU32 GetColorU32(const ImVec4& col);` (`imgui.h:570`)
- Overload: those two. NOT bound: `GetColorU32(ImU32 col, float alpha_mul
  = 1.0f)` (`imgui.h:571`). Nullable: none. Omitted: none.
- Frame: open. Rejected: `Col_COUNT`.
- Preconditions: `idx` indexes `style.Colors[idx]` without a check
  (imgui.cpp:3765). (a).
- Notes: returns an `ImU32`, a number 0..2^32-1 (G3).

### `colorConvertFloat4ToU32`

- JS: `imgui.colorConvertFloat4ToU32(col) -> number`, `col: vec4`
- C++: `IMGUI_API ImU32 ColorConvertFloat4ToU32(const ImVec4& in);`
  (`imgui.h:1102`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: none needed, and no context: it never throws
  `ERR_IMGUI_NO_FRAME` or `ERR_IMGUI_NO_WINDOW`. Rejected: none.
- Preconditions: none (imgui.cpp:3003-3011; a pure function that
  saturates each component and reads no context).
- Notes: the only bound function with no context requirement, besides
  the `version` property.

## Plots

### `plotLines`

- JS: `imgui.plotLines(label, values, valuesOffset = 0, overlayText =
  null, scaleMin = FLT_MAX, scaleMax = FLT_MAX, graphSize = [0, 0]) ->
  undefined`, `values` a `Float32Array` (used in place) or an `Array` of
  numbers
- C++: `IMGUI_API void PlotLines(const char* label, const float* values,
  int values_count, int values_offset = 0, const char* overlay_text =
  NULL, float scale_min = FLT_MAX, float scale_max = FLT_MAX, ImVec2
  graph_size = ImVec2(0, 0), int stride = sizeof(float));` (`imgui.h:812`)
- Overload: that one. NOT bound: the `values_getter` overload
  (`imgui.h:813`). Nullable: `overlay_text`.
- Omitted: `values_count` (the array's length) and `stride` (passed as
  `sizeof(float)`) (design, "Exceptions: Plots").
- Frame: open. Rejected: `valuesOffset` outside 0 .. length - 1 (0 for
  an empty array) (design, "Buffers").
- Label-id: yes.
- Preconditions (in `PlotEx`):
  - `values_offset >= 0`: relied on without a check; the element read is
    `(index + values_offset) % values_count` (widgets:9126-9127, 9138,
    9151), which is negative for a negative offset. (a).
  - the sum `index + values_offset + 1` must not overflow `int`
    (widgets:9151): implied by the design's offset rule once the length
    is at most `INT_MAX / 2`. (a): the binding limits the length.
  - `IM_ASSERT(v_idx >= 0 && v_idx < values_count)` (widgets:9124) and
    `IM_ASSERT(v1_idx >= 0 && v1_idx < values_count)` (widgets:9150) are
    computed from ImGui's own values and hold for any count at or above
    the minimum tested at widgets:9114.
  - label-id (`ItemAdd(total_bb, id, &frame_bb, ...)`, widgets:9086).
- Notes: `scale_min` / `scale_max` use `FLT_MAX` as "compute from the
  data" (widgets:9092); the value is `imgui.FLT_MAX` (G4).

### `plotHistogram`

- JS: `imgui.plotHistogram(label, values, valuesOffset = 0, overlayText =
  null, scaleMin = FLT_MAX, scaleMax = FLT_MAX, graphSize = [0, 0]) ->
  undefined`
- C++: `IMGUI_API void PlotHistogram(const char* label, const float*
  values, int values_count, int values_offset = 0, const char*
  overlay_text = NULL, float scale_min = FLT_MAX, float scale_max =
  FLT_MAX, ImVec2 graph_size = ImVec2(0, 0), int stride =
  sizeof(float));` (`imgui.h:814`)
- Overload: that one. NOT bound: the `values_getter` overload
  (`imgui.h:815`). Nullable: `overlay_text`.
- Omitted: `values_count` and `stride`.
- Frame: open. Rejected: as `plotLines`.
- Label-id: yes.
- Preconditions: as `plotLines` (the same `PlotEx`, called at
  widgets:9218).
- Notes: none.

## Draw list

Every function here is on `imgui.drawList` and acts on
`ImGui::GetWindowDrawList()` (`IMGUI_API ImDrawList*
GetWindowDrawList();`, `imgui.h:482`; imgui.cpp:9546-9550), the current
window's draw list. All need an open frame. Colours are `ImU32` numbers.
`IM_ASSERT_PARANOID` checks in `imgui_draw.cpp` are compiled out
(`IMGUI_DEBUG_PARANOID` is not defined, internal:278-282) and are not
listed.

A draw list holds at most 65535 vertices per draw command with the
16-bit `ImDrawIdx` (imgui.h:3363). Part A's renderer sets
`ImGuiBackendFlags_RendererHasVtxOffset`
(`addons/imgui/imgui_renderer.cpp:577`), so `PrimReserve` starts a new
command when a primitive would cross that limit (draw:768-775), but it
does not test a single primitive's own size (draw:770), and `Render()`
asserts plainly `draw_list->_VtxCurrentIdx < (1 << 16)` (draw:3554). The
only bound functions that can emit an unbounded primitive are
`addPolyline` and `addBezierCubic`; each has a cap for this reason
(ruling 14).

### `drawList.addLine`

- JS: `imgui.drawList.addLine(p1, p2, col, thickness = 1, flags = 0) ->
  undefined`
- C++: `IMGUI_API void AddLine(const ImVec2& p1, const ImVec2& p2, ImU32
  col, float thickness = 1.0f, ImDrawFlags flags = 0);` (`imgui.h:3586`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (draw:1993-2007, and `_AddLine`,
  draw:1866-1992). `ImDrawFlags_InvalidMask_` bits are not tested on this
  path.
- Notes: none.

### `drawList.addRect`

- JS: `imgui.drawList.addRect(pMin, pMax, col, rounding = 0, thickness =
  1, flags = 0) -> undefined`
- C++: `IMGUI_API void AddRect(const ImVec2& p_min, const ImVec2& p_max,
  ImU32 col, float rounding = 0.0f, float thickness = 1.0f, ImDrawFlags
  flags = 0);` (`imgui.h:3589`)
- Overload: that one. The former order (`flags` before `thickness`) is a
  deleted overload under the addon's configuration (`imgui.h:3684`).
  Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: (c) `(flags & ImDrawFlags_InvalidMask_) == 0`:
  "Incorrect parameter. Did you swap 'thickness' and 'flags'?"
  (draw:2353), a `_RET` form. The mask is `~0x7FFFFFF0` (imgui.h:3512):
  bits 0..3 and bit 31, none of which is in an exported enumerator, so
  G1 rejects them first and the message is not raised through the
  binding.
- Notes: none.

### `drawList.addRectFilled`

- JS: `imgui.drawList.addRectFilled(pMin, pMax, col, rounding = 0, flags
  = 0) -> undefined`
- C++: `IMGUI_API void AddRectFilled(const ImVec2& p_min, const ImVec2&
  p_max, ImU32 col, float rounding = 0.0f, ImDrawFlags flags = 0);`
  (`imgui.h:3590`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found on the entry path (draw:2541-2556).
- Notes: none.

### `drawList.addCircle`

- JS: `imgui.drawList.addCircle(center, radius, col, numSegments = 0,
  thickness = 1, flags = 0) -> undefined`
- C++: `IMGUI_API void AddCircle(const ImVec2& center, float radius,
  ImU32 col, int num_segments = 0, float thickness = 1.0f, ImDrawFlags
  flags = 0);` (`imgui.h:3596`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found. `num_segments` is clamped to 3..512
  (draw:2747; `IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_MAX`, internal:924), a
  value at or below 0 means automatic (draw:2735, 2745), and the radius
  is raised to 0.01 (draw:2716).
- Notes: none.

### `drawList.addCircleFilled`

- JS: `imgui.drawList.addCircleFilled(center, radius, col, numSegments =
  0) -> undefined`
- C++: `IMGUI_API void AddCircleFilled(const ImVec2& center, float radius,
  ImU32 col, int num_segments = 0);` (`imgui.h:3597`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found; `num_segments` is clamped as in `addCircle`
  (draw:2791).
- Notes: none.

### `drawList.addTriangleFilled`

- JS: `imgui.drawList.addTriangleFilled(p1, p2, p3, col) -> undefined`
- C++: `IMGUI_API void AddTriangleFilled(const ImVec2& p1, const ImVec2&
  p2, const ImVec2& p3, ImU32 col);` (`imgui.h:3595`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: none.
- Preconditions: none found (draw:2687-2694).
- Notes: filled shapes are expected in clockwise order (imgui.h:3580);
  the other order only changes the anti-aliasing fringe.

### `drawList.addBezierCubic`

- JS: `imgui.drawList.addBezierCubic(p1, p2, p3, p4, col, thickness,
  numSegments = 0, flags = 0) -> undefined`
- C++: `IMGUI_API void AddBezierCubic(const ImVec2& p1, const ImVec2& p2,
  const ImVec2& p3, const ImVec2& p4, ImU32 col, float thickness, int
  num_segments = 0, ImDrawFlags flags = 0);` (`imgui.h:3604`)
- Overload: only one. Nullable: none. Omitted: none.
- Frame: open. Rejected: a `numSegments` outside 0..512.
- Preconditions:
  - `num_segments` is not clamped: a positive value pushes that many
    path points (draw:1812-1814), which is an unbounded allocation and,
    past the vertex limit above, the plain assertion at draw:3554. (a):
    `numSegments` is 0 (automatic) .. 512. 512 points stay under the
    limit: `_AddPolyline` reserves at most four vertices per point
    (draw:1040, 1049).
  - (c) `ImDrawFlags_InvalidMask_` bits (draw:981, in `_AddPolyline`,
    reached through `PathStroke`, draw:2942): rejected by G1 first.
  - `_Data->CurveTessellationMaxError > 0.0f` (draw:1806, plain) is a
    style value `NewFrame()` has already checked (imgui.cpp:11841).
- Notes: `thickness` has no default in this declaration. With
  `numSegments == 0` the point count comes from ImGui's own tessellation
  (draw:1806-1808), bounded by the curve's size, not by an argument.

### `drawList.addPolyline`

- JS: `imgui.drawList.addPolyline(points, col, thickness, flags = 0) ->
  undefined`, `points` a flat `Array` or `Float32Array` `[x0, y0, x1,
  y1, ...]`
- C++: `inline void AddPolyline(const ImVec2* points, int num_points,
  ImU32 col, float thickness, ImDrawFlags flags = 0) {
  _AddPolyline(points, num_points, col, thickness, flags, FLT_MAX); }`
  (`imgui.h:3610`), over `IMGUI_API void _AddPolyline(const ImVec2*
  points, int num_points, ImU32 col, float thickness, ImDrawFlags flags,
  float max_inner_offset);` (`imgui.h:3714`)
- Overload: that one. The former order (`flags` before `thickness`) is a
  deleted overload under the addon's configuration (`imgui.h:3685`).
  Nullable: none.
- Omitted: `num_points` (the sequence's length over two) (design,
  "Exceptions: Point lists").
- Frame: open. Rejected (`ERR_OUT_OF_RANGE`): a sequence of odd length;
  more than 16383 points (a length above 32766).
- Preconditions:
  - the vertex limit described at the top of this section: the
    reservation is `points_count * max_verts_per_point` plus 2 for a
    closed line, or `(points_count - 2) * max_verts_per_point` plus 8 for
    an open one (draw:1044, 1049), with `max_verts_per_point` 2 or 4
    (draw:1040). (a): at most 16383 points, which is at most 65534
    vertices.
  - (c) `ImDrawFlags_InvalidMask_` bits (draw:981): rejected by G1
    first.
  - fewer than two points draw nothing (draw:963).
- Notes: `ImVec2` is two `float`s, so a `Float32Array` of even length
  has the layout `const ImVec2*` needs and can be used in place, like a
  plot's values; an `Array` is copied. `ImDrawFlags_Closed` is `1 << 9`
  in this version (imgui.h:3475).

### `drawList.addText`

- JS: `imgui.drawList.addText(pos, col, text) -> undefined`
- C++: `IMGUI_API void AddText(const ImVec2& pos, ImU32 col, const char*
  text_begin, const char* text_end = NULL);` (`imgui.h:3602`)
- Overload: that one. NOT bound: the overload taking `ImFont* font, float
  font_size, ...` (`imgui.h:3603`). Nullable: none.
- Omitted: `text_end`, passed as NULL.
- Frame: open. Rejected: none.
- Preconditions: none found (draw:2983-2986, 2956-2981); an empty string
  returns early (draw:2962).
- Notes: none.

## Clipper

### `listClipper`

- JS: `imgui.listClipper(count, itemHeight, callback) -> undefined`,
  `callback: (start, end) => void`
- C++: `struct ImGuiListClipper` (`imgui.h:3069`): the constructor
  (`imgui.h:3084`), `IMGUI_API void Begin(int items_count, float
  items_height = -1.0f);` (`imgui.h:3086`), `IMGUI_API bool Step();`
  (`imgui.h:3088`), `IMGUI_API void End();` (`imgui.h:3087`), and the
  fields `DisplayStart`, `DisplayEnd` and `ItemsCount`.
- Overload: not applicable (design, "Exceptions: List clipper"). NOT
  bound: `IncludeItemByIndex`, `IncludeItemsByIndex`, `SeekCursorForItem`,
  and the measuring mode (`items_height <= 0`). Nullable: none. Omitted:
  none.
- Frame: open. Inside the callback `newFrame()` and `render()` throw
  `ERR_IMGUI_USAGE` (design).
- Rejected: a `count` outside 0 .. `INT_MAX - 1`; an `itemHeight` that is
  not greater than 0 (NaN included).
- Preconditions:
  - `items_count` is not validated by `Begin()` (imgui.cpp:3486-3512).
    (a): `count` is 0 .. `INT_MAX - 1`. `INT_MAX` is ImGui's sentinel for
    "unknown count" (imgui.h:3082), which makes `End()` skip the final
    seek (imgui.cpp:3521) and would need `SeekCursorForItem`; it is not
    accepted.
  - `itemHeight > 0`. (a). C++'s default -1 means "measure the first
    item" (imgui.h:3083); that mode is not bound. With a positive height
    the measuring branch (imgui.cpp:3602-3627) is never entered, so its
    recoverable error ("ImGuiListClipper: Failed to calculate item
    height! ...", imgui.cpp:3620) and its plain assertion that, in a
    table, the first range was exactly one row (imgui.cpp:3624) cannot be
    reached.
  - `data != NULL` in `Step()` (imgui.cpp:3563, plain): the binding owns
    the loop and never steps a finished clipper.
  - `Begin()`, each `Step()` and each seek end an open table row with
    `TableEndRow()` (imgui.cpp:3494-3496, 3565-3567, 3462-3465), which
    asserts `window == table->InnerWindow` (tables:2126, plain). (b):
    when `g.CurrentTable` is not NULL and its `IsInsideRow` is set,
    `g.CurrentWindow == g.CurrentTable->InnerWindow`, tested before
    `Begin()`. The design's comparison of window, window-stack depth and
    table after each callback covers the later calls.
  - abandoning: `End()` seeks only when `ItemsCount >= 0 && ItemsCount <
    INT_MAX && DisplayStart >= 0` (imgui.cpp:3521-3522), so setting
    `ItemsCount = -1` skips it, as the design says; the destructor calls
    `End()` (imgui.cpp:3481), and `End()` asserts `data->ListClipper ==
    this` (imgui.cpp:3525, plain), which holds when nested clippers are
    released innermost first.
- Notes: `ItemsCount`, which the binding writes when abandoning, is
  declared in the public header (imgui.h:3074) but commented
  "[Internal]"; it is listed with the internal state below.

## Constants

By ruling 9, an enumerator of an enum type that a bound function takes is
exported, under its C++ name minus the `ImGui` or `Im` prefix and emitted
from the C++ identifier itself, when it is valid for a caller to pass or
to compare with; so are the `_None`, `_COUNT` and named range-marker
enumerators (`Key_NamedKey_BEGIN`, `Key_NamedKey_END`). Not exported: an
enumerator the header marks "[Internal]", and every `...Mask_` or other
trailing-underscore enumerator. No bound function returns an enum.

"Compiled" counts the enumerators that exist with
`IMGUI_DISABLE_OBSOLETE_FUNCTIONS` defined; the obsolete ones sit behind
`#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS` and do not exist in the
addon's build. "Exported" is compiled minus excluded. The exported set
is also what G1 accepts.

| Enum (kind) | `imgui.h` line | Compiled | Excluded (with `imgui.h` line) | Exported | Obsolete, not compiled | Taken by |
|---|---|---|---|---|---|---|
| `ImGuiWindowFlags_` (flags) | 1225 | 30 | 6: `_DockNodeHost` (1253), `_ChildWindow` (1254), `_Tooltip` (1255), `_Popup` (1256), `_Modal` (1257), `_ChildMenu` (1258), under "// [Internal]" (1252) | 24 | 0 | `begin`, `beginChild`, `beginPopup`, `beginPopupModal` |
| `ImGuiChildFlags_` (flags) | 1276 | 10 | 0 | 10 | 0 | `beginChild` |
| `ImGuiInputTextFlags_` (flags) | 1335 | 26 | 0 | 26 | 0 | `inputText`, `inputTextMultiline`, `inputTextWithHint`, `inputFloat`, `inputInt` |
| `ImGuiTreeNodeFlags_` (flags) | 1387 | 22 | 0 | 22 | 1: `_NavLeftJumpsBackHere` (1417) | `treeNodeEx`, `collapsingHeader` |
| `ImGuiPopupFlags_` (flags) | 1426 | 13 | 3: `_MouseButtonShift_` (1439), `_MouseButtonMask_` (1440), `_InvalidMask_` (1441) | 10 | 0 | `openPopup`, `beginPopupContextItem` |
| `ImGuiSelectableFlags_` (flags) | 1445 | 8 | 0 | 8 | 1: `_DontClosePopups` (1457) | `selectable` |
| `ImGuiComboFlags_` (flags) | 1463 | 10 | 1: `_HeightMask_` (1474) | 9 | 0 | `beginCombo` |
| `ImGuiTabBarFlags_` (flags) | 1478 | 13 | 2: `_FittingPolicyMask_` (1493), `_FittingPolicyDefault_` (1494) | 11 | 1: `_FittingPolicyResizeDown` (1497) | `beginTabBar` |
| `ImGuiTabItemFlags_` (flags) | 1502 | 10 | 0 | 10 | 0 | `beginTabItem` |
| `ImGuiFocusedFlags_` (flags) | 1517 | 7 | 0 | 7 | 0 | `isWindowFocused` |
| `ImGuiHoveredFlags_` (flags) | 1531 | 21 | 0 | 21 | 0 | `isWindowHovered`, `isItemHovered` |
| `ImGuiDockNodeFlags_` (flags) | 1571 | 8 | 0 | 8 | 2: `_NoSplit` (1584), `_NoDockingInCentralNode` (1585) | `dockSpaceOverViewport` |
| `ImGuiDir` (enum) | 1637 | 6 | 0 | 6 | 0 | `arrowButton` |
| `ImGuiKey` (enum) | 1661 | 165 | 5: `ImGuiKey_ReservedForModCtrl`, `_ReservedForModShift`, `_ReservedForModAlt`, `_ReservedForModSuper` (1756, under "// [Internal] Reserved for mod storage", 1755), `ImGuiMod_Mask_` (1777) | 160 | 2: `ImGuiKey_COUNT` (1780), `ImGuiMod_Shortcut` (1781) | `isKeyPressed`, `isKeyDown` |
| `ImGuiCol_` (enum) | 1859 | 64 | 0 | 64 | 4: `_TabActive`, `_TabUnfocused`, `_TabUnfocusedActive`, `_NavHighlight` (1927-1930) | `pushStyleColor`, `getColorU32` |
| `ImGuiStyleVar_` (enum) | 1942 | 46 | 0 | 46 | 0 | `pushStyleVar` |
| `ImGuiButtonFlags_` (flags) | 1994 | 7 | 1: `_MouseButtonMask_` (2000) | 6 | 0 | `invisibleButton` |
| `ImGuiColorEditFlags_` (flags) | 2006 | 33 | 6: `_DefaultOptions_` (2047), `_AlphaMask_` (2050), `_DisplayMask_` (2051), `_DataTypeMask_` (2052), `_PickerMask_` (2053), `_InputMask_` (2054) | 27 | 1: `_AlphaPreview` (2058) | `colorEdit3`, `colorEdit4`, `colorButton` |
| `ImGuiSliderFlags_` (flags) | 2066 | 11 | 1: `_InvalidMask_` (2080) | 10 | 0 | `sliderFloat`, `sliderInt`, `sliderFloat2`..`4`, `dragFloat`, `dragInt` |
| `ImGuiMouseButton_` (enum) | 2085 | 4 | 0 | 4 | 0 | `isItemClicked`, `isMouseDown`, `isMouseClicked`, `isMouseDoubleClicked`, `getMouseDragDelta` |
| `ImGuiCond_` (enum) | 2127 | 5 | 0 | 5 | 0 | `setNextWindowPos`, `setNextWindowSize`, `setNextItemOpen` |
| `ImGuiTableFlags_` (flags) | 2162 | 37 | 1: `_SizingMask_` (2211) | 36 | 0 | `beginTable` |
| `ImGuiTableColumnFlags_` (flags) | 2215 | 28 | 4: `_WidthMask_` (2246), `_IndentMask_` (2247), `_StatusMask_` (2248), `_NoDirectResize_` (2249) | 24 | 0 | `tableSetupColumn` |
| `ImGuiTableRowFlags_` (flags) | 2253 | 2 | 0 | 2 | 0 | `tableNextRow` |
| `ImDrawFlags_` (flags) | 3450 | 31 | 6: `_AllowTexForRoundCorners_` (3505), `_RoundMask_` (3508), `_AllowInPushScope_` (3509), `_AllowInFrameScope_` (3510), `_StrokeMask_` (3511), `_InvalidMask_` (3512) | 25 | 10: the `_RoundCorners*` names (3517-3526) | `drawList.addLine`, `addRect`, `addRectFilled`, `addCircle`, `addBezierCubic`, `addPolyline` |

Totals: 617 compiled, 36 excluded, **581 exported enumerators** of 25
enum types. With `imgui.FLT_MIN` and `imgui.FLT_MAX` (G4) the module
exports **583 constants**.

Points that follow from the source:

- Kept on purpose although a caller never passes them as such: the
  `_None` enumerators; `Dir_COUNT` (1644), `Col_COUNT` (1924),
  `StyleVar_COUNT` (1990), `MouseButton_COUNT` (2090),
  `Key_NamedKey_COUNT` (1760); `Key_NamedKey_BEGIN` (1665) and
  `Key_NamedKey_END` (1759). The functions that take those types reject
  the ones that are not valid arguments (their "Rejected" lines).
- Kept because they are valid to compare with: the four output-only
  status flags `TableColumnFlags_IsEnabled`, `_IsVisible`, `_IsSorted`,
  `_IsHovered` (2240-2243). No bound function returns column flags, so
  nothing produces them yet, and `tableSetupColumn` rejects them.
- The `ImGuiMod_*` enumerators are members of `enum ImGuiKey`
  (imgui.h:1772-1777), so they are exported as `imgui.Mod_None`,
  `imgui.Mod_Ctrl`, `imgui.Mod_Shift`, `imgui.Mod_Alt` and
  `imgui.Mod_Super`.
- `ImGuiDir_None` (-1, imgui.h:1639) is the only negative exported
  value.
- For a flags type, G1's accepted bits are the union of the exported
  enumerators. `ImDrawFlags_` has a multi-bit field: the stroke positions
  are `1 << 17` .. `4 << 17` and `7 << 17` (imgui.h:3495-3499), so the
  field values `5 << 17` and `6 << 17` pass the bit test without being
  enumerators. No assertion was found for them (`_GetStrokePos`,
  draw:957, only masks the field).
- Private enumerators declared in `imgui_internal.h` (for example
  `ImGuiInputTextFlags_Multiline`, internal:1125;
  `ImGuiTabItemFlags_Button`, internal:3161;
  `ImGuiDockNodeFlags_DockSpace`, internal:2083) belong to separate
  `...Private_` enum types. They are not enumerators of the public types,
  are not exported, and their bits are rejected by G1.
- The enum types of functions that are not bound are not exported:
  `ImGuiItemFlags_`, `ImGuiDragDropFlags_`, `ImGuiDataType_`,
  `ImGuiSortDirection`, `ImGuiInputFlags_`, `ImGuiConfigFlags_`,
  `ImGuiBackendFlags_`, `ImGuiMouseCursor_`, `ImGuiMouseSource`,
  `ImGuiTableBgTarget_`, `ImGuiListClipperFlags_`,
  `ImGuiMultiSelectFlags_`, `ImGuiViewportFlags_`.
- `ImGuiMouseButton_` arguments accept 0..4; see G1.

### `ImGuiStyleVar_`: which overload each variable takes

From `GStyleVarsInfo[]` (imgui.cpp:3840-3887), which the enum's own
comments (imgui.h:1945-1989) repeat.

Twelve take a `vec2` (`{ 2, ImGuiDataType_Float, ... }`):
`StyleVar_WindowPadding`, `StyleVar_WindowMinSize`,
`StyleVar_WindowTitleAlign`, `StyleVar_FramePadding`,
`StyleVar_ItemSpacing`, `StyleVar_ItemInnerSpacing`,
`StyleVar_CellPadding`, `StyleVar_TableAngledHeadersTextAlign`,
`StyleVar_ButtonTextAlign`, `StyleVar_SelectableTextAlign`,
`StyleVar_SeparatorTextAlign`, `StyleVar_SeparatorTextPadding`.

Thirty-three take a number (`{ 1, ImGuiDataType_Float, ... }`):
`StyleVar_Alpha`, `StyleVar_DisabledAlpha`, `StyleVar_WindowRounding`,
`StyleVar_WindowBorderSize`, `StyleVar_ChildRounding`,
`StyleVar_ChildBorderSize`, `StyleVar_PopupRounding`,
`StyleVar_PopupBorderSize`, `StyleVar_FrameRounding`,
`StyleVar_FrameBorderSize`, `StyleVar_IndentSpacing`,
`StyleVar_ScrollbarSize`, `StyleVar_ScrollbarRounding`,
`StyleVar_ScrollbarPadding`, `StyleVar_GrabMinSize`,
`StyleVar_GrabRounding`, `StyleVar_ImageRounding`,
`StyleVar_ImageBorderSize`, `StyleVar_TabRounding`,
`StyleVar_TabBorderSize`, `StyleVar_TabMinWidthBase`,
`StyleVar_TabMinWidthShrink`, `StyleVar_TabBarBorderSize`,
`StyleVar_TabBarOverlineSize`, `StyleVar_TableAngledHeadersAngle`,
`StyleVar_TreeLinesSize`, `StyleVar_TreeLinesRounding`,
`StyleVar_MenuItemRounding`, `StyleVar_SelectableRounding`,
`StyleVar_DragDropTargetRounding`, `StyleVar_SeparatorSize`,
`StyleVar_SeparatorTextBorderSize`, `StyleVar_DockingSeparatorSize`.

`StyleVar_COUNT` (45) is not a variable; it is exported and
`pushStyleVar` rejects it.

The mismatch check is in `PushStyleVar` itself: imgui.cpp:3900 for the
`float` overload and imgui.cpp:3930 for the `ImVec2` overload, both
`IM_ASSERT_USER_ERROR_RET(..., "Calling PushStyleVar() variant with wrong
type!")`. It is recoverable, and nothing is pushed. The index check
before it is not: `GetStyleVarInfo` asserts plainly `idx >= 0 && idx <
ImGuiStyleVar_COUNT` (imgui.cpp:3891), so the binding range-checks `idx`.

## Recoverable errors, audited

Every `IM_ASSERT_USER_ERROR`, `_RET` and `_RETV` site in `imgui.cpp`,
`imgui_widgets.cpp`, `imgui_tables.cpp` and `imgui_draw.cpp` was read
(the macros are at internal:2374-2376; each calls `ImGui::ErrorLog()`,
imgui.cpp:12099, which calls the error callback at imgui.cpp:12137-12138
and returns `io.ConfigErrorRecoveryEnableAssert`). The list below is the
subset this surface can raise. "Returns" means a `_RET` / `_RETV` form:
the function stops there. "Continues" means the plain form: the function
goes on after reporting.

### Raised by a call

| Binding function | Message | Location | Form |
|---|---|---|---|
| `end` | "Calling End() too many times!" | imgui.cpp:8992 | returns (explicit `return`, 8993) |
| `end` (on a popup) | "Must call EndPopup() and not End()!" | imgui.cpp:8999 | continues: the window is ended |
| `end` (on a child) | "Must call EndChild() and not End()!" | imgui.cpp:9001 | continues: the window is ended |
| `endPopup` | "Calling EndPopup() in wrong window!" | imgui.cpp:13525 | returns |
| `endMenu` | "Calling EndMenu() in wrong window!" | widgets:9755 | returns |
| `endCombo` | "Calling EndCombo() in wrong window!" | widgets:2096 | returns, after `g.BeginComboDepth--` (widgets:2091) |
| `endMainMenuBar` | "Calling EndMainMenuBar() not from a menu-bar!" | widgets:9483 | returns |
| `endTabBar` | "Mismatched BeginTabBar()/EndTabBar()!" | widgets:10068 | returns |
| `beginTabItem` | "Needs to be called between BeginTabBar() and EndTabBar()!" | widgets:10773 | returns false |
| `endTabItem` | "Needs to be called between BeginTabBar() and EndTabBar()!" | widgets:10793 | returns |
| `endTable` | "EndTable() call should only be done while in BeginTable() scope!" | tables:1461 | returns |
| `endTable` | "Mismatching PushID/PopID!" | tables:1591 | continues |
| `endTable` | "Too many PopItemWidth!" | tables:1592 | continues |
| `tableSetupColumn` | "Call should only be done while in BeginTable() scope!" | tables:1739 | returns |
| `tableSetupColumn` | "TableSetupColumn(): called too many times!" | tables:1740 | returns |
| `tableSetupColumn` | "TableSetupColumn(): need to call before first row!" | tables:1741 | returns |
| `tableSetupColumn` | "TableSetupColumn(): can only specify width/weight if sizing policy is set explicitly in either Table or Column." | tables:1715 | returns |
| `tableSetupScrollFreeze` | "Call should only be done while in BeginTable() scope!" | tables:1857 | returns |
| `tableHeadersRow` | "Call should only be done while in BeginTable() scope!" | tables:3286 | returns |
| `tableSetColumnIndex` | "TableSetColumnIndex() invalid column index!" | tables:2299 | returns false |
| `popID` | "Calling PopID() too many times!" | imgui.cpp:10160 | returns |
| `popStyleColor` | "Calling PopStyleColor() too many times!" | imgui.cpp:3823 | continues: pops what there is |
| `popStyleVar` | "Calling PopStyleVar() too many times!" | imgui.cpp:3941 | continues: pops what there is |
| `pushStyleVar` | "Calling PushStyleVar() variant with wrong type!" | imgui.cpp:3900, 3930 | returns: nothing pushed |
| `popItemWidth` | "Calling PopItemWidth() too many times!" | imgui.cpp:12578 | returns (explicit `return`, 12579) |
| `isItemHovered` | "Invalid flags for IsItemHovered()!" | imgui.cpp:5114 | continues |
| `isWindowHovered` | "Invalid flags for IsWindowHovered()!" | imgui.cpp:9225 | continues |
| `drawList.addRect` | "Incorrect parameter. Did you swap 'thickness' and 'flags'?" | draw:2353 | returns |
| `drawList.addPolyline`, `drawList.addBezierCubic` | "Incorrect parameter. Did you swap 'thickness' and 'flags'?" | draw:981 | returns |
| `listClipper` | "ImGuiListClipper: Failed to calculate item height! First item hasn't been submitted by user code, or has not moved the cursor vertically!" | imgui.cpp:3620 | the step returns false (3621) |
| `end`, `endGroup`, a table function ending a cell (after `setCursorScreenPos`) | "Code uses SetCursorPos()/SetCursorScreenPos() to extend window/parent boundaries.\nPlease submit an item e.g. Dummy() afterwards in order to grow window/parent boundaries." | imgui.cpp:11810, called from imgui.cpp:9023, imgui.cpp:12725, tables:2399 | continues |

`render()` never raises "Forgot to call ImGui::NewFrame()?"
(imgui.cpp:6427): the binding's frame check comes first.

Four rows of the table are ImGui's behaviour but cannot be observed
through the binding, because a check of the binding's comes first:
`endCombo` (the binding's own guard makes the same comparison before
calling ImGui), the two draw-list flag rows (G1 rejects the
`ImDrawFlags_InvalidMask_` bits), and the `listClipper` row (the
measuring mode is not bound). `tableSetColumnIndex`'s row is reached
only with a row open, the guard having passed.

### Raised when a window ends

`ErrorRecoveryTryToRecoverWindowState()` (imgui.cpp:12015) is called by
`End()` for the window being ended (imgui.cpp:9039), so these are raised
by `end()`, by whatever calls `End()` internally (`endChild`, `endPopup`,
`endTooltip`, `endMainMenuBar`, `endListBox`, `endTable` for a scrolling
table, `endMenu`, `endCombo`), and at the end of the frame. In the order
the function tests them:

| Left open | Message | Location | Recovery |
|---|---|---|---|
| a table whose inner window is the window | "Missing EndTable()" | imgui.cpp:12021 | `EndTable()` |
| a tab bar begun in the window | "Missing EndTabBar()" | imgui.cpp:12030 | `EndTabBar()` |
| a menu bar | "Missing EndMenuBar()" | imgui.cpp:12040 | `EndMenuBar()` |
| a tree node | "Missing TreePop()" | imgui.cpp:12045 | `TreePop()` |
| a group | "Missing EndGroup()" | imgui.cpp:12050 | `EndGroup()` |
| an ID | "Missing PopID()" | imgui.cpp:12056 | `PopID()` |
| a style colour | "Missing PopStyleColor()" | imgui.cpp:12073 | `PopStyleColor()` |
| a style variable | "Missing PopStyleVar()" | imgui.cpp:12083 | `PopStyleVar()` |

The same function also reports "Missing EndMultiSelect()"
(imgui.cpp:12035), "Missing EndDisabled()" (12061), "Missing
PopItemFlag()" (12078), "Missing PopFont()" (12088) and "Missing
PopFocusScope()" (12093); no bound function opens any of those scopes.

### Raised when the frame ends

`EndFrame()` calls `ErrorRecoveryTryToRecoverState()` (imgui.cpp:6433,
11977), which closes every window still on the stack above the fallback
window and then runs the window recovery above for the fallback window
(imgui.cpp:12008-12009):

| Left open | Message | Location | Recovery |
|---|---|---|---|
| a scrolling table's inner (child) window | "Missing EndTable()" | imgui.cpp:11993 | `EndTable()` |
| any other child window | "Missing EndChild()" | imgui.cpp:11998 | `EndChild()` |
| any other window | "Missing End()" | imgui.cpp:12004 | `End()` |

Each of those `End()` calls runs the window recovery for that window, so
one unclosed `begin()` with an open tree node inside reports "Missing
End()" and then "Missing TreePop()". The binding records the first
message (design).

### Where this differs from the design's list

1. **`endTabItem` is not purely a plain assertion.** Outside a tab bar it
   is recoverable (widgets:10793). What remains plain is
   `tab_bar->LastTabItemIdx >= 0` (widgets:10794), which is the guard's
   condition. `beginTabItem` outside a tab bar is recoverable too
   (widgets:10773); the design does not mention it.
2. **"The table row and column functions outside a table" are three
   different cases.** `tableNextRow` dereferences NULL (tables:2062-2064)
   and needs the guard. `tableNextColumn` and `tableSetColumnIndex`
   return false (tables:2313-2314, 2292-2293). `tableSetupColumn`,
   `tableSetupScrollFreeze` and `tableHeadersRow` are recoverable
   (tables:1739, 1857, 3286).
3. **A popup has no "missing" message of its own.** An unclosed popup,
   modal, combo popup or tooltip is a window: it is reported as "Missing
   End()" (imgui.cpp:12004), and for a popup the `End()` that recovery
   then calls also reports "Must call EndPopup() and not End()!"
   (imgui.cpp:8999). The design's list of things reported when a window
   or the frame ends names "popup" as if it were a separate case.
4. **A menu bar is in the recovery list** ("Missing EndMenuBar()",
   imgui.cpp:12040) and not in the design's.
5. **A child window is its own case** ("Missing EndChild()",
   imgui.cpp:11998); the design says "an unclosed window".
6. **A tab item is not recovered as such.** The source says so
   ("FIXME: Can't recover from inside BeginTabItem/EndTabItem yet",
   imgui.cpp:12027). What is reported for an unclosed tab item is the tab
   bar's "Missing EndTabBar()" or the ID stack's "Missing PopID()".
7. **Recoverable at the call and not in the design's list**: `end()` on a
   child or popup window (imgui.cpp:8999, 9001); the four
   `tableSetupColumn` errors; `tableSetColumnIndex` with a bad index;
   `pushStyleVar` with the wrong overload; `isItemHovered` /
   `isWindowHovered` with flags they do not accept; the draw-list flag
   check; the list clipper's item-height failure; the
   `setCursorScreenPos` boundary error; `endTable`'s two ID and
   item-width checks.
8. **Plain assertions, or unchecked reliance, not in the design's list
   of guarded functions**, each written out in its entry and guarded
   unless said: `beginMenuBar` called again while the menu bar is open
   (widgets:9331; the same through `beginMainMenuBar` is not covered);
   `tableSetupScrollFreeze` after the first row (tables:1858);
   `endTable`, `tableNextRow`, `tableNextColumn`, `tableHeadersRow` and
   `listClipper` when the current window is not the table's inner window
   (tables:1476, 2126); `tableSetColumnIndex` with no row open
   (draw:3483); `beginPopupContextItem` with no ID (imgui.cpp:13617);
   `endGroup` for a group begun in another window (imgui.cpp:12722);
   `endListBox`'s inner `EndGroup()`; `treePop` with an unbalanced
   `pushID` inside the node (widgets:7410); and the empty ID of the
   label-id guard (imgui.cpp:12319). Task 6 added, from walking the table
   functions' paths: `endTable` with the cursor moved before the first
   row (tables:1493); `tableSetupColumn` once the table's names pass
   32767 bytes (tables:1747-1750, 1909; imgui.h:2369); and `end`,
   `endChild` and `endListBox` in a scrolling table's own region, which
   ImGui's window recovery ends twice (imgui.cpp:12019-12023,
   tables:1612-1616, imgui.h:2377).
9. **The design's "Rejected values" has one entry** (the
   `InputTextFlags_Callback*` bits). The audit found further plain
   assertions on flag values. Most are now unreachable under G1; the
   combinations that remain are in the entries' "Rejected" lines.
10. **Confirmed as the design has them**: an unmatched `end`, `endPopup`,
    `endMenu`, `endCombo`, `endTable`, `endTabBar`, `endMainMenuBar`,
    `popID`, `popStyleColor`, `popStyleVar` and `popItemWidth` are
    recoverable at the call; an unclosed window, ID, tree node, group,
    style colour, style variable, tab bar and table are reported when a
    window or the frame ends; `endGroup`, `endChild`, `endTooltip`,
    `endListBox`, `endMenuBar` and `treePop` assert plainly or rely on
    unchecked state; a `pushItemWidth` without its pop is not diagnosed
    (it is not in `ErrorRecoveryTryToRecoverWindowState`).

## Counts

| Group | JavaScript functions |
|---|---|
| Frame | 10 (plus the `version` property) |
| Windows | 15 |
| Docking and settings | 3 |
| Menus | 7 |
| Layout | 20 |
| IDs | 3 |
| Text | 6 |
| Widgets | 28 |
| Trees and tabs | 9 |
| Tables | 8 |
| Popups and tooltips | 10 |
| Item and input queries | 15 |
| Style | 8 |
| Plots | 2 |
| Draw list | 9 |
| Clipper | 1 |
| **Total** | **154** |

Of the 154, three exist today (`newFrame`, `render`, `showDemoWindow`,
the last with a changed signature) and 151 are new. Nine JavaScript
functions bind two C++ overloads each (`menuItem`, `pushID`, `getID`,
`radioButton`, `selectable`, `collapsingHeader`, `pushStyleColor`,
`pushStyleVar`, `getColorU32`), so 163 C++ entry points are reached, of
which five are fields of `ImGuiIO` and one (`listClipper`) is a struct's
protocol. No function was added to the design's list and none was
removed.

By frame requirement: 142 need an open frame; 9 are "either" (`getTime`,
the five IO accessors, `styleColorsDark`, `styleColorsLight`,
`saveIniSettingsToMemory`); 1 must be outside a frame
(`loadIniSettingsFromMemory`); 1 needs neither frame nor context
(`colorConvertFloat4ToU32`); `newFrame` opens the frame. The `version`
property needs nothing.

27 functions carry `Label-id: yes`.

Exported constants: 583, that is 581 enumerators of 25 enum types (617
compiled, 36 excluded as internal, mask or trailing-underscore) plus
`imgui.FLT_MIN` and `imgui.FLT_MAX`.

## Not covered (for the tracker)

Plain assertions, or unchecked reliance, that a JavaScript caller can
reach and that neither an argument check nor a guard prevents. Each is an
abort in a Debug build and undefined behaviour in a Release build, as for
a C++ program.

1. **`beginTable` again with the same ID and a different column count in
   one frame.** `BeginTableEx` asserts plainly `table->ColumnsCount ==
   columns_count` for a second instance (tables:392-394). It is neither
   an argument check nor a read of current state: it needs a lookup in
   `g.Tables` by the ID `BeginTable` would compute (ruling 13).
2. **`beginMainMenuBar()` called again before `endMainMenuBar()`.**
   `BeginMainMenuBar` begins the window `"##MainMenuBar"` (widgets:9466)
   and calls `BeginMenuBar()` (widgets:9476), which asserts plainly
   `!window->DC.MenuBarAppending` (widgets:9331); the field is cleared
   only on a window's first `Begin()` of the frame (imgui.cpp:8759). The
   condition is on a window that is not the current one at the time of
   the call, so a guard would need `FindWindowByName` (internal:3654)
   (ruling 16). The same mistake with `beginMenuBar()` is guarded.
3. **A string ID whose hash is zero.** `BeginChildEx` asserts `id != 0`
   (imgui.cpp:6895; reached from `beginChild` and, through widgets:8989,
   `beginListBox`) and `BeginTabBarEx` asserts `tab_bar->ID != 0`
   (widgets:10005). The ID is a CRC of the string seeded with the ID
   stack; a zero cannot be told from the argument, and is not something a
   caller reaches by mistake.

3b. **Closing a scope ImGui opened internally, with another pair's
   closer.** Some constructs open a group, an ID or a child of their own
   (`BeginMenuBar` calls `BeginGroup()` and `PushID()`, widgets:9332-9333;
   `BeginListBox` a group and a child). A guard checks that something of
   the closer's kind is open, not who opened it. So `beginMenuBar();
   endGroup();` passes `canEndGroup` (the menu bar's group belongs to the
   current window, imgui.cpp:12719-12722) and removes the menu bar's
   group; the window's `end()` then recovers the open menu bar by calling
   `EndMenuBar()` itself (imgui.cpp:12038-12041), which reads the empty
   group stack (widgets:9401). The binding's `canEndMenuBar` never runs,
   because ImGui made the call. The same holds for a tree node whose ID
   was popped: `begin('w'); treeNodeEx('n', TreeNodeFlags_DefaultOpen);
   popID(); end();` passes every guard (`popID` is recoverable, and `end`
   has none), and the window's recovery then calls `TreePop()` itself
   ("Missing TreePop()", imgui.cpp:12045), which asserts plainly
   `window->IDStack.Size > 1` (widgets:7426); reproduced after Task 5. Every
   bound closer that ends a window (`end`, `endChild`, `endPopup`,
   `endMenu`, `endCombo`, `endTooltip`, `endListBox`, `endMainMenuBar`)
   runs that recovery and inherits these sequences; none of them is a call
   the binding makes in its own code. This is crash-capable and not prevented;
   preventing it needs the binding to track which scopes its caller
   opened, which is what part C should decide. ImGui's recovery is
   therefore not sufficient for these sequences.

3c. **Table states a table call cannot leave safely** (Task 6, each
   measured). Every table function relies on `g.CurrentTable` being the
   innermost table begun: its `TempData` and `DrawSplitter` point into a
   slot of `g.TablesTempData` (tables:362-368), which `EndTable()`
   releases (tables:1669-1670) and a later `BeginTable()` reuses, or
   reallocates when the stack grows (tables:362-363). Two sequences of
   bound calls make another table current, both through a window
   appended to (`SetCurrentWindow()`, imgui.cpp:8882, 4890), and both
   abort or hang whatever follows, so a guard on the table calls would
   not prevent it:
   - **an ended scrolling table**: its inner child keeps the table's
     index after `EndTable()` (only the outer window's is reset,
     tables:1677), so `beginChild(id)` with the table's own `strId`, in
     the same window and frame, re-enters that child (the same name,
     imgui.cpp `BeginChildEx`'s `"%s/%s_%08X"`) with the ended table
     current. `tableNextRow()` then aborts in `SetCurrentChannel()`
     (draw:3483), `endTable()` in `TableMergeDrawChannels()`
     (tables:2933), `endChild()` in `End()`'s recovery (imgui.h:2377),
     and leaving it open aborts in the frame-end recovery (tables:2933).
   - **a window holding an open table re-entered while a table begun
     later is still open** (`begin('W1'); beginTable(..); ...;
     begin('W2'); beginTable(..); ...; begin('W1')`): the first table is
     current and not on top of the stack. The nested `end()` makes the
     window recovery call `EndTable()` on it (imgui.cpp:12019-12023),
     which aborted (tables:1493) or, with a third table open, left the
     frame-end recovery looping until the run's time limit.
   A guard would have to refuse the `begin()` / `beginChild()` that
   enters such a window, which needs `FindWindowByName()` (internal:3654)
   and, for `beginChild`, ImGui's child naming scheme (ruling 16).

3d. **A table column layout ImGui cannot build** (Task 6, each measured
   as an abort from the first row function). Both depend on state that
   `TableUpdateLayout()` itself computes from settings and defaults
   before asserting, and cannot be predicted without reimplementing that
   function's enable and sort logic:
   - every column disabled through the user's or the settings' visibility:
     a `TableFlags_Hideable` table with `TableColumnFlags_DefaultHide` on
     each (`TableInitColumnDefaults`, tables:1697), or columns hidden and
     loaded from settings, asserts `LeftMostEnabledColumn >= 0`
     (tables:1005). The variant that can be read before the call, every
     column declared with `TableColumnFlags_Disabled` (which the layout
     honours whatever the visibility, tables:940), is guarded at the
     `tableSetupColumn` that would complete it
     (`canTableSetupColumn:all-disabled`);
   - a `TableFlags_Sortable` table with 64 or more enabled columns
     holding a sort order (`TableColumnFlags_DefaultSort` on each, or
     sorted by the user with `SortMulti`) asserts `sort_order_count < 64`
     in `TableSortSpecsSanitize()` (tables:3154), reached from the layout
     (tables:1357-1358). Short of that, the same function shifts a
     64-bit mask by the column index for every column (tables:3168),
     undefined behaviour past column 63, whenever two or more columns
     hold a sort order in a table of more than 64 columns.

3e. **A tree node that keeps stack data left open in a scrolling table's
   cell** (`treeNodeEx(label, TreeNodeFlags_DrawLinesFull)` or any flag
   that makes it keep stack data, widgets:7097-7100). `endTable()`
   reports "Mismatching PushID/PopID!" (tables:1591, recoverable, so the
   call throws `ERR_IMGUI_USAGE`) and pops the node's ID in place of the
   table's (tables:1595); the `EndChild()` that follows (tables:1615)
   runs the window recovery, whose `TreePop()` asserts plainly that the
   node's ID is the top of the ID stack (widgets:7410): an abort inside
   the same `endTable()` call (measured). Refusing `endTable()` would not
   help: the frame-end recovery ends the table before the tree node
   (imgui.cpp:11990-11994, 12019-12023 before 12043-12047) and meets the
   same assertion. This is item 3b's class (a scope left open across
   another construct's closer). In a non-scrolling table the same
   sequence was measured not to abort: `End()`'s `TreePop()` there runs
   at a depth whose stack-data bit is clear, because the table had
   raised the tree depth (tables:457), and the node's entry is left on
   `g.TreeNodeStack`.

Recorded here for completeness, although not assertions:

4. Guards are ImGui's own conditions and are as weak as they are (the
   design says so). The cases found: `endChild()` passes inside a
   `beginListBox`, a scrolling table or a sub-menu; `endTabItem()` passes
   a second time and after a `beginTabItem()` that returned false, and
   then pops an ID that is not the tab's; `endTabBar()` passes in a child
   window of the tab bar's window; `endMainMenuBar()` passes inside any
   window's menu bar. Each leaves ImGui in a state its own recovery
   handles. (`endChild()` in a scrolling table's own region was on this
   list until Task 6, which measured it aborting and refuses it; a child
   window begun inside a cell of such a table is an ordinary child.)
   `treePop()` passes `canTreePop` in a cell of a non-scrolling table
   with no tree node open, because `BeginTable()` raises the inner
   window's tree depth (tables:457): `TreePop()` then pops the table's
   own ID, and `endTable()` reports "Mismatching PushID/PopID!"; no abort
   was measured.
5. `ImDrawFlags` stroke-position field values `5 << 17` and `6 << 17`
   pass G1's bit test without being enumerators. No assertion was found
   for them.

## Internal state read by bind_guards.cpp

Everything the binding takes from `imgui_internal.h`, with the functions
that use it. `g` is the `ImGuiContext`. This is the list a bump of ImGui
re-audits (it becomes the README's "Bumping ImGui" list). Lines are those
of `imgui_internal.h`.

Read:

| State | Declared | Read by |
|---|---|---|
| `g.CurrentWindow` (the pointer itself) | 2553 | every guard below; `listClipper` (window comparison, design) |
| `g.CurrentWindow->ID` | 2998 | the label-id guard (27 functions), `endGroup`, `endMenuBar` |
| `g.CurrentWindow->Flags` | 2999 | `endChild`, `endListBox` (`ImGuiWindowFlags_ChildWindow`); `endTooltip` (`_Tooltip`); `beginMenuBar`, `endMenuBar` (`_MenuBar`) |
| `g.CurrentWindow->SkipItems` | 3039 | `beginMenuBar`, `endMenuBar`, `endMainMenuBar`, `endTabItem`, `beginPopupContextItem` (ImGui returns before its assertions when it is set, so the guards do too) |
| `g.CurrentWindow->Name` | 2997 | `endCombo`; the test hook `__test.tableScope()` (with the current table's inner window's) |
| `g.CurrentWindow->IDStack` (`Size`, `back()`) | 3068 | `treePop` |
| `g.CurrentWindow->DC.MenuBarAppending` | 2964 | `beginMenuBar`, `endMenuBar`, `endMainMenuBar` |
| `g.CurrentWindow->DC.TreeDepth` | 2967 | `treePop` |
| `g.CurrentWindow->DC.TreeHasStackDataDepthMask` | 2968 | `treePop` |
| `g.TreeNodeStack` (`back().ID`) | 2639 | `treePop` |
| `g.GroupStack` (`Size`, `back().WindowID`) | 2636; `ImGuiGroupData::WindowID` 1273 | `endGroup`, `endMenuBar`, `endMainMenuBar`, `endListBox` |
| `g.CurrentWindowStack` (`Size`; the `Window` of the entry below the top) | 2548 | `endListBox` (the parent window whose group `EndGroup()` will close); `listClipper` (stack depth, design) |
| `g.BeginComboDepth` | 2808 | `endCombo` |
| `g.CurrentTable` | 2764 | `tableNextRow`, `tableSetColumnIndex` (not NULL); `endTable`, `tableNextColumn`, `tableHeadersRow`, `tableSetupScrollFreeze`, `tableSetupColumn`, `end`, `endChild`, `endListBox` (guard applied when not NULL); `listClipper`; `__test.tableScope()` |
| `g.CurrentTable->InnerWindow` | 3415 | `endTable`, `tableNextRow`, `tableNextColumn`, `tableHeadersRow`, `end`, `endChild`, `endListBox`, `listClipper`; `__test.tableScope()` |
| `g.CurrentTable->OuterWindow` | 3414 | `end`, `endChild`, `endListBox` (a scrolling table's region: the inner window differs from it) |
| `g.CurrentTable->IsLayoutLocked` | 3452 | `tableSetupScrollFreeze`, `tableSetupColumn` |
| `g.CurrentTable->IsInsideRow` | 3453 | `tableSetColumnIndex`, `tableNextRow`, `tableNextColumn`, `tableHeadersRow`, `endTable`, `listClipper` |
| `g.CurrentTable->CurrentColumn`, `->ColumnsCount` | 3373, 3371 | `tableNextColumn` (whether it ends the row) |
| `g.CurrentTable->DeclColumnsCount` | 3426 | `tableSetupColumn` |
| `g.CurrentTable->ColumnsNames` (`size()`) | 3416 | `tableSetupColumn` |
| `g.CurrentTable->Columns[i].Flags`, `->TempData->ReconcileColumnsRequests` (`ColumnNewIdx`, `Flags`) | 3243, 3495, 3317 and 3312 | `tableSetupColumn` (all disabled) |
| `g.TablesTempData` (`WindowID`, `TableIndex`), `g.TablesTempDataStacked`; `g.Tables.GetByIndex` | 2767, 2766; 3488, 3489 | `end`, `endChild`, `endListBox` (the recovery's table chain) |
| `g.CurrentTable->RowPosY2`, `->InnerWindow->DC.CursorPos` | 3377, 2939 | `endTable` |
| `g.CurrentTabBar` and `->LastTabItemIdx` | 2773, 3221 | `endTabItem` |
| `g.LastItemData.ID` | 2624 | `beginPopupContextItem` (when `strId` is null) |
| `g.Windows` | 2545 | the test hook `__test.state().windows` (design) |

Also taken from ImGui's private source rather than a header: the
expression of `TREE_NODE_GET_DEPTH_MASK` (widgets:7019-7020), repeated in
`treePop`'s guard, and the format `"##Combo_%02d"` of a combo popup's
window name (widgets:2055, 2094), repeated in `endCombo`'s guard.

Written:

| State | Declared | Written by | Why |
|---|---|---|---|
| `g.ErrorCallback`, `g.ErrorCallbackUserData` | 2876, 2877 | context creation | the design's error callback |
| `g.BeginComboDepth` | 2808 | `newFrame()`, set to 0 before `ImGui::NewFrame()` | ruling 19: nothing in ImGui restores the counter after a combo left open (see `endCombo`); zero is always correct at the start of a frame |
| `ImGuiListClipper::ItemsCount` | imgui.h:3074, commented "[Internal]" | `listClipper`, set to -1 when abandoning | the design's abandon rule (imgui.cpp:3521) |

`g.BeginComboDepth` is the one write that the design did not already
have.

## Open questions

None.

## Rulings applied

Decisions made after the first version of this table, beyond what the
design says. The numbers are those of the questions the first version
asked.

1. `showDemoWindow(pOpen = null)`, `pOpen: ref<bool>`: the existing
   function gets the convention's signature.
2. `getTime` and the five IO accessors are "either" (context created on
   demand, `ERR_IMGUI_NO_WINDOW` with no window);
   `colorConvertFloat4ToU32` needs neither frame nor context.
3. `treePop` is also guarded on the tree node stack's ID equalling the
   top of the ID stack, under ImGui's own depth-mask condition.
4. The `ImGuiID` overloads of `BeginChild` and `OpenPopup` are not bound.
5. Both `CollapsingHeader` overloads are bound: a number or `undefined`
   second argument selects `(label, flags)`; an array or `null` selects
   `(label, pVisible, flags)`, `null` being C++ NULL.
6. Both `PushStyleColor` overloads are bound, dispatched on the second
   argument (array: `ImVec4`; number: `ImU32`).
7. The empty-ID assertion is guarded by a helper in `bind_guards.cpp`;
   the functions it applies to carry `Label-id: yes`.
8. G1: a flags or enum argument with any bit or value that is not an
   exported enumerator of the parameter's type is rejected with
   `ERR_OUT_OF_RANGE`; per function, the combinations that reach a plain
   assertion are rejected too.
9. Exported enumerators: those valid to pass or compare with, plus
   `_None`, `_COUNT` and the named range markers; not the "[Internal]"
   ones nor the masks and other trailing-underscore names. 581 of 617.
10. `imgui.FLT_MIN` and `imgui.FLT_MAX` are exported, exactly the C
    `float` values.
11. G2: a string containing U+0000 is rejected with a `TypeError`, code
    `ERR_INVALID_ARG_VALUE`, everywhere.
12. `tableSetColumnIndex` is guarded on a current table with a row open.
13. `beginTable` with the same ID and another column count in one frame
    is not covered (tracker).
14. `addPolyline`: an odd-length sequence is `ERR_OUT_OF_RANGE`; at most
    16383 points. `addBezierCubic`: `numSegments` 0..512.
15. `listClipper`: `itemHeight` is required and greater than 0 (the
    measuring mode is not bound); `count` is 0 .. `INT_MAX - 1`.
16. A second `beginMainMenuBar()` before `endMainMenuBar()` is not
    covered (tracker).
17. `endListBox` has a two-level guard: the child-window flag, then
    `EndGroup`'s conditions for the parent window.
18. The internal fields the guards read are accepted; they are listed
    under "Internal state read by bind_guards.cpp".
19. `endCombo` is guarded by the binding with ImGui's own window-name
    comparison, before ImGui is called; `newFrame()` resets
    `g.BeginComboDepth` to 0. The source confirms that nothing else
    resets it (imgui.cpp:4549; widgets:2083, 2091) and that zero is
    correct at the start of a frame.
20. G3: `ImU32` and `ImGuiID` arguments accept -2^31 .. 2^32-1, modulo
    2^32; flags, enum and other `int` arguments accept the signed 32-bit
    range.
