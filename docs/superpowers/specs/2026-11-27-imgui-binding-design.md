# Dear ImGui addon, part B: a hand-written JS binding and an application

Date: 2026-11-27. Branch `work-imgui`. Builds on part A
(`docs/superpowers/specs/2026-11-27-imgui-addon-design.md`), which put
ImGui on `hnui` and exposed three calls: `newFrame()`, `showDemoWindow()`,
`render()`.

## Intent

Someone who runs the example should conclude that a real, complex UI
application can be written in JavaScript on this runtime: several docked
windows, the full range of ordinary widgets, animation, and panels whose
content comes from Node's own APIs (`fs`, `process`, `os`), all at frame
rate. ImGui's own demo makes that case for C++; this makes it for
`hermes-node-ui`, and the Node-backed panels are the part the C++ demo
cannot show.

Two deliverables:

1. **The binding**: ImGui functions callable from JavaScript on
   `hermes-node:imgui`, hand-written in C++ in `addons/imgui/`, covering
   what the application calls.
2. **The application**: `examples/imgui-app/`, written only in JavaScript
   against the binding.

Part C later replaces the hand-written functions with generated ones for
the whole API. So the calling convention chosen here is the one the
generator must reproduce. A C++ signature alone does not determine a
binding function (nothing in `bool* p_open` says it may be `NULL`), so the
unit is a signature **plus a row of metadata**, of the fixed kinds listed
under "Metadata". Every rule below is a function of those two, and each
exception is listed as one.

## Goals

1. `examples/imgui-app/app.js` runs under `hermes-node-ui` on Linux (GL)
   and macOS (Metal) and shows the application described under "The
   application".
2. Every binding function follows the conventions under "Calling
   convention", so application code reads like the C++ ImGui code it
   corresponds to, and ImGui's documentation applies.
3. A mistake in JavaScript is a JavaScript error wherever the binding can
   tell: a wrong argument type or count throws a `TypeError` before ImGui
   is called; a call outside a frame throws `ERR_IMGUI_NO_FRAME`; a usage
   error ImGui itself detects and recovers from (unbalanced `begin`/`end`,
   `pushID`/`popID` and the like) throws `ERR_IMGUI_USAGE`.
4. Tested to part A's standard: assertions from read-backs and exact
   pixels, each test shown able to fail by a recorded mutation, both
   backends.
5. The per-frame cost of the application is measured and recorded (JS
   time per frame, Debug and Release), so that later work has a number to
   compare with. No target is set.

## Non-goals

- The whole ImGui API (part C). Functions the application does not call
  are not bound, with the exceptions listed under "Surface".
- A generator, TypeScript declarations, or an npm package.
- Textures or images from JavaScript, font loading, drag and drop,
  multi-select, table sorting specs, `InputText` callbacks.
- Persisting the layout: `io.IniFilename` stays NULL. The application
  loads a default layout from a string and never writes a file.
- A retained or declarative layer on top of ImGui.
- Everything part A lists as a non-goal (multi-viewport, IME, Windows,
  release packaging).

## Anti-goals

- A convention that needs per-function judgment to apply. It would make
  part C a rewrite of the application.
- A binding function that allocates a JavaScript object per call on its
  ordinary path (tuple returns and the like). Getters that return a vector
  are the stated exception and take an optional output array.
- Claiming the process cannot be crashed from JavaScript. ImGui has
  assertions that are not recoverable; see "Errors".

## Approaches

The question was how a JavaScript caller passes what C++ passes by
pointer (`bool Checkbox(const char* label, bool* v)`).

- **Reference arrays (chosen).** A pointer to N values is an array of N
  values mutated in place; the return value is what C++ returns. One rule
  covers scalars, vectors and strings, a generator can emit it from the
  signature, nothing is allocated per call, and code reads like the C++.
  Cost: a scalar lives in a one-element array.
- **Object and key** (`imgui.checkbox('Show', state, 'show')`).
  Application state stays plain, but every such call takes an extra
  argument, vectors still need arrays (two rules), and native code does a
  named property lookup per call.
