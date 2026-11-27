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
generator must reproduce: every rule below has to be derivable from a C++
signature, and each exception is listed as one.

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
| `const ImVec2&`, `const ImVec4&` | An array of 2 or 4 numbers (any array-like with numeric elements is not accepted: a real `Array`). |
| Returns `ImVec2` / `ImVec4` | Returns a new array, or fills and returns the array passed as an extra argument. That argument comes after every C++ parameter, so defaults before it are passed as `undefined`: `imgui.getWindowPos(out)`, `imgui.getMouseDragDelta(0, undefined, out)`. |
| `bool* p`, `int* p`, `float* p`, `float v[N]`, `int v[N]` | A reference array: an `Array` of at least N elements of the right type, read before the call and written after it. A `NULL`-able pointer accepts `undefined` or `null`. |
| `char* buf, size_t buf_size` | A one-element reference array holding a string, plus the capacity in bytes as the next argument (`imgui.inputText('Name', name, 256)`). The string is written back whenever the buffer differs from it after the call, whatever the widget returned: with `InputTextFlags_EnterReturnsTrue` ImGui edits the buffer on every keystroke and returns true only on Enter. A new string is created only then. |
| A default argument | May be omitted or `undefined`; trailing only. |
| Returns `bool` or a number | Returned as is. `void` returns `undefined`. |

Every binding function checks its arguments before calling ImGui, and
checks that a frame is open (between `newFrame()` and `render()`), except
the few that ImGui allows outside one: the settings functions, the style
functions and `version`. Those need the ImGui context, which part A
creates at the first `newFrame()` that finds a window; they create it the
same way, and with no window they throw `ERR_IMGUI_NO_WINDOW`.
`loadIniSettingsFromMemory` is the one function that must be called
*outside* a frame, and throws `ERR_IMGUI_USAGE` inside one: it rebuilds
the dock nodes, which the frame's dock space is using. (The application's
Reset Layout therefore sets a flag and loads before the next
`newFrame()`.)

`begin`/`end` pairing follows ImGui exactly and is the caller's job, as in
C++: `end()` and `endChild()` are always called; every other `endX()` only
when its `beginX()` returned true.

### Exceptions

Each of these exists because the C++ shape has no mechanical translation.
Part C carries the same list.

- **Draw list.** `ImDrawList*` is not exposed as an object. The functions
  live on `imgui.drawList` and act on the current window's draw list
  (`ImGui::GetWindowDrawList()`): `imgui.drawList.addLine(p1, p2, col,
  thickness)`. Colours there are `ImU32` numbers, made with
  `imgui.getColorU32(vec4)` or `imgui.colorConvertFloat4ToU32(vec4)`.
- **List clipper.** `ImGuiListClipper` is a C++ object with a loop
  protocol. It is bound as three functions over one implicit stack:
  `imgui.listClipperBegin(count, itemHeight)`, `imgui.listClipperStep(out)`
  (fills `out[0]`, `out[1]` with the display range; returns false at the
  end, which also ends it), `imgui.listClipperEnd()` for leaving early.
- **Plots.** `plotLines` and `plotHistogram` take the values as a
  `Float32Array` (used in place, no copy) or an `Array` of numbers, and no
  `stride` argument; the getter overloads are not bound.
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
- **Paired functions ImGui does not guard.** See "Errors": the binding
  keeps a depth count for these pairs, which a generator needs as a table
  beside the signatures.

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
- **Clipper**: `listClipperBegin`, `listClipperStep`, `listClipperEnd`.
- **Constants**: every enumerator of the enum types the functions above
  take or return. Each is emitted from the C++ identifier itself (a macro
  over the name), so a value cannot be mistyped.

## Errors

- **Wrong arguments**: `TypeError` with `code` `ERR_INVALID_ARG_TYPE` (or
  `ERR_OUT_OF_RANGE` for a number outside the C++ type, or a reference
  array that is too short), naming the function and the argument. ImGui is
  not called.
- **No frame**: the existing `ERR_IMGUI_NO_FRAME`.
- **Usage errors ImGui recovers from.** ImGui v1.93 reports these through
  `IM_ASSERT_USER_ERROR` and repairs its stacks: an unmatched `end`,
  `endPopup`, `endMenu`, `endCombo`, `endTable`, `endTabBar`,
  `endMainMenuBar`, `popID`, `popStyleColor`, `popStyleVar` or
  `popItemWidth`, and at the end of the frame whatever was left open. The
  addon sets `io.ConfigErrorRecoveryEnableAssert = false` at every
  `newFrame()` (so a recoverable error never aborts a Debug build) and
  `io.ConfigErrorRecoveryEnableTooltip = false` (ImGui's tooltip carries a
  button that turns the asserts back on), and installs ImGui's error
  callback. An error reported during a binding call makes that call throw
  an `Error` with `code` `ERR_IMGUI_USAGE` and ImGui's message, after
  ImGui has recovered. Errors ImGui reports when the frame ends (an
  unclosed `begin`, a `pushID` never popped) make `render()` throw the
  same way, after it has drawn the recovered frame. Every such error is
  also traced (`imgui: usage error: <msg>`). The one case that only
  traces: the discarding `newFrame()` that part A already has for a frame
  left open by an earlier throw.
  - The error callback is `ImGuiContext::ErrorCallback`, which v1.93
    declares in `imgui_internal.h`. That is the binding's one use of the
    internal header; it stays in one file and is named in the README's
    "Bumping ImGui" section.
