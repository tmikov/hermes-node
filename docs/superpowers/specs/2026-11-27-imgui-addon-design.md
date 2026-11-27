# Dear ImGui addon, part A: ImGui running on `hnui`

Date: 2026-11-27. Branch `work-imgui`. Builds on the UI compositor
(`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`), whose four
slices are complete on GL/X11 and Metal.

## Intent

The compositor exists so that a UI toolkit can run inside `hermes-node-ui`.
Dear ImGui is that toolkit: the compositor's design names an ImGui addon as
"the real target" and defers it, noting that the input, cursor and
clipboard paths cannot be called proven until one exists. This project
builds it.

The addon is a **product component**, not an example. It is one of many
possible `hnui` addons, technically, but it is the one this work is for: it
has its own directory, its own tests and a built-in module name. Only the
programs that use it are examples.

The work is three projects, in order. This document designs the first and
treats the other two as constraints.

- **A (this document):** vendor ImGui, write its backend on `hnui`, load it
  by a built-in name, and show ImGui's own demo window from a JS program.
- **B (later):** a hand-written JS binding for a subset of ImGui, and a JS
  application that uses it (a complex UI written in JavaScript).
- **C (later):** a binding generated from ImGui's API metadata, replacing
  the hand-written subset.

## Goals

1. `require('hermes-node:imgui')` works in `hermes-node-ui` on Linux (GL)
   and macOS (Metal), from a plain script.
2. A short JS program shows `ImGui::ShowDemoWindow()` at frame rate, and
   every part of it works: mouse, wheel, keyboard, text entry, cursor
   shapes, copy and paste, window resize, HiDPI.
3. The addon uses **only the public API**: `hnui.h`, `node_api.h`, and the
   `sg_*` declarations of the vendored `sokol_gfx.h`. It is the public
   API's first serious client; whatever it cannot do is a defect in
   `hnui`, fixed there.
4. It is tested to the standard of the rest of the UI suite: assertions
   from traces and read-backs, exact pixels, both backends.

## Non-goals

- Any ImGui call from JavaScript beyond the three in "JS surface" (B).
- A binding generator (C).
- Multi-viewport (ImGui windows as OS windows): the compositor has one
  window by design.
- IME composition, gamepad input, a font-loading API, a change of DPI
  scale while running.
- Windows.
- Release packaging. `HERMES_NODE_ENABLE_UI` is off in releases today;
  shipping the addon in one is a separate decision.
- `--build-exe` for UI programs, beyond not making it worse.

## Anti-goals

- The addon including a header that is not public, or being linked into
  `hermes-node-ui`. Either removes the proof in goal 3.
- A modified ImGui. `external/imgui/imgui` stays untouched upstream source.
- A test that asserts from timing, or from a trace line that prints what
  was requested and not what was done.

## Approaches

Where the UI code lives decides the size of the work.

- **A. Native only.** ImGui and its backend in the addon; the demo window
  called from native code. Smallest; proves the deferred paths. Nothing is
  written in JavaScript.
- **B. A plus a hand-written JS subset.** Shows what is particular to this
  project. Costs the binding and an application worth looking at.
- **C. A plus a generated binding.** Complete, upgradeable by
  regeneration. The generator is a project before anything is on screen.

**Chosen: all three, in the order A, B, C.** A first because it finds the
gaps in `hnui.h` cheaply; C last because it can replace B's subset without
changing the addon's shape.

How a program gets the addon:

- **1. A real `.node` file loaded by a built-in name (chosen, "for now").**
  Keeps the addon honestly outside the host while programs name no build
  path.
- 2. Linked into `hermes-node-ui` as an embedder module. One file, but the
  public API loses its client.
- 3. A `.node` file loaded by path. No resolver, but second-class.

## Layout

| Path | What |
|---|---|
| `external/imgui/imgui/` | ImGui, unmodified. `external/imgui/README.md` and wrapper `CMakeLists.txt` beside it, as for Sokol. |
| `addons/imgui/` | The addon: backend, module surface, its own `CMakeLists.txt` and README. A new top-level directory, for addons that are products. |
| `examples/imgui-demo/` | The JS program that shows the demo window, and a README saying how to run it. |
| `test/imgui-*.js` | Its lit tests, with the other UI tests. |

Everything is built only when `HERMES_NODE_ENABLE_UI` is on.