- **Tuple returns** (`[changed, v] = imgui.checkbox('Show', v)`).
  Idiomatic, but an array is allocated per widget per frame, and calls
  with several outputs get awkward.

The user chose reference arrays.

## Calling convention

These are the rules; "Exceptions" lists every function that departs from
them.

| C++ | JavaScript |
|---|---|
| `ImGui::BeginChild` | `imgui.beginChild`: the name with a lower-case first letter. |
| Overloads | One function, dispatched on argument types, only where the types differ (`pushID(string)` / `pushID(number)`). Where they do not, a suffix (`treeNode(label)`, `treeNodeEx(label, flags)` as in C++). |
| `const char*` | A string. `NULL`-able ones accept `undefined` or `null`. Encoded to UTF-8 per call. |
| `const char* fmt, ...` | A finished string. `%` is not interpreted (the binding calls the `Unformatted` variant or passes `"%s"`). |
| `const char* text, const char* text_end` | One string; `text_end` is dropped. |
| `const char* format` of a slider, drag or input | A string holding exactly one conversion that fits the value's type (`%.3f`, `%d`, `%5.1f ms`); anything else throws before ImGui is called, because ImGui executes the format. |
| A pointer to an ImGui struct with no JavaScript form (`const ImGuiViewport*`, `const ImGuiWindowClass*`, `ImGuiStyle*`) | Omitted from the JavaScript signature and passed as `NULL`: `dockSpaceOverViewport(dockspaceId, flags)`, `styleColorsDark()`. |
| `bool`, `int`, `float`, `double`, `ImGuiID`, `ImU32` | A boolean or a number. Integers are range-checked. |
| An enum or flags type | A number. Every enumerator of every enum type that a bound function takes or returns is a constant on the module under its C++ name minus the `ImGui` or `Im` prefix: `imgui.WindowFlags_NoTitleBar`, `imgui.Col_Text`, `imgui.Key_Enter`, `imgui.DrawFlags_Closed`. |
| `const ImVec2&`, `const ImVec4&`, or either by value | An array of 2 or 4 numbers (any array-like with numeric elements is not accepted: a real `Array`). |
| Returns `ImVec2` / `ImVec4` | Returns a new array, or fills and returns the array passed as an extra argument. That argument comes after every C++ parameter, so defaults before it are passed as `undefined`: `imgui.getWindowPos(out)`, `imgui.getMouseDragDelta(0, undefined, out)`. |
| `bool* p`, `int* p`, `float* p`, `float v[N]`, `int v[N]` | A reference array: an `Array` of at least N elements of the right type, read before the call and written after it. A `NULL`-able pointer accepts `undefined` or `null`. |
| `char* buf, size_t buf_size` | A one-element reference array holding a string, plus the capacity in bytes as the next argument (`imgui.inputText('Name', name, 256)`). The string is written back whenever the buffer differs from it after the call, whatever the widget returned: with `InputTextFlags_EnterReturnsTrue` ImGui edits the buffer on every keystroke and returns true only on Enter. A new string is created only then. |
| A default argument | May be omitted or `undefined`; trailing only. |
| Returns `bool` or a number | Returned as is. `void` returns `undefined`. |

Every binding function checks its arguments before calling ImGui, and
checks that a frame is open (between `newFrame()` and `render()`), except
exactly these, which ImGui allows outside one: `styleColorsDark`,
`styleColorsLight`, `loadIniSettingsFromMemory`,
`saveIniSettingsToMemory`, and `version` (a property, no context). The
four functions need the ImGui context, which part A creates at the first
`newFrame()` that finds a window; they create it the same way, and with
no window they throw `ERR_IMGUI_NO_WINDOW`. Context creation is one
function, and it configures error recovery and installs the error
callback (see "Errors") before it returns, so no binding call can meet a
context with ImGui's defaults.
`loadIniSettingsFromMemory` is the one function that must be called
*outside* a frame, and throws `ERR_IMGUI_USAGE` inside one: it rebuilds
the dock nodes, which the frame's dock space is using. (The application's
Reset Layout therefore sets a flag and loads before the next
`newFrame()`.)

