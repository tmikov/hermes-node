# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-11-27-imgui-addon-part-a.md`
(implementation plan) and its companion design doc
`docs/superpowers/specs/2026-11-27-imgui-addon-design.md`.

## Status

| Task | Description | Status |
|------|-------------|--------|
| Task 1 | Vendor Dear ImGui | done |
| Task 2 | Extract loadNativeAddon() from process.dlopen | done |
| Task 3 | Load a named addon; the addon skeleton | done |
| Task 4 | The frame protocol, and a cleared target | done |
| Task 5 | The renderer's draw path | done (GL; Metal shader unverified) |
| Task 6 | Texture scenes and tests | done (GL) |
| Task 7 | Input | done (GL) |
| Task 8 | Cursor and clipboard | done (GL; Metal unverified) |

## Mutations

Each test must be shown able to fail: the change made, and what it did.

| Task | Test | Mutation | Result |
|------|------|----------|--------|
| 1 | `test/imgui-pin.js` | README tag changed to `v1.92.0-docking` | `README WRONG README=v1.92.0-docking HEADER=v1.93.0-docking`, test failed |
| 2 | the 14 dlopen/addon tests (`test-native-addon.js`, `bundle-natives.js`, ...) | `loadNativeAddon` looks up `napi_register_module_v1_x` instead | 6 of 14 failed (`test-native-addon`, `test-native-addon-pkg`, `test-cli-eval-require`, `bundle-natives`, `bundle-natives-errors`, `build-exe-natives`); restored |
| 3 | `test/imgui-missing.js` | `stat()` check in `named_addons.cpp` replaced by `if (false)` | the missing file reached dlopen: `MISSING ERR_DLOPEN_FAILED path=true`, test failed; restored |
| 3 | `test/ui-imgui-feature.js`, `test/imgui-load.js` | `registerNamedAddons()` call in `main.cpp` disabled | `ui-imgui-feature.js` FAIL (`agree binary=off lit=off` fails ON), `imgui-load.js` UNSUPPORTED; restored |
| 3 | `test/imgui-symbols.js` | the three visibility properties removed from `addons/imgui/CMakeLists.txt` (before the export list existed; with it, this alone no longer fails) | `EXPORTS bad: 2562 unexpected (2506 ImGui)`, test failed; restored |
| 3 | `test/imgui-load.js` | init trace line emitted twice | ERR run failed on its `-NOT` line; restored |
| 3 | `test/imgui-symbols.js` | with a temporary `std::vector<std::string>` in the init: export list kept | PASS, 2 exports |
| 3 | `test/imgui-symbols.js` | same `std::vector` probe, `--version-script` option dropped | `EXPORTS bad: 16 unexpected`, test failed; restored |
| 3 | `test/imgui-symbols.js` | export list and the three visibility properties both dropped | `EXPORTS bad: 2562 unexpected (2506 ImGui)`, test failed; restored |
| 4 | `test/ui-present.js` | give-up branch of `maybeCapture()` sets `publicationId` to 0 again | `REBLIT matched=false framesObserved=240 presenting=WRONG got=0 want=1`, test failed; restored |
| 4 | `test/ui-imgui-frame.js` | (a) target taken in `newFrame()` instead of `render()` | the throwing tick traced `drew`; TRACE run failed on `nothing drawn`; restored |
| 4 | `test/ui-imgui-frame.js` | (b) clear colour set to black | `CLEARED WRONG 8,8=0,0,0 ...` at all five points, test failed; restored |
| 4 | `test/ui-imgui-frame.js` | (c) `ImGui::EndFrame()` removed from the second `newFrame()` | ImGui asserted (`Forgot to call Render() or EndFrame()`), `EXITCODE 134`, test failed; restored |
| 4 | `test/ui-imgui-frame.js` | (d) `render()` throws when `hnui_frame_target` refuses | the OUTSIDE `render()` threw and the script stopped, test failed; with OUTSIDE temporarily wrapped in a `try`, also `EXIT threw imgui.render(): ...`; restored |
| 4 | `test/ui-imgui-frame.js` | (unplanned, found while writing it) `harness.tick` resolving from inside the last callback | OUTSIDE's `render()` ran at the host's post-callback checkpoint, still inside the tick, and drew: `OUTSIDE render=true`, test failed; the harness now resolves from a `setImmediate` |
| 5 | `test/ui-imgui-draw.js` | (RED) the Task 4 renderer, scenes and test new | `FILL WRONG` (all clear), `CLIP WRONG`, then ImGui asserted on `many` (no `RendererHasVtxOffset`), `EXITCODE 134` |
| 5 | `test/ui-imgui-draw.js` | (a) `sg_update_buffer` in place of both `sg_append_buffer` calls | `[sokol-produce:error] VALIDATE_UPDATEBUF_ONCE` at the first two-pair case, then the validation panic; test failed; restored |
| 5 | `test/ui-imgui-draw.js` | (b) 0 as the base vertex | `MANY WRONG 638,478=26,26,31` and the same in `MANYTWICE` (the commands past 65,536 vertices drew the first quads again); restored |
| 5 | `test/ui-imgui-draw.js` | (c) `sg_apply_scissor_rectf` skipped | `CLIP WRONG 50,50=0,255,0 ...` at all four outside points; restored |
| 5 | `test/ui-imgui-draw.js` | (d) the projection's y flipped (scissor untouched) | CLIP still passed (a full-window fill covers the window either way up; the scissor alone shapes it); `MANY WRONG 2,2=0,0,255 ...` and `MANYTWICE WRONG` caught it; restored |
| 5 | `test/ui-imgui-draw.js` | (d') the scissor's `origin_top_left` false | `CLIP WRONG 200,150=26,26,31 101,101=26,26,31` (the rectangle lands at y 280..380); restored |
| 5 | `test/ui-imgui-draw.js` | (e) both buffers start at 1,000,000 elements | no growth trace; GROWN run failed; restored |
| 5 | `test/ui-imgui-draw.js` | (f) the reset callback re-applies only the pipeline | Sokol validation caught it first (`VALIDATE_DRAW_REQUIRED_BINDINGS_OR_UNIFORMS_MISSING`: uniforms not re-applied); test failed; restored |
| 5 | `test/ui-imgui-draw.js` | (f') the reset callback re-applies pipeline and uniforms, not the viewport | `CALLBACK WRONG 600,440=26,26,31 320,240=26,26,31`; restored |
| 5 | `test/ui-imgui-draw.js` | the pipeline rebuilt on every draw | PIPELINE run failed on its `-NOT` line; restored |
| 5 | `test/ui-imgui-draw.js` | the atlas's `created` trace printed twice | TEXTURE run failed on its `-NOT` line; restored (a first attempt printed a differently-shaped duplicate *before* the real line, which the positive line skipped and the `-NOT` could not see: a mutation must reproduce the real line) |
| 5 | `test/ui-imgui-draw.js` | (g, review) the user-callback call in `drawList` deleted | before USERCALLBACK existed, CALLBACK stayed green (the reset re-applies the viewport anyway); with it: `USERCALLBACK WRONG 600,440=0,255,0`, test failed; restored |
| 6 | `test/ui-imgui-textures.js` | (RED) scenes `quad` and `glyphs` not yet written | `Error: imgui.__test.scene(): unknown scene 'quad'`, `EXITCODE 1` |
| 6 | `test/ui-imgui-textures.js` | (a) `WantUpdates` ignored (only `WantCreate` uploads) | `GLYPHS1 WRONG abc=19 xyz=0` and no texture line after the marker; ImGui did **not** assert (the brief expected it to); restored |
| 6 | `test/ui-imgui-textures.js` | (b) every command bound to the white fallback view | `QUAD WRONG 105,105=255,255,255 want 255,0,0 295,105=... want 0,255,0 105,295=... want 0,0,255` (the white corner passes, as it must); GLYPHS0/1 stayed green, as predicted. This closes Task 5's review gap: no test failed under this mutation before; restored |
| 6 | `test/ui-imgui-textures.js` | (c) the previous image and view not destroyed on replace | no test fails (`ui-imgui-textures.js`, `ui-imgui-draw.js`, `ui-imgui-frame.js` all PASS), as expected; checked by reading instead: `updateTexture` destroys `record->view` then `record->image` only after the new pair is valid, and the leak would exhaust Sokol's pools only after thousands of replacements; restored |
| 6 | `test/ui-imgui-textures.js` | TEXTURE run against a doctored `%t.err`: the `replaced` lines after `MARK glyphs1` deleted; separately, every `replaced` rewritten as `created` | both fail FileCheck (no line after the marker; a second `created` before it) |
| 7 | `test/ui-imgui-input.js` | (RED, and (c)) no listener registered: `platformInit` as before this task | `TIMEOUT HOVER move`, `EXITCODE 1`. REAL cannot be reached from there, so it was also run alone (a temporary copy of the test with every case but REAL removed): `TIMEOUT REAL` under the mutation, `REAL apress=1` without it; restored |
| 7 | `test/ui-imgui-input.js` | (a) buttons passed through unrenumbered | `RIGHT mouseDown=false,false,true`, `MIDDLE mouseDown=false,true,false`, test failed; restored |
| 7 | `test/ui-imgui-input.js` | (b) repeated key-downs passed on | test PASSES: `AddKeyEvent` filters a down for a key whose latest state is already down ("Filter duplicate" in `AddKeyAnalogEvent`), so the drop is redundant and this test cannot tell; the drop is kept, as the spec states it; restored |
| 7 | `test/ui-imgui-input.js` | (d) `IMGUI_USE_WCHAR32` dropped from `imgui_config.h` | `ASTRAL WRONG ["h","i","\ufffd"]` (ImGui replaced the code point), test failed; restored |
| 7 | `test/ui-imgui-input.js` | (e) modifier events skipped | test PASSES, as expected: no case here uses a modifier (Task 8's chords do); restored |
| 7 | `test/ui-imgui-input.js` | (f, review of Task 7, run in Task 8) the listener's `key != ImGuiKey_None` guard removed | test PASSES (`UNKNOWN ok apress=1`): `AddKeyAnalogEvent` returns at once for `ImGuiKey_None` and filters duplicate downs, so UNKNOWN proves only "no crash, and A still presses once"; undetectable here, recorded in the test's header; restored |
| 7 | `test/ui-imgui-input.js` | (g, review of Task 7, run in Task 8) `AddFocusEvent(true)` for `HNUI_EVENT_FOCUS` removed | test PASSES: a regained focus only clears `AppFocusLost`, which no case reads; undetectable here, recorded in the test's header; restored |
| 8 | `test/ui-imgui-clipboard.js` | (RED) the two `platform_io` clipboard functions not yet set | stdout PASSED all four cases: ImGui's own default clipboard (an in-process buffer) serves copy and paste when no backend sets the functions, so only the TRACE run failed (no `clipboard read: no owner`, no `imgui: clipboard` lines) |
| 8 | `test/ui-imgui-cursor.js` | (RED) `platformAfterRender` doing nothing | `TIMEOUT CURSOR 1`, `EXITCODE 1` |
| 8 | `test/ui-imgui-clipboard.js` | (a) get returns `""` on failure, untraced | stdout unchanged; TRACE failed on `imgui: clipboard get: status 4`; restored |
| 8 | `test/ui-imgui-clipboard.js` | (b) set does not call `hnui_write_clipboard` | `PASTE text=hello`, `CUTPASTE text=` (nothing owns the selection, every read is status 4), test failed; restored |
| 8 | `test/ui-imgui-clipboard.js` | (c) `addModifiers` skipped for key events | `PASTE text=hello`, `CUT text=hello` (no chord is a shortcut), test failed; restored |
| 8 | `test/ui-imgui-cursor.js` | (d) TextInput mapped to `ARROW` | TRACE failed on `imgui: cursor 1 -> hnui 2`; the trace held `cursor 1 -> hnui 1` and no `command cursor 2` line at all, so the second line fails too; restored |
| 8 | `test/ui-imgui-clipboard.js` | (e) `HNUI_EVENT_PASTE` also inserted into the field | test PASSES, `PASTE text=hellohello` (twice, not three times): the chords are injected below Sokol, which produces its paste event only from a real key, so no `HNUI_EVENT_PASTE` arrives; this test cannot see that double insertion; restored |
| 8 | `test/ui-imgui-cursor.js` | (extra) one cache: visibility acted on only when the shape also changed | TRACE failed on `imgui: cursor hidden` (the pointer does not move in SOFTCURSOR); restored |
| 8 | `test/ui-imgui-cursor.js` | (review) the shape cache updated while hidden | `HIDDENMOVE cursor=1`, test failed; TRACE alone also fails, on SHOWN's `imgui: cursor 1 -> hnui 2` (nothing set after the cursor is shown again); restored |

## Notes

### Task 1

- Vendored from a fresh `--depth 1` clone of tag `v1.93.0-docking`; the
  twelve files verified byte-identical with `diff`.

### Task 3

- `HNUI_CHECK_VERSION(env)` is the first statement of the addon's
  `NAPI_MODULE_INIT()` (checked by reading, Step 7(c)). No test reaches a
  mismatched host with this addon: the addon and the host are built from
  the same `hnui.h` in one tree, and the mismatched-addon case is
  `test/ui-version-check.js`'s, against `hnui_bad_version_addon`.
- libstdc++ declares namespace `std` with default visibility, so a
  `std::` template instantiated in the addon (a `std::vector<char>` in the
  first draft of `trace`) is exported as a weak symbol despite
  `-fvisibility=hidden`; `test/imgui-symbols.js` caught ten of them. Ruled:
  an explicit export list on top of the visibility properties
  (`addons/imgui/exports.map` as an ELF version script,
  `exports.darwin.txt` as ld64's `-exported_symbols_list`, both a
  `LINK_DEPENDS`), so the addon's code may use std containers freely. The
  macOS half has not been linked anywhere yet.
- `test/imgui-missing.js` hard-links the `hermes-node-ui` binary into
  `%t.dir` (copying it, about 190 MB under ASAN, only when the link fails);
  `/proc/self/exe` reports the link's own path, so the executable's
  directory is `%t.dir`. A symlink would not do: it is realpath'd back to
  `bin/`.
- The UI-off configuration (`cmake-build-asan-noui`) still configures and
  builds `hermes-node`.

### Task 4

- The tick-machine barrier `resync(h)` relies on was confirmed by reading
  `lib/compositor/tick_machine.cpp`: `shouldSendTickLocked` refuses unless
  the state is `kIdle`, and a drawing tick leaves `kIdle`
  (`endCallbacksDrew(true)` -> `kGpuPending` -> `kPublishedAwaitingTake`)
  until `takeFrame` (which also sets `presentingPublicationId_`) or a drop
  returns it. So once a callback-only tick has run, the last publication
  has been taken or dropped. Matches the independent review.
- A promise resolved from inside a frame callback continues at the host's
  checkpoint after that callback, still inside the tick, where
  `hnui_frame_target` answers. `harness.tick` therefore resolves from a
  `setImmediate`; code after `await tick(...)` runs outside the tick.
- ImGui 1.93 merged `ImDrawListFlags` into `ImDrawFlags`, and with
  `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` the old names are gone: the `fill`
  scene turns anti-aliased fill off with
  `PushDrawFlag(ImDrawFlags_AAFill, false)` / `PopDrawFlag()`.
- No other ImGui precondition surfaced: with
  `ImGuiBackendFlags_RendererHasTextures` set in `rendererInit` and a
  framebuffer scale that is never 0, `NewFrame()` and `Render()` ran
  without asserting.
- The shared header gained a small module section, `frameOpen()`,
  `lastTarget()` and `throwNoFrame()`, which the test hooks need and the
  plan's block did not list.
- `utils/format.sh` does not cover `addons/` (its `FORMAT_DIRS` is `lib
  tools include unittests`), so the addon's sources were formatted with
  `clang-format-18 -style=file` directly.

### Task 5

- Orientation: imgui_impl_opengl3's projection unchanged (ImGui's y = 0
  at clip-space +1) and every viewport and scissor rectangle passed with
  `origin_top_left = true`, on GL as well: Sokol converts top-left
  rectangles against the pass height, and the presenter shows the
  target's high rows at the top, as `demo_visual.c`'s `sgl_ortho(0, w, h,
  0)` already assumes. CLIP decides the scissor's orientation and MANY
  the projection's (mutations (d) and (d') above). Captured colours are
  exact (0 or 255, or the clear colour) for every scene.
