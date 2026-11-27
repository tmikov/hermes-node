# hermes-node-imgui

The Dear ImGui addon for `hermes-node-ui`. It builds
`hermes-node-imgui.node`, which a program loads as
`require('hermes-node:imgui')`. ImGui itself is vendored, unmodified, in
`external/imgui/imgui/` at tag `v1.93.0-docking`; this directory holds
ImGui's backend on `hnui` and `sokol_gfx`, the module surface, and the test
hooks.

Design: `docs/superpowers/specs/2026-11-27-imgui-addon-design.md`. Plan
and the record of what was measured:
`docs/superpowers/plans/2026-11-27-imgui-addon-part-a.md` and
`docs/superpowers/plans/progress-imgui-addon.md`.

## What it is and is not

It is a product component, not an example: the toolkit the UI compositor
exists to host. It is also the first non-trivial client of the public
addon API, and it uses nothing else. Its sources include only `hnui.h` /
`hnui_abi.h`, `node_api.h`, the declarations of the vendored `sokol_gfx.h`
(never `SOKOL_GFX_IMPL`; every `sg_*` symbol resolves against the host when
the addon is loaded) and ImGui. Anything it cannot do through that API is a
gap in `hnui`, to be fixed there.

Not supported:

- ImGui calls from JavaScript beyond `newFrame()`, `showDemoWindow()` and
  `render()`. A JS binding for ImGui is later work.
- Multi-viewport (ImGui windows as OS windows): the compositor has one
  window.
- IME composition, gamepad input, loading fonts, a change of DPI scale
  while running.
- Windows.
- Published releases. The release workflows build without
  `HERMES_NODE_ENABLE_UI`, so no release tarball carries the addon or
  `hermes-node-ui`; how the addon would be packaged is undecided. (This is
  about what is shipped, not the build type: a local Release build with
  the option on builds both.)
- `imgui.ini`: `io.IniFilename` and `io.LogFilename` are NULL, so the
  addon never writes a file into the current directory.

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

## JS surface

```js
const ui = require('hermes-node:ui');
const imgui = require('hermes-node:imgui');

async function main() {
  await ui.createWindow(
      {title: 'ImGui', width: 1280, height: 800, highDpi: true});
  function frame() {
    imgui.newFrame();
    imgui.showDemoWindow();
    imgui.render();
    ui.requestAnimationFrame(frame);
  }
  ui.requestAnimationFrame(frame);
}
main();
```

That is `examples/imgui-demo/demo.js` without its error handling.
`requestAnimationFrame` is `hermes-node:ui`'s (also installed as a global);
the window object has none.

- `imgui.newFrame()` reads the window's state with
  `hnui_get_window_state`, sets ImGui's display size, framebuffer scale and
  time step, and calls `ImGui::NewFrame()`. It does **not** take the
  frame's target. It returns `false`, having done nothing, only when this
  runtime has no window; otherwise `true`. The ImGui context is created at
  the first `newFrame()` that finds a window, for the window
  `hnui_get_window(env)` returns. There is one context per process.
- `imgui.showDemoWindow()` calls `ImGui::ShowDemoWindow()`.
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
  the first call after a throw left a frame open.
- More than one `newFrame()`/`render()` pair in a tick is legal, in one
  callback or in several. Each `render()` clears the target, so the last
  one's picture is presented.
- `render()` or `showDemoWindow()` with no frame open throws an error with
  code `ERR_IMGUI_NO_FRAME` naming the call, everywhere, an `'exit'`
  listener included. A correctly paired `newFrame()`/`render()` does not
  throw in CLOSING; `render()` returns `false` there.
- `imgui.version` is ImGui's `IMGUI_VERSION` string.

The program owns the frame: the addon registers no frame callback of its
own. Every call runs on the JS thread that owns the window.

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
- Input that arrives before the first `newFrame()` is not seen by ImGui:
  the context, and with it the listener, do not exist yet, and nothing
  replays earlier events. In practice ImGui starts with the mouse position
  unknown, so nothing shows as hovered until the next mouse event, even
  with the pointer already over the window; a key or button held across
  that first frame is not known to be down.
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
| `context created (imgui <version>, docking=<0\|1>)` | The first `newFrame()` that found a window. |
| `event listener registered (id <n>)`, `event listener FAILED (id 0)` | The context's event listener. |
| `event rejected: size <n>` | The first event too small for this `hnui` ABI; printed once. |
| `frame discarded` | `newFrame()` found a frame open. |
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
`wheel` scene; and `cursor`, the ImGui cursor shape last set (-1 before
any). ImGui spreads queued input over frames, so a test runs frames until
the state it expects appears, bounded by a frame count.

## Tests

`test/imgui-*.js` need no window: `imgui-pin.js` (the README's tag against
`IMGUI_VERSION`, and the docking branch), `imgui-load.js`,
`imgui-missing.js` (a hard-linked copy of the binary in an empty
directory, where one process meets the addon absent and then unloadable),
`imgui-retry.js` (the same script, then the real addon put in place and
loaded by the same process), `imgui-symbols.js`. The window tests are
`test/ui-imgui-*.js`, so that `utils/check-ui-stderr-rule.py` covers them,
and share `test/fixtures/imgui/harness.js`: `ui-imgui-frame.js`,
`-draw.js`, `-textures.js`, `-input.js`, `-clipboard.js`, `-cursor.js`,
`-resize.js` and `-demo.js`. Pixels are compared exactly at sample points of captured
presented frames. Each test's header lists its cases and the mutations it
was shown to fail under.

`imgui-load.js`, `imgui-retry.js`, `imgui-symbols.js` and every window
test are gated on
the lit feature `ui-imgui`, which `test/lit.cfg` sets when
`hermes-node-ui` runs `test/fixtures/imgui/probe.js` successfully.
`imgui-pin.js` requires only `ui-enabled`, and `imgui-missing.js` and
`ui-imgui-feature.js` only `ui`. `test/ui-imgui-feature.js` is the
feature's forcing function: it fails when `ui` is on and the addon does
not load.

## Verified, and not

Everything above was run on Linux, GL backend, under Xvfb at framebuffer
scale 1. Not run yet:

- **macOS and Metal.** The MSL shader has never been compiled, the
  `exports.darwin.txt` link has never been made, and no test has run
  there.
- **A 2x framebuffer scale.** Under Xvfb the scale is 1, so the `highdpi`
  run of `test/ui-imgui-resize.js` repeats the scale-1 geometry, and a
  `DisplayFramebufferScale` hard-coded to 1 passes every test.

Some code paths no test reaches; the progress file's Mutations table and
the tracker (`dz list`) record them.

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
5. Run every `test/imgui-*.js` and `test/ui-imgui-*.js`, on both backends.