`begin`/`end` pairing follows ImGui exactly and is the caller's job, as in
C++: `end()` and `endChild()` are always called; every other `endX()` only
when its `beginX()` returned true.

### Metadata

What a binding function needs beyond its C++ signature. The plan contains
the full table of bound functions, one row each, with the JavaScript
signature and these columns; that table is part of what the plan's review
reviews, and part C's generator reads the same columns.

- **Overload**: which C++ overload the row binds, and its JavaScript
  name. Overloads not listed are not bound. In particular: `pushID` and
  `getID` bind the string and the `int` overloads (dispatched on type),
  not `const void*`; `treeNode(label)` and `treeNodeEx(label, flags)` bind
  only those two, not the `str_id, fmt` or pointer-ID forms;
  `drawList.addText(pos, col, text)` binds the overload without a font.
- **Nullable**: which pointer and string parameters accept `undefined` /
  `null`.
- **Length**: for a pointer that is not written `v[N]`, how many elements
  it addresses (one, for every function in this surface).
- **Omitted**: parameters dropped from the JavaScript signature and
  passed as `NULL` or zero: struct pointers with no JavaScript form,
  `text_end`, and the `callback` / `user_data` pair of the `inputText`
  family.
- **Frame**: whether the function needs an open frame (all but the four
  above) or must be outside one (`loadIniSettingsFromMemory`).
- **Preconditions**: the checks the binding makes before calling ImGui,
  beyond argument types; see "Errors".
- **Rejected values**: flag bits and argument combinations refused before
  ImGui is called. In this surface: the `InputTextFlags_Callback*` bits
  (each needs a callback, and ImGui asserts or calls through NULL without
  one).

### Buffers

- **`inputText` family.** The capacity is an integer from 1 to 2^24. The
  binding owns a buffer of that many bytes for the call. The incoming
  string is encoded to UTF-8 and, if longer than capacity minus one,
  truncated at a character boundary; the buffer is always terminated.
  After the call the buffer is compared with the *original* string's
  encoding, and written back if it differs, so a truncation is written
  back too.
- **Plots.** There is no count argument: the count is the array's length.
  `valuesOffset` must be an integer from 0 to length minus one (0 for an
  empty array). For a `Float32Array` the data pointer is taken after every
  other argument has been read, and nothing that can run JavaScript
  happens between that and the ImGui call.

### Re-entrancy and write-back

Reading an element of a reference array, or writing one, can run
JavaScript (an accessor defined on an index). So:

- Every argument is read into native storage before ImGui is called;
  ImGui never sees JavaScript memory except a `Float32Array`'s data, as
  above.
- The binding keeps an "inside a call" flag for the whole of each binding
  function, `newFrame()` and `render()` included. Entering any of them
  while it is set throws `ERR_IMGUI_REENTRANT` and calls nothing. The one
  place it is lowered is around the `listClipper` callback, below.
- If reading an argument throws, the exception propagates and ImGui is
  not called.
- Outputs are written after the call, in parameter order. If a write
  throws, the exception propagates; ImGui's state has already changed and
  later outputs are not written.
- If the call raised a usage error, nothing is written back and
  `ERR_IMGUI_USAGE` is thrown.

### Exceptions

Each of these exists because the C++ shape has no mechanical translation.
Part C carries the same list.

- **Draw list.** `ImDrawList*` is not exposed as an object. The functions
  live on `imgui.drawList` and act on the current window's draw list
  (`ImGui::GetWindowDrawList()`): `imgui.drawList.addLine(p1, p2, col,
  thickness)`. Colours there are `ImU32` numbers, made with
  `imgui.getColorU32(vec4)` or `imgui.colorConvertFloat4ToU32(vec4)`.
