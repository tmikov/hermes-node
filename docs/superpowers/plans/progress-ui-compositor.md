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
| D — lifecycle, Metal, suite | 15–17 | Task 15 (as 15a–15f) **complete**; 16 and 17 not started | Task 15: 2026-11-19 |

Slice A: 49 tests in `unittests/CompositorTest.cpp`. Slice B: 33 commits, 75
files, and the suite at 414 unit tests plus 201 JS tests. Slice C: eight
tasks, a whole-slice review and one fix wave; the suite stands at 449 unit
tests and 220 JS tests, with 4 unsupported (one needs `examples/` installed,
three are CI- or macOS-only gates). Task 15: six tasks and eight commits;
the suite stood at 452 unit tests and 224 JS tests, with 4 unsupported.
A whole-branch review of Task 15 returned ready after fixes, and one fix
wave (2026-11-19) closed its list: the runtime-return path's CLOSING is now
pinned in-process, three more CLOSING branches have cases, the exit hooks
act only for the runtime that instantiated the module, `hnui.h` gained a
"Closing" section, and the three tests that run on macOS without a GPU
backend bound themselves without `timeout`. The suite stands at 452 unit
tests and 225 JS tests (the new one is `test/ui-exit-late-require.js`),
with 4 unsupported.

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
`ui, ui-gpu`. `test/ui-no-window-support.js` runs there and passes. No
window was opened on macOS: that is Task 16.

Later the same day, on the same machine, the Metal half was probed ahead of
Task 16. The two `sokol_gfx` implementation files compile for Metal as
Objective-C++ with no warnings; `utils/check-sgp-isolation.py` reads the
Mach-O objects correctly through Apple's `nm` (156 `sg_*` against 156
`sgp_*`, and an un-renamed control is refused); and both instances ran
concurrently on one `MTLDevice` under Metal API and GPU validation. That run
was offscreen and clear-only -- no drawable, presentation, shader or
pipeline. The exit-hook commit (`Enter CLOSING before 'exit' listeners on
process.exit and fatal paths`) passed there with 442 unit tests. The suite
then hung twice in `test-dns-resolve.js`, which led to the c-ares fix (`Re-arm
the c-ares timeout timer`: the timeout timer fired once and was never
re-armed, so a query to a server that never answered stayed pending
forever). At that commit the Mac suite was green twice under load: 442 unit
tests, 200 JS tests passing and 27 unsupported.

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

## What Task 15 actually produced

A window closes, and every way a windowed process ends is now defined.
Written out in `docs/superpowers/plans/2026-11-19-ui-compositor-task15.md`;
each task there carries an "As built" note for where it departed from the
plan.

- **15a** split `ui_module.cpp` by thread into `ui_module.cpp` (JS thread),
  `ui_platform.cpp` (platform thread), `ui_test_hooks.cpp` and
  `ui_internal.h`, whose every declaration states its owner thread and
  guard. A pure move, checked line by line against the old file.
- **15b** added `ProcessExitHooks` to `lib/process` and CLOSING on the
  `process.exit()` and fatal paths, plus `__waitPlatformFrames`.
- **15c** made CLOSING fail clipboard reads of both kinds, refuse commands,
  and trace every refused `post()`; and captured `process._preExit` once, so
  a program cannot skip the fatal path's hook by deleting the property.
- **15d** ended a runtime that returns with a window requested
  (`uiBeforeRuntimeCleanup`, a new `_exit()` site that restores the
  terminal) and released the loop handle on a rejected `createWindow`.
- **15e** made `close` cancelable (`event.preventDefault()`,
  `hnui_cancel_close`), made an uncancelled `close` and `win.close()` end
  the process, added `__waitPlatformQuitRequests`, and closed tracker
  `01a0d88a-6518`.
- **15f** made the two `win.close()` endings checkable, corrected every
  comment that still described the old behaviour, and wrote this.

**Before and after, measured on `cmake-build-asan` under `utils/with-xvfb.py`:**

