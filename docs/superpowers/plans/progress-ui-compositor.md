# UI compositor — progress

Tracks `docs/superpowers/plans/2026-09-21-ui-compositor.md`, which implements
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`. Branch
`work-imgui`. Everything is behind `HERMES_NODE_ENABLE_UI`, default OFF.

## Status

| Slice | Tasks | State | Finished |
|---|---|---|---|
| A — foundations, no GPU | 1–4 | **complete** | 2026-09-22 |
| B — one window presenting one known pattern | 5–10 | **complete** | 2026-09-25 |
| C — the rest of the surface area | 11–14 (as 11a–11d, 12, 13, 14a, 14b) | **complete** | 2026-11-13 |
| D — lifecycle, Metal, suite | 15–17 | not started | — |

Slice A: 49 tests in `unittests/CompositorTest.cpp`. Slice B: 33 commits, 75
files, and the suite at 414 unit tests plus 201 JS tests. Slice C: eight
tasks, a whole-slice review and one fix wave; the suite stands at 449 unit
tests and 220 JS tests, with 4 unsupported (one needs `examples/` installed,
three are CI- or macOS-only gates).

**First macOS build, 2026-11-19** (Apple M4 Max, macOS 27.2, Apple clang 21,
ASAN Debug with `HERMES_NODE_ENABLE_UI=ON`): the tree configures and builds
with no errors and no warnings in the UI code, including
`tools/hermes-node-ui/ui_module_nowindow.cpp`, which no Linux build
compiles. 439 unit tests pass, the difference from Linux being
`TestHooksTest`'s ten, which is built only with a GPU backend. 199 JS tests
pass and 25 are unsupported: the 23 gated on `ui-gpu` or `display`,
`bundle-yargs.js`, and `ui-sokol-validation-feature.js`. That last one
failed on the first run, because it required only `ui` while asserting a
`--version` line compiled only with window support; its gate is now
`ui, ui-gpu`. `test/ui-no-window-support.js` runs there and passes. Nothing
window- or GPU-related was compiled or run on macOS: that is Task 16.

## What Slice B actually produced

`hermes-node-ui` opens a real window, the reference addon draws a fixed
pattern into a compositor surface **on the JS thread**, the platform thread
blits it, and `test/ui-present.js` compares the presented pixels. The
comparison is **exact** — 0 or 255, no tolerance — with samples kept 8px from
any edge. That is stronger than a threshold rather than weaker, and it is
affordable because the capture's agreement rule forces target size to equal
framebuffer size, so the blit is 1:1, linear filtering degenerates to a copy,
the pattern is flat-shaded, and nothing on the path converts colour (the
swapchain is RGBA8, not sRGB). The presenter's own clear is `32/255`,
deliberately a value the pattern never produces, so a missing blit is
legible rather than ambiguous.

Per task: **5** the `hermes-node:` embedder-module prefix; **6** the binary,
`UiHost`, and the windowless path; **7** the public C ABI (`hnui_abi.h`);
**8** Sokol vendored at `c0db757`, the GPU target, two `sokol_gfx` instances
under renamed symbols, and the window; **9** test hooks — controllable
completion, frame capture, synthetic event injection; **10** the picture.

## What Slice C actually produced

Everything between "a window presents a picture" and "a program can be
written against it", except closing:

- **11a–11b, input.** A pure `sapp_event` → `UiEvent` converter, an
  `EventDispatcher`, the window object as an `EventEmitter`, native listeners
  before the JS `emit`, and a window-state mirror that travels with each
  event.
- **11c, commands.** Title, fullscreen, icon, cursor, pointer lock, clipboard
  write — pushed from the JS thread, applied on the platform thread, each
  traced by what was read back.
- **11d, resize.** A change of framebuffer size starts a new generation; the
  old one is destroyed on the owner thread. This is where the destroy
  callback wired in Slice B first ran.
- **12, clipboard read.** Our own X11 read with a status, replies paired by
  server timestamp; a Promise for JS and a synchronous call for native code.
- **13, `requestAnimationFrame`.** One registration-ordered list for native
  and JS callbacks, microtask checkpoints between them, cancellation,
  exception routing, and a timestamp with `process.uptime()`'s origin.
- **14a–14b, the C API and the reference addon.** The remaining `hnui_*`
  entry points, the version check, behavioural tests of the size-prefix
  rule, and the addon's `sokol_gl` visual mode and textual conformance mode.

**The whole-slice review's finding is the one to carry forward.** It found
no defect in the shipped path — it drove real keys, pastes, resizes and close
requests from a second X client and every one behaved — but six mutations of
load-bearing lines left every window test green. The cause was specific:
every event test used `__injectEvent`, which enters *below* Sokol, so nothing
above the injection point was ever executed. Removing `desc.event_cb =
onEvent` outright passed the suite. The fix wave added
`test/ui-real-events.js` and closed five more unpinned invariants. Slice B's
signature failure was "something that looked like verification and was not";
Slice C's was the same shape with one cause, and the rule that follows from
it is written into CLAUDE.md: inject for what happens after the converter,
send a real X event for anything that involves Sokol or the window system.

Decisions made while expanding and executing the slice are Rulings 50–69:
50–59 in the plan's "Slice C pre-flight rulings", the rest in the task text,
the spec, or Tasks 15–17 where they bind. The ones a user would notice: touch
events are cut from v1 (Sokol produces them on no v1 platform); the clipboard
read is ours rather than a Sokol patch; the frame timestamp's origin is
`process.uptime()`'s because this runtime has no `performance` global; a
Sokol panic now terminates the process on all three loggers; and until Task
15 the close button emits `close` and leaves the window open.

## Things a newcomer needs to know

- **Five lit features, and they are not interchangeable.** `ui-enabled`
  probes the *plain* `hermes-node` binary for `--test-embedder-module` and
  really means "the CMake option was compiled in". `ui` probes the separate
  `hermes-node-ui` binary. `ui-gpu` is `ui-enabled` **and** a backend
  platform, mirroring `if(NOT (UNIX AND NOT APPLE))` in
  `lib/compositor-gpu/CMakeLists.txt`. `display` starts a real Xvfb.
  `sokol-validation` is read from the second line of `hermes-node-ui
  --version`. The names mislead: `ui-enabled` is the one that sounds like it
  means the UI and is the one that does not. CLAUDE.md says why `display`
  and `sokol-validation` each have a canary test.
- **Window tests run under `utils/with-xvfb.py`**, not `xvfb-run -a`, whose
  display allocation races under parallel load.
- **Environment variables**: `HERMES_NODE_UI_TEST_HOOKS`,
  `HERMES_NODE_UI_GL_ROUTE`, `HERMES_NODE_UI_GL_COMPLETION`, and
  `HERMES_NODE_DEBUG_NATIVE=UI` for tracing. `test/lit.cfg` replaces the
  environment for ShTest, so an exported value cannot reach a lit test —
  load-bearing, because `ui-present.js`'s third run depends on the
  completion variable being *unset*.
- **Assertions come from tracing, never from timing.** The suite runs 16-way
  parallel.
- **`sg_*` on the JS thread, `sgp_*` on the main thread.** One `_sg` global
  per process is why the presenting copy is compiled under renamed symbols;
  `utils/gen-sokol-rename.py` generates them and
  `utils/check-sgp-isolation.py` proves the two objects share no symbols.

## Known defects and limitations

Filed in `dz/`, not duplicated here — `dz list` is authoritative. As of the
end of Slice C the UI ones are: closing the window hanging the process when
the quit is not cancelled (Task 15 owns it); a Slice A test asserting a
wall-clock throughput floor; lit setting no per-test timeout; the completion
probe's 2 s handshake budget; the no-window stub's text being executed by no
test on Linux; and five filed by the Slice C review — the custom-cursor bind
being verified by no test, the input queue growing without bound after a
late `createWindow` rejection, callback ids being `int`, a clipboard reply
landing inside Sokol's event batch, and (not UI code) a throw inside a
`.then()` never reaching an `uncaughtException` listener.

## Obligations carried into later tasks

All live in the plan itself, at the task that discharges them, because task
briefs are cut from the plan and an obligation recorded anywhere else is one
no brief will carry. **Task 15** carries eight from Slice C, numbered in its
paragraph: splitting `ui_module.cpp` by thread as Slice D's first commit;
failing outstanding clipboard reads of both kinds on entering CLOSING;
releasing frame callbacks at teardown; `win.close()` and revisiting every
test that ends through `process.exit()`; testing the close handshake with a
real `WM_DELETE_WINDOW`; deciding how a native listener cancels a close; the
refused-post edges; and `process.exit()` inside a listener letting a frame
callback run after `'exit'`. **Task 16** owns a backend-local copy of the
`sg_view` asserts (optional — `HnuiAbiTest` already covers every platform)
and, since Slice C, the macOS clipboard read (`sapp_get_clipboard_string`
is reliable there). **Task 17** keeps macOS gating, the lifecycle
assertions, a stderr-check sweep of four tests, and one small rejection
test; the real-framebuffer resize it used to carry was discharged by
`test/ui-real-events.js`.