- **List clipper.** `ImGuiListClipper` is a C++ object with a loop
  protocol, and one left open by a throw cannot be cleaned up later. It is
  bound as one function that owns the loop:
  `imgui.listClipper(count, itemHeight, (start, end) => { ... })`. The
  callback is called once per range `Step()` yields; the clipper lives on
  the native stack and is ended when the callback returns or throws, and a
  throw then propagates. The callback may call any binding function.
- **Plots.** `plotLines` and `plotHistogram` take the values as a
  `Float32Array` (used in place, no copy) or an `Array` of numbers, with
  no count and no `stride` argument (see "Buffers"); the getter overloads
  are not bound.
- **Settings.** `loadIniSettingsFromMemory(string)` and
  `saveIniSettingsToMemory()` (returns a string) take and return strings
  without a size.
- **IO accessors.** `ImGuiIO` is not exposed as an object. The fields the
  application reads are functions: `getFrameRate()`, `getDeltaTime()`,
  `getDisplaySize(out)`, `wantCaptureMouse()`, `wantCaptureKeyboard()`.
- **`combo`.** The convenience overloads taking item arrays are not bound;
  `beginCombo` / `selectable` / `endCombo` are.
- **Point lists.** `const ImVec2* points, int num_points`
  (`drawList.addPolyline`) is one flat sequence of numbers, `[x0, y0, x1,
  y1, ...]`, as an `Array` or a `Float32Array`; the count is its length
  over two.
- **`getColorU32`.** C++ has three overloads, two of which take a number
  first. An array argument selects the `ImVec4` one, a number the
  `ImGuiCol` one (`getColorU32(imgui.Col_Text, alphaMul)`); the `ImU32`
  one is not bound.

## Surface

About 150 functions, in these groups. The plan fixes the exact list by
writing the application's panels against it; a function may be added to
the list when a panel needs it and removed when nothing calls it, but the
conventions may not change.

- **Frame** (existing): `newFrame`, `render`, `showDemoWindow`, `version`.
  Added: `showMetricsWindow`, `getTime`, and the IO accessors above.
- **Windows**: `begin`, `end`, `beginChild`, `endChild`,
  `setNextWindowPos`, `setNextWindowSize`, `setNextWindowBgAlpha`,
  `getWindowPos`, `getWindowSize`, `getContentRegionAvail`,
  `isWindowFocused`, `isWindowHovered`, `getScrollY`, `getScrollMaxY`,
  `setScrollHereY`.
- **Docking and settings**: `dockSpaceOverViewport`,
  `loadIniSettingsFromMemory`, `saveIniSettingsToMemory`.
- **Menus**: `beginMainMenuBar`, `endMainMenuBar`, `beginMenuBar`,
  `endMenuBar`, `beginMenu`, `endMenu`, `menuItem` (both overloads: with a
  `selected` boolean, and with a reference array).
- **Layout**: `sameLine`, `separator`, `separatorText`, `spacing`,
  `dummy`, `newLine`, `indent`, `unindent`, `beginGroup`, `endGroup`,
  `setNextItemWidth`, `pushItemWidth`, `popItemWidth`,
  `getCursorScreenPos`, `setCursorScreenPos`, `getTextLineHeight`,
  `getTextLineHeightWithSpacing`, `getFrameHeight`, `calcTextSize`,
  `alignTextToFramePadding`.
- **IDs**: `pushID`, `popID`, `getID`.
- **Text**: `text`, `textColored`, `textDisabled`, `textWrapped`,
  `bulletText`, `labelText`.
- **Widgets**: `button`, `smallButton`, `invisibleButton`, `arrowButton`,
  `checkbox`, `radioButton` (both overloads), `progressBar`, `bullet`,
  `sliderFloat`, `sliderInt`, `sliderFloat2..4`, `dragFloat`, `dragInt`,
  `inputText`, `inputTextMultiline`, `inputTextWithHint`, `inputFloat`,
  `inputInt`, `beginCombo`, `endCombo`, `selectable` (both overloads),
  `colorEdit3`, `colorEdit4`, `colorButton`, `beginListBox`, `endListBox`.
