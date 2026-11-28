# hermes-node-imgui

The Dear ImGui addon for `hermes-node-ui`. It builds
`hermes-node-imgui.node`, which a program loads as
`require('hermes-node:imgui')`. ImGui itself is vendored, unmodified, in
`external/imgui/imgui/` at tag `v1.93.0-docking`; this directory holds
ImGui's backend on `hnui` and `sokol_gfx`, the module surface (a
hand-written JavaScript binding of 154 ImGui functions and 583
constants), and the test hooks.

Part A (the backend and the frame protocol): design
`docs/superpowers/specs/2026-11-27-imgui-addon-design.md`, plan and the
record of what was measured
`docs/superpowers/plans/2026-11-27-imgui-addon-part-a.md` and
`docs/superpowers/plans/progress-imgui-addon.md`. Part B (the binding and
`examples/imgui-app/`): design
`docs/superpowers/specs/2026-11-27-imgui-binding-design.md`, plan
`docs/superpowers/plans/2026-11-27-imgui-binding.md`, the surface table
(one entry per bound function: JavaScript signature, C++ declaration,
metadata, preconditions with their source lines)
`docs/superpowers/plans/2026-11-27-imgui-binding-surface.md`, and progress
`docs/superpowers/plans/progress-imgui-binding.md`.

## What it is and is not

It is a product component, not an example: the toolkit the UI compositor
exists to host. It is also the first non-trivial client of the public
addon API, and it uses nothing else. Its sources include only `hnui.h` /
`hnui_abi.h`, `node_api.h`, the declarations of the vendored `sokol_gfx.h`
(never `SOKOL_GFX_IMPL`; every `sg_*` symbol resolves against the host when
the addon is loaded) and ImGui. Anything it cannot do through that API is a
gap in `hnui`, to be fixed there. Of ImGui's headers, `imgui_internal.h` is
included by one file, `bind/bind_guards.cpp`, and nothing else.

Not supported:

- The rest of ImGui's API. Only the functions listed under "The surface"
  are bound; a generated binding of the whole API is later work (part C).
  Within the bound functions, what is left out on purpose: the draw
  list's `PushClipRect` / `PopClipRect` (tracker 01a21f89-a743), the getter
  overloads of the plots, `combo`'s item-array overloads, the `ImGuiID`
  overloads of `BeginChild` and `OpenPopup`, the `const void*` overloads
  of `PushID` and `GetID`, the `str_id, fmt` and pointer forms of
  `TreeNode`, the measuring mode of the list clipper, and `InputText`
  callbacks.
- Textures or images from JavaScript, loading fonts, drag and drop,
  multi-select, table sort specs.
- Multi-viewport (ImGui windows as OS windows): the compositor has one
  window.
- IME composition, gamepad input, a change of DPI scale while running.
- Windows.
- Published releases. The release workflows build without
  `HERMES_NODE_ENABLE_UI`, so no release tarball carries the addon or
  `hermes-node-ui`; how the addon would be packaged is undecided. (This is
  about what is shipped, not the build type: a local Release build with
  the option on builds both.)
- `imgui.ini`: `io.IniFilename` and `io.LogFilename` are NULL, so the
  addon never writes a file into the current directory. Settings travel
  as strings (`saveIniSettingsToMemory()`, `loadIniSettingsFromMemory()`),
  and storing them is the program's business.

## Building and finding it