| Scenario | Before | After |
|---|---|---|
| A real `WM_DELETE_WINDOW`, `close` listener that does not cancel, `process.exitCode = 7` (`test/ui-close.js` DEFAULT) | `close` emitted, window stays, killed by `timeout` (124) | exit 7, the `'exit'` listener's eight lines through a pipe |
| `createWindow()` then a synchronous `throw` (`test/ui-exit-paths.js` THROW) | error and `'exit'` listener, then `window ready` after them, then 124 | exit 1, eight lines and a barrier through a pipe, CLOSING before the listener |
| `HERMES_NODE_UI_GL_ROUTE=bogus`, rejection handled, script returns (`test/ui-route-refusal.js` RET) | `REJECTED ...`, then 124 | exit 5, the program's `process.exitCode` |
| A rejected `createWindow` (`HERMES_NODE_UI_GL_ROUTE=bogus`), handled, the script returning, with `stdin.setRawMode(true)` on a pty (`test/ui-exit-tty.js`) | 124, `LEFT-IN-RAW-MODE` | terminal restored |
| `process.exit(0)` in a `keydown` listener with a frame requested (`test/ui-exit-paths.js` LISTENER) | the frame callback ran after `'exit'` listeners | refused, traced |

**Suite totals:** 449 unit tests and 220 JS tests before 15b; after 15f,
**452 unit tests and 224 JS tests, 4 unsupported**, with no failure. The
three new unit tests are `ProcessExitHooksTest.HooksReceiveWhatTheCallSitePassed`,
`OwnerDispatchTest.CloseFromInsideAPostedFunctionRunsTheRestAndRefusesLaterPosts`
and `TickMachineTest.NoTickIsSentOnceTheTableIsClosing`; the four new JS
files are `test/ui-exit-paths.js`, `test/ui-exit-tty.js`, `test/ui-close.js`
and, unrelated to the UI, `test/test-dns-timeout.js` from the c-ares fix.