- **Trees and tabs**: `treeNode`, `treeNodeEx`, `treePop`,
  `collapsingHeader`, `setNextItemOpen`, `beginTabBar`, `endTabBar`,
  `beginTabItem`, `endTabItem`.
- **Tables**: `beginTable`, `endTable`, `tableSetupColumn`,
  `tableSetupScrollFreeze`, `tableHeadersRow`, `tableNextRow`,
  `tableNextColumn`, `tableSetColumnIndex`.
- **Popups and tooltips**: `openPopup`, `beginPopup`, `beginPopupModal`,
  `beginPopupContextItem`, `endPopup`, `closeCurrentPopup`, `setTooltip`,
  `setItemTooltip`, `beginTooltip`, `endTooltip`.
- **Item and input queries**: `isItemHovered`, `isItemClicked`,
  `isItemActive`, `isItemDeactivatedAfterEdit`, `getItemRectMin`,
  `getItemRectMax`, `setItemDefaultFocus`, `setKeyboardFocusHere`,
  `isKeyPressed`, `isKeyDown`, `isMouseDown`, `isMouseClicked`,
  `isMouseDoubleClicked`, `getMousePos`, `getMouseDragDelta`.
- **Style**: `styleColorsDark`, `styleColorsLight`, `pushStyleColor`,
  `popStyleColor`, `pushStyleVar` (number and vector), `popStyleVar`,
  `getColorU32`, `colorConvertFloat4ToU32`.
- **Plots**: `plotLines`, `plotHistogram`.
- **Draw list** (`imgui.drawList`): `addLine`, `addRect`, `addRectFilled`,
  `addCircle`, `addCircleFilled`, `addTriangleFilled`, `addBezierCubic`,
  `addPolyline`, `addText`, `pushClipRect`, `popClipRect`.
- **Clipper**: `listClipper`.
- **Constants**: every enumerator of the enum types the functions above
  take or return. Each is emitted from the C++ identifier itself (a macro
  over the name), so a value cannot be mistyped.

## Errors

- **Wrong arguments**: `TypeError` with `code` `ERR_INVALID_ARG_TYPE` (or
  `ERR_OUT_OF_RANGE` for a number outside the C++ type, or a reference
  array that is too short), naming the function and the argument. ImGui is
  not called.
- **No frame**: the existing `ERR_IMGUI_NO_FRAME`.
- **Re-entry**: `ERR_IMGUI_REENTRANT`, above.
- **Usage errors ImGui recovers from.** ImGui v1.93 reports these through
  `IM_ASSERT_USER_ERROR` and repairs its state. From its source, for this
  surface: an unmatched `end`, `endPopup`, `endMenu`, `endCombo`,
  `endTable`, `endTabBar`, `endMainMenuBar`, `popID`, `popStyleColor`,
  `popStyleVar` or `popItemWidth` when called; and, when a window or the
  frame ends, an unclosed window, ID, tree node, group, style colour,
  style variable, popup, tab bar or table. (The plan audits this list
  against `ErrorRecoveryTryToRecoverWindowState` and each function, and
  the README carries the audited list.) Context creation sets
  `io.ConfigErrorRecoveryEnableAssert = false`, so a recoverable error
  never aborts a Debug build, and `io.ConfigErrorRecoveryEnableTooltip =
  false`, because ImGui's tooltip carries a button that turns the asserts
  back on; `newFrame()` sets both again. It also installs ImGui's error
  callback, which records the first message.
  - An error reported during a binding call makes that call throw an
    `Error` with `code` `ERR_IMGUI_USAGE` and ImGui's message, after ImGui
    has recovered. ImGui's state is consistent afterwards and the frame
    may continue.
  - Errors ImGui reports when the frame ends make `render()` throw the
    same way. `render()` then has ended the ImGui frame and closed the
    binding's frame, has **not** taken the frame's target and has drawn
    nothing, so the tick presents nothing, exactly as for a throw from
    application code. This holds with or without a target: a usage error
    takes precedence over the `false` return.
  - Every such error is traced (`imgui: usage error: <msg>`). The one
    case that only traces: the discarding `newFrame()` that part A already
    has for a frame left open by an earlier throw.
  - The error callback is `ImGuiContext::ErrorCallback`, declared in
    `imgui_internal.h`.