It is built only when `HERMES_NODE_ENABLE_UI` is on, into the same
directory as `hermes-node-ui` (`HERMES_TOOLS_OUTPUT_DIR`, the build's
`bin/`). ImGui is compiled into it with hidden visibility, and an explicit
export list (`exports.map`, an ELF version script; `exports.darwin.txt`,
ld64's `-exported_symbols_list`) limits its exports to the two functions
`NAPI_MODULE_INIT()` defines. The list is needed on top of the visibility
settings because libstdc++ gives namespace `std` default visibility, so any
`std::` template the addon instantiates would otherwise be exported. Keep
the two files in step. `test/imgui-symbols.js` checks the result with `nm`.

ImGui's configuration is `imgui_config.h`, named by `IMGUI_USER_CONFIG`:
`IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`
(the clipboard goes through `hnui`, never to the platform directly), and
`IMGUI_USE_WCHAR32` (so a character outside the Basic Multilingual Plane
reaches a text field intact).

`hermes-node-ui` registers the embedder module `hermes-node:imgui`
(`tools/hermes-node-ui/named_addons.cpp`, a table with one row). Its init
does not contain ImGui: it loads `hermes-node-imgui.node` from the
directory of the running executable, realpath'd, through the same loader
`process.dlopen` uses (`loadNativeAddon`), and returns that file's
exports. So:

- `hermes-node-ui` has no link dependency on the addon, and a program that
  never asks for ImGui does not load it. The addon is loaded as a real
  `.node` file, rather than linked into the host, so that it can reach the
  host only through the public API.
- It loads at the first `require()` or `require.resolve()` of the name.
  `require.resolve('hermes-node:imgui')` returns the specifier itself.
- An absent file throws `MODULE_NOT_FOUND`, naming the full path tried. A
  file that exists and does not load throws `ERR_DLOPEN_FAILED`, as from
  `process.dlopen`. A failed load is not cached, so a later `require()`
  tries again.
- Plain `hermes-node` does not register the name, and `require()` of it
  there throws `MODULE_NOT_FOUND`.

The addon calls `HNUI_CHECK_VERSION(env)` first in its init, so an addon
built against a different `hnui` ABI or Sokol pin fails to load.

## The frame

```js
const ui = require('hermes-node:ui');
const imgui = require('hermes-node:imgui');

async function main() {
  await ui.createWindow(
      {title: 'ImGui', width: 1280, height: 800, highDpi: true});
  const show = [true];
  function frame() {
    imgui.newFrame();
    if (show[0]) {
      // The title bar's close button clears show[0]; so does the button.
      if (imgui.begin('Hello', show)) {
        imgui.text('imgui ' + imgui.version);
        if (imgui.button('Close'))
          show[0] = false;
      }
      imgui.end();
    }
    imgui.render();
    ui.requestAnimationFrame(frame);
  }
  ui.requestAnimationFrame(frame);
}
main();
```

`examples/imgui-demo/demo.js` is the same loop around `showDemoWindow()`;
`examples/imgui-app/` is a whole application.
`requestAnimationFrame` is `hermes-node:ui`'s (also installed as a global);
the window object has none.

- `imgui.newFrame()` reads the window's state with
  `hnui_get_window_state`, sets ImGui's display size, framebuffer scale and
  time step, and calls `ImGui::NewFrame()`. It does **not** take the
  frame's target. It returns `false`, having done nothing, only when this
  runtime has no window; otherwise `true`. The ImGui context is created at
  the first `newFrame()` that finds a window, for the window
  `hnui_get_window(env)` returns, or by the first binding call that needs
  one (below). There is one context per process.
- `imgui.render()` calls `ImGui::Render()`, then asks `hnui_frame_target`
  for the target and draws into it. It returns whether it drew. With no
  target (outside a frame callback, or in CLOSING) it ends the ImGui frame,
  draws nothing and returns `false`.
- **`render()`, not `newFrame()`, takes the target**, because taking it is
  what tells the host the tick drew. A pair interrupted by a throw between
  the two calls takes no target, so if nothing else drew in that tick,
  nothing is presented for it.
- `newFrame()` while a frame is already open ends the open frame
  (`ImGui::EndFrame()`), traces `frame discarded`, and starts a new one. It
  does not throw: the addon cannot tell a second call in one callback from
  the first call after a throw left a frame open. Whatever the discarded
  frame left open is reported by ImGui's recovery and traced, and belongs
  to no call, so it is not thrown.
- More than one `newFrame()`/`render()` pair in a tick is legal, in one
  callback or in several. Each `render()` clears the target, so the last
  one's picture is presented.
- `render()`, `showDemoWindow()` and every other function that needs a
  frame throw `ERR_IMGUI_NO_FRAME` with no frame open, everywhere, an
  `'exit'` listener included. A correctly paired `newFrame()`/`render()`
  does not throw in CLOSING; `render()` returns `false` there.
- **A usage error ImGui reports while the frame ends** (a window, ID or
  scope left open, see "Errors") makes `render()` throw `ERR_IMGUI_USAGE`
  after ImGui has recovered. It has then ended the ImGui frame, has
  **not** taken the target and has drawn nothing, so it contributes
  nothing to the tick, exactly like a throw from application code; the
  tick still presents if another `render()` in it drew. The usage error
  takes precedence over the `false` return.
- `newFrame()` and `render()` throw `ERR_IMGUI_USAGE`, doing nothing,
  when called from inside a `listClipper` callback.
- `imgui.version` is ImGui's `IMGUI_VERSION` string.

The program owns the frame: the addon registers no frame callback of its
own. Every call runs on the JS thread that owns the window.

## The binding

### Calling convention

A binding function is the ImGui function's name with a lower-case first
letter, and its arguments are the C++ ones translated by a fixed rule;
a value ImGui returns through a pointer is a **reference array**, an
`Array` the binding reads before the call and writes after it:

```js
const check = [true];
if (imgui.checkbox('Check', check))
  log('now ' + check[0]);
```

The rules, as `bind/bind_util.cpp` reads and writes them:

| C++ | JavaScript |
|---|---|
| `bool` | A boolean, exactly (`typeof` `'boolean'`). |
| `int` | A number that is an integer from -2^31 to 2^31-1. A non-integer is `ERR_OUT_OF_RANGE`, like an integer out of range. |
| `float` | A number that is finite and within the C `float` range, wherever it appears (a scalar, a vector component, a reference-array element, a plot value). `NaN`, the infinities and a magnitude above `FLT_MAX` are `ERR_OUT_OF_RANGE`. |
| `ImU32`, `ImGuiID` | An integer from -2^31 to 2^32-1, taken modulo 2^32, so a colour assembled with JavaScript's signed bitwise operators is accepted. Returned as 0 .. 2^32-1. |
| An enum or flags type | An `int`, then checked against the exported constants of its type: a flags argument may set only bits some exported enumerator sets, an enum argument must equal one. Otherwise `ERR_OUT_OF_RANGE`. `ImGuiCond` is an enum here, as its header says. A mouse button is an integer from 0 to 4 (ImGui has five buttons and names three). A key must also be a named key or exactly one `Mod_` constant. |
| `const char*` | A string, encoded to UTF-8 per call. A string containing U+0000 is `ERR_INVALID_ARG_VALUE` (ImGui would read it only up to that character). A nullable one also accepts `undefined` or `null`, which pass `NULL`. |
| `const char* fmt, ...` | A finished string: `%` is not interpreted (the binding calls the `Unformatted` variant or passes `"%s"`). |
| `const char* text, const char* text_end` | One string; `text_end` is dropped. |
| `const char* format` of a slider, drag or input | A string with exactly one printf conversion of the value's kind (`f F e E g G a A` for a float, `d i u x X o` for an int), with flags, a width and a precision and nothing else (no `*`, `$`, length modifier or `%n`; `%%` is a literal). The conversion, from `%` to its conversion character, is at most 30 bytes, and its width and precision at most two digits each. Otherwise `ERR_INVALID_ARG_VALUE`. |
| `const ImVec2&`, `const ImVec4&`, or by value | A real `Array` (not a typed array or other array-like) of at least 2 or 4 elements, each a `float`; elements past the 2nd or 4th are ignored. |
| Returns `ImVec2` / `ImVec4` | A new `Array`, or the `Array` passed as the extra last argument, elements 0..1 or 0..3 filled, and that array returned. The extra argument comes after every C++ parameter, so defaults before it are passed as `undefined`: `imgui.getWindowPos(out)`, `imgui.getMouseDragDelta(0, undefined, out)`. It is checked before ImGui is called. |
| `bool* p`, `int* p`, `float* p`, `float v[N]`, `int v[N]` | A reference array: an `Array` of at least N elements (1 for a plain pointer) of the element's type as above, read before the call and written back after it, in parameter order. A nullable one accepts `undefined` or `null`, which pass `NULL` and are not written. |
| `char* buf, size_t buf_size` | A one-element reference array holding a string, then the capacity in bytes, an integer from 1 to 2^24: `imgui.inputText('Name', name, 256)`. The string is encoded and, if longer than capacity minus one, cut at a character boundary. After the call the buffer is compared with the original string's encoding, and written back whenever they differ, whatever the widget returned (a truncation is written back too); a new string is created only then. |
| A struct pointer with no JavaScript form (`const ImGuiViewport*`, `const ImGuiWindowClass*`, `ImGuiStyle*`) | Omitted, passed as `NULL`. |
| A default argument | May be omitted or `undefined`; `null` is a value, accepted only where the parameter is nullable. |
| Returns `bool` or a number | Returned as is; `void` returns `undefined`. |
| `FLT_MAX`, `-FLT_MIN` as meaningful values | `imgui.FLT_MAX`, `imgui.FLT_MIN`: exactly the C `float` values (`-Number.MIN_VALUE` would convert to `-0.0f`). |

Constants: every enumerator of the 25 enum types the bound functions
take, under its C++ name minus `ImGui` or `Im` (`imgui.WindowFlags_NoTitleBar`,
`imgui.Col_Text`, `imgui.Key_Enter`, `imgui.Mod_Ctrl`,
`imgui.DrawFlags_Closed`), emitted from the C++ identifier itself
(`bind/bind_constants.cpp`), so a value cannot be mistyped. Enumerators
the header marks "[Internal]", masks and the other trailing-underscore
names are not exported; `_None`, `_COUNT` and the named range markers
are. 581 enumerators, plus `FLT_MIN` and `FLT_MAX`.

Overloads are one function, dispatched on an argument's JavaScript type,
only where the types differ: `pushID` and `getID` (a string or a number);
`pushStyleColor(idx, col)` (an `Array` is the `ImVec4` overload, a number
the `ImU32` one); `pushStyleVar(idx, val)` (an `Array` is `ImVec2`, a
number `float`); `getColorU32` (below); `collapsingHeader(label, flags)` or
`collapsingHeader(label, pVisible, flags)` (a number or `undefined` second
selects the first, an `Array` or `null` the second); `menuItem` (a
boolean, `undefined` or `null` third argument is `selected`, an `Array` is
`pSelected`); `radioButton(label, active)` or `radioButton(label, v,
vButton)`; `selectable(label, selected)` or `selectable(label,
pSelected)`.

Every binding function, in one fixed shape: refuses re-entry, reads and
checks every argument, checks the frame, runs its guards, counts the call
and calls ImGui, throws if ImGui reported a usage error during the call,
writes the reference arrays back, and returns. A rejected call does not
reach ImGui.

`begin`/`end` pairing follows ImGui exactly and is the caller's job, as in
C++: `end()` and `endChild()` are always called; every other `endX()` only
when its `beginX()` returned true.

**Re-entry and accessors.** Reading or writing an element of an `Array`
can run JavaScript (an accessor on the index). Every argument is read into
native storage before ImGui is called; ImGui never sees JavaScript memory
except a `Float32Array`'s data (below). While any binding function is
running, `newFrame()` and `render()` included, calling another throws
`ERR_IMGUI_REENTRANT` and calls nothing; the one place that is lowered is
a `listClipper` callback. An accessor that throws on read prevents the
call; one that throws on write propagates after the call, ImGui's state
already changed and later outputs not written. A call ImGui reported a
usage error for writes nothing back.

### Exceptions to the convention

- **Draw list.** `ImDrawList*` is not an object here. The functions are on
  `imgui.drawList` and act on the current window's draw list
  (`ImGui::GetWindowDrawList()`): `imgui.drawList.addLine(p1, p2, col,
  thickness)`. Their colours are `ImU32` numbers, from
  `imgui.getColorU32()` or `imgui.colorConvertFloat4ToU32()`. They are
  named `drawList.<name>` in error messages and in `__test.bindCalls()`.
  `addText` binds the overload without a font.
- **List clipper.** `imgui.listClipper(count, itemHeight, (start, end) =>
  { ... })` owns the whole `Begin()` / `Step()` / `End()` loop; the clipper
  lives on the native stack. `count` is 0 .. 2^31-2, `itemHeight` is
  required and at least 1 (ImGui's measuring mode is not bound), and
  `count * itemHeight` is at most 2^30. The callback is called
  synchronously once per range, its return value ignored. Inside it,
  binding functions may be called, a nested `listClipper` included, but
  not `newFrame()` or `render()` (`ERR_IMGUI_USAGE`). After each callback
  the binding compares the current window (and how many times it was begun
  this frame), the window-stack depth, and the current table (and its
  instance this frame) with what they were at entry. If they match, the
  loop continues, or after a throw the clipper is ended normally and the
  exception propagates. If they do not (the callback closed the clipper's
  window or table, left one open, or ended and began one again), the
  clipper is **abandoned**: released with `ItemsCount` set to -1, so that
  `End()` moves no window's cursor and `Step()` is not called again, and
  `ERR_IMGUI_USAGE` is thrown, or the callback's own exception if it
  threw.
- **Plots.** `plotLines` / `plotHistogram(label, values, valuesOffset = 0,
  overlayText = null, scaleMin = FLT_MAX, scaleMax = FLT_MAX, graphSize =
  [0, 0])`: `values` is an `Array` of numbers (copied) or a
  `Float32Array` (used in place, its data taken after every other argument
  has been read; a detached one is `ERR_INVALID_ARG_VALUE`, an infinity or
  `NaN` in it `ERR_OUT_OF_RANGE`). No count and no stride: the count is the
  length. `valuesOffset` is 0 .. length-1 (0 for an empty array).
- **Point lists.** `drawList.addPolyline(points, col, thickness, flags =
  0)` takes `[x0, y0, x1, y1, ...]` as an `Array` or a `Float32Array`; the
  count is its length over two, and an odd length is `ERR_OUT_OF_RANGE`.
- **`getColorU32`.** An `Array` first argument selects the `ImVec4`
  overload, a number the `ImGuiCol` one (`getColorU32(imgui.Col_Text,
  alphaMul)`); the `ImU32` one is not bound.
- **IO accessors.** `ImGuiIO` is not an object here. `getFrameRate()`,
  `getDeltaTime()`, `getDisplaySize(out)`, `wantCaptureMouse()` and
  `wantCaptureKeyboard()` read its fields.
- **Settings.** `loadIniSettingsFromMemory(string)` and
  `saveIniSettingsToMemory()` (which returns a string) take and return
  strings without a size. **Give `loadIniSettingsFromMemory` only text
  `saveIniSettingsToMemory` produced.** ImGui parses settings without
  validating them, and its docking handler builds dock nodes from whatever
  IDs and parents the text names: a node naming itself as its parent makes
  the load never return, a window docked into a node that does not exist
  aborts the load, and a chain of 300 nested nodes aborts the next save
  (each measured, loaded between frames). The binding does not check the
  text, which would be ImGui's parser written again; settings are trusted
  input, as ImGui's own `imgui.ini` is. `loadIniSettingsFromMemory` must
  be called *outside* a frame and throws `ERR_IMGUI_USAGE` inside one: it
  rebuilds the dock nodes, which the frame's dock space is using.
- **`combo`.** The convenience overloads taking item arrays are not
  bound; `beginCombo` / `selectable` / `endCombo` are.

### Outside a frame

Every function needs an open frame (between `newFrame()` and `render()`)
and throws `ERR_IMGUI_NO_FRAME` otherwise, except:

- **Inside or outside a frame**, creating the context on demand
  (`ERR_IMGUI_NO_WINDOW` with no window): `getTime`, `getFrameRate`,
  `getDeltaTime`, `getDisplaySize`, `wantCaptureMouse`,
  `wantCaptureKeyboard`, `styleColorsDark`, `styleColorsLight`,
  `saveIniSettingsToMemory`.
- **Outside a frame only**, creating the context on demand:
  `loadIniSettingsFromMemory`.
- **No frame and no context**: `colorConvertFloat4ToU32`, and the
  `version` property.
- `newFrame()` opens the frame.

The context is created by one function, which configures error recovery
and installs the error callback (see "Errors") before any binding call can
meet it.

### The surface

Generated from `test/fixtures/imgui/bind-groups.js`, which is the
checked-in authority: `test/imgui-bind-surface.js` fails if the module's
exports and that file disagree, in either direction, functions or
constants. The surface table has each function's signature.

- **Frame** (10, and `version`): `newFrame`, `render`, `showDemoWindow`,
  `showMetricsWindow`, `getTime`, `getFrameRate`, `getDeltaTime`,
  `getDisplaySize`, `wantCaptureMouse`, `wantCaptureKeyboard`.
- **Windows** (15): `begin`, `end`, `beginChild`, `endChild`,
  `setNextWindowPos`, `setNextWindowSize`, `setNextWindowBgAlpha`,
  `getWindowPos`, `getWindowSize`, `getContentRegionAvail`,
  `isWindowFocused`, `isWindowHovered`, `getScrollY`, `getScrollMaxY`,
  `setScrollHereY`.
- **Layout** (20): `sameLine`, `separator`, `separatorText`, `spacing`,
  `dummy`, `newLine`, `indent`, `unindent`, `beginGroup`, `endGroup`,
  `setNextItemWidth`, `pushItemWidth`, `popItemWidth`,
  `getCursorScreenPos`, `setCursorScreenPos`, `getTextLineHeight`,
  `getTextLineHeightWithSpacing`, `getFrameHeight`, `calcTextSize`,
  `alignTextToFramePadding`.
- **IDs** (3): `pushID`, `popID`, `getID`.
- **Text** (6): `text`, `textColored`, `textDisabled`, `textWrapped`,
  `bulletText`, `labelText`.
- **Widgets** (28): `button`, `smallButton`, `invisibleButton`,
  `arrowButton`, `checkbox`, `radioButton`, `progressBar`, `bullet`,
  `sliderFloat`, `sliderInt`, `sliderFloat2`, `sliderFloat3`,
  `sliderFloat4`, `dragFloat`, `dragInt`, `inputText`,
  `inputTextMultiline`, `inputTextWithHint`, `inputFloat`, `inputInt`,
  `beginCombo`, `endCombo`, `selectable`, `colorEdit3`, `colorEdit4`,
  `colorButton`, `beginListBox`, `endListBox`.
- **TreesTabs** (9): `treeNode`, `treeNodeEx`, `treePop`,
  `collapsingHeader`, `setNextItemOpen`, `beginTabBar`, `endTabBar`,
  `beginTabItem`, `endTabItem`.
- **Menus** (7): `beginMainMenuBar`, `endMainMenuBar`, `beginMenuBar`,
  `endMenuBar`, `beginMenu`, `endMenu`, `menuItem`.
- **Tables** (8): `beginTable`, `endTable`, `tableSetupColumn`,
  `tableSetupScrollFreeze`, `tableHeadersRow`, `tableNextRow`,
  `tableNextColumn`, `tableSetColumnIndex`.
- **Popups** (10): `openPopup`, `beginPopup`, `beginPopupModal`,
  `beginPopupContextItem`, `endPopup`, `closeCurrentPopup`, `setTooltip`,
  `setItemTooltip`, `beginTooltip`, `endTooltip`.
- **Queries** (15): `isItemHovered`, `isItemClicked`, `isItemActive`,
  `isItemDeactivatedAfterEdit`, `getItemRectMin`, `getItemRectMax`,
  `setItemDefaultFocus`, `setKeyboardFocusHere`, `isKeyPressed`,
  `isKeyDown`, `isMouseDown`, `isMouseClicked`, `isMouseDoubleClicked`,
  `getMousePos`, `getMouseDragDelta`.
- **Style** (8): `styleColorsDark`, `styleColorsLight`, `pushStyleColor`,
  `popStyleColor`, `pushStyleVar`, `popStyleVar`, `getColorU32`,
  `colorConvertFloat4ToU32`.
- **Plots** (2): `plotLines`, `plotHistogram`.
- **DrawList** (9, on `imgui.drawList`): `addLine`, `addRect`,
  `addRectFilled`, `addCircle`, `addCircleFilled`, `addTriangleFilled`,
  `addBezierCubic`, `addPolyline`, `addText`.
- **Clipper** (1): `listClipper`.
- **Docking** (3): `dockSpaceOverViewport`, `loadIniSettingsFromMemory`,
  `saveIniSettingsToMemory`.

Each group has a window test, `test/ui-imgui-bind-<group>.js` (Frame's is
`ui-imgui-bind-args.js`; Layout, IDs, Text and Queries share
`-layout.js`), which ends with `harness.checkGroupCoverage(h,
'<group>')`: it fails unless every function of the group has a non-zero
`__test.bindCalls()` count, that is, has actually reached ImGui in that
test. A function added to `bind-groups.js` without a test fails its
group's test; one added to the module and not to `bind-groups.js` fails
`imgui-bind-surface.js`.

### Errors

| Class, `code` | When |
|---|---|
| `TypeError`, `ERR_INVALID_ARG_TYPE` | An argument of the wrong type, or missing. |
| `TypeError`, `ERR_INVALID_ARG_VALUE` | A string with U+0000; a format that does not fit; a detached `Float32Array`. |
| `RangeError`, `ERR_OUT_OF_RANGE` | A number outside its range (a non-integer `int` included); a flag bit or enum value that is not exported, or not accepted by that function; a vector or reference array with too few elements; a limit exceeded (below). |
| `Error`, `ERR_IMGUI_NO_FRAME` | A function that needs a frame, called with none open. |
| `Error`, `ERR_IMGUI_NO_WINDOW` | A function that needs the context, called before any window exists. |
| `Error`, `ERR_IMGUI_USAGE` | A usage error ImGui reported and recovered from; a guarded precondition (below) that does not hold; `loadIniSettingsFromMemory` in a frame; `newFrame()` / `render()` in a clipper callback; an abandoned clipper. |
| `Error`, `ERR_IMGUI_REENTRANT` | A binding function called while another is running (from an accessor of its arguments). |

Every message starts `imgui.<name>(): ` and names the argument or the
condition. Each is thrown before ImGui is called, except an
`ERR_IMGUI_USAGE` that ImGui reported (thrown after the call, once ImGui
has recovered) and an abandoned clipper's.

**Usage errors ImGui recovers from.** ImGui reports these through
`IM_ASSERT_USER_ERROR` and repairs its state. Context creation sets
`io.ConfigErrorRecovery`, and clears `ConfigErrorRecoveryEnableAssert` (so
a recoverable error never aborts a Debug build),
`ConfigErrorRecoveryEnableTooltip` (ImGui's tooltip carries a button that
turns the asserts back on) and `ConfigErrorRecoveryEnableDebugLog` (with it
set ImGui `printf`s every error to stdout, from inside a library); and it
installs ImGui's error callback, once, which traces every message and
records the first. The four flags are then set again before every call
into ImGui that can report an error: by every binding function just
before its ImGui call (`Call::enterImGui()`, `render()`'s `Render()`
included), by `newFrame()` before it ends a discarded frame, and by the
list clipper after each callback, before its own `Step()` and `End()`.
That is what keeps them off: ImGui code can turn them back on, and the
demo window does (`showDemoWindow()` with its Configuration >
Configuration node open sets all three outputs on every frame,
imgui_demo.cpp:626-627, and its Tools > Debug Options menu has a checkbox
for the assert). Setting them at context creation and at each
`newFrame()` alone left the rest of such a frame, its `render()` and the
next discard running with the assert on, so a recoverable usage error
aborted a Debug build. `newFrame()` also resets the combo depth (see
"Bumping ImGui"); nothing touches the callback after creation. A message
reported during a binding call makes that call throw `ERR_IMGUI_USAGE`
with ImGui's message, after ImGui has recovered; ImGui's state is
consistent afterwards and the frame may continue. One reported while the
frame ends makes `render()` throw (above). Each is traced as `usage
error: <message>`. The audited list for this surface (the surface table
has each site's line):

- **Raised by a call:** `end` ("Calling End() too many times!"; on a popup
  "Must call EndPopup() and not End()!", on a child "Must call EndChild()
  and not End()!", both of which end the window anyway); `endPopup`,
  `endMenu`, `endCombo` (in the wrong window); `endMainMenuBar` (not from a
  menu bar); `endTabBar` (mismatched); `beginTabItem` and `endTabItem`
  (outside a tab bar); `endTable` (outside a table; "Mismatching
  PushID/PopID!", "Too many PopItemWidth!"); `tableSetupColumn` (outside a
  table, too many calls, after the first row, a width without an explicit
  sizing policy); `tableSetupScrollFreeze` and `tableHeadersRow` (outside a
  table); `tableSetColumnIndex` (an invalid index); `popID`,
  `popStyleColor`, `popStyleVar`, `popItemWidth` (too many); `pushStyleVar`
  (the overload of the wrong type: nothing pushed); `isItemHovered` and
  `isWindowHovered` (flags they do not accept); and `end`, `endGroup` or a
  table call ending a cell after `setCursorScreenPos` moved the cursor past
  the window's or parent's boundary without an item after it. Four rows
  the audit found cannot be reached through the binding because a check
  of its own comes first: `endCombo` (its guard), the draw list's flag
  check (G1 rejects the bits) and the clipper's item-height failure (the
  measuring mode is not bound).
- **Raised when a window ends**, by `end()` and by everything that ends a
  window internally (`endChild`, `endPopup`, `endTooltip`,
  `endMainMenuBar`, `endListBox`, `endMenu`, `endCombo`, and `endTable` of a
  scrolling table), in this order: "Missing EndTable()", "Missing
  EndTabBar()", "Missing EndMenuBar()", "Missing TreePop()", "Missing
  EndGroup()", "Missing PopID()", "Missing PopStyleColor()", "Missing
  PopStyleVar()". An open tab item is not recovered as such (ImGui's own
  FIXME); it shows as the tab bar's or the ID stack's message.
- **Raised when the frame ends**, by `render()`: every window still begun
  is ended, as "Missing EndTable()" (a scrolling table's inner child),
  "Missing EndChild()" or "Missing End()" (any other window, a popup,
  modal, combo or tooltip included), and each such end runs the window
  recovery above. The first message is the one thrown.

Not diagnosed, as in C++: a `pushItemWidth` without its pop. ImGui clears
that stack when the window next begins.

### Guarded preconditions

Some bound functions assert plainly in ImGui, or dereference without
checking, when called in the wrong state: the mistakes the pairing rule
invites (`endListBox()` after a `beginListBox()` that returned false).
For each, `bind/bind_guards.cpp` evaluates **the condition ImGui asserts
or relies on, read from ImGui's own state**, and throws `ERR_IMGUI_USAGE`
without calling ImGui when it does not hold. Because the state read is
ImGui's, the guard stays right after ImGui has recovered from an earlier
error in the same frame, and follows ImGui's own rules (an `endChild()`
after a `beginChild()` that returned false is valid; a `treeNodeEx` with
`TreeNodeFlags_NoTreePushOnOpen` opens nothing to pop). A guard returns
early wherever ImGui returns before its own assertion (a window with
`SkipItems` set, for the closers that test it), so it does not refuse
what ImGui accepts, except where said below. A refusal is counted under
its condition's name (`__test.guards()`) and traced as a usage error.

| Condition | Refuses |
|---|---|
| `labelId:window-id` | A label that would give the item the current window's own ID (an empty label, or one ending in `###`, with nothing pushed), which `ItemAdd()` asserts against. Applied by the 27 functions whose label is an ID (the surface table marks them `Label-id: yes`). Stricter than ImGui, which skips its assertion for a clipped item. |
| `canEndGroup:no-group`, `:wrong-window` | `endGroup()` with no group open, or with the top group begun in another window. |
| `canEnd:table-region` | `end()` inside a scrolling table's own region, which ImGui's window recovery would end twice. The check walks ImGui's recovery chain through the plain tables nested in that window. |
| `canEndChild:not-child`, `:table-region` | `endChild()` outside a child window; in a scrolling table's region, as for `end()`. |
| `canEndListBox:not-child`, `:table-region`, `:no-group`, `:wrong-window` | `endListBox()` outside a child window, in a scrolling table's region, or with the list box's group missing or another window's group on top in the parent window. |
| `canEndCombo:no-combo`, `:wrong-window` | `endCombo()` with no combo open, or in another window than the combo's popup. ImGui's own check is recoverable, but it decrements a depth counter before checking and never resets it, so the binding makes the same comparison first; `newFrame()` sets that counter to 0 (see "Bumping ImGui"). |
| `canTreePop:no-node`, `:id-mismatch`, `:id-stack` | `treePop()` with no tree node open; with an ID pushed inside a node that keeps stack data still on the ID stack; after the node's own ID was popped. |
| `canEndTabItem:no-tab-item` | `endTabItem()` with no tab item begun in the current tab bar. |
| `canBeginMenuBar:already-open` | `beginMenuBar()` while that window's menu bar is open. |
| `canEndMenuBar:no-menu-bar-flag`, `:not-open`, `:no-group`, `:wrong-window` | `endMenuBar()` in a window without `WindowFlags_MenuBar`, with no menu bar open, or with the menu bar's own group ended. |
| `canEndMainMenuBar:no-group`, `:wrong-window` | `endMainMenuBar()` with the menu bar's own group ended. |
| `canEndTooltip:not-tooltip` | `endTooltip()` outside a tooltip. |
| `canBeginPopupContextItem:no-id` | `beginPopupContextItem()` with no `strId` after an item that has no ID (`text`, `dummy`). |
| `canTableRowCall:no-table` | `tableNextRow()` outside a table (ImGui dereferences NULL). The other row calls report a missing table themselves or return false. |
| `canTableRowCall:wrong-window` | `endTable()`, or `tableNextRow()`, `tableNextColumn()` or `tableHeadersRow()` that would end a row, in another window than the table's own (a window begun inside a cell is current). |
| `canTableSetColumnIndex:no-table`, `:no-row` | `tableSetColumnIndex()` outside a table (stricter than ImGui, which returns false), or before the first `tableNextRow()`. |
| `canTableSetupScrollFreeze:layout-locked` | `tableSetupScrollFreeze()` after the table's first row. |
| `canEndTable:cursor-moved` | `endTable()` with no row begun and the cursor moved by `setCursorScreenPos()`. |
| `canTableSetupColumn:name-offset` | `tableSetupColumn()` with a label once the table's column names pass 32767 bytes. |
| `canTableSetupColumn:all-disabled` | The `tableSetupColumn()` that would declare the last column with every column `TableColumnFlags_Disabled` (queued column changes included). |
| `canListClipper:wrong-window` | `listClipper()` with a table current, in another window than the table's own, whether or not a row is open (the clipper ends a row there and seeks the table's row position from that window's cursor). |
| `canDockSpaceOverViewport:twice` | `dockSpaceOverViewport()` a second time in a frame with the same ID (unless `DockNodeFlags_KeepAliveOnly`). A zero ID is the default ImGui computes, and the guard computes it the same way. |
| `canDockSpaceOverViewport:not-root` | `dockSpaceOverViewport()` with the ID of a split node's child. Stricter than ImGui twice over: ImGui accepts the call and only marks the node, which then asserts at the next submission; and with the host window's `SkipItems` set, which the guard cannot see before the call, ImGui would accept a second call or a child's ID that the guard refuses. |

A guard checks that something of the closer's kind is open, as ImGui's
own assertion does, and no more. Where ImGui's check is weaker than
correct pairing (a second `endTabItem()` passes it), so is the binding's.
And it does not know who opened a scope: a closer applied to a scope
ImGui opened internally for another construct can pass its guard and
crash later, inside ImGui's recovery. That, and the other preconditions
the binding leaves unprevented, is tracker issue 01a21f89-a69d, which
points to the surface table's "Not covered" section.

### Limits

Where ImGui has no bound and a large value is an unbounded allocation, a
failed assertion later, or an undefined conversion to `int`, the binding
imposes one, and the error's message names it. Every limit below throws a
`RangeError` with `ERR_OUT_OF_RANGE`, except the first, which is part of
the format rule and throws a `TypeError` with `ERR_INVALID_ARG_VALUE`:

- a format's conversion, at most 30 bytes (ImGui copies it into 32-byte
  buffers behind a plain assertion), and its width and precision at most
  two digits each, so at most 99 (both reach `vsnprintf` every frame,
  where `%.999999999f` takes seconds, and ImGui itself ignores a
  precision over 99): `ERR_INVALID_ARG_VALUE`;
- plots, at most 4096 values (above that ImGui's own index arithmetic can
  reach its plain assertion), and `graphSize[0]` below 2^31;
- `drawList.addPolyline`, at most 16383 points, and `addBezierCubic`, at
  most 512 segments (a single primitive must fit 16-bit indices);
- `listClipper`, `itemHeight` at least 1 and `count * itemHeight` at most
  2^30;
- an `inputText` family capacity, 1 to 2^24 bytes;
- an item width (`setNextItemWidth`, `pushItemWidth`) and the sizes of
  `setNextWindowSize`, `beginChild`, `inputTextMultiline` and
  `beginListBox`, below 2^31 (ImGui converts a positive one to `int`; a
  negative one, relative to the available space, is accepted at any
  magnitude);
- `setNextWindowPos`'s position, from -2^31 to below 2^31 on each
  component;
- `beginTable`, 1 to 511 columns, and `outerSize` (a positive
  component) and `innerWidth` below 2^31; `tableSetupScrollFreeze`, 0 to
  511 columns and 0 to 127 rows;
- `setKeyboardFocusHere`'s offset, -1 to 2147483646 (ImGui stores
  `offset + 1` in an `int`);
