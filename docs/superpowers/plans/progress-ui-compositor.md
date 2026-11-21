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
| D — lifecycle, Metal, suite | 15–17 (as 15a–15f, 16a–16f, 17a–17f) | **complete** | 2026-11-21 (Task 15: 2026-11-19; Tasks 16 and 17: 2026-11-21) |

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

**Task 16, the Metal backend, 2026-11-19 to 2026-11-21.** Six tasks (16a-16f)
across two machines; see "What Task 16 actually produced" below. Suite
totals at the end of it, both machines at "Keep test pasteboards off the
user's clipboard", 242 JS tests: **on the Mac, three runs, each 456 unit
tests and JS 237 passed, 5 unsupported, 0 failed; on Linux, 456 unit
tests and JS 232 passed, 10 unsupported, 0 failed.** Task 16f deleted one
JS test (`test/ui-no-window-support.js`), which is why the Mac's
unsupported count is one lower than the 6 it had after Task 16e. The
Mac's five unsupported are `bundle-yargs.js` (`examples-installed`),
`ui-clipboard.js` and `ui-real-events-demo.js` (`ui-backend-gl`),
`ui-sokol-validation-ci.js` (`ci`) and `ui-display-busy.js`
(`display-busy`, a canary that is unavailable on a healthy run). The
Mac also ran `ui-events.js` and `ui-close.js` 32 times with no failure,
and built a Release configuration with the UI on, whose window tests ran
once: 34 passed, 5 unsupported, 0 failed. **Two checks of the Mac
verification were not done**: the `display` canary with its promise
broken (the screen stayed unlocked, and the display was not put to
sleep), and the Dock Quit by hand (nobody was at the Mac). Both were
done during Task 17.

**Task 17, the full gated suite, 2026-11-21.** Six tasks (17a-17f); see
"What Task 17 actually produced" below. It added two JS tests, so there
are 244. Suite totals at the close of Slice D, 456 unit tests on both
machines: **on Linux, 456 unit tests passed and JS 234 passed, 10
unsupported, 0 failed; on the Mac under ASAN, three runs, each 456 unit
tests passed and JS 239 passed, 5 unsupported, 0 failed; on the Mac in a
Release build, once, JS 238 passed, 6 unsupported, 0 failed.** The
Release unit run was 455 of 456, then 456 once the failing test, which
depended on scheduling, was fixed. The Mac's JS runs and its three ASAN
whole-suite runs were on the commit before that fix ("Stop relying on
one drain for injected events"); after it only the unit suites were
rerun there, in both builds. Linux's ten unsupported are `bundle-yargs.js`
(`examples-installed`), `ui-sokol-validation-ci.js` (`ci`),
`ui-display-busy.js` (`display-busy`) and `ui-display-feature.js`
(`display-expected`, which this machine did not set), and the six
Metal-only tests (`ui-clipboard-mac.js`, `ui-metal-clear.js`,
`ui-metal-maintenance.js`, `ui-metal-maintenance-wake.js`,
`ui-metal-occlusion.js`, `ui-terminate.js`). The Mac's five are the five
it had after Task 16; its Release build adds `ui-sokol-validation.js`,
whose validation layer a Release build compiles out.

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

## What Task 16 actually produced

A window opens, draws, presents, resizes, closes and terminates on macOS,
through Metal, behind the same `backend.h` the GL backend implements.
Written out in `docs/superpowers/plans/2026-11-19-ui-compositor-task16.md`,
whose "as built" sections record what each task measured and where it
departed from its text. Linux implemented what the Linux suite can prove;
a second agent on an Apple-silicon Mac implemented what only macOS can
compile, and the Linux side reviewed that by reading. Sub-tasks, by commit
subject:

- **16a** (Linux) put the GPU backend and the platform services behind
  seams, added the backend lit features, `display-expected`,
  `%with-display` and `%timeout`, and moved real window events behind one
  driver script: "Put the GPU backend and the platform services behind
  seams", "Add the backend lit features, display-expected and the display
  wrappers", "Move the UI RUN lines to %with-display and %timeout, with
  per-backend checks", "Reach real window events through one driver
  script", and five commits hardening the macOS display lock and probe.
- **16b** (Mac): "Build the Metal toolchain and clear the window". It
  measured that a window covered from the moment it opens keeps a valid
  swapchain, which the design had assumed false.
- **16c** (Mac): "Draw, complete and blit through Metal", after "Give the
  demo addon's pattern shader a Metal spelling". The exact-pixel test
  holds on Metal.
- **16d** (Mac): "Reclaim Metal resources; drive real window events" --
  the maintenance pass, the invalid swapchain, and `ui.__driveWindow`.
  Three of the plan's predictions were refuted there, among them that a
  nil drawable kills a Debug build (Sokol skips it).
- **16e** (Mac): "Retry deferred Metal maintenance; tighten tests" and
  "Close, terminate and read the pasteboard on macOS", then the review's
  fixes, "Keep test pasteboards off the user's clipboard": the wrapper
  releases the private pasteboard it names, four scripts refuse to run
  on macOS without one, and `terminate:`'s trace reads back a count.
- **16f** (Linux): "Remove the backend-less UI build", "Probe for a
  display on macOS CI", and the documents ("Record the Metal backend in
  the documents", "Record the Mac verification of Task 16").

Rulings 80-88 are recorded in the main plan's Task 16 paragraph. The two
that reverse the spec: no Sokol patch was needed for the command-buffer
accessor (80), and the presenting instance needs no maintenance, so
nothing suspends blits (81).

What a green suite on both machines does **not** show, each filed in the
tracker: that a real Dock Quit, Cmd-Q or quit Apple event reaches the
`terminate:` override (the one second-process route tried reached no
handler at all, twice, and the check by hand was skipped); that the
maintenance pass's in-flight guard matters (removing it changes nothing
any tool reports, even with the hazard forced); display sleep; the
whole suite under a Release build (the window tests ran there once, and
passed); Intel Macs; fullscreen on macOS; and what GitHub's macOS
runners have for a display. One more is not filed, because it is a
check that was skipped rather than a defect: the `display` canary was
not seen to fail on the Mac with its promise broken during the final
verification.