- **Preconditions ImGui does not check recoverably.** Several bound
  functions assert plainly, or dereference without checking, when called
  in the wrong state: `endGroup`, `endChild`, `endTooltip`, `endListBox`,
  `endMenuBar`, `endTabItem`, `treePop`, `drawList.popClipRect`, and the
  table row and column functions outside a table. These are the mistakes
  the pairing rule invites (calling `endListBox()` after a
  `beginListBox()` that returned false). For each, the binding first
  evaluates **the same condition ImGui asserts or relies on, read from
  ImGui's own state** (the group stack's size, the current window's tree
  depth and flags, the current table and tab bar, the draw list's clip
  stack), and throws `ERR_IMGUI_USAGE` without calling ImGui when it does
  not hold. Because the state read is ImGui's, it stays right after ImGui
  has recovered from an earlier error in the same frame, and it follows
  ImGui's own rules (an `endChild()` after a `beginChild()` that returned
  false is valid; a `treeNodeEx` with `TreeNodeFlags_NoTreePushOnOpen`
  opens nothing to pop). The plan lists each function's conditions with
  the source lines they come from.
  - These reads need `imgui_internal.h`. Together with the error callback
    they are the binding's whole use of the internal header, confined to
    one file (`bind_guards.cpp`) and named in the README's "Bumping
    ImGui" section: a bump re-audits that file against ImGui's source.
  - A guard makes the call safe, not right: where ImGui's own check is
    weaker than correct pairing (a second `endTabItem()` passes it), so is
    the binding's.
- **Not diagnosed, as in C++**: a `pushItemWidth` without its pop (ImGui
  clears that stack when the window next begins) and a
  `drawList.pushClipRect` without its pop (the window's own pop then
  removes the wrong entry; the draw list is rebuilt next frame). Neither
  is in ImGui's recovery and neither crashes.
- **Assertions ImGui does not recover from**, other than those, remain
  `assert`: an abort in a Debug build and undefined behaviour in a Release
  build, as for a C++ program. The binding's argument checks cover the
  ones a wrong JavaScript value reaches directly (a negative count, an
  enum value out of range, a table column count over ImGui's limit, a
  format string, a rejected flag); each bound function's preconditions are
  written out in the plan from ImGui's source. What is not covered is
  recorded in the tracker. This is the limit of goal 3.
- **A throw from application code inside a frame** is unchanged from part
  A: the exception takes the host's ordinary path for a frame callback,
  and if the program survives it the next `newFrame()` discards the open
  frame.

## The application

`examples/imgui-app/`: `app.js` (window, frame loop, menu, dock space,
layout), one file per panel under `panels/`, `layout.js` (the default
layout as an ini string), `README.md`. No npm dependencies.

```
┌ File  View  Help ──────────────────────────────────────────────┐
│┌ Files ─────────┐┌ Widgets ──────────────┐┌ Monitor ──────────┐│
││ ▸ addons/      ││ [Button] ☑ Check      ││ heap  ▁▂▃▅▆▇▆▅   ││
││ ▾ examples/    ││ Slider ━━●━━━  0.42   ││ rss   ▃▃▃▄▄▄▄▅   ││
││    demo.js     ││ Text  [hello_____]    ││ frame 16.6 ms     ││
││ ▸ test/        ││ Combo [dark     ▾]    │└───────────────────┘│
│└────────────────┘│ Color ■  Table ▦      │┌ Canvas ───────────┐│
│┌ Viewer ────────┐│ Tabs | Tree | Popup   ││  animated shapes  ││
││ text of the    │└───────────────────────┘│                   ││
││ selected file  │┌ Log ──────────────────┐│                   ││
│└────────────────┘│ 12:01:03 opened a.js  │└───────────────────┘│
└──────────────────┴───────────────────────┴─────────────────────┘
```