- a mouse button, 0 to 4;
- `sliderFloat` and `sliderFloat2`..`4`: `vMin >= -FLT_MAX/2` and
  `vMax <= FLT_MAX/2`; `sliderInt`: `vMin >= INT_MIN/2` and
  `vMax <= INT_MAX/2` (the two bounds ImGui's slider asserts, and nothing
  more: `vMin` is not checked from above, nor `vMax` from below);
  `setScrollHereY`'s ratio from 0 to 1.

Beyond those, each function refuses the flag combinations and values
ImGui asserts on plainly (`InputTextFlags_Callback*`, `Col_COUNT`,
`TabItemFlags_Leading` with `_Trailing`, and so on); the surface table's
"Rejected" line of each entry lists them.

## The backend

Platform half (`imgui_platform.cpp`):

- **Everything is in logical units.** The display size is the window's
  logical size and `DisplayFramebufferScale` is framebuffer size over
  logical size (1 for a zero-sized window), both read from
  `hnui_get_window_state` every frame, so a resize needs no event handling.
  `hnui` reports mouse positions in logical units and the wheel in Sokol's
  scroll units, and both go to ImGui unconverted. Neither the style nor the
  font size is scaled.
- One native event listener (`hnui_add_event_listener`), registered when
  the context is created, feeds ImGui's input queue. Native listeners run
  before JavaScript ones, so ImGui has an event before the program's own
  listeners see it. Mouse buttons are renumbered (`hnui` uses the DOM's
  left 0, middle 1, right 2; ImGui left 0, right 1, middle 2). Key codes
  are Sokol's, which are GLFW's, so the key table is ImGui's GLFW backend's
  table at the pinned tag, re-typed with numeric values. A `KEY_DOWN` with
  `repeat` set is dropped, since ImGui generates key repeat itself; a
  repeated `TEXT_INPUT` is passed on. Modifiers are sent with every mouse
  button and key event. Focus and blur go to `AddFocusEvent`, mouse leave
  moves the mouse to `-FLT_MAX`.