**Departures from the spec, and where it was edited** (15e, Step 7, in
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`): the `quit`
command is gone from "Input and platform services"; "Closing" there says
every quit is cancelled, CLOSING included, and why, then how a listener
cancels; "Lifecycle" says the window lives "until the process ends", that
a rejected `createWindow` releases its handle, that abnormal return ends in
`_exit()` from the JS thread rather than tearing the window down, that
CLOSING fails reads of both kinds and is entered on every path that emits
`'exit'`, what "unavailable" means for a command, and that CLOSED is the
`_exit()`; "The JS API" gains `preventDefault()` and `win.close()`; "The
native C API" gains `hnui_cancel_close`. These are Rulings 70-79 in the
plan's Task 15 paragraph.

**What review established, and is worth carrying.** Five planned or proposed
checks could not fail on the mutation written beside them. Four were
FileCheck `-NOT` lines sitting between two `-DAG` lines or after the last
positive line, where they cannot see what precedes their anchor; the fifth
was an alternation satisfied by either arm. Each was rewritten as a separate
FileCheck run or an `--implicit-check-not`, and proven by a mutation that
leaves every positive line matching. That rule is now in CLAUDE.md. Two
planned mutations predicted a hang and got an exit instead, because CLOSING
releases the loop handle and the runtime-return hook then ends the process
with `process.exitCode`; a test whose only failure signal is `timeout` is
blind to that mutant.

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
end of Slice C the UI ones were: closing the window hanging the process when
the quit is not cancelled (closed by Task 15); a Slice A test asserting a
wall-clock throughput floor; lit setting no per-test timeout; the completion
probe's 2 s handshake budget; the no-window stub's text being executed by no
test on Linux; and five filed by the Slice C review — the custom-cursor bind
being verified by no test, the input queue growing without bound after a
late `createWindow` rejection, callback ids being `int`, a clipboard reply
landing inside Sokol's event batch, and (not UI code) a throw inside a
`.then()` never reaching an `uncaughtException` listener. Task 15 added one,
also not UI code: an unhandled rejection exits through an `_exit(1)` that
does not restore the terminal.

## Obligations carried into later tasks

All live in the plan itself, at the task that discharges them, because task
briefs are cut from the plan and an obligation recorded anywhere else is one
no brief will carry. **Task 15's eight, carried from Slice C, are
discharged**; the plan's Task 15 paragraph names the task and test for each.
**Task 16** owns a backend-local copy of the `sg_view` asserts (optional —
`HnuiAbiTest` already covers every platform); the macOS clipboard read,
which the Mac probes showed must read `NSPasteboard` directly, because
`sapp_get_clipboard_string()` truncates silently and cannot tell empty text
from no text; compiling the two `sokol_gfx` files as Objective-C++, with a
`#error` in `sokol_config.h` when `SOKOL_METAL` is defined without
`__OBJC__`; and, from Task 15, keeping "the quit is always cancelled and the
JS thread `_exit`s", with a check that `sapp_cancel_quit()` from `event_cb`
inside `windowShouldClose:` cancels the close as it does on X11, tested
with a real close request. **Task 17** keeps macOS gating, a stderr-check
sweep of four tests, and one small rejection test; the lifecycle assertions
it was waiting for now exist (`test/ui-exit-paths.js`, `test/ui-close.js`,
`test/ui-exit-tty.js`), and the real-framebuffer resize it used to carry was
discharged by `test/ui-real-events.js`. From Task 15 it also inherits one
unenforced assumption: `test/ui-close.js` REPLACED and `test/ui-events.js`
rely on two back-to-back `__injectEvent` calls being taken by one
`drainInjectedEvents()` call (measured 64/64 at 16-way parallel on Linux),
which a new platform's suite run should confirm or the injector should make
explicit.

**Also Task 16's, found by the whole-branch review of Task 15 and checked
against `external/sokol/sokol/sokol_app.h`.** On macOS, `[NSApp terminate:]`
-- the Dock's Quit, a Cmd-Q menu item, logout -- never raises
`QUIT_REQUESTED` in the vendored Sokol. That event comes only from the
window delegate's `windowShouldClose:`. Sokol's app delegate implements
`applicationShouldTerminateAfterLastWindowClosed:` and
`applicationWillTerminate:`, and no `applicationShouldTerminate:`, so AppKit
terminates without asking: `applicationWillTerminate:` calls
`_sapp_call_cleanup()` (our `cleanup_cb`), then `_sapp_macos_discard_state()`
and `_sapp_discard_state()`, and AppKit then exits 0 from the main thread
with the JS thread still running. That breaks three things Task 15
established: every quit request is cancelled, `cleanup_cb` never runs, and
the JS thread ends the process with its status. The Metal task must either
intercept termination -- for instance by adding
`applicationShouldTerminate:` to Sokol's delegate class at run time
(`class_addMethod` on `_sapp_macos_app_delegate`, since the vendored source
stays unmodified), answering `NSTerminateCancel` and forwarding the request
to the JS thread as `close` -- or document the exception where Task 15's
guarantees are stated (CLAUDE.md, the spec's Lifecycle, `hnui.h`), and test
whichever it chooses.

**And a second: two tests reach "fails before `sapp_run()`, no display
needed" only on Linux.** `test/ui-exit-tty.js` and `test/ui-route-refusal.js`
(all three of its runs, RET included) get there through
`HERMES_NODE_UI_GL_ROUTE=bogus`, which only the GL backend refuses; they are
gated on `ui-gpu` without `display`, and their RUN lines use `timeout` and
`timeout --foreground`, which a stock macOS lacks. Once a Metal backend makes
`ui-gpu` true on macOS, each will either open a real window or fail. Task 16
must re-gate them (on a GL-route feature, or `linux`), or give them a route
the Metal backend refuses before `sapp_run()` too, and bound them without
`timeout` there.