- **Shell.** A main menu bar and `dockSpaceOverViewport`. The default
  layout is loaded with `loadIniSettingsFromMemory` before the first
  frame. *File*: Open Folder (a modal with a text field), Quit
  (`win.close()`). *View*: a toggle per panel, Reset Layout, Dark / Light,
  ImGui Demo, ImGui Metrics. *Help*: About (a modal showing `imgui.version`
  and `process.versions`).
- **Files.** A lazily-loaded directory tree, rooted at `process.argv[2]`
  or the current directory. A directory is read with `fs.promises.readdir`
  when first opened, and shows "loading" until the promise resolves: the
  UI keeps drawing while Node does I/O. Errors are shown in the tree and
  logged. In every panel the promise's result is stored in application
  state by its continuation; the frame callback only draws the state it
  finds and never awaits.
- **Viewer.** The selected file's first 1 MiB, read with
  `fs.promises.open` and bounded `FileHandle.read` calls (not `readFile`,
  which reads the whole file whatever is shown), the handle closed in a
  `finally`; then split into lines and drawn through the list clipper so a
  long file costs only its visible lines. A truncated file says so. A file that is not valid UTF-8 text
  is shown as a hex dump. Above it a table of the file's `stat` fields.
- **Widgets.** A gallery in tabs: basic (buttons, checkbox, radio,
  sliders, drags, inputs, combo, colour editors, progress bar), a table
  with frozen headers filled from `os.cpus()` and `process.versions`,
  trees and collapsing headers, popups (context menu, modal) and tooltips.
- **Monitor.** `process.memoryUsage()` (rss, heap used, heap total),
  `os.loadavg()`, frame rate and JS time per frame, sampled a few times a
  second by a `setInterval` timer into `Float32Array` ring buffers and
  drawn with `plotLines` / `plotHistogram`.
- **Canvas.** Animation drawn with the draw list into a child window:
  orbiting circles, a rotating polygon, a Bézier curve whose control
  points move, and trails; a speed slider, a pause checkbox and a colour.
  Positions come from the frame callback's timestamp, so speed does not
  depend on frame rate. Dragging in the canvas moves the centre
  (`invisibleButton`, `isItemActive`, `getMouseDragDelta`).
- **Log.** The application's own events (opened file, read error, layout
  reset), newest at the bottom, auto-scrolling unless the user has
  scrolled up, with a Clear button and a filter field.

The application has one test mode, selected by an environment variable:
it then uses a fixed tree fixture as its root, a fixed clock for the
canvas, and prints a line per state change (`APP opened <path> <bytes>
lines=<n>`, `APP dir <path> entries=<n>`, `APP error <path> <code>`), so
the test below has read-backs that do not depend on the machine.

## Structure of the binding

- `addons/imgui/bind/`: `bind_util.h` (argument readers and writers:
  string, number with range, vector, reference array of each kind; the
  frame check; the usage-error check; the registration macro), then one
  `.cpp` per group above, `bind_constants.cpp`, and `bind_guards.cpp`
  (the error callback and the precondition reads, the only unit that
  includes `imgui_internal.h`).
- A binding function is a few lines over those helpers, in a fixed shape:
  enter (the re-entrancy flag), read and check arguments, check the frame
  and the preconditions, call ImGui, check for a usage error, write back,
  return. The fixed shape is what part C generates.
- `imgui_module.cpp` keeps the frame protocol and calls one registration
  function per group.
- The addon's export list and public-API-only rule are unchanged.
  `imgui-symbols.js` keeps passing unmodified.

## Tests

To part A's rules: window tests named `test/ui-imgui-*.js` under
`%with-display` with the Sokol exclusions, read-backs and exact pixels,
no timing, each test shown able to fail by a mutation recorded in the
progress file.

New test hooks, under `HERMES_NODE_UI_TEST_HOOKS` as before:
`__test.bindCalls()` returns, per binding function, how many times it
has **called ImGui** (counted immediately before the call, so a rejected
call does not count); `__test.state()` gains `windows`, the name, active
flag and docked flag of each ImGui window in the last ended frame.