- **Unbalanced calls ImGui does not guard.** Several bound functions
  assert plainly, or do not check at all, when called without their
  opening call: `endGroup`, `endChild`, `endTooltip`, `endListBox`,
  `endMenuBar`, `treePop`, `drawList.popClipRect`, and the table row and
  column functions outside a table. These are the mistakes the pairing
  rule above invites (calling `endListBox()` after a `beginListBox()` that
  returned false). The binding keeps a depth count per such pair, reset at
  `newFrame()`, raised only when the opening call succeeded, and throws
  `ERR_IMGUI_USAGE` itself, without calling ImGui, when a closing call
  would take the count below zero. This guards underflow, the case that
  crashes; an opening call left unclosed is ImGui's end-of-frame error
  above. The counts are not per window, so closing in a different window
  from the opening one is not detected.
- **Assertions ImGui does not recover from**, other than those, remain
  `assert`: an abort in a Debug build and undefined behaviour in a Release
  build, as for a C++ program. The binding's argument checks cover the
  ones a wrong JavaScript value reaches directly (a negative count, an
  enum value out of range, a table column count over ImGui's limit, a
  format string); each bound function's preconditions are written out in
  the plan from ImGui's source. What is not covered is recorded in the
  tracker. This is the limit of goal 3.
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
  logged.
- **Viewer.** The selected file, read with `fs.promises.readFile`, capped
  at 1 MiB, split into lines and drawn through the list clipper so a long
  file costs only its visible lines. A file that is not valid UTF-8 text
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
canvas, and prints a line per state change (`APP opened <path> <bytes>`),
so the test below has read-backs that do not depend on the machine.

## Structure of the binding

- `addons/imgui/bind/`: `bind_util.h` (argument readers and writers:
  string, number with range, vector, reference array of each kind; the
  frame check; the usage-error check; the registration macro), then one
  `.cpp` per group above, and `bind_constants.cpp`.
- A binding function is a few lines over those helpers, in a fixed shape:
  read and check arguments, check the frame, call ImGui, check for a usage
  error, write back, return. The fixed shape is what part C generates.
- `imgui_module.cpp` keeps the frame protocol and calls one registration
  function per group.
- The addon's export list and public-API-only rule are unchanged.
  `imgui-symbols.js` keeps passing unmodified.

## Tests

To part A's rules: window tests named `test/ui-imgui-*.js` under
`%with-display` with the Sokol exclusions, read-backs and exact pixels,
no timing, each test shown able to fail by a mutation recorded in the
progress file.

- **`imgui-bind-surface.js`**: the module's exact export list and the
  value of a sample of constants per enum type, against a checked-in
  list. An added or removed function fails it.
- **Coverage rule**: every exported function is called by at least one
  test. `utils/check-imgui-bind-coverage.py` checks it from the checked-in
  list and the test sources, run by an ungated lit test, the way
  `ui-stderr-rule.js` runs its script.
- **`ui-imgui-bind-args.js`**: for each convention, wrong types, counts
  and ranges throw the documented error and do not reach ImGui (the item
  count read back from the test hooks does not move); calls outside a
  frame throw `ERR_IMGUI_NO_FRAME`.
- **`ui-imgui-bind-errors.js`**: an unmatched `end`, `pushID` without
  `popID`, `endTable` without `beginTable`, and each pair the binding
  guards itself (`endListBox` after a false `beginListBox`, `treePop`,
  `endGroup`, `popClipRect`, `tableNextRow` outside a table):
  `ERR_IMGUI_USAGE` from the call or from `render()`, the trace line, and
  the next frame works. Also `loadIniSettingsFromMemory` inside a frame,
  and a format string with the wrong conversion.
- **One test per group** (`ui-imgui-bind-widgets.js`, `-windows.js`,
  `-tables.js`, `-popups.js`, `-drawlist.js`, `-plots.js`, ...): the
  script builds a small UI, reads item rectangles back with
  `getItemRectMin/Max`, injects mouse and key events at them through the
  existing harness, and checks the reference arrays and return values
  (click the checkbox: `v[0]` flips and the call returns true; drag the
  slider; type into the field). Draw-list and plot tests check exact
  pixels of flat-coloured shapes, as part A's scenes do.
- **`ui-imgui-app.js`**: the application in test mode: all panels present
  (window count and names read back), a click on a fixture file produces
  `APP opened`, the viewer's line count matches the fixture, the canvas
  shows the expected pixels at the fixed clock, Quit ends with status 0,
  and no usage error is traced.
- The Mac round runs all of it on Metal at 2x, and a person looks at the
  application once on each platform (recorded as a hand check).

## Constraints

- Part A's rules stand: public API only (plus the one internal-header use
  above), logical units, no file written by the addon, new files carry the
  project's copyright line, no commit hashes in documents.
- Hermes: no `performance` global (the application takes time from the
  frame callback and `process.hrtime`), CommonJS only.
- The binding adds no host change. If a panel needs something `hnui`
  lacks, that is an `hnui` gap fixed in the host in its own commit.

## Left to the builder

- The exact function list within the groups, fixed by writing the panels.
- The helpers' names and the macro shape in `bind_util.h`.
- How the usage-error flag is kept (per context) and reset.
- The default layout's ini text, produced by arranging the windows once
  and calling `saveIniSettingsToMemory()`.
- The fixture tree for the application's test mode.
- Which ImGui preconditions each function's argument checks cover, from
  reading ImGui's source for that function.