*Since then (Task 17, 2026-11-21):* a real Dock Quit was checked by hand
and reaches the override; display sleep and the whole suite under
Release were measured; and the canary was seen to fail with the screen
locked. The rest of that list stands. See "What Task 17 actually
produced".

## What Task 17 actually produced

No product code: tests, one checker, and the measurements that close the
slice. Written out in
`docs/superpowers/plans/2026-11-21-ui-compositor-task17.md`, whose "as
built" sections hold the numbers. Sub-tasks, by commit subject:

- **17a** (Linux): "Exclude Sokol error lines in every window test".
  Every `%with-display` RUN line's stderr is now read with
  `--implicit-check-not='[sapp:error'` and `'[sokol-'`. Nine files
  lacked it, not the four the outline named. The rule is a program,
  `utils/check-ui-stderr-rule.py`, and `test/ui-stderr-rule.js` runs it,
  ungated, so a window test added without the exclusions fails the
  suite.
- **17b** (Linux): "Test hnui_get_window after a late rejection".
  `test/ui-late-rejection.js` provokes two of the three late failures
  from a script and asserts no window is published. Publishing the
  window before it is complete is a use-after-free under ASAN.
- **17c** (Linux): "Stop relying on one drain for injected events".
  `test/ui-close.js` REPLACED went red, and `test/ui-events.js` TICKS
  stayed green with the call it pins removed, once injected batches were
  split across frames. Both now inject the second event from the first
  one's listener and wait on `ui.__waitPlatformFrames(1)`. **No hook was
  added**: an atomic multi-event inject would guarantee one drain, and
  what the tests need is one dispatch pass, which a drain does not give
  when a dispatch for a real event is already posted.
- **17d** (Mac ran it): "Record the Mac check of Task 17 on Metal". The
  census of a healthy Metal run: no `[sapp:` line at any level and no
  `[sokol-present` line, where GL prints one DPI warning. The injected
  Sokol lines turn 24, 24 and 23 files red there; the late-rejection
  mutation and the five dispatch-pass rows read as on Linux.