- Input that arrives before the context exists is not seen by ImGui: the
  listener does not exist yet, and nothing replays earlier events. In
  practice ImGui starts with the mouse position unknown, so nothing shows
  as hovered until the next mouse event, even with the pointer already
  over the window; a key or button held across that first frame is not
  known to be down.
- The time step comes from a monotonic clock read in `newFrame()`, never
  below 1e-5 s (ImGui asserts on a time step that is not positive), and
  1/60 s on the first frame.
- Cursor: after a drawing `render()`, ImGui's requested cursor is mapped to
  `hnui_set_cursor`, and `hnui_show_cursor` hides the system cursor when
  ImGui asks for none or draws its own (`io.MouseDrawCursor`). Shape and
  visibility are cached separately, and each call is made only when its
  value changed; no shape is set while the cursor is hidden. Wait and
  Progress have no `hnui` counterpart and get the arrow.
- Clipboard: ImGui's set function is `hnui_write_clipboard`, its get
  function `hnui_read_clipboard_sync` with a 500 ms deadline, the result
  released with `hnui_free`. A failed read is an empty paste and one trace
  line. `HNUI_EVENT_PASTE` is ignored: ImGui pastes on its own key chord,
  and handling both would paste twice.
- `io.ConfigMacOSXBehaviors` is ImGui's default, which follows the build
  platform, so the shortcut modifier is Cmd on macOS and Ctrl elsewhere.