- `sg_buffer_usage.dynamic_update` is marked deprecated at the Sokol pin
  ("will be replaced with a .write_persistent flag"); it still works, and
  `sg_append_buffer` refuses an immutable buffer
  (`VALIDATE_APPENDBUF_USAGE`). A Sokol bump may need this changed.
- The device objects (shader, sampler, white image and view, buffers) are
  made at the first `rendererDraw`, inside a frame callback, not in
  `rendererInit`: `newFrame()` (which creates the context) may run outside
  one, and `demo_visual.c` documents resource creation as belonging inside
  a frame callback. `rendererInit` keeps the flags, the texture limits,
  the reset callback and the backend checks.
- The Metal shader source is written against `imgui_impl_metal.mm` and
  `sokol_gl.h`'s Metal simulator source; it has not been compiled. It
  waits for the Mac round.
- Each list is appended and then drawn before the next list is appended:
  growing replaces the buffer, and a list's offsets belong to the buffer
  it was appended to. MANYTWICE grows mid-frame (1,536,000 to 3,072,000
  vertex bytes) with draws already issued from the old buffer.
- `harness.tick()` throws on an empty list, `ui-imgui-frame.js` compares
  the framebuffer scale as a float32, and `utils/format.sh` covers
  `addons/` (the tidy-up commit before this task's).
- Review round 1: USERCALLBACK (the quarter-viewport callback with no
  reset after it) is what proves a user callback is called; `split=` now
  compares the command count with FILL's; a pipeline that fails for a
  format and sample count is not retried for that pair; a partial
  device-object failure destroys what was made. The `dynamic_update`
  deprecation is a tracker issue (`dz list`).

### Task 6

- After the font atlas's first upload, the 40-unit `XYZ` made ImGui
  **replace** texture 1 (same id, 512x128 both times), not create a second
  texture; the test accepts either. A `replaced` also appears before the
  marker, during GLYPHS0: ImGui 1.93 rasterizes glyphs on first use, so even
  `abc` at the default size updates the atlas. That is why the TEXTURE run
  anchors the GLYPHS1 check on a marker the test writes to stderr.
- QUAD's four samples were exact (0 or 255) with the linear clamp-to-edge
  sampler: 5 units in from a 200-unit quad's corner is well inside the outer
  half-texel, where clamping leaves one texel's colour unmixed.
- The `quad` scene makes its image and view on first use and checks
  `hnui_frame_target` first, so a first use outside a frame callback
  throws rather than creating Sokol resources there. No test reaches that
  throw.
- No change to `imgui_renderer.cpp` was needed: no defect surfaced.

### Task 7

- ImGui floors the mouse position (`AddMousePosEvent`), so the test aims
  at whole-number coordinates; a first draft aimed at the field's centre,
  which was at a half unit, and waited forever for the position to arrive.
- The test runs REPEATCHAR while the field is still focused from the text
  cases, before APRESS unfocuses it, rather than after APRESS as the plan
  lists them; each case is unchanged.
- APRESS waits for the repeated key-down to reach a JavaScript `keydown`
  listener (`e.repeat`), which runs after the native one, before its three
  frames: that is what makes the three frames follow the repeat's delivery
  rather than race it.
- The REAL key reached ImGui under Xvfb with nothing arranged for focus,
  as in `test/ui-real-events.js`: Sokol takes a KeyPress the driver sends
  to the window directly.
- `BackendPlatformName` changed from `hermes-node-ui (hnui)` to the plan's
  `hermes-node hnui`; no test read the old value.

### Task 8

- On Linux, a paste with nothing copied reads back `HNUI_UNAVAILABLE` (4):
  the host traces `clipboard read: no owner` under a fresh Xvfb. The test
  expects 4 on macOS too, on the ground that a new named pasteboard holds
  no string type, which the host reports as unavailable; not run there.
- ImGui has a default clipboard of its own (an in-process buffer), used
  when no backend sets the `platform_io` functions. So a clipboard test
  that reads only the field passes with no backend at all; the TRACE lines
  are what show the host was used.
- The `widgets` scene takes a string argument, `softcursor` or
  `hardcursor`; any other string is refused, and a string given to any
  other scene is a TypeError. Integer arguments are unchanged.
- The cursor's shape cache starts at `ImGuiMouseCursor_None`, so the first
  frame sets ARROW once: the host starts at Sokol's DEFAULT, not ARROW.
  `state().cursor` is that cache (the shape last set), -1 before any frame.
- Every cursor and clipboard trace is printed **before** its `hnui_*`
  call, so it precedes the host's trace of applying the command on the
  platform thread. Traced after the call, the host's line could print
  first under load. A line printed before the call cannot claim the call
  happened, so the write is traced as `clipboard write requested, <n>
  bytes`; the host's read-back line is the proof that it took effect.
- No test sends a real Ctrl+V, so a double paste (the chord and
  `HNUI_EVENT_PASTE` both handled) would go unnoticed: tracker issue
  01a21c37-6c02.
- The listener traces `event rejected: size <n>` once, the first time
  `hnui_size_ok` fails, instead of dropping silently. No test reaches it:
  the host built with this tree never sends a short event.