- **17e** (Mac ran it): "Record the Mac suite runs for Task 17", with
  "Give the surface table stress test's consumer its turns" before it.
  The first whole-suite run under Release found one unit test,
  `SurfaceTableTest.ProducerConsumerStressCompletesRealCycles`, failing
  8 of 8: its progress check assumed the consumer thread gets a share of
  an unfair mutex. A test defect; fixed, and its named mutation still
  fails it.
- **17f** (Linux): "Close Slice D of the UI compositor", the documents
  and the tracker.

Three checks were made by hand at the Mac on 2026-11-21, recorded in
"Task 17f as built":

- **A real Dock Quit reaches the `terminate:` override**, through
  `performClose:`. The trace line that shows it is printed after
  `performClose:` returns, so a run in which nothing prevents the close
  can end, through the JS thread's `process.exit()`, before the line is
  printed; its absence there proves nothing. Cmd-Q and the quit Apple
  event are still unproved.
- **Display sleep with a window open**: frames slowed to about one a
  second while the display slept, the swapchain was reported invalid
  once and valid again on waking, frames resumed, and no Sokol line was
  printed. One run of 15 s.
- **The `display` canary with its promise broken**: with the screen
  locked and `HERMES_NODE_UI_EXPECT_DISPLAY=1`,
  `test/ui-display-feature.js` failed and `test/ui-present.js` was
  UNSUPPORTED; unlocked, both passed.

What a green suite on both machines still does **not** show, each in the
tracker: the GL backend on NVIDIA's driver, native Xorg or Intel GL (the
suite's window tests draw through llvmpipe under Xvfb, always); Intel
Macs; Cmd-Q or a quit Apple event reaching `terminate:`; the in-flight
maintenance guard; fullscreen on macOS; and what GitHub's macOS runners
have for a display.

## Things a newcomer needs to know

- **The lit features are not interchangeable.** `ui-enabled`
  probes the *plain* `hermes-node` binary for `--test-embedder-module` and
  really means "the CMake option was compiled in". `ui` probes the separate
  `hermes-node-ui` binary. `ui-gpu` is `ui-enabled` **and** a backend
  platform, mirroring `if(NOT UNIX)` in
  `lib/compositor-gpu/CMakeLists.txt`, which is a configure error now, so
  the two agree wherever the option configured. `display` starts a real
  Xvfb on Linux and, on macOS, asks about the console session and then
  opens a window and requires a drawable. `sokol-validation` is read from
  the second line of `hermes-node-ui --version`, and `ui-backend-gl` /
  `ui-backend-metal` from the third. `display-expected` narrows
  `display`'s canary. The names mislead: `ui-enabled` is the one that
  sounds like it means the UI and is the one that does not. CLAUDE.md
  names each feature's canary test.
- **Window tests run under `%with-display <seconds>`**
  (`utils/with-display.py`): on Linux `timeout` over `utils/with-xvfb.py`,
  not `xvfb-run -a`, whose display allocation races under parallel load;
  on macOS a machine-wide lock, so window tests run one at a time.
- **Environment variables**: `HERMES_NODE_UI_TEST_HOOKS`,
  `HERMES_NODE_UI_GL_ROUTE`, `HERMES_NODE_UI_GL_COMPLETION` (both refused
  by name on Metal), `HERMES_NODE_UI_TEST_NO_ACTIVATE`,
  `HERMES_NODE_UI_TEST_PASTEBOARD`, `HERMES_NODE_UI_EXPECT_DISPLAY`, and
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
does not restore the terminal. Task 16 closed the no-window stub's issue as
moot (the stub is deleted) and added nine: surface generations piling up
within one event-delivery pass; fullscreen on macOS untested; no main
menu, so Cmd-Q does nothing; a cancelled or slow `close` leaving a logout
waiting; a real Dock Quit reaching `terminate:` unproved; the maintenance
in-flight guard verified by nothing; display sleep untried, and a
Release build run only as far as its window tests, once; Intel Macs
untested; and whether GitHub's macOS runners have a
display. Task 17 closed the display-sleep and Release issue (both were
measured), narrowed the Dock Quit one to Cmd-Q and the quit Apple event
(a real Dock Quit was checked by hand and reaches `terminate:`), and
added one: the GL backend is untested on NVIDIA's driver, native Xorg
and Intel GL.