Renderer half (`imgui_renderer.cpp`), written against `sokol_gfx`
directly:

- One shader, with hand-written GLSL 410 and Metal Shading Language
  sources, chosen at run time from `sg_query_backend()`. The MSL source is
  compiled by Sokol at run time.
- One pipeline: alpha blending, no depth, `ImDrawVert` layout, 16-bit
  indices. Its colour format and sample count are the target's, and it is
  rebuilt only if a later target differs. A format and sample count it
  failed for is not retried.
- The device objects (shader, sampler, a 1x1 white image for commands with
  no texture, and the two buffers) are made at the first drawing
  `render()`, inside a frame callback, not when the context is created:
  `newFrame()` may run outside a frame callback, and Sokol resources are
  made inside one.
- **Vertex and index data are appended, never updated** (`sg_append_buffer`
  on buffers created with `usage.dynamic_update`). Sokol allows one
  `sg_update_buffer` per buffer per Sokol frame, and a Sokol frame here is
  the host's commit, which every `render()` of a tick shares. Each draw
  list is appended and then drawn before the next is appended. When an
  append would not fit, the buffer is replaced by one at least twice the
  size, which has had no append in this frame; draws already issued from
  the old buffer are unaffected.
- **Textures are replaced, never updated**, following ImGui's texture
  protocol (`ImGuiBackendFlags_RendererHasTextures`). `sg_update_image`
  takes a whole image, once per Sokol frame, so for a texture ImGui wants
  created or updated the renderer makes a new immutable image and view
  from the texture's full pixels, hands ImGui the new view's id, and
  destroys the previous pair once the new one is valid. ImGui's status for
  the texture changes only then. The cost is a whole-atlas upload per
  change; the atlas changes when new glyphs or sizes first appear. Only
  ImGui's default format, RGBA32, is uploaded. An ImGui texture id is a
  Sokol view id; one linear clamp-to-edge sampler serves every texture.