- **`imgui-bind-surface.js`**: the module's exact export list and the
  value of a sample of constants per enum type, against a checked-in list
  (`test/fixtures/imgui/bind-groups.js`, the function names by group). An
  added or removed function fails it.
- **Coverage rule, by execution**: each group's test ends by reading
  `__test.bindCalls()` and failing unless every function in its group has
  a non-zero count, and the surface test checks that the groups together
  are the export list. So every bound function has reached ImGui in some
  test, and a function added to the list without a test fails its group's
  test.
- **`ui-imgui-bind-args.js`**: for each convention, wrong types, counts,
  ranges, formats, rejected flags and buffer rules throw the documented
  error, and the function's `bindCalls()` count does not move; calls
  outside a frame throw `ERR_IMGUI_NO_FRAME`; the four out-of-frame
  functions work before the first `newFrame()`; an index accessor that
  re-enters throws `ERR_IMGUI_REENTRANT`; an accessor that throws on read
  prevents the call, and one that throws on write propagates after it.
- **`ui-imgui-bind-errors.js`**: an unmatched `end`, `pushID` without
  `popID`, `endTable` without `beginTable`; each precondition the binding
  checks itself, including the two the rule must get right (`endChild`
  after a false `beginChild` is accepted; `treePop` after a
  `NoTreePushOnOpen` node throws); a binding-checked closer called after
  ImGui has recovered from an earlier error in the same frame;
  `loadIniSettingsFromMemory` inside a frame. For each: `ERR_IMGUI_USAGE`
  from the call or from `render()`, the trace line, no publication for a
  throwing `render()` (with a target, without one, and in CLOSING), and
  the next frame works. A throw makes the harness's `frame()` skip its
  bookkeeping, as part A's exception test found, so these cases drive the
  frame callback themselves and call `harness.resync` before the next
  ordinary frame.
- **One test per group** (`ui-imgui-bind-widgets.js`, `-windows.js`,
  `-tables.js`, `-popups.js`, `-drawlist.js`, `-plots.js`, ...): the
  script builds a small UI, reads item rectangles back with
  `getItemRectMin/Max`, injects mouse and key events at them through the
  existing harness, and checks the reference arrays and return values
  (click the checkbox: `v[0]` flips and the call returns true; drag the
  slider; type into the field, with and without `EnterReturnsTrue`).
  Draw-list and plot tests check exact pixels of flat-coloured shapes, as
  part A's scenes do. The `listClipper` test checks the ranges it is
  given and that a throw from its callback leaves the next frame working.
- **`ui-imgui-app.js`**: the application in test mode: every panel's
  window is in `state().windows`, active and docked; a click on a fixture
  file (at the rectangle the application prints for it in test mode)
  produces `APP opened` with the fixture's byte and line counts; an
  unreadable fixture entry produces `APP error`; the canvas shows the
  expected pixels at the fixed clock; Quit ends with status 0; and no
  usage error is traced.
- The Mac round runs all of it on Metal at 2x, and a person looks at the
  application once on each platform (recorded as a hand check).

## Constraints

- Part A's rules stand: public API only (plus `imgui_internal.h` in
  `bind_guards.cpp`, above), logical units, no file written by the addon, new files carry the
  project's copyright line, no commit hashes in documents.
- Hermes: no `performance` global (the application takes time from the
  frame callback and `process.hrtime`), CommonJS only.
- The binding adds no host change. If a panel needs something `hnui`
  lacks, that is an `hnui` gap fixed in the host in its own commit.

## Left to the builder

- The exact function list within the groups, fixed by writing the panels.
- The helpers' names and the macro shape in `bind_util.h`.
- How the usage-error and re-entrancy flags are kept and reset.
- The default layout's ini text, produced by arranging the windows once
  and calling `saveIniSettingsToMemory()`.
- The fixture tree for the application's test mode.
- Which ImGui preconditions each function's argument checks cover, from
  reading ImGui's source for that function.