## Obligations carried into later tasks

All live in the plan itself, at the task that discharges them, because task
briefs are cut from the plan and an obligation recorded anywhere else is one
no brief will carry. **Task 15's eight, carried from Slice C, are
discharged**; the plan's Task 15 paragraph names the task and test for each.
**Task 16's, listed below, are discharged or filed**: the asserts are in
`metal_backend.mm`; the pasteboard is read directly; the Sokol units
compile as Objective-C++ with ARC and `sokol_config.h` refuses otherwise;
the close button's cancel is tested with a real `performClose:`
(`test/ui-real-events.js`, `test/ui-close.js`); termination is intercepted
by overriding `terminate:` (`test/ui-terminate.js`), verified in-process
only; and the tests that assumed GL keep their RUN lines, the Metal
backend refusing the GL variables before `sapp_run()`. They were:
**Task 16** owned a backend-local copy of the `sg_view` asserts (optional —
`HnuiAbiTest` already covers every platform); the macOS clipboard read,
which the Mac probes showed must read `NSPasteboard` directly, because
`sapp_get_clipboard_string()` truncates silently and cannot tell empty text
from no text; compiling the two `sokol_gfx` files as Objective-C++, with a
`#error` in `sokol_config.h` when `SOKOL_METAL` is defined without
`__OBJC__`; and, from Task 15, keeping "the quit is always cancelled and the
JS thread `_exit`s", with a check that `sapp_cancel_quit()` from `event_cb`
inside `windowShouldClose:` cancels the close as it does on X11, tested
with a real close request. **Task 17's, listed next, are discharged**:
the stderr-check sweep by `test/ui-stderr-rule.js`, which enforces the
rule over every window test (nine files needed it, not four); the
rejection test by `test/ui-late-rejection.js`; and the unenforced
one-drain assumption by rewriting the two assertions that leaned on it,
`test/ui-close.js` REPLACED and `test/ui-events.js` TICKS, around
`ui.__waitPlatformFrames(1)`, so that nothing assumes it any more (the
injector was not made explicit, and the Task 17 plan says why). They
were: **Task 17** kept a stderr-check
sweep of four tests and one small rejection test (macOS gating, which it
also carried, was done by Task 16); the lifecycle assertions
it was waiting for now exist (`test/ui-exit-paths.js`, `test/ui-close.js`,
`test/ui-exit-tty.js`), and the real-framebuffer resize it used to carry was
discharged by `test/ui-real-events.js`. From Task 15 it also inherits one
unenforced assumption: `test/ui-close.js` REPLACED and `test/ui-events.js`
rely on two back-to-back `__injectEvent` calls being taken by one
`drainInjectedEvents()` call (measured 64/64 at 16-way parallel on Linux),
which a new platform's suite run should confirm or the injector should make
explicit. Task 16f's Mac verification ran a 32-run loop for exactly
that on 2026-11-21: 32 of 32 passed on macOS. That confirms it there;
nothing enforces it, so the choice of making the injector explicit is
still Task 17's.

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
gated on `ui-gpu` without `display`, and their RUN lines use `timeout` and `timeout --foreground`. Three more tests are gated on `ui-gpu` without `display` and will start running on macOS the same day: `test/ui-null-window.js`, `test/ui-version-check.js` and `test/ui-exit-late-require.js`; check each against the Metal backend too. On `timeout`: macOS 27 ships one in `/usr/bin` that accepts `timeout <n> <command>` and `--foreground` (measured on the Mac, 2026-11-19), older releases ship none, and 46 RUN lines in the window tests use the plain form. Once a Metal backend makes
`ui-gpu` true on macOS, each will either open a real window or fail. Task 16
must re-gate them (on a GL-route feature, or `linux`), or give them a route
the Metal backend refuses before `sapp_run()` too, and bound them without
`timeout` there.