- `ImGuiBackendFlags_RendererHasVtxOffset` is set: a command's vertex
  offset becomes the draw's base vertex, so a list past 65,536 vertices
  draws with 16-bit indices. A backend without `draw_base_vertex` is
  refused at context creation with an unconditional stderr line.
- ImGui's reset-render-state callback re-applies the pipeline, the
  viewport and the projection; any other user callback is called.
- `render()` owns one pass on the target: it clears to {26, 26, 31} and
  draws. It does not commit; the host commits once the tick's last
  callback returns.
- The GPU objects belong to the host's producing `sokol_gfx` instance and
  survive a resize. There is no teardown: `hnui` gives no notification and
  the process ends in `_exit()`.

## Trace lines

Under `HERMES_NODE_DEBUG_NATIVE=UI` the addon prints these to stderr, each
prefixed `[ui] imgui: `, each written with one `fwrite`. A healthy program
prints none of them every frame. Two can repeat once something has gone
wrong: `frame discarded`, at every `newFrame()` that finds a frame open (so
a program that throws between `newFrame()` and `render()` on every tick
prints it on every tick), and `<vertex|index> buffer could not grow`, at
every `render()` whose data does not fit while the larger buffer cannot be
made. A texture that fails to upload keeps its ImGui status, so its upload
is retried at every `render()`, but `texture <id> failed` is printed once
per texture, until an upload of it succeeds or the renderer destroys it. A
texture ImGui discards itself, because no upload of it ever succeeded,
never reaches the renderer and stays marked as traced; nothing can observe
that, since ImGui never reuses a texture id. Clipboard lines carry
lengths, never contents.

| Line | When |
|---|---|
| `addon loaded (imgui <version>)` | The addon's init finished. |
| `context created (imgui <version>, docking=<0\|1>)` | The context was created (the first `newFrame()` or binding call that found a window). |
| `event listener registered (id <n>)`, `event listener FAILED (id 0)` | The context's event listener. |
| `event rejected: size <n>` | The first event too small for this `hnui` ABI; printed once. |
| `frame discarded` | `newFrame()` found a frame open. |
| `usage error: <message>` | A usage error: each message ImGui's error callback receives (several can come from one call; the first is thrown), each guard refusal, and `loadIniSettingsFromMemory`'s refusal inside a frame. |
| `listClipper: abandoned, the callback changed the window scope` | A clipper was abandoned. |
| `device objects failed; nothing will be drawn` | The shader, sampler, white image or buffers could not be made. |
| `pipeline created (format <f>, samples <n>)`, `pipeline failed (format <f>, samples <n>)` | A pipeline was made, or could not be, for a target. |
| `texture <id> created <w>x<h>`, `texture <id> replaced <w>x<h>` | An ImGui texture uploaded for the first time, or uploaded again under the same id. |
| `texture <id> failed (format <f>)`, `texture <id> failed` | A texture in a format other than RGBA32, or whose image or view was not valid. Printed once per texture; the upload is retried at every `render()`. |
| `texture <id> destroyed` | ImGui asked for a texture to be destroyed. |
| `vertex buffer grown to <n> bytes`, `index buffer grown to <n> bytes` | A buffer replaced by a larger one. |
| `vertex buffer could not grow to <n> bytes`, `index buffer could not grow to <n> bytes` | The larger buffer could not be made; the rest of that `render()` draws nothing. |
| `unsupported sokol backend <n>` | The host's backend is neither GL core nor Metal on macOS; the renderer only clears. |
| `cursor hidden`, `cursor shown` | Printed before the `hnui_show_cursor` call. |
| `cursor <imgui cursor> -> hnui <hnui cursor>` | Printed before the `hnui_set_cursor` call. |
| `clipboard write requested, <n> bytes` | Printed before the `hnui_write_clipboard` call; the host's own read-back line is what shows it took effect. |
| `clipboard get: ok, <n> bytes`, `clipboard get: status <s>` | A paste's read finished, or failed with that `hnui` status. |
| `test quad image created (view <n>)` | The `quad` test scene's first use. |