## Vendoring ImGui

- **The docking branch, at its newest release tag** when the work starts
  (a tag of the form `v<version>-docking`). Docking is on
  (`ImGuiConfigFlags_DockingEnable`), because a complex UI is the goal and
  dockable panels are most of what makes ImGui UIs look like applications.
  Multi-viewport stays off. *(My call.)*
- The pin is the **tag**, recorded in `external/imgui/README.md`, which
  also lists the vendored files and why each is there. A test checks the
  README's version against `IMGUI_VERSION` in the header, as
  `test/sokol-pin.js` does for Sokol.
- Vendored: the core sources and headers at the repository root
  (`imgui*.cpp`, `imgui*.h`, `imstb_*.h`, `imconfig.h`), `imgui_demo.cpp`
  included, and `LICENSE.txt`. Not vendored: `backends/`, `examples/`,
  `misc/`, `docs/`.
- `THIRD_PARTY_LICENSES.md` gains ImGui (MIT).
- ImGui is compiled **into the addon**, with hidden visibility, so its
  symbols are the addon's own and a second addon carrying another ImGui
  cannot collide with it. Configuration goes through a config header of
  ours named by `IMGUI_USER_CONFIG`, never by editing `imconfig.h`.

## Loading by name

`hermes-node-ui` registers a second embedder module, `hermes-node:imgui`,
beside `hermes-node:ui`. Its init function does not contain ImGui: it
loads the addon file and returns that file's exports.

- The file is `hermes-node-imgui.node`, looked for **in the directory of
  the running executable**, realpath'd (the rule the kit and bundle roots
  already use). CMake builds it into the same directory as the binary.
- Loading goes through the same code as `process.dlopen`, so
  `napi_register_module_v1` is the entry and a file that exists and will
  not load (a missing dependency, an unresolved symbol) is
  `ERR_DLOPEN_FAILED`, as there. `process.dlopen`'s own contract does not
  change. Today
  that code is a static N-API callback (`processDlopen`,
  `lib/process/node_process.cpp`); its body is extracted into a C++
  function both callers use. That refactor is a commit of its own.
- An absent file is the one case classified **before** the shared loader
  is called: the resolver checks for the file itself and throws with
  `code = 'MODULE_NOT_FOUND'` and a message naming the full path tried.
- It loads at the first `require()` **or `require.resolve()`** of the
  name (the embedder route initializes a module for both), and a failed
  load is not cached, so a later attempt tries again. A program that never asks for ImGui
  pays nothing and `hermes-node-ui` has no link dependency on the addon.
- The mechanism is a table of name and file name with one row, not a
  search path and not a naming convention any file can satisfy. *(My
  call: a second product addon adds a row; a third-party addon is loaded
  by path, as today.)*
- Plain `hermes-node` does not register it: `require('hermes-node:imgui')`
  there fails as `hermes-node:ui` does.
- Under `--bundle`, embedder modules are already served from outside the
  container, so this needs no bundle change; the file must sit beside the
  executable.

The addon calls `HNUI_CHECK_VERSION(env)` at init, as every `hnui` addon
must.

## JS surface

Three functions and a string, and nothing else in A:

```js
const ui = require('hermes-node:ui');
const imgui = require('hermes-node:imgui');

const win = await ui.createWindow(
    {title: 'ImGui', width: 1280, height: 800, highDpi: true});
function frame() {
  imgui.newFrame();
  imgui.showDemoWindow();
  imgui.render();
  win.requestAnimationFrame(frame);
}
win.requestAnimationFrame(frame);
```

- `imgui.newFrame()` feeds ImGui the display size (from
  `hnui_get_window_state`) and the time step, and calls
  `ImGui::NewFrame()`. It does **not** take the frame's target. It returns
  `false`, having done nothing, only when this runtime has no window.
- `imgui.showDemoWindow()` calls `ImGui::ShowDemoWindow()`.
- `imgui.render()` calls `ImGui::Render()`, then asks `hnui_frame_target`
  for the target and draws into it. It returns whether it drew: `false`
  when there is no target (outside a frame callback, or in CLOSING), in
  which case the ImGui frame is ended and nothing is drawn.