One line is printed whether or not tracing is on: `sokol backend lacks
draw_base_vertex`, because a renderer that silently draws nothing is the
failure it exists to report.

The cursor and clipboard lines are printed before their `hnui_*` call so
that they precede the host's trace of applying the command on the platform
thread.

## Test hooks

With `HERMES_NODE_UI_TEST_HOOKS` set (unset, empty or `0` is off, as for
the host), the module also exports `__test`; otherwise it is absent.

`__test.scene(name[, arg])` draws a fixed native scene into the open frame,
so it is called between `newFrame()` and `render()`; with no frame open it
throws `ERR_IMGUI_NO_FRAME`. `arg` is an integer (default 0), except for
`widgets`, which also takes the string `softcursor` or `hardcursor`. Every
drawing scene uses the background draw list with anti-aliased fill off, so
rectangle edges are exact.

| Scene | Draws |
|---|---|
| `none` | Nothing. |
| `fill` | A full-display red fill. |
| `clip` | A full-display green fill under the clip rectangle (100,100)-(300,200). |
| `many` | 19,200 4-unit quads, 160 by 120, tiling (0,0)-(640,480): 76,800 vertices in one list, which makes ImGui split it into commands with non-zero vertex offsets and makes both buffers grow. Quad (c, r) is red, green or blue for (c + r) % 3 = 0, 1, 2. |
| `two` | A full-display fill, red for arg 0, blue otherwise. |
| `callback` | A user callback that shrinks the viewport to the top-left quarter, then (for arg 0 only) ImGui's reset callback, then a full-display green fill. |
| `quad` | A 2x2 image of its own (red, green / blue, white) over (100,100)-(300,300). The image is made on first use, which must be inside a frame callback. |
| `glyphs` | `abc` at the default size at (20,20); for a non-zero arg also `XYZ` at 40 units at (20,200), which makes ImGui update the font atlas. |
| `wheel` | Nothing; records `io.MouseWheel` when it is non-zero, inside the frame (ImGui clears it when the frame ends). |
| `widgets` | A window `T` at (50,50), 300x200, holding a 100x30 button `B` and a text field; counts clicks, presses of A, and the wheel. `softcursor` sets `io.MouseDrawCursor`, `hardcursor` clears it, and the setting stays for later frames. |

`__test.bindCalls()` returns an object mapping each binding function's
name to how many times it has called ImGui; a function that never has is
absent. The count is taken immediately before the ImGui call
(`Call::enterImGui()`) and nowhere else, so a rejected call does not
count; and only under the test hooks, its one reader, so an ordinary run
does no counting. Draw-list functions are named `drawList.<name>`. This is what the
coverage rule reads.

`__test.guards()` returns an object mapping every guard condition
registered in `bind_guards.cpp` (`canEndGroup:no-group`,
`labelId:window-id`, ...) to how many times it has refused; every one is
present, refused or not, so `test/ui-imgui-bind-errors.js` can fail on a
condition with no refusal case (`harness.checkGuardCoverage`).

`__test.setDoubleClickTime(seconds)` sets `io.MouseDoubleClickTime` (and `io.MouseSingleClickDelay` to `seconds + 0.2`, which ImGui requires to be larger), creating the context (`ERR_IMGUI_NO_WINDOW` without a window), and returns the previous value, so a double-click test does not depend on how long its frames take.

`__test.setTooltipDelay(stationary[, delayShort])` sets `style.HoverStationaryDelay` and `style.HoverDelayShort` (the second defaulting to the first), the two delays of the hover check `setItemTooltip()` makes, creating the context (`ERR_IMGUI_NO_WINDOW` without a window), and returns their previous values as `[stationary, delayShort]`, so `setTooltipDelay(...previous)` restores both. With 0 a tooltip test does not depend on how long its frames take.

`__test.setErrorOutputs(on)` sets ImGui's three error outputs
(`io.ConfigErrorRecoveryEnableAssert`, `...EnableDebugLog`,
`...EnableTooltip`) all to `on`, as the demo window does, creating the
context (`ERR_IMGUI_NO_WINDOW` without a window), and returns the values
they had as `[assert, debugLog, tooltip]`: what the last binding call left
in force. `test/ui-imgui-bind-recovery.js` uses it to show that the next
binding call turns them off again.

`__test.tableScope()`, inside a frame (`ERR_IMGUI_NO_FRAME` otherwise), returns `{table, currentWindow, innerWindow}`: whether ImGui has a current table, and the names of the current window and of that table's inner window (`""` without one), so a test of a table guard can show it reached the state the guard is about before relying on it.

`__test.detachArrayBuffer(buffer)` detaches an `ArrayBuffer` with `napi_detach_arraybuffer`, so that a test can pass the binding a `Float32Array` whose buffer is gone (this runtime has no `ArrayBuffer.prototype.transfer` or `structuredClone`, and `hermes-node-ui` takes no VM options); the binding then throws `ERR_INVALID_ARG_VALUE`.

`__test.state()` returns, as of the last completed `newFrame()`/`render()`
pair (except `frameOpen`, which is now): `frameOpen`, `displayWidth`,
`displayHeight`, `scaleX`, `scaleY`; `targetGeneration`, `targetWidth`,
`targetHeight` of the last target drawn into; the last drawing frame's
`commands`, `vertices`, `indices` and `liveTextures`; `atlasTexId` (the
font atlas's ImGui texture id, which a re-created texture does not keep);
`mouseX`, `mouseY`, `mouseDown` (three booleans, ImGui's numbering);
`wantCaptureMouse`, `wantCaptureKeyboard`, `wantTextInput`; from the
`widgets` scene `clicks`, `keyAPresses`, `keyADown`, `text`, `wheel`,
`buttonRect`, `fieldRect` (`[x0, y0, x1, y1]`); `lastWheel` from the
`wheel` scene; `cursor`, the ImGui cursor shape last set (-1 before
any); and `windows`, one `{name, active, docked}` per window ImGui knows,
in ImGui's order, as of the last ended frame (empty before the context
exists). ImGui spreads queued input over frames, so a test runs frames
until the state it expects appears, bounded by a frame count.

## Tests

`test/imgui-*.js` need no window: `imgui-pin.js` (the README's tag against
`IMGUI_VERSION`, and the docking branch), `imgui-load.js`,
`imgui-missing.js` (a hard-linked copy of the binary in an empty
directory, where one process meets the addon absent and then unloadable),
`imgui-retry.js` (the same script, then the real addon put in place and
loaded by the same process), `imgui-symbols.js`, `imgui-bind-surface.js`
(the exports against `bind-groups.js`, functions and every constant's name
and a sample of values), and `imgui-app-viewer.js` (the application's
file viewer, under plain `hermes-node`). The window tests are
`test/ui-imgui-*.js`, so that `utils/check-ui-stderr-rule.py` covers them,
and share `test/fixtures/imgui/harness.js`. Part A's: `ui-imgui-frame.js`,
`-draw.js`, `-textures.js`, `-input.js`, `-clipboard.js`, `-cursor.js`,
`-resize.js` and `-demo.js`. The binding's: `ui-imgui-bind-args.js` (the
convention, the Frame group, re-entry, accessors),
`ui-imgui-bind-errors.js` (usage errors and a refusal case for every guard
condition), `-recovery.js` (the error outputs kept off whatever turns them
on), `-constants.js`, one per group (`-windows.js`, `-layout.js`,
`-widgets.js`, `-input-text.js`, `-trees-tabs.js`, `-menus.js`,
`-popups.js`, `-tables.js`, `-style.js`, `-plots.js`, `-drawlist.js`,
`-clipper.js`, `-docking.js`); the application's: `ui-imgui-app.js` and
`ui-imgui-adopt.js` (the harness's driving of an application's own
frame). Pixels are compared exactly at sample points of captured presented
frames, and the binding's tests open their windows without `highDpi`, so
at scale 1. Each test's header lists its cases; the mutations each was
shown to fail under are in the progress files' Mutations tables.

`imgui-load.js`, `imgui-retry.js`, `imgui-symbols.js`,
`imgui-bind-surface.js` and every window test are gated on the lit
feature `ui-imgui`, which `test/lit.cfg` sets when `hermes-node-ui` runs
`test/fixtures/imgui/probe.js` successfully. `imgui-pin.js` requires only
`ui-enabled`, and `imgui-missing.js` and `ui-imgui-feature.js` only `ui`.
`test/ui-imgui-feature.js` is the feature's forcing function: it fails
when `ui` is on and the addon does not load.

## Verified, and not

Part A was run on Linux, GL backend, under Xvfb at framebuffer scale 1,
and on one Apple-silicon Mac, Metal backend, on a 2x display (2026-11-27;
`progress-imgui-addon.md` has the numbers). On the Mac the `highdpi` run of
`test/ui-imgui-resize.js` sees a scale of 2, and a
`DisplayFramebufferScale` hard-coded to 1 fails it; under Xvfb that
mutation passes every test.

The binding and `examples/imgui-app/` have been run only on Linux, GL,
under Xvfb at scale 1 (2026-11-27 and 2026-11-28), in a Debug ASAN build
and a Release build; the Mac round has not been run yet (tracker
01a21f89-a889). The application's
JavaScript time per frame, measured driven with all six panels open
(`test/fixtures/imgui/app-measure.js`, 600 frames after 10, an Intel Xeon
E-2288G, 2026-11-28, re-run the same day after the final review's
fixes): about 12.9 ms in the ASAN build and 1.2 ms in Release, at 250
binding calls a frame; with a directory of 5000 entries open in the Files
panel, 402 calls, about 24 ms and 1.3 to 1.6 ms. The script needs the
test hooks, so these include the per-call counting that
`__test.bindCalls()` reads and that a run without the hooks does not do.
The progress file has both runs of each, before and after, the maxima,
and what the occasional slow ASAN frame looks like.

Not run:

- **The binding on Metal**, and the application at scale 2.
- **Real keys on macOS.** Part A's tests inject Cmd chords and surrogate
  halves below Sokol; nothing was typed.
- **A Release build of the addon on macOS**, an Intel Mac, and Metal at
  scale 1.
- **Nobody has looked at the demo window or the application on the Mac**;
  the tests read pixels. On Linux the application was looked at once in a
  capture.

Some code paths no test reaches, and some properties no test would notice
losing; the progress files ("Mutations", "Behaviours no test detects")
and the tracker (`dz list`) record them.

## Bumping ImGui

1. Re-sync `external/imgui/imgui/` and update the pinned tag, as
   `external/imgui/README.md` describes, and run `test/imgui-pin.js`.
2. Build the addon. API that was renamed or removed fails there.
3. Compare `translateKey()` in `imgui_platform.cpp` with
   `ImGui_ImplGlfw_KeyToImGuiKey` in the new tag's
   `backends/imgui_impl_glfw.cpp`, and the comment naming the tag.
4. Re-read the texture protocol (`ImTextureData`, `ImTextureStatus`) and
   `ImGuiPlatformIO`'s clipboard and reset-callback fields against
   `updateTexture()` and `rendererInit()`; their names and meaning have
   changed between ImGui releases.
5. Re-audit `bind/bind_guards.cpp` against the new source: every guard
   repeats an assertion of ImGui's, with its line, and every internal
   field below must still mean what the guard assumes. Then the binding's
   own source references (the surface table's line numbers; each
   function's preconditions; the recoverable errors, by grepping
   `IM_ASSERT_USER_ERROR`; and the enumerator lists in
   `bind/bind_constants.cpp`, against `imgui.h`'s enums, with
   `test/fixtures/imgui/bind-groups.js`).
6. Run every `test/imgui-*.js` and `test/ui-imgui-*.js`, on both backends.

What the binding takes from `imgui_internal.h`, all of it in
`bind/bind_guards.cpp` (`g` is the `ImGuiContext`; the surface table's
"Internal state read by bind_guards.cpp" has each declaration's line):

- Read by the guards: `g.CurrentWindow` and its `ID`, `Flags`,
  `SkipItems`, `Name`, `IDStack`, `BeginCount`, `DC.MenuBarAppending`,
  `DC.TreeDepth`, `DC.TreeHasStackDataDepthMask`, `DC.CursorPos` (of the
  table's inner window); `g.CurrentWindowStack` (its `Size`, and
  `ImGuiWindowStackData::Window` of the entry below the top, the list
  box's parent in `canEndListBox`); `g.GroupStack` and
  `ImGuiGroupData::WindowID`; `g.TreeNodeStack`; `g.BeginComboDepth`;
  `g.CurrentTabBar->LastTabItemIdx`; `g.LastItemData.ID`; `g.CurrentTable`
  and its `InnerWindow`, `OuterWindow`, `IsLayoutLocked`, `IsInsideRow`,
  `CurrentColumn`, `ColumnsCount`, `DeclColumnsCount`, `ColumnsNames`,
  `Columns[i].Flags`, `TempData->ReconcileColumnsRequests`, `RowPosY2`,
  `InstanceCurrent`; `g.TablesTempData`, `g.TablesTempDataStacked`,
  `g.Tables`; `DockContextFindNodeByID()` and the node's `ParentNode`,
  `LastFrameActive`, `IsDockSpace()`; `g.FrameCount`; `ImHashStr`,
  `ImFormatString`.
- Read by the test hooks: `__test.state().windows` reads `g.Windows`, each
  window's `Name`, `Active`, `WasActive` and `DockIsActive`, and
  `g.WithinFrameScope`; `__test.tableScope()` reads `g.CurrentTable`, its
  `InnerWindow`'s `Name`, and `g.CurrentWindow`'s `Name`.
- Copied from ImGui's private source rather than a header, and so checked
  by eye at a bump: the expression of `TREE_NODE_GET_DEPTH_MASK`
  (`canTreePop`), the combo popup's window name `"##Combo_%02d"`
  (`canEndCombo`), and `DockSpaceOverViewport`'s host window name
  `"WindowOverViewport_%08X"` and label `"DockSpace"`
  (`canDockSpaceOverViewport`, for the default ID).
- **Written**, the whole list: `g.ErrorCallback` and
  `g.ErrorCallbackUserData` (context creation: the error callback), and
  `g.BeginComboDepth`, set to 0 by every `newFrame()` before
  `ImGui::NewFrame()` (nothing in ImGui resets it after a combo left open,
  and zero is always right at the start of a frame). Outside that header,
  `bind/bind_clipper.cpp` writes `ImGuiListClipper::ItemsCount`, declared
  in `imgui.h` but commented "[Internal]", to -1 when it abandons a
  clipper; and the four `io.ConfigErrorRecovery*` fields above are public
  configuration.