- **`render()`, not `newFrame()`, takes the target, because taking it is
  what tells the host the tick drew.** A pair interrupted by a throw
  between the two calls therefore takes no target and draws nothing. If
  nothing else drew in that tick, nothing is presented for it; the other
  order would present a surface nothing drew into.
- `newFrame()` while a frame is already open ends the open one
  (`ImGui::EndFrame()`), traces `imgui: frame discarded`, and starts the
  new one. It does not throw: the addon cannot tell a second call in one
  callback from a call in the next callback after a throw, and `hnui`
  gives it no tick identity to tell with. *(My call, over adding one to
  `hnui`.)*
- **More than one `newFrame()`/`render()` pair in a tick is legal**, in
  one callback or in several: the host runs every callback of a tick
  before it submits once. Each `render()` clears the target, so the last
  one's picture is the one presented. The renderer is built for it (see
  "Renderer half": appended buffers, replaced textures).
- `showDemoWindow()` or `render()` with no frame open throws a JS error
  naming the call, everywhere, an `'exit'` listener included. A correctly
  paired `newFrame()`/`render()` never throws in CLOSING: `render()`
  returns false there.
- `imgui.version` is ImGui's version string.

The JS program owns the frame, and the addon registers no frame callback
of its own. That is the shape B needs: B adds functions to call between
`newFrame()` and `render()` and changes nothing else. *(My call, over a
native frame callback that would have to be reordered around JS ones.)*

The ImGui context is created at the first successful `newFrame()`, for
the window `hnui_get_window(env)` returns. One context per process.

## The backend

One C++ file per job in `addons/imgui/`; the split is the builder's, but
platform, renderer and module surface are separate units.

**Platform half** (what ImGui's GLFW or SDL backend does):

- A native event listener (`hnui_add_event_listener`), registered when the
  context is created, translates each `hnui_event` to ImGui's `io.Add*Event`
  calls: mouse position, buttons, wheel, keys with modifiers, text input,
  focus and blur, mouse leave. Native listeners run before JS ones, so
  ImGui has the event before the program's own listeners see it.
- Mouse buttons are renumbered: `hnui` uses DOM numbering (middle 1,
  right 2) and ImGui the other (right 1, middle 2).
- A `KEY_DOWN` with `repeat` set is not passed on as a new press: ImGui
  generates key repeat itself from the key being held. A repeated
  `TEXT_INPUT` is text and is passed on.
- `key_code` is GLFW's numbering; the key table is ImGui's own GLFW
  backend's table, re-typed against `hnui`.
- **Everything is in logical units.** Display size is the window's
  logical size and `DisplayFramebufferScale` is framebuffer size over it,
  both from `hnui_get_window_state`, read every frame, so a resize needs
  no event handling. `hnui` already reports mouse positions in logical
  units and the wheel in Sokol's scroll units, so both go to ImGui
  unconverted.
- Delta time comes from a monotonic clock read in `newFrame()`.
- Cursor: after `Render()`, ImGui's requested cursor is mapped to
  `hnui_set_cursor` / `hnui_show_cursor`, only when it changed.
- Clipboard: set is `hnui_write_clipboard`; get is
  `hnui_read_clipboard_sync` with a 500 ms deadline, its result released
  with `hnui_free`. A timeout or error is an empty paste and one trace
  line. `HNUI_EVENT_PASTE` is ignored: ImGui pastes on its own key
  chord, and handling both would paste twice.
- macOS behaviours (`io.ConfigMacOSXBehaviors`) follow the build platform,
  which is ImGui's default.
- `io.IniFilename` is NULL: a UI toolkit must not drop `imgui.ini` into
  the current directory of whatever program loaded it. B can expose the
  setting. *(My call.)*
- HiDPI follows from the logical-unit model and nothing else: with the
  texture protocol below, ImGui rasterizes fonts at the framebuffer scale
  itself. The style is not scaled and the font size is not multiplied;
  doing either as well would double the UI at 2x. `createWindow` defaults
  `highDpi` to false, so the demo and one test pass `highDpi: true`.
  Xvfb is always scale 1: the 2x case is seen only on the Mac.

**Renderer half**, written against `sokol_gfx` directly. Sokol's
`sokol_imgui.h` is not used: it takes its input from `sokol_app` events,
which an addon never sees, and its shaders come from a tool this
repository does not have.

- One pipeline, alpha blending, no depth, scissor on; vertex layout is
  `ImDrawVert`. Its colour format and sample count come from the target,
  so it is rebuilt if a later target differs.
- One shader with hand-written sources for the two backends: GLSL at the
  version the producing GL context provides (the builder takes it from
  what `demo_visual.c`'s `sokol_gl` already compiles to), and Metal
  Shading Language. Which one is used is decided at run time from
  `sg_query_backend()`.
- **Vertex and index data go through `sg_append_buffer`, never
  `sg_update_buffer`.** Sokol allows one update of a buffer per Sokol
  frame and any number of appends, and a Sokol frame here is a host
  commit, not an ImGui frame: two `render()` calls in one tick share
  one. Each `render()` appends all its draw lists and draws with the
  returned offsets. When an append would not fit, the buffer is replaced
  by a larger one (which has had no append in this frame) and the old
  one destroyed; draws already issued from the old one are unaffected.
- **Textures follow ImGui's texture protocol**
  (`ImGuiBackendFlags_RendererHasTextures`), which is how the font atlas
  arrives and grows. Sokol's `sg_update_image` takes a whole image, once
  per Sokol frame, so the adapter does not use it: for a texture ImGui
  wants created **or updated**, the renderer creates a new immutable
  `sg_image` from the texture's full pixel data, with a texture view,
  gives ImGui the new id, and destroys the previous image and view. One
  path, legal any number of times per Sokol frame; its cost is a whole
  atlas upload per change, and the atlas changes only when new glyphs or
  sizes appear. A texture ImGui wants destroyed is destroyed. ImGui's
  status for a texture is set only after the Sokol objects were made and
  are valid. An ImGui texture id is the Sokol view's id; one linear
  sampler, the addon's, serves every texture. The exact status fields
  are read from the vendored tag.
- `ImGuiBackendFlags_RendererHasVtxOffset` is set, with 16-bit indices:
  a draw command's vertex offset becomes the draw's base vertex, or the
  vertex buffer binding's offset, whichever this Sokol supports on both
  backends.
- `render()` owns one pass on the target: it clears to a fixed dark
  colour and draws. Targets are single-sampled on both backends today;
  the pipeline takes the sample count from the target regardless. Sharing the
  pass with another renderer is not needed until a program wants a 3D
  view under its UI, which is B's or later.
- Everything frame-scoped is produced and consumed inside the one frame
  callback, which is what `hnui_frame_target`'s contract requires.
- The addon's GPU objects belong to the producing instance and survive a
  resize: a new surface generation changes the target, not them.
- No teardown: `hnui` gives no notification and the process ends in
  `_exit()`. The addon registers no cleanup hook and frees nothing on the
  way out.

**Tracing.** Under `HERMES_NODE_DEBUG_NATIVE=UI` the addon prints lines
prefixed `imgui:`, each saying what was done or read back: context
created (version, flags, font scale), each texture created, updated or
destroyed with its size, pipeline created with format and sample count,
buffers grown, cursor set, clipboard get and set outcomes by length (never
contents), a discarded frame. Per-frame lines are not printed.

## Test hooks

With `HERMES_NODE_UI_TEST_HOOKS` set, the module also exports `__test`,
absent otherwise:

- `__test.scene(name)`: called between `newFrame()` and `render()`, draws
  a fixed native scene in place of the demo window. The scenes are what
  the tests below need: a full-window flat fill through the background
  draw list; a window at a fixed position holding one button and one text
  field; a textured quad from a small generated texture; `many`, more
  than 65,535 vertices of flat quads in one draw list tiling the window
  in known colours, which forces buffer growth and non-zero vertex
  offsets; `clip`, a fill under a clip rectangle smaller than itself; and
  `glyphs`, text at one size, then, from a later frame on, also text at a
  second size and with characters not used before, which forces an atlas
  update after the first upload.
- `__test.state()`: returns what ImGui holds, read back: display size and
  framebuffer scale, mouse position, `WantCaptureMouse`,
  `WantCaptureKeyboard`, `WantTextInput`, the scene's button click count,
  the text field's contents, the last cursor ImGui requested, and the
  last frame's draw-list totals (commands, vertices, indices, live
  textures).

These exist so that the tests can assert on ImGui's state before B gives
JavaScript a way to build UI. B's binding replaces most of their use.

## Tests

All gated like the other window tests (`ui`, `display`), run under
`%with-display`, their stderr read with the Sokol exclusions. One new lit
feature, `ui-imgui`, is decided by running: `require('hermes-node:imgui')`
succeeds in the built binary. It has a forcing-function test, red when
`ui` is on and the addon is not loadable.

| Test | Proves |
|---|---|
| load | The name resolves; `version` matches the pin; the context trace line; plain `hermes-node` refuses the name. `require.resolve()` of the name loads it too, and a following `require()` returns the same object. |
| missing file | A copy of the binary in an empty directory throws `MODULE_NOT_FOUND` naming the path, from `require()` and from `require.resolve()`, twice in one process. A file there that is not a loadable addon throws `ERR_DLOPEN_FAILED`. Needs no display. |
| pixels | The flat-fill scene and the textured quad, captured from the presented frame, compared exactly at sample points. Per backend. |
| many, clip | `many`: exact colours at sample points in the first, a middle and the last quad, and the buffer-growth trace. `clip`: the fill's colour inside the clip rectangle, the clear colour outside it. |
| glyphs | The texture trace shows a replacement after the first upload; captured pixels inside the old text's and the new text's boxes are not all the background colour, before and after. |
| two renders | Two pairs in one callback, and two callbacks in one tick, each pair a different flat fill: the presented frame is the last one's, with no Sokol line (validation is on in Debug). |
| mouse | Injected move, down and up over the scene's button: click count goes 0 to 1; `WantCaptureMouse` true over the window and false off it. Right and middle buttons read back in ImGui's numbering. A repeated key-down adds no press; a repeated text event adds a character. |
| keyboard and text | Click the text field, inject `TEXT_INPUT` and key events: contents, `WantTextInput`, and an edit with arrow keys and backspace. One key through the real-event driver (`window_driver.js`), since injection enters below Sokol. |
| clipboard | Select-all and copy from the field, move the caret to the end, paste: contents doubled; the host's clipboard read-back trace. On macOS it carries the `hnui-test-` pasteboard guard the other clipboard-writing tests have. |
| cursor | Hovering the text field: ImGui requests the I-beam, and the host's own trace shows the cursor it set. |
| resize | A real resize: display size follows, the generation changes, live textures unchanged, a frame still presents. |
| demo window | `showDemoWindow()` for a fixed number of frames, one captured: pixels inside the demo window's rectangle differ from the clear colour and pixels outside it equal it, so a renderer that submits nothing fails. No Sokol line; in a Debug build this runs under Sokol's validation layer. |
| misuse | `render()` and `showDemoWindow()` with no frame open: the errors. `newFrame()` twice: the discard trace, no throw. `render()` outside a frame callback returns false. A throw between `newFrame()` and `render()`, caught by an `uncaughtException` listener, with the next frame's callback registered before the throw and nothing else drawing: the host's trace shows that tick drew nothing, and the next frame draws after a discard trace. |
| exit | `process.exit()` from inside a frame, and an `'exit'` listener calling a paired `newFrame()` and `render()`: no throw, `render()` false, status preserved. |

The Mac runs go through the gist channel, as for Tasks 16 and 17, and the
plan has verification rounds there: the Metal shader, the Metal pixel
test, the pasteboard guard and HiDPI at 2x are not done until the Mac has
run them. The formatter runs on Linux only.

## `hnui` gaps

The addon is expected to find some. Each is fixed in `hnui`, in a commit of
its own with its own test, and recorded in the tracker if not fixed at
once. Adding an export, or appending a field under the size-prefix rules
of `hnui_abi.h`, needs no ABI bump; an incompatible change of layout or
of an existing function's meaning does. None is known yet. One was considered and declined: a tick
identity on `hnui_target` (see "JS surface").

## Documentation

- `addons/imgui/README.md`: what it is, how it is loaded, the JS surface,
  what is not supported.
- `CLAUDE.md`: a short section under the UI compositor, and `addons/` in
  Conventions.
- A progress file beside the plan.

## Left to the builder

- The file split inside `addons/imgui/`.
- The exact ImGui tag, taken when the work starts.
- GLSL version and shader text.
- Initial buffer sizes and growth policy.
- Test file names and how the cases are grouped into files.
- Whether `examples/imgui-demo/` carries a `package.json` (it has no
  dependencies).
