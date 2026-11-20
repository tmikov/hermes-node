# Task 16: the Metal backend, written out (16a-16f)

Expands the paragraph "Task 16 -- The Metal backend" of
`docs/superpowers/plans/2026-09-21-ui-compositor.md`. Written 2026-11-19 on
Linux, from the spec, the two macOS spike notes, the Mac probes of
2026-11-19 and the tree as Task 15 left it.

**What was and was not verified while writing this.** Everything said about
this repository's code and about vendored Sokol was read in the tree.
Three things were run on Linux: that a function appended to a translation
unit after `#include <sokol_gfx.h>` (with `SOKOL_GFX_IMPL`) can read the
file-static `_sg` (`clang++ -fsyntax-only`, dummy backend); that this
tree's `FileCheck` accepts `--check-prefixes=A,A-gl`, ignores directives of
an unselected `A-metal` prefix, and tolerates a selected prefix with no
directives; and that `.superpowers/` is git-ignored. **Nothing here was
compiled or run for macOS.** Every statement about how AppKit, Metal or
Apple's toolchain behaves that is not quoted from the spike notes or the
Mac probes is marked `UNVERIFIED:` with the probe that settles it; the
probes are collected at the end and should run before the task that depends
on them.

Baselines. Linux: 452 unit tests, 225 JS tests, 4 unsupported. Mac
(2026-11-19, no GPU backend): 442 unit, 200 JS passed, 29 unsupported.

## How this task is executed

Two machines. The **Linux driver** owns `work-imgui`, plans, reviews, and
implements everything the Linux suite can prove. The **Mac agent**
implements everything only macOS can compile, on its own branch
`xtest/work-imgui-mac`, from a brief cut from one task below.

Rules that hold for every Mac-implemented task:

- The Mac agent starts from the commit the driver names by subject on the
  channel branch, and commits on `xtest/work-imgui-mac`. Commit messages
  ASCII, no emojis, no commit hashes in any file.
- Every new file carries `Copyright (c) Tzvetan Mikov.` and the MIT header.
- **All Objective-C(++) is compiled with ARC** (`-fobjc-arc`). Sokol's
  texture injection is only correct under ARC: `_sg_mtl_create_image` ends
  its loop with `_SG_OBJC_RELEASE(mtl_tex)`, which is `obj = nil` under ARC
  and `[obj release]` without it -- an injected texture would be released
  out from under its owner.
- **The Mac agent does not run a formatter.** `utils/format.sh` requires
  clang-format 18, which the Mac does not have; the driver formats `.mm`
  and `.m` files on Linux when folding a task in (16a teaches `format.sh`
  those extensions).
- The Mac agent edits shared files only inside `if(APPLE)` / `#if
  defined(__APPLE__)` / `-metal` check-prefix regions, and lists any edit
  outside one in its report instead of making it silently. The driver
  reads the whole diff, checks that rule, runs `./utils/format.sh -f` and
  `cmake --build cmake-build-asan --target check-hermes-node` on Linux, and
  folds the commits into `work-imgui`.
- The driver cannot compile `.mm`. Its review is by reading, so each Mac
  task's verification block includes the runtime checkers that stand in
  for a second compiler: Metal API and GPU validation, the Main Thread
  Checker, and zombies.

**Mac commands used by every task** (zsh or bash, BSD userland):

```bash
cd <checkout>; S=$(pwd); B=$S/cmake-build-asan
# build only what a task names:
cmake --build $B --target hermes-node-ui hnui_demo_addon hnui_bad_version_addon 2>&1 | tail -3
# run named lit tests with exactly the params CMake passes (16a adds the script):
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B -a $S/test/<name>.js
# whole suite:
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu cmake --build $B --target check-hermes-node 2>&1 | tail -15
```

`caffeinate -dimsu` keeps the display awake and wakes it if asleep; it does
not unlock a locked screen. A locked screen or a display that sleeps
mid-run is handled by the `display` feature (Design (f)).

---

## Design

### (a) What `backend.h` requires, and how each entry maps to Metal

`CompositorBackend` has eighteen pure virtuals after 16a (seventeen today,
plus `workerLoopTurn`). Sources: spec "Surfaces", "Completion, per
backend", "Presenting"; spike results sections 1-5; `gl_backend.cpp` and
`gl_present.cpp` for what the shared callers already assume.

| Method (thread) | GL today | Metal |
|---|---|---|
| `selectContextRoute` (platform, before `sapp_run`) | Parses `HERMES_NODE_UI_GL_ROUTE` and `HERMES_NODE_UI_GL_COMPLETION`; refuses unknown and unbuilt routes. | There is no route. Two checks, in this order: (1) a non-empty `HERMES_NODE_UI_GL_ROUTE` or `HERMES_NODE_UI_GL_COMPLETION` is **refused by name** -- a VM setting that silently does nothing is the failure shape this codebase refuses, and this is also the "route the Metal backend refuses before `sapp_run()`" the Task 16 paragraph asks for; (2) `MTLCreateSystemDefaultDevice()` returning nil is refused ("no Metal device"). Nothing touches AppKit before both pass, so a machine with no window server or no GPU gets a rejected Promise, not a crash. |
| `createSharedContext` (platform, `init_cb`) | Creates the worker's GLX context; sets up the presenter. | Nothing to create: the device is shared (spike 2, 5). Takes `sapp_get_environment().metal.device` into a strong `id<MTLDevice>`, sets up the presenting instance (`sgp_setup(sglue_environment())`, blit shader in MSL, pipeline, linear clamp sampler). With test hooks on, clears `framebufferOnly` on Sokol's `CAMetalLayer` so a presented frame can be read back (see `readPresentedPixels`). |
| `makeCurrentOnWorker` (worker) | `glXMakeContextCurrent` surfaceless; `sg_setup`; the stage-2 fence probe. | Nothing to bind. Installs the worker's first autorelease pool, then `sg_setup` with `environment.metal.device` = that device and the same defaults GL passes (`color_format` = the presenter's, `depth_format = NONE`, `sample_count = 1`). **No completion probe exists and no completion mode is selected**: there is one mechanism. |
| `createGenerationSurfaces` (worker) | `glTexStorage2D` x3, then `sg_make_image` with `gl_textures[0]`, two views each. | `sg_make_image` x3 with `usage.color_attachment` and `usage.resolve_attachment`, Sokol allocating the texture; two views each. The native texture is read back with `sg_mtl_query_image_info(img).tex[0]` and kept as a strong `id<MTLTexture>` in the generation map for the presenter. (Spike 2 did exactly this.) Refused with an error when the id-pool guard trips (see Maintenance). |
| `destroyGenerationSurfaces` (worker) | `sg_destroy_*`, `glDeleteTextures` -- immediate. | `sg_destroy_view` x6, `sg_destroy_image` x3. Sokol defers each Metal release to `frame_index + SG_NUM_INFLIGHT_FRAMES + 1` and collects only inside `sg_commit()`, so this sets `maintenancePending_` (see Maintenance). |
| `frameTarget`, `generationSize` (worker / either) | Map lookup under `surfacesMutex_`. | Identical. `color_format` is the presenter's, which on macOS is `SG_PIXELFORMAT_BGRA8` (`_sapp_mtl_color_format`). |
| `submitAndTrackCompletion` (worker) | `sg_commit`, `glFenceSync`, `glFlush`; block or hand the fence over. | Read Sokol's frame command buffer through the accessor (b) **before** `sg_commit()`; `addCompletedHandler:` whose block calls `onComplete(user, ref, buffer.status == MTLCommandBufferStatusCompleted)`; then `sg_commit()`. `Error` prints the buffer's `error` to stderr and reports `ok=false`, which the shared caller already treats as terminal. A frame that began no pass has no buffer (`_sg_mtl_commit` commits only `if (_sg.mtl.cmd_buffer)`): commit, complete inline with `ok=true`, trace it -- the same thing GL does for such a frame, whose fence signals at once. |
| `setConsumerCompletion` | Stores the sink. | Identical. |
| `wrapForPresenting` (platform) | `sgp_make_image` with `gl_textures[0]`, a texture view. | `sgp_make_image` with `mtl_textures[0]`, a texture view. One texture, not `SG_NUM_INFLIGHT_FRAMES`: the image is immutable, so it has one slot (spike 2). |
| `acquireFrameSwapchain` (platform) | `sglue_swapchain()` once. | The same, plus one guard: valid only if `!swapchain.invalid && swapchain.metal.current_drawable != nullptr`. `_sapp_mtl_swapchain_next` only `SOKOL_ASSERT`s the drawable, so a Release build would otherwise carry nil into `sg_begin_pass`. False before the presenter is set up, as GL's is. |
| `presentClear` | Clear pass, commit. | Identical through `sgp_*`. No completion handler: nothing holds a reference. |
| `blit` (platform) | Blit pass, commit, consumer fence. | Blit pass; consumer handler on the presenting instance's frame command buffer, installed before `sgp_commit()`, calling the consumer sink with the buffer's status; with test hooks on, a copy of the drawable into a staging buffer is encoded into the same command buffer first. A blit that could not be drawn calls the sink inline with `ok=true`, as GL does. |
| `releaseWrappers` | `sgp_destroy_*`. | Identical. The deferred release is collected by the presenter's ordinary frames (see Maintenance). |
| `presentInvalid` | Empty. | Submits nothing and commits nothing. Traces each transition into and out of the invalid state once. No read reference exists to release: the shared `onFrame` takes one only on the `blit` path. |
| `pollCompletions` | Answers the probe; polls fences. | Empty, and that is the whole body: completion arrives on handlers Metal runs. |
| `readPresentedPixels` | `glReadPixels`, flipped. | `waitUntilCompleted` on the blit's command buffer (kept strongly under test hooks), then copy the staging buffer out, converting BGRA to RGBA. Rows are already top-first. |
| `workerLoopTurn` (worker, **new in 16a**) | Empty. | Drain and replace the worker's autorelease pool; run maintenance if due. |

**GL machinery with no Metal equivalent, and what the tests do.**

- *Context routes.* Replaced by the refusal above. `test/ui-route-refusal.js`
  and `test/ui-exit-tty.js` keep their RUN lines and reach "fails before
  `sapp_run()`" through the same variable; the message differs, so the
  message checks are per backend.
- *Polling versus blocking, and the startup probe.* Replaced by one
  handler. `test/ui-present.js`'s three runs exist to cover three GL
  arms. On Metal the two forced-mode runs cannot set the variable (it is
  refused), so their RUN lines take it from a substitution that is empty on
  Metal, and their completion assertions are per backend.
- *Backend-aware tests, the mechanism.* 16a adds a `%ui-backend`
  substitution (`gl`, `metal` or `none`, read from the binary) and rewrites
  the affected RUN lines to `--check-prefixes=X,X-%ui-backend`. Lines every
  backend must print stay under `X`; GL-only lines move to `X-gl`; the Mac
  agent adds `X-metal`. One test file serves both backends, and a line
  nobody ported is visibly absent rather than silently skipped.

**Autorelease pools (spec: "All worker-thread Metal work runs inside
`@autoreleasepool`, not only frames").** Two layers:

1. Every backend method that runs on the worker wraps its body in
   `@autoreleasepool {}`.
2. Addon code runs on the worker outside any backend method -- frame
   callbacks, and `sg_make_*` at addon init -- so the worker also gets a
   pool per event-loop turn. `makeCurrentOnWorker` pushes one
   (`objc_autoreleasePoolPush`); the shared code starts an unreferenced
   `uv_check_t` on the JS loop whose callback calls
   `gBackend->workerLoopTurn()`, which pops it and pushes the next. The
   check phase follows the poll phase, so the objects a tick autoreleased
   are drained in the turn that created them.

The check callback returns without calling the backend when
`uiClosing()` or when a frame callback is on the stack
(`gWindow->inFrameCallback`): `process.exit()` inside a frame callback
reaches `flushPendingWrites`, which runs a nested `uv_run`, and a pool must
not be popped -- nor a maintenance commit issued -- under an open pass.

UNVERIFIED: `objc_autoreleasePoolPush` / `objc_autoreleasePoolPop` may be
called from ARC code through hand-written `extern "C"` declarations (probe
P2).

**Maintenance (spec: "Resource churn needs admission control").**

- *Producer.* `destroyGenerationSurfaces` sets `maintenancePending_`.
  `workerLoopTurn` runs maintenance when it is pending and
  `producerInFlight_ == 0` -- a counter the backend increments in
  `submitAndTrackCompletion` and decrements at the end of the completion
  handler. The loop-turn guard above supplies "no open pass"; the counter
  supplies "no producer GPU work outstanding" and "no completion handler
  pending". Maintenance is `sg_commit()` repeated until the accessor
  reports no pending release, at most eight times. From an idle instance
  that is **four** commits, not three: `_sg_mtl_commit` collects with the
  frame index *before* `sg_commit` increments it, and a release made at
  index F is due at F+3. The safe point matters because Sokol's command
  buffers hold unretained references: an empty commit takes no semaphore,
  so advancing the clock under an in-flight frame would free a resource an
  addon destroyed in that frame while the GPU still reads it.
- *Presenter.* **Nothing is built**, and this departs from the spec, which
  specifies suspending blits so that empty maintenance commits can run on
  the presenting instance. The presenter needs no empty commits: it commits
  a real pass every valid frame, each `sg_begin_pass` waits on Sokol's
  semaphore, and so its release clock advances under exactly the
  back-pressure Sokol was designed for. Wrappers are released only on the
  `wrappersReleasable` trigger, by which time every blit that sampled them
  has completed. While the swapchain is invalid no wrapper is created
  (`onFrame` returns before `takeFrame`), so nothing accumulates either.
  Proposed Ruling 81; the spec is corrected in 16f.
- *"Does not begin a further generation change until the previous one's
  releases have been collected."* Not enforced as a gate, and this also
  departs from the spec: holding back a resize until a producer frame
  completes would need a deferred-generation state in shared code. What is
  enforced is the bound the rule exists for. A generation's destruction
  queues at most nine releases against a pool of 2,688 (`_sg_mtl_init_pool`
  with default pool sizes), every loop turn with nothing in flight drains
  them, and `createGenerationSurfaces` refuses -- an error, which the
  shared code already turns into a terminated session with a message --
  when the pool's free count is below 256. That replaces a Sokol assert
  (compiled out in Release) with a sentence. Proposed Ruling 82.

### (b) The command-buffer accessor: no patch to vendored Sokol

**Decision: the accessor is defined in our own two implementation
translation units, after the include, and `external/sokol/sokol` stays
untouched.** The spec says the buffer "is reachable only through a new
accessor" and concludes a patch is required. The first half is true; the
conclusion does not follow. `_sg` is `static _sg_state_t _sg` in whichever
unit defines `SOKOL_GFX_IMPL`, and `sokol_gfx_impl.cpp` and
`sokol_gfx_present_impl.cpp` are those units -- code written after their
`#include <sokol_gfx.h>` sees `_sg` directly. (Checked on Linux with the
dummy backend: a function returning `_sg.frame_index` appended that way
compiles.)

What is exposed, per instance (`lib/compositor-gpu/sokol_gfx_ext.inc`,
included by both units with `HNUI_SG_EXT(name)` defined to paste
`hnui_sg_` or `hnui_sgp_`):

```cpp
#if defined(SOKOL_METAL)
extern "C" {
/// Sokol's frame command buffer (__bridge, not retained): non-null only
/// between the frame's first sg_begin_pass and sg_commit.
const void *HNUI_SG_EXT(mtl_command_buffer)(void);   // _sg.mtl.cmd_buffer
/// Free slots in the Metal id-pool.
int HNUI_SG_EXT(mtl_idpool_free)(void);              // _sg.mtl.idpool.free_queue_top
/// Releases queued and not yet collected.
int HNUI_SG_EXT(mtl_idpool_pending)(void);           // (release_queue_front - release_queue_back) mod num_slots
}
#endif
```

Why this over the alternatives the spec anticipated:

- *A patch file applied at build time* keeps the vendored digests but adds
  a build step, a second `sokol_gfx.h` on the include path, and a patched
  header the rename generator and every addon must agree on.
- *A modified vendored copy with a recorded diff* breaks the
  `external/$lib/$lib` convention for six lines.
- *This* changes no vendored byte, needs no mechanism, and fails the same
  loud way a patch would on a Sokol bump that renames a field: it stops
  compiling.

Consequences: `test/sokol-pin.js` and the README's digest list stay true as
they are. **The patch-carrying mechanism stays unsettled**, which is the
honest state -- the two contingent patches remain contingent, and the first
one that is actually needed settles it. 16f corrects the spec ("Surfaces",
"What Sokol gives us", "Build and integration") and
`external/sokol/README.md`'s sentence that a patch is required. The
`sgp_rename.h` count stays 156 and `test/sokol-rename.js` is unchanged.
Proposed Ruling 80.

Isolation is extended rather than assumed: `utils/check-sgp-isolation.py`
learns that the producing object defines `hnui_sg_*` and no `hnui_sgp_*`
(and the reverse), and gains `--producing-user` / `--presenting-user`
arguments naming the backend objects that *call* each instance; a user
object with an undefined reference to the other family is an error. On
Linux those are `gl_backend.cpp.o` and `gl_present.cpp.o`; on macOS
`metal_backend.mm.o` and `metal_present.mm.o`. This is the check that
catches the presenter installing its handler on the producer's buffer.

**Fallback, only if the Mac shows the appended definition cannot work**
(no reason is known): carry the same three functions as a modified vendored
`sokol_gfx.h`, with `external/sokol/patches/0001-metal-ext.patch`, a second
digest column in the README ("upstream" and "as patched"), and
`test/sokol-pin.js` extended to check that `patch -R` of the committed file
reproduces the upstream digest. Do not build this unless needed.

### (c) Threading on macOS

- **`sapp_run()` is `[NSApp run]` and never returns.** `main()` waits on
  the condition variable first, which spike 5 measured as harmless, and
  the JS thread's `_exit()` under a running `[NSApp run]` terminates
  cleanly (spike 5). Nothing changes in `UiHost`.
- **The main thread's pools are Sokol's**: `_sapp_macos_frame` wraps
  `_sapp_frame` in `@autoreleasepool`, and AppKit drains one per event.
  Our platform-thread code adds none, except around the one-time work in
  `uiPlatformBeforeRun()`, which runs before any run loop.
- **The JS thread's pools are ours**, as in (a).
- **Completion handlers run on a Metal-owned thread.** They call
  `onProducerComplete` / `onConsumerComplete`, which the shared code
  already documents as "any thread": the surface table locks, and
  generation destruction is posted to the owner. They never call `sg_*`
  or `sgp_*`.
- **Invalid swapchain.** `sapp_acquire_swapchain` reports `invalid` while
  Sokol's obscured timer is running, which it starts on
  `windowDidMiniaturize:` and whenever `occlusionState` loses
  `NSWindowOcclusionStateVisible` -- a covered window, a locked screen, a
  sleeping display. Frames keep arriving (from the `NSTimer`), and each
  returns from `onFrame` after `presentInvalid()`: no take, no blit, no
  tick. **So `requestAnimationFrame` callbacks stop while the window
  cannot be seen**, which is broader than the spec's "no ticks while the
  window is minimized" and matches what a browser does for a hidden tab. A
  frame whose GPU work was in flight still completes and publishes; it is
  taken when the window is visible again. 16f records this in the spec.
- **Resize.** `windowDidResize:` runs `_sapp_macos_update_dimensions` on
  the main thread: it sets `drawableSize`, recreates Sokol's depth and
  MSAA textures, and raises `RESIZED` synchronously. Our `event_cb`
  forwards it; the JS thread starts the new generation before that
  event's listeners run (existing code, `uiFollowFramebufferSize`). Until
  a frame of the new generation is published the presenter stretches the
  old one with linear filtering. No Metal-specific code is involved
  beyond `createGenerationSurfaces`.
- **Retina.** With `highDpi: false` (the default, and what every test
  passes) Sokol sets `dpi_scale = 1`, so framebuffer size equals the
  window's size in points on any display, and `CAMetalLayer.contentsScale`
  is 1 -- the system magnifies with `kCAFilterNearest`. **The exact-pixel
  test is therefore unaffected by the display it runs on**: target size
  equals framebuffer size equals the requested size, and the read-back is
  of the drawable, before any magnification. With `highDpi: true`,
  `framebufferWidth == round(width * dpiScale)` and `dpiScale` is the
  screen's `backingScaleFactor`; one test line asserts that relation
  without assuming the factor is 2. A requested window larger than the
  screen is shrunk by AppKit; no test asks for more than 640x480.
- **Vertical orientation.** A Metal texture's row 0 is its top row, and
  clip-space y points up, so the blit's fragment coordinate is
  `(p.x, 1 - p.y)` where GL's is `p`. `test/ui-present.js`'s quadrant
  pattern is asymmetric top to bottom, so a wrong flip is a red test, not a
  convention.

### (d) Closing on macOS

**The window's close button** needs no new code. `windowShouldClose:` in
`_sapp_macos_window_delegate` sets `quit_requested`, raises
`QUIT_REQUESTED` synchronously, and returns `NO` when the handler cleared
the flag; `uiHandlePlatformEvent` calls `sapp_cancel_quit()` first thing.
`_sapp_macos_frame`'s `performClose:` runs only if a flag is still set. So
the close is cancelled and forwarded exactly as on X11 (read, not run).

**Termination is intercepted by making `NSApp` an instance of our own
subclass**, created before `sapp_run()`:

```objc
@interface HnuiApplication : NSApplication @end
@implementation HnuiApplication
- (void)terminate:(id)sender;   // never calls super
@end
```

`uiPlatformBeforeRun()` calls `[HnuiApplication sharedApplication]`;
Sokol's later `[NSApplication sharedApplication]` returns that instance.
`terminate:` turns the request into the cancelable close: if the window
exists it calls `[window performClose:sender]`, which runs Sokol's
`windowShouldClose:` and so the path above; before the window exists it
hands a synthesized `QUIT_REQUESTED` to `uiHandlePlatformEvent` directly.
It never calls `super`, so AppKit never asks the delegate, never runs
`applicationWillTerminate:`, never calls `cleanup_cb`, and never exits 0
from the main thread. The three things Task 15 established hold again:
every quit request is cancelled, `cleanup_cb` never runs, the JS thread
ends the process with its status.

Why this and not the alternatives:

- *Adding `applicationShouldTerminate:` to Sokol's delegate* (a category,
  or `class_addMethod`) works for Dock Quit but must answer
  `NSTerminateCancel`, and that answer **cancels a logout or shutdown**
  outright: the system shows "the application interrupted logout" even
  when the program was about to exit. Overriding `terminate:` neither
  cancels nor confirms. If no listener cancels, the JS thread `_exit`s
  within a loop turn, the process is gone, and the logout continues.
- *Patching Sokol* is what (b) just avoided.
- *Swizzling* is a subclass with worse error messages.

Chromium overrides `-[NSApplication terminate:]` for the same reason (its
`BrowserCrApplication`), so the override is an established use.

UNVERIFIED: that `[NSApplication sharedApplication]` after
`[HnuiApplication sharedApplication]` returns the subclass instance, that
Dock Quit and the quit Apple event reach `terminate:`, and that
`performClose:` from inside it reaches the delegate (probes P1, P4).

**What remains impossible, and is documented rather than fixed** (16f:
CLAUDE.md, the spec's Lifecycle, `hnui.h`'s Closing section):

- A `close` listener that cancels, or a JS thread busy in synchronous
  code, leaves a logout or shutdown waiting; the system gives up after its
  own timeout and reports the application. A request carries no reason,
  so a program cannot tell logout from Quit.
- Force Quit and `SIGKILL` run no `'exit'` listener, as everywhere.
- A termination request that arrives before the window is ready is queued
  like any early event and delivered once it is; a window that never
  becomes ready never delivers it.
- Sokol creates no main menu, so Cmd-Q does nothing in a
  `hermes-node-ui` program. Tracker issue in 16f, not fixed here.

### (e) Input and services on macOS

- **Events.** `sapp_event_convert.cpp` converts every `sapp_event` type
  the macOS backend raises and has no platform conditional; `SUPER` maps
  to `HNUI_MOD_META`. Nothing changes. Cmd+V raises `CLIPBOARD_PASTED`
  (`_sapp_macos_view keyDown:`), and the converter reads the text through
  the same seam as `readClipboard()`.
- **The platform seam.** 16a replaces `ui_platform.cpp`'s three
  `#ifdef HERMES_NODE_UI_HAVE_X11` blocks with functions declared in
  `tools/hermes-node-ui/platform_services.h`, implemented by
  `platform_services_x11.cpp` (the existing code, moved) and, in 16e,
  `platform_services_mac.mm`. Linux's traces stay byte-identical.
- **Clipboard read.** `[NSPasteboard pasteboardWithName:]` on the
  platform thread, behind `ClipboardResult`: no `NSPasteboardTypeString`
  among `types` is `kUnavailable`; `stringForType:` returning nil is
  `kError`; a UTF-8 length above the limit is `kTooLarge`; otherwise
  `kOk`. `kTimeout` is never produced by the read itself -- AppKit has no
  deadline to offer -- so it arises only from the request's own deadline
  having passed (`applyReadClipboard`'s existing skip). No path returns
  text from an earlier read, because nothing is cached.
- **Clipboard write** is ours too on macOS (`clearContents`,
  `setString:forType:`), not `sapp_set_clipboard_string`, for one reason:
  the pasteboard's name must be ours to choose. It is the general
  pasteboard unless `HERMES_NODE_UI_TEST_PASTEBOARD` names another, which
  the test wrapper sets to a per-run unique name so that the suite never
  overwrites the clipboard of whoever is using the Mac and parallel suite
  runs do not share one. Proposed Ruling 87.
- **Title read-back** is `[(NSWindow *)sapp_macos_get_window() title]`,
  traced in the same format as X11's (`command title: readback='...'`).
- **Icon read-back**: Sokol sets `NSApp.dockTile.contentView` to an
  `NSImageView`; the trace reads that view's image back and prints its
  first representation's pixel size (`command icon: dock tile 16x16`).
  UNVERIFIED: that the representation reports the source image's pixel
  size rather than the tile's (settled in 16e Step 3 by running it).
- **Clipboard-write read-back**: the string read back from the pasteboard
  (`command clipboard: pasteboard_bytes=<n> bytes=<n>`).
- **Cursor shape, visibility, pointer lock**: Sokol's calls, and the
  traces already print Sokol's own read-back (`sapp_get_mouse_cursor`,
  `sapp_mouse_shown`, `sapp_mouse_locked`), so they are identical on both
  backends. On macOS `sapp_show_mouse(false)` and `sapp_lock_mouse(true)`
  act on the real pointer of the logged-in session until undone or until
  the process exits; the tests that issue them undo them in the same
  frame, as they already do.
- **Fullscreen** is `[window toggleFullScreen:]`, an animated transition
  into a new Space. `test/ui-commands.js` toggles twice in one frame,
  which on X11 is two property changes. UNVERIFIED and deliberately not
  measured: what AppKit does with a second toggle during the first's
  animation. The test skips that pair on Metal and a tracker issue
  records fullscreen on macOS as untested (16e, 16f).

### (f) Testing on macOS without Xvfb

**Lit features after this task.**

| Feature | Decided by | Forcing function |
|---|---|---|
| `ui-gpu` | Platform predicate mirroring `lib/compositor-gpu/CMakeLists.txt`'s guard, which becomes `if(NOT UNIX)`: `os.name == 'posix'`. Flipped by 16b in the same commit as the guard. | `test/ui-backend-feature.js` (new): the binary's `gpu backend:` line must equal what the platform is expected to build. |
| `ui-backend-gl`, `ui-backend-metal`, `%ui-backend` | Running `hermes-node-ui --version` and reading a new third line, `gpu backend: gl|metal|none`. | The same test: it compares the binary's line, lit's `%ui-backend` and `%ui-backend-expected`. |
| `display` | Linux: starting Xvfb through the tests' own wrapper (unchanged in substance). macOS: **running the binary** on `test/fixtures/hnui-demo/display_probe.js` through the same wrapper and requiring the trace `present: drawable acquired` -- a window opened, was visible, and yielded a drawable. A locked screen, a sleeping display, no window server and no Metal device all make it false. | `test/ui-display-feature.js`, re-gated from `ui-gpu, ci` to `ui-gpu, display-expected`. |
| `display-expected` (new) | True on Linux CI (`ci` and not darwin), and wherever the ambient environment has `HERMES_NODE_UI_EXPECT_DISPLAY=1`. | It is itself the narrowing; the Mac agent's commands always set the variable, so a probe regression on the Mac is red there. |
| `sokol-validation`, `ci`, `ui`, `ui-enabled` | Unchanged. | Unchanged. |

**The wrapper.** `%with-xvfb` and the bare `timeout` in RUN lines become
`%with-display <seconds>` (`utils/with-display.py`) and `%timeout`:

- On Linux `with-display.py N CMD...` `exec`s `timeout N <python>
  with-xvfb.py CMD...` -- the identical process tree to today's
  `timeout N %with-xvfb CMD...`.
- On macOS it (1) takes an exclusive `flock` on
  `$TMPDIR/hermes-node-ui-display.lock`, **outside** the timeout, so that
  window tests run one at a time machine-wide however many lit workers
  there are; (2) sets `HERMES_NODE_UI_TEST_NO_ACTIVATE=1` and a unique
  `HERMES_NODE_UI_TEST_PASTEBOARD`; (3) runs the command in its own
  process group with the timeout, exits 124 after killing the group on
  expiry, otherwise the command's status.
- `%timeout` is `timeout` on Linux and `<python> utils/timeout.py` on
  macOS, on every release, so the suite does not depend on which macOS
  ships `/usr/bin/timeout`. It accepts `--foreground`.

Serialising was designed as not optional, on this reasoning: Sokol
centres every window, so two tests' windows of the same size cover each
other exactly; the covered one loses `NSWindowOcclusionStateVisible`, its
swapchain goes invalid, and its capture never matches. **Measured false
for a window covered from the moment it opens** (see "Task 16b as
built"): it keeps a valid swapchain indefinitely. What remains true is a
window covered *after* it opened, which is what a second test's window
appearing does to the first's, so the lock stays.

**Not stealing focus.** Sokol calls `[NSApp activateIgnoringOtherApps:YES]`
and sets the Regular activation policy on every launch -- roughly sixty
launches per suite run, each of which would take the keyboard from
whoever is at the Mac and bounce a Dock icon. Under
`HERMES_NODE_UI_TEST_NO_ACTIVATE`, `HnuiApplication` refuses
`activateIgnoringOtherApps:`, forces
`NSApplicationActivationPolicyAccessory` in `setActivationPolicy:`, and
`uiPlatformAfterInit()` calls `[window orderFrontRegardless]` so the
window is on screen without the application being active. **No test
asserts focus**: `test/ui-conformance.js` already accepts either value,
and focus events in the suite are injected. UNVERIFIED: that such a window
stays visible to the occlusion machinery and still routes a synthesized
key event (probe P1). If it does not, the fallback is to drop the
no-activate mode and accept focus stealing on the test machine; nothing
else in this plan depends on it.

**Display asleep or locked.** The `display` probe is false, every window
test reports UNSUPPORTED, and `test/ui-display-feature.js` is red wherever
a display was promised. A display that sleeps mid-run fails the tests in
progress at their timeout, which is a true report of the environment.

**Real events in place of `x11_window_driver.c`.** A second process cannot
drive another's window on macOS without a permission the session may not
have: `CGEventPost` and the Accessibility API need Accessibility access,
and AppleScript through System Events needs Automation consent, which
prompts. None is assumed and none is used. Instead the binary gains, under
`HERMES_NODE_UI_TEST_HOOKS` and only on macOS, `ui.__driveWindow(command)`,
which runs one AppKit call on the main thread and returns what it read
back. Where each technique enters, and so what it proves:

| Command | Call | Enters at | Proves | Does not prove |
|---|---|---|---|---|
| `close` | `[window performClose:nil]` | `NSWindow`, above Sokol's delegate | `windowShouldClose:`, the `quit_requested` logic, `sapp_cancel_quit()` keeping the window, `event_cb` being installed | hit-testing of the title-bar button; delivery from the window server |
| `key <c>` | `[window sendEvent:]` with a synthesized `NSEventTypeKeyDown` / `KeyUp` | `NSWindow`'s dispatch to its first responder | Sokol's `keyDown:`, the key-code table, modifier and `CHAR` generation, `event_cb` | key-window routing, the window server, Sokol's Cmd-key-up monitor |
| `resize <w> <h>` | `[window setContentSize:]` | a real window resize | `windowDidResize:`, `drawableSize`, the swapchain resize, `RESIZED`, a new generation of the real framebuffer | the live-resize tracking loop a user's drag runs in |
| `miniaturize`, `deminiaturize` | `[window miniaturize:nil]` | a real miniaturize | `ICONIFIED` / `RESTORED`, the obscured timer, `swapchain.invalid`, `presentInvalid` | occlusion by another window |
| `terminate` | `[NSApp terminate:nil]` | `NSApplication`, where Dock Quit arrives after AppKit handles the quit event | `HnuiApplication`'s override and everything below it | the Apple-event handler in front of it |
| `state` | reads | -- | -- | -- |

`__injectEvent` enters **below** Sokol, at `uiHandlePlatformEvent`, and
proves nothing above the converter; that is unchanged. Every `__driveWindow`
row enters above Sokol's own classes, which is the property
`test/ui-real-events.js` exists for. What no row reaches is the path from
the window server into AppKit, which is Apple's code on both sides.

One out-of-process route may exist for termination alone.
`NSRunningApplication -terminate` from a second process is how the Dock
asks an application to quit. UNVERIFIED: whether it needs no consent and
reaches `terminate:` (probe P4). If it does, `test/ui-terminate.js` uses
`test/fixtures/hnui-demo/mac_quit_sender.m` as a real second-process
request; if not, that test uses `__driveWindow('terminate')` alone and
says so.

`test/fixtures/hnui-demo/window_driver.js` (16a) is the one place tests
reach a driver: the X11 child on Linux, `ui.__driveWindow` on macOS, the
same `send()` / `onLost()` / `closeOnce()` interface.

**Frame capture and the exact-pixel test.** `CAMetalLayer.framebufferOnly`
is true as Sokol creates the layer, and such a drawable cannot be read.
With test hooks on, `createSharedContext` sets it to NO on the layer
reached through `sapp_macos_get_window()`. Every blit then encodes, into
Sokol's own frame command buffer after the swapchain pass, a copy of the
drawable's texture into a shared-storage `MTLBuffer`; `readPresentedPixels`
waits for that buffer and copies it out. So the bytes compared are the
bytes rendered into the drawable that was presented, read in the same
command buffer that presented it. The comparison stays exact (0 or 255):
the blit is 1:1, the layer's format is `BGRA8Unorm` (not sRGB; the
colour space tag affects compositing, not the texture), and the presenter's
clear stays 32/255. UNVERIFIED: that `framebufferOnly` can be cleared after
Sokol created the layer and before the first `nextDrawable`, and that a
blit encoder may read a drawable's texture after `presentDrawable:` was
registered on the same command buffer (probe P3).

**The 28 UI tests that are UNSUPPORTED on the Mac today** (the 29th is
`bundle-yargs.js`):

- *Run unchanged apart from the RUN-line rewrite (18):* `sokol-isolation`,
  `sokol-rename`, `ui-create-window`, `ui-demo-visual`, `ui-early-events`,
  `ui-event-exceptions`, `ui-events`, `ui-exit-late-require`,
  `ui-exit-paths`, `ui-exit-tty`, `ui-null-window`, `ui-raf`,
  `ui-sokol-panic`, `ui-sokol-validation`, `ui-sokol-validation-feature`,
  `ui-version-check`, `ui-wrong-thread`, `ui-display-feature` (with the
  variable set).
- *Run with `-metal` check lines (7):* `ui-present` (completion traces),
  `ui-resize-generation` (the forced-poll variable), `ui-route-refusal`
  (the message), `ui-abi-sizes`, `ui-commands` and `ui-conformance`
  (clipboard and icon read-back; fullscreen skipped; the clipboard timeout
  reached by a zero deadline instead of a mute owner), `ui-close`
  (through `window_driver.js`).
- *Run through the in-process driver (1):* `ui-real-events` -- KEY, GEN
  and CLOSE. Its DEMO run, which starts the demo with hooks off and closes
  it from a second process, moves to `test/ui-real-events-demo.js`, gated
  `ui-backend-gl`: nothing on macOS can close a hooks-off program's window
  from outside.
- *Linux only, with a macOS counterpart (1):* `ui-clipboard` is about the
  X11 selection protocol (a mute owner, a late reply, INCR). It is gated
  `ui-backend-gl`; `test/ui-clipboard-mac.js` covers what the pasteboard
  can do.
- *CI only (1):* `ui-sokol-validation-ci`.

The five tests named in the Task 16 paragraph, decided:
`ui-null-window`, `ui-version-check` and `ui-exit-late-require` open no
window, touch no AppKit and need no device; they run on macOS, CI
included, with `%timeout`. `ui-exit-tty` and `ui-route-refusal` keep
`HERMES_NODE_UI_GL_ROUTE=bogus`, which the Metal backend refuses before
`sapp_run()`; `ui-route-refusal`'s three message checks become per backend.

`test/ui-no-window-support.js` and `ui_module_nowindow.cpp` describe a
configuration no supported platform has once this lands. 16f deletes both
and makes the option on an unsupported platform a configure error.

New tests: `ui-backend-feature` (both), `ui-real-events-demo` (GL),
`ui-metal-clear`, `ui-metal-maintenance`, `ui-metal-occlusion`,
`ui-terminate`, `ui-clipboard-mac` (Metal).

### (g) CI

`ci.yml`'s `asan-debug-o1` entry already builds macOS with the option on.
After 16b that job compiles the Metal backend and `ui-gpu` is true there.
Nobody knows whether GitHub's macOS runners have a Metal device or a
window-server session, and the job is safe either way because nothing
assumes:

- The nine tests gated on `ui-gpu` alone, and `ui-sokol-validation-ci.js`
  with them in the Debug job, open no window and create no device (the
  refusal precedes `MTLCreateSystemDefaultDevice`, which precedes any
  AppKit call), so they pass with neither.
- Every window test is gated on `display`, which is the probe. No device,
  no session, a locked or absent display: false, UNSUPPORTED, green.
- `display-expected` is false on macOS CI, so `ui-display-feature.js` is
  UNSUPPORTED there rather than red.
- The probe is bounded (60 s inside the wrapper, 90 s around it) and the
  wrapper kills the process group, so a binary that hangs on a runner
  with no window server costs a minute once per lit configuration, not a
  stuck job.

**How the first PR run tells us.** 16f adds one informational step to the
macOS job, `continue-on-error: true`, after the build: it runs the probe
script by hand with `HERMES_NODE_DEBUG_NATIVE=UI` and prints the exit
status and stderr. Three outcomes are distinguishable in the log: `route:
metal (device=...)` then `present: drawable acquired` (a display: window
tests ran); the "no Metal device" refusal; or a device with no drawable
(no usable session). If the first, a follow-up commit sets
`HERMES_NODE_UI_EXPECT_DISPLAY: 1` in the macOS job's environment so the
canary guards it from then on; otherwise the outcome goes into a tracker
issue and the job stays as it is.

### (h) Out of scope

Stays for Task 17 or later, and is not to be solved inside a task here:

- Intel Macs and their GPUs (Sokol selects managed storage there; nothing
  in this design depends on shared storage, but nothing was measured), and
  the x86_64 slice under Rosetta, which another round covers.
- Real input from the window server: `CGEvent`, Accessibility, System
  Events. The live-resize tracking loop. A window dragged between displays
  of different scale (`windowDidChangeScreen:`).
- Fullscreen on macOS; a main menu; `NSTerminateLater`-style logout
  handling.
- sRGB and HDR swapchains.
- Task 17's list as it stands: the stderr-check sweep, the late-rejection
  `hnui_get_window()` test, the unenforced two-injected-events assumption
  (16f measures it on the Mac but does not change the injector).
- `--build-exe` for UI programs, `--inspect` in UI mode, idle sleep.

### Rulings proposed (the driver confirms or overrides before 16a)

- **80** -- no Sokol patch; the accessor lives in our implementation units
  (b).
- **81** -- no presenter-side maintenance suspension on Metal (a).
- **82** -- a generation change is not held back for maintenance; the pool
  is guarded instead (a).
- **83** -- the Metal backend refuses the GL environment variables by name
  (a).
- **84** -- termination is intercepted by an `NSApplication` subclass
  overriding `terminate:` (d).
- **85** -- real-event tests on macOS use an in-process driver above
  Sokol's classes; nothing needing Accessibility or Automation consent is
  used (f).
- **86** -- `backend.h` gains `workerLoopTurn()`; `createGlBackend()`
  becomes `createCompositorBackend()` (a).
- **87** -- on macOS both clipboard directions are ours, and tests use a
  private named pasteboard (e).
- **88** -- `requestAnimationFrame` stops while the window is occluded,
  not only while minimized (c).

---

## Review focus

Five failure modes most likely to bite that no test covers, each assigned:

1. **A completion handler on the wrong buffer, or installed too late.**
   The presenter reads the producer's accessor (the handler then fires for
   the wrong frame and a read reference is released while its blit is in
   flight), or a handler is added after `sg_commit()` (Metal raises), or
   the `Error` status path -- which no test can provoke -- reports `ok`.
   -- 16c: the isolation check's user-object rule catches the first; the
   second and third are read line by line, and the run under
   `MTL_DEBUG_LAYER=1` is the evidence for the second.
2. **A maintenance commit that outruns in-flight GPU work.** Command
   buffers hold unretained references, so an early release is a
   use-after-free on the GPU that no assertion sees and that shows as a
   wrong pixel once in a thousand runs, if at all. -- 16d: the safe-point
   predicate is three conditions written in one place; verification runs
   `test/ui-metal-maintenance.js` under API and GPU validation with an
   addon destroying a buffer in the frame before a resize.
3. **The per-turn autorelease pool popped under a live frame.** A nested
   `uv_run` (`flushPendingWrites`, reached by `process.exit()` inside a
   frame callback) runs the check callback while a pass is open. -- 16c:
   the guard is in shared code and has a Linux trace; the Mac run of
   `test/ui-exit-paths.js` FRAME under `NSZombieEnabled=YES` and
   `OBJC_DEBUG_MISSING_POOLS=YES` is the evidence.
4. **An AppKit call from the JS thread.** `__driveWindow`, the pasteboard
   and `sapp_macos_get_window()` are all main-thread only, and a wrong
   thread usually works. -- 16d and 16e: every new `.mm` entry point
   states its thread in its header, and both tasks' verification runs
   under the Main Thread Checker.
5. **A nil drawable.** `nextDrawable` returns nil after a one-second
   timeout when the layer has no drawable to give -- a display that goes
   to sleep between the occlusion notification and the next frame is the
   likely way -- and in a Debug build Sokol asserts on it before our guard
   can run. -- 16d: the Release-build guard is ours and is read; the Debug
   abort is recorded as a tracker issue with what was observed, not fixed.

---

## Task 16a: seams, features and wrappers (no Metal code)

**Who:** Linux. **Mac verifies** that the tree still builds and the suite
is where it was.

Everything here is provable on Linux: the GL suite passes before and after
with the same traces. Nothing in it enables a backend on macOS.

**Files:**
- Modify: `lib/compositor-gpu/backend.h`, `gl_backend.cpp`,
  `gl_present.cpp`, `sokol_config.h`, `sokol_gfx_impl.cpp`,
  `sokol_gfx_present_impl.cpp`, `CMakeLists.txt` (non-Apple parts only);
  `tools/hermes-node-ui/ui_platform.cpp`, `ui_module.cpp`, `ui_internal.h`,
  `main.cpp`, `CMakeLists.txt`; `CMakeLists.txt` (addon backend define);
  `utils/check-sgp-isolation.py`, `utils/format.sh`; `test/lit.cfg`; every
  `test/ui-*.js` with a `timeout` or `%with-xvfb` RUN line;
  `test/sokol-isolation.js`; `test/ui-display-feature.js`;
  `test/ui-real-events.js`, `test/ui-close.js`,
  `test/fixtures/hnui-demo/demo.js`.
- Create: `lib/compositor-gpu/present_constants.h`,
  `lib/compositor-gpu/sokol_gfx_ext.inc`, `lib/compositor-gpu/sokol_gfx_ext.h`;
  `tools/hermes-node-ui/platform_services.h`,
  `tools/hermes-node-ui/platform_services_x11.cpp`;
  `utils/with-display.py`, `utils/timeout.py`, `utils/lit-run.sh`;
  `test/fixtures/hnui-demo/window_driver.js`,
  `test/fixtures/hnui-demo/display_probe.js`;
  `test/ui-backend-feature.js`, `test/ui-real-events-demo.js`.

**Interfaces produced:**

```cpp
// backend.h
/// Worker thread, once per event-loop turn, with no frame callback on the
/// stack and not once the window is CLOSING. GL: nothing. Metal: drain
/// and replace the worker's autorelease pool, then run maintenance if due.
virtual void workerLoopTurn() = 0;

/// Replaces createGlBackend(). Null, having printed why, if none.
CompositorBackend *createCompositorBackend();
/// "gl" or "metal": the backend this binary was built with.
const char *compositorBackendName();

// present_constants.h
constexpr float kPresenterClearLevel = 32.0f / 255.0f;   // moved from gl_present.cpp

// sokol_gfx_ext.h -- declarations for both families, no Sokol types
extern "C" {
const void *hnui_sg_mtl_command_buffer(void);   const void *hnui_sgp_mtl_command_buffer(void);
int hnui_sg_mtl_idpool_free(void);              int hnui_sgp_mtl_idpool_free(void);
int hnui_sg_mtl_idpool_pending(void);           int hnui_sgp_mtl_idpool_pending(void);
}

// platform_services.h -- platform thread only, every function
ClipboardResult uiPlatformReadClipboard(int timeoutMs, size_t maxBytes);
void uiPlatformWriteClipboard(const std::string &utf8);
/// The text after "command clipboard: ", read back after a write.
std::string uiPlatformDescribeClipboardWrite(size_t bytesWritten);
/// False if the platform cannot read the title back.
bool uiPlatformWindowTitle(std::string &out);
/// The text after "command icon: ", read back after sapp_set_icon().
std::string uiPlatformDescribeIcon();
/// Before sapp_run(), after selectContextRoute() succeeded. X11: nothing.
void uiPlatformBeforeRun();
/// In init_cb, after createSharedContext() succeeded. X11: nothing.
void uiPlatformAfterInit();
```

`sokol_gfx_ext.inc` is written here with its body under `#if
defined(SOKOL_METAL)`, so on Linux it compiles to nothing; both
implementation units define `HNUI_SG_EXT` and include it after
`<sokol_gfx.h>`. `sokol_config.h` gains `#if defined(SOKOL_METAL) &&
!defined(__OBJC__)` / `#error` naming the CMake property to set.

`hermes-node-ui --version` prints a third line in every build:
`gpu backend: <gl|metal|none>` (`none` from the no-window build).

**lit (`test/lit.cfg`):**

- `%ui-backend`: the third `--version` line's value, `none` if absent.
  Features `ui-backend-gl` / `ui-backend-metal` accordingly.
- `%ui-backend-expected`: `metal` on darwin, `gl` on other posix.
- `%gl-completion-poll`, `%gl-completion-block`: the
  `HERMES_NODE_UI_GL_COMPLETION=poll|block` assignment when the backend is
  `gl`, empty otherwise.
- `%with-display` = `<python> <source>/utils/with-display.py`; `%timeout`
  as in Design (f). `%with-xvfb` is removed.
- `display`: Linux probe unchanged except that it goes through
  `with-display.py`; darwin probe gated on `ui-backend-metal`, running
  `with-display.py 60 <hermes-node-ui> display_probe.js` with
  `HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI`, true when the
  status is 0, stdout has the assembled sentinel `DISPLAY_PROBE_OK` and
  stderr has `present: drawable acquired`. `subprocess` timeout 90,
  `TimeoutExpired` caught.
- `display-expected`: as in Design (f). `%display-state` unchanged.
- `ui-gpu` is **not** changed in this task.

`display_probe.js`: `createWindow({width: 160, height: 120}).catch(() =>
{})`, then `ui.__waitPlatformFrames(5)`, print the sentinel assembled from
two string halves, `process.exit(0)`.

**Trace lines added (exact):**

```
worker loop: first turn
worker loop: turn skipped (closing)                 once
worker loop: turn skipped (frame callback)          once
```

All three are printed by the shared check callback (Step 4).

**Steps:**

- [ ] **Step 1: Write `test/ui-backend-feature.js`** (`REQUIRES: ui,
  ui-gpu`): `%hermes-node-ui --version | grep '^gpu backend: '` joined
  with `lit=%ui-backend expected=%ui-backend-expected`, checked as
  `gpu backend: [[B:[a-z]+]] lit=[[B]] expected=[[B]]`. Run it: it fails
  (no such line, no such substitutions). *Mutation after Step 3: delete
  the `--version` line; lit computes `none` and the test is red.*
- [ ] **Step 2: Extend the isolation test first.** In
  `test/sokol-isolation.js` add `--producing-user
  %sgp_objdir/gl_backend.cpp.o --presenting-user
  %sgp_objdir/gl_present.cpp.o` behind a new substitution pair
  `%sgp_producing_user` / `%sgp_presenting_user` (lit params supplied by
  CMake, so 16b can name the Metal objects). Run: fails on the unknown
  argument. Then implement in `check-sgp-isolation.py`: the two family
  rules for `hnui_sg_*` / `hnui_sgp_*`, and for each user object, no
  undefined reference to the other family (`sg_` and `hnui_sg_` against
  `sgp_` and `hnui_sgp_`). *Mutation: add a call to `sg_query_backend()`
  in `gl_present.cpp` after un-defining the rename for that one name; the
  test is red naming it. Revert.*
- [ ] **Step 3: Rename the factory and add the name.**
  `createCompositorBackend()`, `compositorBackendName()`, the `--version`
  line. Step 1's test passes.
- [ ] **Step 4: Add `workerLoopTurn()`** to `backend.h` and an empty GL
  override whose comment says why it is empty. In `ui_module.cpp`, after
  `makeCurrentOnWorker` succeeds in `onSharedContextReady`, start an
  unreferenced `uv_check_t` on the loop `createWindow` already holds. Its
  callback: if `uiClosing()`, trace `worker loop: turn skipped (closing)`
  once and return; if a frame callback is on the stack, trace `worker
  loop: turn skipped (frame callback)` once and return; otherwise call the
  backend, tracing `worker loop: first turn` the first time. Tests, written
  first: add `worker loop: first turn` to `test/ui-raf.js`'s main-run
  TRACE, and `worker loop: turn skipped (closing)` to
  `test/ui-exit-paths.js` FRAME-TRACE -- `process.exit()` inside a frame
  callback reaches `flushPendingWrites`, whose nested `uv_run` runs the
  check phase. *Mutations: do not start the handle -- `ui-raf.js` is red;
  delete the two guards -- FRAME is red. On GL the guard protects nothing
  yet; its Metal half is 16c's, and FRAME's header says so.*
- [ ] **Step 5: Move the platform services behind the seam.** Create
  `platform_services.h` and `platform_services_x11.cpp` from the three
  `#ifdef HERMES_NODE_UI_HAVE_X11` blocks of `ui_platform.cpp`
  (`readClipboardNow`, the title and icon read-backs, the clipboard-write
  read-back) and from `sapp_set_clipboard_string`; `ui_platform.cpp` calls
  the functions and loses the define. `uiRunPlatformLoop` calls
  `uiPlatformBeforeRun()` after `selectContextRoute` succeeds and before
  `sapp_run`; `onInit` calls `uiPlatformAfterInit()` after
  `createSharedContext` succeeds. The four traces must be byte-identical:
  `test/ui-commands.js`, `ui-clipboard.js`, `ui-abi-sizes.js`,
  `ui-conformance.js` and `ui-exit-paths.js` pass unchanged.
- [ ] **Step 6: Shared constant and the neutral failure text.**
  `kPresenterClearLevel` into `present_constants.h`; in `ui_module.cpp`
  replace the two "(GL_WAIT_FAILED)" suffixes in
  `terminateGraphicsSession` reasons with "(the backend reported the
  completion as failed)", since Metal reaches the same line with a command
  buffer error. No test matches the old text.
- [ ] **Step 7: The accessor files and the `#error`.** Create
  `sokol_gfx_ext.inc` and `.h`; include the `.inc` from both
  implementation units. On Linux nothing changes in the objects:
  `test/sokol-isolation.js` and `test/sokol-rename.js` pass, count still
  156.
- [ ] **Step 8: The wrappers.** Write `utils/with-display.py`
  (`with-display.py SECONDS COMMAND...`; Linux branch `exec`s as in Design
  (f); darwin branch written to that description and exercised only on
  the Mac), `utils/timeout.py` (`[--foreground] SECONDS COMMAND...`; exit
  124 on expiry; without `--foreground` it runs the command in a new
  process group and kills the group; with it, it does neither and signals
  the child alone), and `utils/lit-run.sh <build dir> [lit options]
  <tests...>`, which takes every `--param` from the `check-hermes-node-js`
  command in `<build dir>/build.ninja` and runs
  `python3 <build dir>/bin/hermes-lit` with them.
- [ ] **Step 9: Rewrite the RUN lines, mechanically.** `timeout N
  %with-xvfb` becomes `%with-display N`; `%not timeout N %with-xvfb`
  becomes `%not %with-display N`; every other `timeout` becomes
  `%timeout`. In `ui-present.js` and `ui-resize-generation.js` the
  `HERMES_NODE_UI_GL_COMPLETION=...` assignments become the two
  substitutions. Then run the whole Linux suite: same totals, and
  `ps`-level identity is checked once by hand
  (`%with-display 5 sh -c 'ps -o args= -p $PPID'` shows `with-xvfb.py`
  under `timeout`).
- [ ] **Step 10: Per-backend check prefixes.** For `ui-present.js`
  (POLLERR, BLOCKERR, PROBEERR), `ui-route-refusal.js` (BOGUS, BOGUSERR,
  PB, PBERR, RET, RETERR), `ui-commands.js` (TRACE), `ui-abi-sizes.js`
  (APPLIED), `ui-conformance.js` (CHECK, TRACE): change `--check-prefix=X`
  to `--check-prefixes=X,X-%ui-backend` and move the lines that name a GL
  or X11 fact to `X-gl`: completion mode and fence lines, the route
  messages, `owns_selection`, `_NET_WM_ICON`, the fullscreen pair, the
  mute-owner clipboard timeout. `ui-commands.js`, and `demo.js`'s
  conformance mode, take the backend as an argument (`%ui-backend`
  appended to the RUN line) and skip nothing on `gl`. Linux suite green.
  *Mutation: change one `-gl` line's text; its test is red -- the prefix
  is really selected.*
- [ ] **Step 11: `window_driver.js`.** Move `startDriver()` out of
  `ui-real-events.js` and the `spawnSync(... '--close' ...)` call out of
  `ui-close.js` into `test/fixtures/hnui-demo/window_driver.js`:
  `startDriver({ui, driverPath, title})` returning `{send, onLost, stop}`
  and `closeOnce({ui, driverPath, title})`. The darwin branch calls
  `ui.__driveWindow(command)` and maps its answer onto the same lines
  (`SENT <command>`, `STATE viewable=<0|1> <w>x<h>`); `onLost` never
  fires there. Split `ui-real-events.js`'s DEMO run into
  `test/ui-real-events-demo.js` (`REQUIRES: ui, ui-gpu, ui-backend-gl,
  display`). Gate `ui-clipboard.js` on `ui-backend-gl`. Linux suite green.
- [ ] **Step 12: `display-expected`**, and re-gate
  `test/ui-display-feature.js` on `ui-gpu, display-expected`. *Mutation,
  as that file names it: make the Linux probe return False with `CI=1` in
  the environment; red.*
- [ ] **Step 13: The addon's backend define.** In the top-level
  `CMakeLists.txt` replace the two literal `SOKOL_GLCORE` definitions on
  `hnui_demo_addon` and `hnui_bad_version_addon` with the PUBLIC compile
  definitions of `hermesNodeCompositorGpu`, so the addon cannot be built
  for a backend the host was not.
- [ ] **Step 14: `utils/format.sh`** also finds `*.mm` and `*.m`.
- [ ] **Step 15: Format, run `check-hermes-node`, commit** ("Put the GPU
  backend and the platform services behind seams").

**Mac verification** (the backend is still absent there):

```bash
cmake --build $B --target check-hermes-node 2>&1 | tail -15
# expect: 442 unit tests pass; JS failures 0; unsupported 29 or 30
#   (ui-backend-feature and ui-real-events-demo are new and UNSUPPORTED here)
$B/bin/hermes-node-ui --version
# expect a line: gpu backend: none
utils/lit-run.sh $B -a $S/test/ui-windowless.js $S/test/ui-module-requirable.js | tail -5
# expect: both PASS -- proves lit-run.sh found the params
python3 utils/timeout.py 2 sleep 10; echo "status $?"
# expect: status 124, after about 2 s
python3 utils/with-display.py 5 sh -c 'echo "$HERMES_NODE_UI_TEST_NO_ACTIVATE $HERMES_NODE_UI_TEST_PASTEBOARD"'
# expect: "1 hnui-test-<digits>"
( python3 utils/with-display.py 30 sleep 4 & ); sleep 1; /usr/bin/time -p python3 utils/with-display.py 30 true
# expect: real is about 3 s, not 0 -- the second run waited for the lock
```

---

## Task 16b: the toolchain, end to end -- a window showing the clear colour

**Who:** Mac agent. **Linux verifies** by reading, by the Linux suite, and
by checking that no line outside an `APPLE` region changed.

The smallest thing that proves the whole build on the Mac: both `sokol_gfx`
instances compiled for Metal by CMake as Objective-C++ with ARC, the
isolation check on Mach-O, `sokol_app` compiled for Metal, and
`hermes-node-ui` opening a window that presents the presenter's clear
colour. No surface sharing: the worker half refuses.

Run probes P1 and P2 first; their results decide Step 5 and Step 2.

**Files:**
- Create: `lib/compositor-gpu/metal_backend.mm`,
  `lib/compositor-gpu/metal_present.mm`,
  `lib/compositor-gpu/metal_present.h`,
  `tools/hermes-node-ui/mac_app.mm`, `tools/hermes-node-ui/mac_app.h`,
  `tools/hermes-node-ui/platform_services_mac.mm`,
  `test/ui-metal-clear.js`.
- Modify (APPLE regions only): `lib/compositor-gpu/CMakeLists.txt`,
  `tools/hermes-node-ui/CMakeLists.txt`, top-level `CMakeLists.txt` (lit
  params for the two user objects), `test/lit.cfg` (the `ui-gpu`
  predicate, one line), `test/ui-route-refusal.js` (`-metal` lines).

**Interfaces consumed:** everything 16a produced.

**Interfaces produced:**

```cpp
// metal_present.h -- platform thread only; names no Sokol or ObjC type
class MetalPresenter {
 public:
  bool setup(void *mtlDevice, std::string &error);   // sgp_setup + blit pipeline
  int colorFormat() const; int sampleCount() const;
  bool acquireSwapchain();                           // exactly once per frame
  int swapchainWidth() const; int swapchainHeight() const;
  void clear();
  // 16c adds wrap / releaseWrappers / isWrapped / blit / readPixels
};

// mac_app.h -- main thread only
/// Make NSApp an HnuiApplication. Before sapp_run(). Idempotent.
void uiMacInstallApplication();
/// The window Sokol created, or nil. (void * so the header stays C++.)
void *uiMacWindow();
```

`metal_backend.mm` holds `MetalBackend final : CompositorBackend`,
`createCompositorBackend()` and `compositorBackendName()` returning
`"metal"`. It is the producing unit: it includes `sokol_gfx.h` without the
rename. `metal_present.mm` is the presenting unit: `sgp_rename.h` before
`sokol_gfx.h`, and the one definition of `SOKOL_GLUE_IMPL`. In this task
`makeCurrentOnWorker` returns false with `error = "the Metal backend's
drawing half is not built yet"`, `createGenerationSurfaces`,
`wrapForPresenting`, `frameTarget`, `generationSize` and
`readPresentedPixels` return false, and `blit`, `submitAndTrackCompletion`
and the rest are empty or complete inline -- every one with a comment
naming the task that fills it, so nothing reads as finished.

`HnuiApplication` in this task overrides `activateIgnoringOtherApps:` and
`setActivationPolicy:` only (Design (f)); `terminate:` is 16e's.

**CMake, the decisions:**

- `lib/compositor-gpu/CMakeLists.txt`: the guard becomes `if(NOT UNIX)`.
  Under `if(APPLE)`: sources are `metal_backend.mm metal_present.mm` in
  place of `gl_backend.cpp gl_present.cpp`; PUBLIC definition
  `SOKOL_METAL`; PUBLIC link `"-framework Cocoa" "-framework QuartzCore"
  "-framework Metal"`; no X11 or OpenGL lookups.
- The two shared `sokol_gfx*_impl.cpp` files keep their names -- so their
  object names, and the lit params that point at them, do not change --
  and are compiled as Objective-C++ through a per-source property:
  `set_source_files_properties(sokol_gfx_impl.cpp
  sokol_gfx_present_impl.cpp PROPERTIES COMPILE_OPTIONS
  "-xobjective-c++;-fobjc-arc")`. The `.mm` files get `-fobjc-arc` the
  same way. This keeps the language `CXX`, so Hermes's `CMAKE_CXX_FLAGS`
  still apply; `hermes/lib/Platform/Intl/CMakeLists.txt` is the precedent
  for a `.mm` in `add_hermes_library` with `-fobjc-arc` and
  `UNITY_BUILD false`.
  UNVERIFIED: that this exact spelling works under `add_hermes_library`'s
  flags (Step 1 is the test). Fallback order if it does not:
  `target_compile_options` on a dedicated OBJECT library for the two
  files; then `enable_language(OBJCXX)` with `LANGUAGE OBJCXX` and the
  CXX flags copied.
- `tools/hermes-node-ui/CMakeLists.txt`, under `if(APPLE)` inside the
  backend block: `sokol_app_impl.cpp` gets the same per-source property;
  add `mac_app.mm` and `platform_services_mac.mm`.
- `check-sgp-isolation` and the lit params name
  `metal_backend.mm.o` / `metal_present.mm.o` as the user objects on
  Apple.

`platform_services_mac.mm` in this task implements `uiPlatformBeforeRun`
(`uiMacInstallApplication()`), `uiPlatformAfterInit` (`orderFrontRegardless`
under the no-activate variable) and `uiPlatformWindowTitle`; the clipboard
and icon functions return `kError` / a string beginning `not implemented`
until 16e.

**Trace lines (exact):**

```
route: metal (device=<MTLDevice name>)
app: HnuiApplication installed (activate=<0|1>)
presenting instance ready: color_format <n>, sample_count <n>
present: drawable acquired (<w>x<h>)
present: first clear committed
```

`drawable acquired` is printed once, at the first valid acquisition, with
the size read from the drawable's own texture -- not from `sapp_width()`.
`first clear committed` is printed once, after `sgp_commit()` returned.

**Refusal texts (exact, each also the rejection's message):**

```
HERMES_NODE_UI_GL_ROUTE is set ('<value>') but does not apply to this build, which presents through Metal and has no GL context route. Unset it.
HERMES_NODE_UI_GL_COMPLETION is set ('<value>') but does not apply to this build, which presents through Metal and has no GL completion mode. Unset it.
no Metal device is available (MTLCreateSystemDefaultDevice returned nil), so there is nothing to open a window on.
```

**Steps:**

- [ ] **Step 1: Compile the two `sokol_gfx` instances through CMake.**
  Edit `lib/compositor-gpu/CMakeLists.txt` as above with a
  `metal_backend.mm` that contains only the factory returning null. Build
  `hermesNodeCompositorGpu`. Expected: no warnings from the two Sokol
  objects. Then `cmake --build $B --target check-sgp-isolation`: expect
  156 `sg_*` against 156 `sgp_*`, plus three `hnui_sg_*` and three
  `hnui_sgp_*`. If the per-source property does not take, walk the
  fallback order and report which worked.
- [ ] **Step 2: Prove the accessor.** In a scratch file outside the tree,
  call `hnui_sg_mtl_command_buffer()` before a pass (expect null), inside
  one (non-null), after `sg_commit()` (null), linking the two objects;
  this is probe P2's first half and may already be done. If it cannot
  work, stop and report: the fallback in Design (b) is the driver's call.
- [ ] **Step 3: Write `test/ui-metal-clear.js`** (`REQUIRES: ui,
  ui-backend-metal, display`), RUN under `%with-display 60` with hooks and
  tracing on. The script: `createWindow({width: 320, height: 240})`,
  expect the rejection whose message contains `not built yet` (print
  `REJECTED ok`), `ui.__waitPlatformFrames(3)`, `process.exit(0)`. Checks:
  status 0; stdout `REJECTED ok`; stderr, in order, the five trace lines
  above with `drawable acquired (320x240)`; implicit-not `[sapp:error` and
  `[sokol-`. Run it: it fails at configuration (`ui-backend-metal` is not
  a feature yet).
- [ ] **Step 4: The presenter.** `MetalPresenter::setup` (the MSL blit
  shader is written now although nothing draws with it until 16c, so that
  shader compilation is part of the toolchain proof): vertex function
  `vs_main` taking `[[vertex_id]]`, fragment `fs_main` with
  `[[texture(0)]]` and `[[sampler(0)]]`, `uv = (p.x, 1 - p.y)`;
  `sg_shader_desc` with `vertex_func.entry`, `fragment_func.entry`,
  `views[0].texture.msl_texture_n = 0`, `samplers[0].msl_sampler_n = 0`.
  `acquireSwapchain` with the nil-drawable guard; `clear` with
  `kPresenterClearLevel`.
- [ ] **Step 5: The application subclass and the platform functions**,
  per P1's result. If P1 showed the no-activate window is not visible,
  implement `HnuiApplication` without the two overrides, leave the
  variable unread, and say so in the report.
- [ ] **Step 6: `selectContextRoute`, `createSharedContext`, the refusing
  worker half; link `hermes-node-ui`.** Flip `ui-gpu` in `test/lit.cfg`
  to the posix predicate, with the comment rewritten to say what it now
  mirrors.
- [ ] **Step 7: Run `test/ui-metal-clear.js`** until it passes. Then make
  each mutation, watch it fail, revert: (a) skip `sgp_commit()` in
  `clear()` -- `first clear committed` is missing; (b) drop the
  `orderFrontRegardless` call (if Step 5 kept it) -- measured: the test
  STAYS GREEN, see "Task 16b as built"; no test observes this call; (c) return true from
  `acquireSwapchain` without calling `sglue_swapchain()` -- Sokol panics
  or the size is wrong.
- [ ] **Step 8: Add the `-metal` lines to `test/ui-route-refusal.js`** for
  BOGUS, BOGUSERR, PB, PBERR, RET, RETERR from the texts above, and run
  it with `test/ui-exit-tty.js`, `ui-null-window.js`,
  `ui-version-check.js`, `ui-exit-late-require.js`,
  `ui-sokol-validation-feature.js`, `ui-backend-feature.js`,
  `sokol-isolation.js`, `sokol-rename.js`. All nine pass. Run the nine
  once more with the display asleep or the screen locked if that can be
  arranged, to confirm none needs a display; otherwise run them with
  `DYLD_PRINT_LIBRARIES=1` on one and confirm AppKit is never
  initialised (no `HnuiApplication installed` trace).
- [ ] **Step 9: Commit** on `xtest/work-imgui-mac` ("Build the Metal
  toolchain and present a clear colour"), and report: which CMake route
  worked, the build's warning count, P1's and P2's outcomes, the exact
  stderr of one `ui-metal-clear.js` run.

**Mac verification:**

```bash
cmake --build $B --target hermes-node-ui hnui_demo_addon hnui_bad_version_addon check-sgp-isolation 2>&1 | tail -8
# expect: the script's two success lines, "sgp isolation OK: ..." and "sgp users OK: ...", 0 warnings in lib/compositor-gpu and tools/hermes-node-ui
nm -gU $B/bin/hermes-node-ui | grep -c ' _sg_'      # expect >= 156
nm -gU $B/bin/hermes-node-ui | grep -c ' _hnui_'    # expect >= 15
$B/bin/hermes-node-ui --version | sed -n 3p          # expect: gpu backend: metal
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B -a \
  $S/test/ui-metal-clear.js $S/test/ui-backend-feature.js $S/test/ui-display-feature.js \
  $S/test/ui-route-refusal.js $S/test/ui-exit-tty.js $S/test/ui-null-window.js \
  $S/test/ui-version-check.js $S/test/ui-exit-late-require.js \
  $S/test/sokol-isolation.js $S/test/sokol-rename.js 2>&1 | tail -15
# expect: 10 passed, 0 failed
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI \
  python3 utils/with-display.py 30 $B/bin/hermes-node-ui $S/test/ui-metal-clear.js 2>&1 | grep -ci 'validation\|assert'
# expect: 0 lines beyond Metal's own "validation enabled" banner (report the banner count)
```

Do not run anything that asks macOS for Automation or Accessibility
consent (System Events scripting, `CGEventPost`), here or in any later
task. While `ui-metal-clear.js` runs, note whether the frontmost
application changed and report it in a sentence.

The other window tests fail on the Mac from this commit until 16c (the
`display` feature is now true and the worker half refuses). That is
expected and is why this task's list of ten is explicit.

---

## Task 16c: the surface pipeline -- draw, complete, blit, capture

**Who:** Mac agent. **Linux verifies** by reading (Review focus 1 and 3)
and by the Linux suite.

The gate is the picture: `test/ui-present.js` compares presented pixels
exactly on Metal.

Run probe P3 first; its result decides Step 6.

**Files:**
- Modify: `lib/compositor-gpu/metal_backend.mm`, `metal_present.mm`,
  `metal_present.h`; `test/ui-present.js` (`-metal` lines);
  `test/ui-metal-clear.js`.
- No shared C++ changes are expected. If one is needed, stop and report.

**Interfaces consumed:** `hnui_sg_mtl_command_buffer`,
`hnui_sgp_mtl_command_buffer`; `uiTestHooksEnabled()` (`test_hooks.h`);
`kPresenterClearLevel`; `uiMacWindow()`.

**Interfaces produced** (`metal_present.h`, platform thread only):

```cpp
bool wrap(GenerationId gen, void *const *mtlTextures, int count,
          int width, int height, std::string &error);   // idempotent
void releaseWrappers(GenerationId gen);
bool isWrapped(GenerationId gen) const;
/// Draw ref into the swapchain and commit. onComplete runs when that
/// command buffer finished, on a Metal thread. False if nothing was
/// submitted, in which case onComplete is never called.
bool blit(SurfaceRef ref,
          void (*onComplete)(void *user, SurfaceRef ref, bool ok), void *user);
bool readPixels(int width, int height, uint8_t *out);   // RGBA, top row first
```

**Trace lines (exact; each "once" line is printed the first time only):**

```
worker autorelease pool: installed
worker autorelease pool: first drain
producing instance ready: color_format <n>
completion mode: handler (metal)
generation <g>: 3 surfaces at <w>x<h>
generation <g>: surfaces destroyed
completion: producer handler ran (status=<completed|error>)            once
completion: frame had no command buffer; completed inline               once
wrapped generation <g> for presenting (<w>x<h>)
released presenting wrappers for generation <g>
completion: consumer handler ran (status=<completed|error>)             once
capture: staging copy enabled (framebufferOnly=<0|1>)
```

The two `handler ran` lines are printed from **inside** the handler block,
after it read the status, so each is evidence the handler executed on a
real buffer. `framebufferOnly` is read back from the layer after it was
set. The four lines shared with GL (`producing instance ready`,
`generation ...` twice, `wrapped ...`, `released ...`) keep GL's exact
text, because shared tests match them.

**What each piece must do:**

- `makeCurrentOnWorker`: push the pool, `sg_setup`, trace. `producerLog`
  as in `gl_backend.cpp`, same instance name `sokol-produce`. Add the
  `hnui_gpu_view` / `sg_view` static_asserts (Ruling 46).
- `workerLoopTurn`: pop and push the pool, tracing the first drain.
  Maintenance is 16d's; leave a marked hook.
- `submitAndTrackCompletion`: inside `@autoreleasepool`. Read the
  accessor; if null, `sg_commit()`, trace, `onComplete(user, ref, true)`.
  Otherwise increment `producerInFlight_`, `addCompletedHandler:`, then
  `sg_commit()` -- in that order, since a handler added after commit is
  an error. The block captures only the function pointer, `user`, `ref`
  and `this`; it prints the buffer's `error.localizedDescription` to
  stderr on `MTLCommandBufferStatusError`, calls `onComplete`, then
  decrements the counter.
- `blit`: pass, pipeline, bindings, draw, `sgp_end_pass`; under test
  hooks, the staging copy (Step 6); the consumer handler; `sgp_commit()`.
  Under test hooks the command buffer is also kept in a strong member for
  `readPixels`.
- `wrapForPresenting`: copy the three texture pointers out under
  `surfacesMutex_`, then `MetalPresenter::wrap`.

**Steps:**

- [ ] **Step 1: Add `-metal` lines to `test/ui-present.js`.** POLLERR and
  BLOCKERR share them (both runs are ordinary runs on Metal):
  `[ui] completion mode: handler (metal)`, `[ui] completion: producer
  handler ran (status=completed)`, `[ui] completion: consumer handler ran
  (status=completed)`, in the order the trace emits them relative to the
  shared `capture: written` line; PROBEERR-metal the same. Run: fails at
  `window ready` (the worker half still refuses).
- [ ] **Step 2: The producer.** `makeCurrentOnWorker`,
  `createGenerationSurfaces`, `destroyGenerationSurfaces`, `frameTarget`,
  `generationSize`. Run `test/ui-create-window.js`: passes (a window
  resolves, a second `createWindow` throws, exit 0). Update
  `test/ui-metal-clear.js`: the window now resolves, so the script prints
  `RESOLVED ok` instead and still expects the clear lines, since it draws
  nothing.
- [ ] **Step 3: Producer completion.** `submitAndTrackCompletion` as
  above. Run `test/ui-raf.js`: its ticks advance (a frame that never
  completes leaves the machine in `kGpuPending` and the test at its
  timeout). *Mutation: call `onComplete` inline before `sg_commit()`
  instead of from the handler -- the `producer handler ran` line is
  missing from `ui-present.js`.*
- [ ] **Step 4: Wrapping and the blit.** *Mutations after Step 6:* (a)
  skip the draw call -- the capture is the 32/255 clear colour; (b) use
  `uv = p` -- the top and bottom quadrants swap; (c) never call the
  consumer sink -- after three presented frames no slot is FREE and
  `ui-raf.js` stalls; (d) nearest instead of linear -- `ui-present.js`
  still passes (1:1), `ui-resize-generation.js` STRETCHED is 16d's check.
- [ ] **Step 5: The per-turn pool.** Run `test/ui-raf.js` and, by direct
  invocation, `test/ui-exit-paths.js`'s `frame` and `listener` cases; all
  pass and stderr of one run shows `first drain`. (The file as a whole
  waits for 16e: its READS case needs the clipboard read.) *Mutation: remove the pop -- `first drain` is missing;
  add it as a `-metal` line to `ui-raf.js`'s TRACE.*
- [ ] **Step 6: Capture.** Clear `framebufferOnly` in
  `createSharedContext` when `uiTestHooksEnabled()`; allocate the staging
  `MTLBuffer` at the swapchain's size (reallocate when it changes); encode
  `copyFromTexture:...toBuffer:` after the pass; `readPixels` waits on the
  kept command buffer and converts. If P3 showed the drawable cannot be
  read this way, stop and report -- the alternative (render the blit a
  second time into our own texture) reads back something that was not
  presented and is not acceptable without the driver's ruling.
- [ ] **Step 7: Run `test/ui-present.js` until all three runs pass**, then
  `ui-demo-visual.js`, `ui-raf.js`, `ui-events.js`, `ui-early-events.js`,
  `ui-event-exceptions.js`, `ui-sokol-panic.js`,
  `ui-sokol-validation.js`, `ui-wrong-thread.js`, `ui-metal-clear.js`.
  For each that fails on a line naming an X11 or GL fact rather than on
  behaviour, add the `-metal` line and list it in the report; do not
  weaken a shared line.
- [ ] **Step 8: Make every mutation named in Steps 3-5, watch its test
  fail, revert.**
- [ ] **Step 9: Commit** ("Draw, complete and blit through Metal") and
  report the full stderr of one `ui-present.js` run.

**Mac verification:**

```bash
cmake --build $B --target hermes-node-ui hnui_demo_addon check-sgp-isolation 2>&1 | tail -4
T="ui-create-window ui-present ui-demo-visual ui-raf ui-events ui-early-events ui-event-exceptions ui-sokol-panic ui-sokol-validation ui-wrong-thread ui-metal-clear"
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B -a $(for t in $T; do echo $S/test/$t.js; done) 2>&1 | tail -20
# expect: 11 passed, 0 failed
# the picture, by eye, once:
HERMES_NODE_UI_TEST_HOOKS=1 python3 utils/with-display.py 60 $B/bin/hermes-node-ui $S/test/ui-present.js $B/hnui_demo_addon.node js /tmp/hnui-present.ppm; sips -s format png /tmp/hnui-present.ppm --out /tmp/hnui-present.png 2>/dev/null; file /tmp/hnui-present.png
# expect: a PNG whose four quadrants are the fixed pattern, upright (attach it to the report)
# the checkers standing in for a second compiler:
for e in "MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1" "NSZombieEnabled=YES OBJC_DEBUG_MISSING_POOLS=YES"; do
  env $e HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI python3 utils/with-display.py 120 \
    $B/bin/hermes-node-ui $S/test/ui-exit-paths.js frame $B/hnui_demo_addon.node > /tmp/hnui-c.out 2> /tmp/hnui-c.err
  echo "$e -> status $? ; $(grep -ci 'zombie\|failed assertion\|Metal API Validation.*error' /tmp/hnui-c.err) finding(s), $(grep -c 'autoreleased with no pool' /tmp/hnui-c.err) missing-pool line(s)"
done
# expect: the status ui-exit-paths.js FRAME documents, and 0 findings, twice
```

**Missing-pool lines are judged per thread, not counted to zero.** Task
16b measured 179 of them in one run with no code of ours at fault: about
157 on four threads this project never runs code on (AppKit's and
Metal's own), and 22 on the main thread after `sapp_run` starts
(`NSConcreteValue`, `_sapp_macos_app_delegate`). Each line carries the
thread it was on. So 16c traces the JS thread's `pthread_self()` once and
the criterion is: **zero lines on the JS thread**, no line on any thread
naming an `MTL*`, `CAMetalDrawable` or `AGX*` class, and a main-thread
count no higher than the same run of the commit before. Report the
per-thread, per-class table.

---

## Task 16d: generations, maintenance, the invalid swapchain, real resize

**Who:** Mac agent. **Linux verifies** by reading (Review focus 2 and 5),
by the Linux suite, and by running the Linux half of
`window_driver.js`'s users.

**Files:**
- Create: `tools/hermes-node-ui/mac_test_driver.mm`,
  `tools/hermes-node-ui/mac_test_driver.h`,
  `test/ui-metal-maintenance.js`, `test/ui-metal-occlusion.js`.
- Modify: `lib/compositor-gpu/metal_backend.mm`, `metal_present.mm`;
  `tools/hermes-node-ui/ui_module.cpp` (one export, inside `#if
  defined(__APPLE__)`), `CMakeLists.txt` (APPLE region);
  `test/ui-resize-generation.js`, `test/ui-real-events.js` (`-metal`
  lines); `test/fixtures/hnui-demo/demo_addon.c` (one test-only function).

**Interfaces produced:**

```cpp
// mac_test_driver.h
/// JS thread (NAPI). Exported as ui.__driveWindow only when the test hooks
/// are on. Runs one command on the main thread and returns its answer
/// line; throws if the main thread did not run it within 10 s, or for an
/// unknown command. Blocks the JS thread meanwhile; the main thread never
/// waits on the JS thread, so this cannot deadlock.
napi_value driveWindow(napi_env env, napi_callback_info info);
```

Commands and answers (the answer is what `window_driver.js` maps onto the
X11 driver's lines):

```
key <char>            -> SENT key <char>
resize <w> <h>        -> SENT resize <w> <h>
close                 -> SENT close
miniaturize           -> SENT miniaturize
deminiaturize         -> SENT deminiaturize
terminate             -> SENT terminate          (16e uses it)
state                 -> STATE viewable=<0|1> <w>x<h> miniaturized=<0|1>
```

`viewable` is `occlusionState & NSWindowOcclusionStateVisible`; `<w>x<h>`
is the content view's bounds. The implementation is `dispatch_async` to
the main queue plus a mutex, a condition variable and a deadline, like
`__waitPlatformFrames`.

`demo_addon.c` gains `churnBufferThisFrame()`: inside a frame callback,
create and destroy one small `sg_buffer`, so that a release is queued in a
frame that is about to be in flight.

**Trace lines (exact):**

```
drive: <command> applied (<read-back>)
maintenance: <n> commit(s), idpool free <a> -> <b>, pending <p>
present: swapchain invalid; nothing submitted
present: swapchain valid again
```

Read-backs: `key` prints `first_responder_is_view=<0|1>`; `resize` prints
`content=<w>x<h>` read after the call; `close` prints `visible=<0|1>` read
after `performClose:` returned; `miniaturize` prints
`miniaturized=<0|1>`. `<a>`, `<b>` and `<p>` come from the accessor before
and after the commits. The two `present:` lines are printed on
transitions only.

**Maintenance, as Design (a) fixes it:** `destroyGenerationSurfaces` sets
the flag; `workerLoopTurn`, after the pool, runs
`while (hnui_sg_mtl_idpool_pending() > 0 && n < 8) sg_commit()` only if
the flag is set and `producerInFlight_ == 0`, clears the flag when pending
reached zero, and traces. `createGenerationSurfaces` refuses when
`hnui_sg_mtl_idpool_free() < 256`:

```
the Metal resource pool is nearly exhausted (<n> slots free): surface generations are being replaced faster than their GPU resources can be reclaimed.
```

**Steps:**

- [ ] **Step 1: The driver.** `mac_test_driver.mm` with `state`, `resize`
  and `key`; export it. Run `test/ui-real-events.js`: KEY and GEN are the
  targets. Add `-metal` TRACE lines for the three `drive:` lines. *Before
  implementing GEN's Metal side it should already pass* -- the resize path
  is shared code plus `createGenerationSurfaces` -- so its role here is
  to prove the resize is real: GEN asserts a capture at the new size with
  target and framebuffer both 400x300. *Mutation: make `resize` call
  `uiHandlePlatformEvent` with a synthesized RESIZED instead of
  `setContentSize:` -- GEN's capture never matches, because the drawable
  stayed 320x240.*
- [ ] **Step 2: `close` in the driver**, and CLOSE in
  `ui-real-events.js`: the listener cancels, `state` reports
  `viewable=1`. *Mutation: remove `desc.event_cb = onEvent` in
  `ui_platform.cpp` -- KEY and CLOSE are red (this is the mutation the
  Slice C review found nothing caught under injection). Mutation: remove
  `sapp_cancel_quit()` -- the window closes, AppKit terminates because
  Sokol answers `applicationShouldTerminateAfterLastWindowClosed:` YES,
  and the run ends with status 0 and `cleanup_cb` in the trace.*
- [ ] **Step 3: Write `test/ui-metal-maintenance.js`** (`REQUIRES: ui,
  ui-backend-metal, display`). The script draws one frame calling
  `churnBufferThisFrame()`, then issues twelve `resize` commands
  alternating 320x240 and 400x300 with **no** `requestAnimationFrame`
  between them (an idle program -- the case in which nothing but
  maintenance ever commits), waiting for each `resize` event, then draws
  one frame and captures it at the final size. Checks: twelve
  `generation <g>: surfaces destroyed` lines for generations 1 to 12;
  after each, a `maintenance: {{[1-4]}} commit(s), idpool free
  {{[0-9]+}} -> {{[0-9]+}}, pending 0`; the final capture written; an
  awk one-liner over the trace asserting the **last** `idpool free` value
  is within 16 of the **first** (the pool does not drain); implicit-not
  `[sokol-`. Run: fails (no maintenance lines).
- [ ] **Step 4: Implement maintenance and the guard.** The test passes.
  *Mutations:* (a) never run maintenance -- no `maintenance:` lines, and
  the free count falls by about nine per generation; (b) one commit
  instead of the loop -- `pending` is not 0; (c) drop the
  `producerInFlight_ == 0` condition -- **no test fails**, which is
  Review focus 2: record in the code why the condition is there, and see
  verification below.
- [ ] **Step 5: `test/ui-resize-generation.js` on Metal.** It uses
  injected RESIZED and held producer completions; add `-metal` lines
  where it names the forced-poll mode. All its cases must pass, STRETCHED
  included (linear filtering at 2:1), and the LIFETIME pair `owner=0`
  then `owner=1`. UNVERIFIED: on Metal the last reference of a generation
  can be dropped by a Metal completion thread or by the platform thread,
  both `owner=0`; if it is ever dropped on the JS thread the pair reads
  `owner=1`, `owner=1` -- report what is seen rather than adjusting the
  check.
- [ ] **Step 6: Write `test/ui-metal-occlusion.js`.** Draw and capture one
  frame; `miniaturize`; wait for the `minimize` event; request a frame
  and wait ten platform frames (`__waitPlatformFrames(10)`);
  `deminiaturize`; wait for `restore`; draw and capture again. Checks, in
  order: `present: swapchain invalid; nothing submitted`; no `frame
  tick:` line between that and `present: swapchain valid again`
  (`-NOT` between two positive matches); both captures written; status
  0. Implement the two transition traces in `presentInvalid` and
  `acquireFrameSwapchain`. *Mutation: in `acquireFrameSwapchain` ignore
  `swapchain.invalid` -- Sokol's validation panics on the nil drawable
  (Debug), or the guard returns false anyway; then also remove the nil
  guard -- the run dies in `sg_begin_pass`.*
- [ ] **Step 7: Commit** ("Reclaim Metal resources between generations,
  and drive real window events") and report. In the report, state what
  happened when the display was put to sleep by hand during a run of
  `ui-raf.js` (`pmset displaysleepnow`), if the session allows it: the
  status, the last twenty trace lines, and whether a Sokol assert fired.
  That observation becomes the tracker issue for Review focus 5.

**Mac verification:**

```bash
cmake --build $B --target hermes-node-ui hnui_demo_addon 2>&1 | tail -3
T="ui-real-events ui-resize-generation ui-metal-maintenance ui-metal-occlusion ui-present ui-raf"
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B -a $(for t in $T; do echo $S/test/$t.js; done) 2>&1 | tail -15
# expect: 6 passed, 0 failed
# Review focus 2: maintenance under API and GPU validation, 20 times
for i in $(seq 1 20); do
  MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI \
    python3 utils/with-display.py 120 $B/bin/hermes-node-ui $S/test/ui-metal-maintenance.js $B/hnui_demo_addon.node /tmp/hnui-m.ppm \
    > /tmp/hnui-m.out 2> /tmp/hnui-m.err; echo -n "$? "
  grep -ci 'validation.*error\|failed assertion\|use of.*after' /tmp/hnui-m.err | tr '\n' ' '
done; echo
# expect: twenty pairs "0 0"
# Review focus 4: Main Thread Checker
MTC=$(xcode-select -p)/usr/lib/libMainThreadChecker.dylib; ls $MTC
DYLD_INSERT_LIBRARIES=$MTC HERMES_NODE_UI_TEST_HOOKS=1 python3 utils/with-display.py 120 \
  $B/bin/hermes-node-ui $S/test/ui-real-events.js $B/hnui_demo_addon.node none /tmp/hnui-r.ppm 2>&1 | grep -c 'Main Thread Checker'
# expect: 0
```

If `libMainThreadChecker.dylib` is not at that path, report where it is, or that an ASAN build refuses the insertion, rather than
skipping the check silently.

---

## Task 16e: closing, termination, clipboard and the read-backs

**Who:** Mac agent. **Linux verifies** by reading (Review focus 4) and by
the Linux suite.

Run probe P4 first; its result decides Step 2.

**Files:**
- Create: `test/ui-terminate.js`, `test/ui-clipboard-mac.js`,
  `test/fixtures/hnui-demo/mac_pasteboard_tool.m`, and -- only if P4
  passed -- `test/fixtures/hnui-demo/mac_quit_sender.m`.
- Modify: `tools/hermes-node-ui/mac_app.mm`,
  `platform_services_mac.mm`, `mac_test_driver.mm`; top-level
  `CMakeLists.txt` (APPLE region: the fixture tools and their lit params
  `mac_pasteboard_tool`, `mac_quit_sender`); `test/lit.cfg` (register the
  two params with `_register_path_param`); `test/ui-close.js`,
  `ui-commands.js`, `ui-abi-sizes.js`, `ui-conformance.js` (`-metal`
  lines); `test/fixtures/hnui-demo/demo.js` (darwin branch of the
  clipboard-timeout step).

**Interfaces produced:**

```objc
// mac_app.mm
@implementation HnuiApplication
/// Main thread. Never calls super. Window present: performClose:. Absent:
/// a synthesized QUIT_REQUESTED to uiHandlePlatformEvent.
- (void)terminate:(id)sender;
@end
```

```
mac_pasteboard_tool <name> set <text>      write a string
mac_pasteboard_tool <name> clear           no types at all
mac_pasteboard_tool <name> png             one non-string type only
mac_pasteboard_tool <name> big <bytes>     a string of that many 'x'
        each prints "DONE <changeCount>" and exits 0
mac_quit_sender <pid>                      NSRunningApplication terminate; prints "SENT terminate <0|1>"
```

**Trace lines (exact):**

```
app: terminate requested; forwarded as a close request (window=1)
app: terminate requested; no window yet, forwarded directly
command title: readback='<title>'
command icon: dock tile <w>x<h>
command clipboard: pasteboard_bytes=<n> bytes=<n>
clipboard: pasteboard '<name>'                                    once, at first use
```

The two `app:` lines are each printed after the call they describe
returned: the first after
`performClose:`, the second after the direct hand-off to
`uiHandlePlatformEvent`. The shared `quit requested: cancelled, forwarded
as close` line is printed inside either call, so it precedes them, and
that order is what `test/ui-terminate.js` matches.

**Steps:**

- [ ] **Step 1: Write `test/ui-terminate.js`** (`REQUIRES: ui,
  ui-backend-metal, display`), modelled on `test/ui-close.js`, three
  cases: DEFAULT (no listener, `process.exitCode = 7`, an `'exit'`
  listener that prints; expect status 7, the `'exit'` line, `lifecycle:
  CLOSING (reason=close)`), CANCEL (a `close` listener calls
  `preventDefault()` and prints; then `state` reports `viewable=1`; ends
  through `process.exit(0)`), EARLY (terminate requested through the
  driver between calling `createWindow()` and awaiting it: the `close` is
  delivered once the window is ready, and either trace line is accepted,
  since the request may land before or after AppKit created the window).
  Every case's trace check carries `--implicit-check-not='cleanup_cb'
  --implicit-check-not='sapp_run returned'` and matches `quit requested:
  cancelled` before `app: terminate requested`. Run with the driver's `terminate` command: it
  fails -- AppKit terminates, status 0, `cleanup_cb` in the trace. That
  failing run is the record of the defect Task 15's review found.
- [ ] **Step 2: Implement `terminate:`.** The three cases pass. If P4
  passed, add a fourth, REAL: the script spawns `%mac_quit_sender` with
  its own pid and expects the same lines as DEFAULT; build the tool. If
  P4 failed, write in the test's header that every request in the file
  enters at `-[NSApplication terminate:]` and what that leaves unproved.
  *Mutations:* (a) call `[super terminate:]` -- DEFAULT exits 0 with
  `cleanup_cb` in the trace; (b) always take the direct hand-off, never
  `performClose:` -- the process still ends correctly, so the behaviour
  checks pass, and what goes red is DEFAULT's trace check: it requires
  the `(window=1)` line and carries `--implicit-check-not='no window
  yet'`. That is the only thing separating "went through Sokol's
  delegate" from "entered below it", which is the distinction this test
  exists for.
- [ ] **Step 3: The read-backs** in `platform_services_mac.mm`: title,
  icon, clipboard write. Add `-metal` lines to `ui-commands.js` (icon,
  clipboard; no fullscreen pair -- the script skips it when its backend
  argument is `metal`, with a comment pointing at the tracker issue),
  `ui-abi-sizes.js` and `ui-conformance.js`. For the icon, run it and
  write the line from what the read-back actually reports; if it reports
  the tile's size rather than 16x16, read the `CGImage`'s size instead
  and say which was used.
- [ ] **Step 4: Write `test/ui-clipboard-mac.js`** (`REQUIRES: ui,
  ui-backend-metal, display`), both read paths (the Promise and
  `hnui_read_clipboard_sync` through the addon) for each case: OWN (our
  write, `'first'`), FOREIGN (`mac_pasteboard_tool ... set second`,
  spawned and awaited), NONE (`clear`: `''` and `HNUI_UNAVAILABLE`),
  NONSTRING (`png`: unavailable, and in particular not `'second'`), BIG
  (`big 1048577`: too large), ABANDONED (a sync read with a zero deadline
  times out and the next read still works), PASTE (an injected
  `CLIPBOARD_PASTED` with no substituted text under OWN carries
  `'first'`). The pasteboard name comes from the environment the wrapper
  set; the script passes `process.env.HERMES_NODE_UI_TEST_PASTEBOARD` to
  the tool. Trace check: `clipboard: pasteboard 'hnui-test-` and no read
  with a status the case does not expect. Run: fails (`kError`).
- [ ] **Step 5: Implement the read and the write.** *Mutations:* (a) cache
  the last successful string and return it when the type is absent --
  NONE and NONSTRING are red; (b) ignore the environment variable -- the
  `pasteboard 'hnui-test-` line is missing (and the run has just written
  to the real clipboard: restore it by hand); (c) no size check -- BIG
  resolves.
- [ ] **Step 6: `test/ui-close.js` on Metal**, all nine cases through
  `window_driver.js`'s `closeOnce`. LATE (a second close during CLOSING)
  and REPLACED need care: report any case whose premise does not
  translate, with the trace, instead of changing its checks.
- [ ] **Step 7: `ui-conformance.js` on Metal.** `demo.js`'s darwin branch
  replaces the mute owner with `hnui_read_clipboard_sync(w, 0, &out)`;
  the stdout line says `(zero deadline)` and is a `CHECK-metal` line. The
  MSAA resolve step and the capture must pass unchanged.
- [ ] **Step 8: Make each named mutation, watch it fail, revert. Commit**
  ("Close, terminate and read the pasteboard on macOS").

**Mac verification:**

```bash
cmake --build $B --target hermes-node-ui hnui_demo_addon mac_pasteboard_tool 2>&1 | tail -3
T="ui-terminate ui-close ui-clipboard-mac ui-commands ui-abi-sizes ui-conformance ui-real-events ui-exit-paths"
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B -a $(for t in $T; do echo $S/test/$t.js; done) 2>&1 | tail -15
# expect: 8 passed, 0 failed
pbpaste | head -c 40; echo        # run before and after the block above
# expect: identical output both times -- the suite did not touch the real clipboard
MTC=$(xcode-select -p)/usr/lib/libMainThreadChecker.dylib
DYLD_INSERT_LIBRARIES=$MTC HERMES_NODE_UI_TEST_HOOKS=1 python3 utils/with-display.py 120 \
  $B/bin/hermes-node-ui $S/test/ui-clipboard-mac.js $B/hnui_demo_addon.node $B/mac_pasteboard_tool 2>&1 | grep -c 'Main Thread Checker'
# expect: 0
# by hand, once, hooks off: the Dock's Quit on a running demo
python3 utils/with-display.py 60 env -u HERMES_NODE_UI_TEST_NO_ACTIVATE $B/bin/hermes-node-ui $S/test/fixtures/hnui-demo/demo.js $B/hnui_demo_addon.node js; echo "status $?"
# if a person is at the Mac: right-click the Dock icon, Quit; expect the window to close and "status 0".
# if nobody is: say so in the report. This one check has no automated form.
```

---

## Task 16f: the suite on both machines, CI, and the documents

**Who:** Linux. **Mac verifies** the whole suite three times.

**Files:**
- Modify: `.github/workflows/ci.yml`; `CLAUDE.md`;
  `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`;
  `docs/superpowers/plans/2026-09-21-ui-compositor.md`;
  `docs/superpowers/plans/progress-ui-compositor.md`;
  `external/sokol/README.md`; `include/hermes/node-compat/ui/hnui.h`
  (comments); `lib/compositor-gpu/CMakeLists.txt`, `backend.h`
  (comments that say "Metal arrives with Task 16");
  `tools/hermes-node-ui/CMakeLists.txt`, `main.cpp`, `ui_host.cpp`;
  `test/lit.cfg` (comments).
- Delete: `tools/hermes-node-ui/ui_module_nowindow.cpp`,
  `test/ui-no-window-support.js`.
- Create: tracker issues (below).

**Steps:**

- [ ] **Step 1: Remove the backend-less configuration.** With a backend on
  both supported platforms, `ui_module_nowindow.cpp` is compiled nowhere
  and its test can run nowhere. Delete both;
  `lib/compositor-gpu/CMakeLists.txt`'s early `return()` becomes
  `message(FATAL_ERROR ...)` naming the two supported platforms;
  `tools/hermes-node-ui/CMakeLists.txt` loses the `else()` branch and
  `HERMES_NODE_UI_HAVE_WINDOW` becomes unconditional there; `main.cpp`
  and `ui_host.cpp` lose their `#ifndef` halves. `--version`'s `none`
  value goes with them, and `test/lit.cfg`'s `%ui-backend` default stays
  `none` for a binary that printed no line. Linux suite green.
- [ ] **Step 2: CI.** Add to the macOS half of `ci.yml`, after "Build &
  test", `if: runner.os == 'macOS' && matrix.config == 'asan-debug-o1'`,
  `continue-on-error: true`, a step "UI display probe (informational)"
  that runs `python3 utils/with-display.py 60 build/bin/hermes-node-ui
  test/fixtures/hnui-demo/display_probe.js` with
  `HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI`, then prints
  `probe status $?`. A comment above it holds Design (g)'s three
  outcomes and what to do after the first run. Do not set
  `HERMES_NODE_UI_EXPECT_DISPLAY` in the workflow in this task.
- [ ] **Step 3: Correct the spec.** "Surfaces": the completion paragraph
  keeps its mechanism and loses "This requires a Sokol patch, and it is
  the design's only required one" in favour of where the accessor lives
  and why no patch is needed; the maintenance bullets on suspending
  submissions are replaced by Rulings 81 and 82 with their reasons, and
  the three-versus-four commit arithmetic is corrected. "What Sokol gives
  us": the `sg_mtl_command_queue` bullet. "Build and integration": the
  required-patch bullet becomes "no patch is required today". "Frames":
  "No ticks while the window is minimized" becomes minimized or occluded,
  on macOS (Ruling 88). "Input and platform services": the macOS
  clipboard, and a Termination paragraph under Closing with what remains
  impossible. "Lifecycle": `cleanup_cb` never runs on macOS either, and
  why. "Testing": macOS is tested through an in-process driver and a
  probe-decided `display`; CI Metal availability is "reported by the
  first run" until it is. "Spike": items 3b and 5 gain "exercised by the
  implementation, 2026-11".
- [ ] **Step 4: CLAUDE.md, UI Compositor section.** Slice D's state; the
  features table (six features plus `display-expected`,
  `ui-backend-*`); `%with-display` in place of the Xvfb paragraph, with
  the macOS lock and why it exists; the environment variables added
  (`HERMES_NODE_UI_TEST_NO_ACTIVATE`, `HERMES_NODE_UI_TEST_PASTEBOARD`,
  `HERMES_NODE_UI_EXPECT_DISPLAY`) and that the GL ones are refused on
  Metal; the paragraph on what each `__driveWindow` command proves; the
  accessor and the rule that `external/sokol/sokol` is still unmodified;
  ARC for every Objective-C++ unit; that the Mac cannot run the
  formatter.
- [ ] **Step 5: The plan and the progress file.** Tick Task 16 with its
  sub-tasks by commit subject, record Rulings 80-88 in the Task 16
  paragraph, move what Task 17 still owns into its paragraph, update the
  suite totals for both machines from Step 7's numbers.
- [ ] **Step 6: Tracker issues** (`node examples/ditz2/ditz2/dist/cli/main.js`,
  per CLAUDE.md), one each, with the measurement where there is one:
  fullscreen on macOS is untested; no main menu, so Cmd-Q does nothing;
  a nil drawable aborts a Debug build inside Sokol (with 16d's
  observation); a cancelled or slow `close` makes a logout time out;
  whether GitHub's macOS runners have a display (closed by the first PR
  run); Intel Macs untested. Stage `dz/` with this commit.
- [ ] **Step 7: Format, run `check-hermes-node`, commit** ("Record the
  Metal backend, and remove the backend-less build").

**Mac verification:**

```bash
for i in 1 2 3; do
  HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu cmake --build $B --target check-hermes-node 2>&1 | tail -12
done
# expect, three times: unit tests 452 passed (TestHooksTest's ten now build here);
#   JS: 0 failed; UNSUPPORTED exactly: bundle-yargs, ui-sokol-validation-ci,
#   ui-clipboard, ui-real-events-demo
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B --show-unsupported $S/test 2>&1 | grep '^UNSUPPORTED'
# expect: those four names and no others
# the same suite with the canary's promise broken on purpose, screen locked or display asleep:
HERMES_NODE_UI_EXPECT_DISPLAY=1 utils/lit-run.sh $B $S/test/ui-display-feature.js $S/test/ui-present.js 2>&1 | tail -6
# expect: ui-display-feature FAILS and ui-present is UNSUPPORTED -- report it, or say the screen could not be locked
# the unenforced assumption Task 17 inherited: two injected events taken by one drain
for i in $(seq 1 32); do HERMES_NODE_UI_EXPECT_DISPLAY=1 utils/lit-run.sh $B $S/test/ui-events.js $S/test/ui-close.js 2>&1 | grep -c '^FAIL'; done | sort | uniq -c
# expect: "32 0"; anything else is reported with one failing trace, not fixed here
```

After the first PR run, the driver reads the macOS job's probe step and
acts as Design (g) says.

---

## Probes to run before starting

Each settles one or more `UNVERIFIED:` items. Run them on the Mac that
will run the task; none needs Accessibility or Automation consent, and
none should be given it. They are throwaway: keep them outside the
repository (`/tmp/hnui-probe`).

### P1 -- AppKit: subclass, no-activate window, in-process events (before 16b; also decides 16d and 16e)

Settles: `[NSApplication sharedApplication]` returning the subclass
instance; a window shown with `orderFrontRegardless` under the Accessory
policy with activation refused being visible to the occlusion machinery
and yielding a drawable; the frontmost application being unchanged;
`-[NSWindow sendEvent:]` delivering a synthesized key to the first
responder of a non-key window; `setContentSize:` raising
`windowDidResize:`; `performClose:` reaching `windowShouldClose:` and
leaving the window open on NO; an overridden `terminate:` being reached by
`[NSApp terminate:]` and by P4's sender; `miniaturize:` changing the
occlusion state. Written blind on Linux: fix what does not compile, and
report the fix.

```objc
// p1.m -- clang -fobjc-arc -framework Cocoa -framework QuartzCore -framework Metal p1.m -o p1
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
static int gTerm, gShould, gResize, gKey, gLinger;
static pid_t gFrontBefore;
@interface PApp : NSApplication @end
@implementation PApp
- (void)terminate:(id)sender { gTerm++; printf("TERMINATE_OVERRIDE calls=%d\n", gTerm); fflush(stdout);
  [self.windows.firstObject performClose:sender]; }
- (void)activateIgnoringOtherApps:(BOOL)flag { /* refused */ }
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)p {
  return [super setActivationPolicy:NSApplicationActivationPolicyAccessory]; }
@end
@interface PView : NSView @end
@implementation PView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e { gKey++; }
@end
@interface PDel : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property NSWindow *w; @property CAMetalLayer *layer;
@end
@implementation PDel
- (BOOL)windowShouldClose:(id)s { gShould++; return NO; }
- (void)windowDidResize:(NSNotification *)n { gResize++; }
- (void)after:(double)sec run:(void (^)(void))b {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * 1e9)), dispatch_get_main_queue(), b); }
- (void)applicationDidFinishLaunching:(NSNotification *)n {
  NSApp.activationPolicy = NSApplicationActivationPolicyRegular;      // as Sokol does
  self.w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 320, 240)
      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
      backing:NSBackingStoreBuffered defer:NO];
  self.w.releasedWhenClosed = NO; self.w.delegate = self;
  PView *v = [PView new]; self.layer = [CAMetalLayer layer];
  self.layer.device = MTLCreateSystemDefaultDevice(); v.wantsLayer = YES; v.layer = self.layer;
  self.w.contentView = v; [self.w makeFirstResponder:v]; [self.w center];
  [NSApp activateIgnoringOtherApps:YES]; [self.w makeKeyAndOrderFront:nil];  // as Sokol does
  [self.w orderFrontRegardless];                                              // ours
  [self after:1.0 run:^{
    printf("NSAPP_CLASS %s\n", class_getName([NSApp class]));
    printf("FRONT before=%d after=%d self=%d\n", gFrontBefore,
        NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier, getpid());
    printf("VISIBLE %d KEY %d ACTIVE %d\n", (int)((self.w.occlusionState & NSWindowOcclusionStateVisible) != 0),
        (int)self.w.isKeyWindow, (int)NSApp.isActive);
    printf("DRAWABLE %d\n", (int)([self.layer nextDrawable] != nil));
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:self.w.windowNumber context:nil
        characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
    [self.w sendEvent:e]; printf("KEY_VIA_WINDOW_SENDEVENT %d\n", gKey);
    [self.w setContentSize:NSMakeSize(400, 300)];
    printf("RESIZE notifications=%d content=%dx%d\n", gResize,
        (int)self.w.contentView.bounds.size.width, (int)self.w.contentView.bounds.size.height);
    [self.w performClose:nil];
    printf("PERFORMCLOSE shouldClose=%d visible=%d\n", gShould, (int)self.w.isVisible);
    [NSApp terminate:nil];
    printf("TERMINATE calls=%d shouldClose=%d alive=1\n", gTerm, gShould);
    [self.w miniaturize:nil];
    [self after:2.0 run:^{
      printf("MINIATURIZED %d VISIBLE %d\n", (int)self.w.isMiniaturized,
          (int)((self.w.occlusionState & NSWindowOcclusionStateVisible) != 0));
      [self.w deminiaturize:nil];
      [self after:2.0 run:^{
        printf("RESTORED VISIBLE %d\n", (int)((self.w.occlusionState & NSWindowOcclusionStateVisible) != 0));
        fflush(stdout);
        if (gLinger) { printf("LINGER pid=%d\n", getpid()); fflush(stdout); [self after:20.0 run:^{ exit(0); }]; }
        else exit(0);
      }];
    }];
  }];
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    gLinger = argc > 1;
    gFrontBefore = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    [PApp sharedApplication];
    [NSApplication sharedApplication];           // as Sokol does, afterwards
    PDel *d = [PDel new]; NSApp.delegate = d; [NSApp run];
  }
}
```

Run `./p1`. Wanted: `NSAPP_CLASS PApp`; `FRONT before=X after=X` with X
not this pid; `VISIBLE 1`; `DRAWABLE 1`; `KEY_VIA_WINDOW_SENDEVENT 1`;
`RESIZE notifications>=1 content=400x300`; `PERFORMCLOSE shouldClose=1
visible=1`; `TERMINATE calls=1 shouldClose=2 alive=1`; `MINIATURIZED 1
VISIBLE 0`; `RESTORED VISIBLE 1`. Report every line as printed, `KEY` and
`ACTIVE` included. If `VISIBLE` or `DRAWABLE` is 0, rerun with the two
overrides removed and report both -- that decides whether the no-activate
mode exists at all. Note by eye whether a Dock icon appeared and whether
the keyboard focus moved.

### P2 -- the accessor, the handler, maintenance arithmetic, the pool (before 16b Step 2)

Extend the two-instance offscreen probe of 2026-11-19 (worker thread plus
main thread, one `MTLDevice`). Append to each `SOKOL_GFX_IMPL` unit, after
the include, a function returning `(__bridge const void *)_sg.mtl.cmd_buffer`
and one returning `_sg.mtl.idpool.free_queue_top`. On the worker, for 2,000
frames: after `sg_end_pass` and before `sg_commit`, read the buffer,
`addCompletedHandler:` a block that records `status` and a frame counter
under a lock, commit. Report: that the accessor is null before the first
pass and after commit; that all 2,000 handlers ran with
`MTLCommandBufferStatusCompleted` and in frame order; the same under
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`. Then, with nothing in flight:
note the free count, create and destroy one image and two views, and call
`sg_commit()` with no pass until the free count returns to the noted
value; report how many commits that took (this plan says four). Finally
wrap each worker frame in `objc_autoreleasePoolPush()` /
`objc_autoreleasePoolPop()` (declared by hand as `extern "C" void
*objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *);`)
in an ARC unit, run under `OBJC_DEBUG_MISSING_POOLS=YES`, and report
whether it compiles, links, and leaves the "autoreleased with no pool"
count at zero.

### P3 -- reading a presented drawable (before 16c Step 6)

In a `sokol_app` + `sokol_gfx` Metal program (the spike's probe 2 will
do): in `init_cb`, take `((__bridge NSWindow *)sapp_macos_get_window())
.contentView.layer`, confirm it is a `CAMetalLayer`, set
`framebufferOnly = NO`, and print the value read back. Each frame: clear
the swapchain to (32, 64, 128)/255, `sg_end_pass`, then on the command
buffer from P2's accessor create a `blitCommandEncoder` and
`copyFromTexture:` the drawable's texture (the
`sapp_swapchain.metal.current_drawable` taken this frame) `toBuffer:` a
shared-storage `MTLBuffer` of `width * height * 4`, `endEncoding`, keep a
strong reference to the command buffer, `sg_commit`, `waitUntilCompleted`,
and read the first pixel. Wanted: bytes `80 40 20 ff` (BGRA), on the
first frame and on frame 100, with no message under API and GPU
validation. Report also the drawable texture's `pixelFormat` and
`storageMode`, and what happens if `framebufferOnly` is left YES (the
exact validation message), since that is the failure a future Sokol bump
would reintroduce.

### P4 -- a termination request from a second process (before 16e)

```objc
// quit.m -- clang -fobjc-arc -framework Cocoa quit.m -o quit
#import <Cocoa/Cocoa.h>
int main(int argc, char **argv) {
  @autoreleasepool {
    NSRunningApplication *a = [NSRunningApplication runningApplicationWithProcessIdentifier:atoi(argv[1])];
    printf("FOUND %d\n", (int)(a != nil));
    printf("SENT terminate %d\n", (int)[a terminate]);
  }
}
```

Run `./p1 linger &`, wait for `LINGER pid=<n>`, then `./quit <n>`.
Wanted: `FOUND 1`, `SENT terminate 1`, and P1 printing a second
`TERMINATE_OVERRIDE calls=2` while staying alive, **with no consent
dialog appearing**. Report all four facts. If a dialog appears, dismiss
it with Don't Allow, report that, and 16e uses the in-process route only.
Repeat once with P1's two activation overrides removed (a Regular
application), since `NSRunningApplication` may treat an Accessory
application differently.

### Not a probe: the first PR run

Whether GitHub's macOS runners provide a Metal device and a window-server
session cannot be measured from either machine. 16f's informational CI
step measures it, and Design (g) says what each outcome leads to.

---

## Probe results (Mac, 2026-11-19 and 2026-11-20)

Run on an Apple M4 Max, macOS 27.2, Xcode 27.1, Apple clang 21. These
settle every `UNVERIFIED:` item above; where one of them contradicts the
text of a task, this section wins.

**Confirmed.**

- `[NSApplication sharedApplication]` called later (as Sokol does) returns
  the instance of our subclass created first.
- A window shown with `orderFrontRegardless` under the two activation
  overrides (`activateIgnoringOtherApps:` refused, `setActivationPolicy:`
  forced to Accessory) is visible to occlusion tracking, yields a drawable,
  and never becomes the frontmost application: 9 of 9 runs, `isKeyWindow`
  and `isActive` false throughout. Without the overrides the probe took
  focus in 6 of 7 runs. `miniaturize:` gives `occlusionState` not visible,
  `deminiaturize:` gives visible again.
- `-[NSWindow sendEvent:]` delivers a synthesized key event to the first
  responder of a window that is not key. `setContentSize:` raises
  `windowDidResize:`. `performClose:` reaches `windowShouldClose:` and
  leaves the window open on NO, including when called from inside an
  overridden `terminate:`.
- The command-buffer accessor (`_sg.mtl.cmd_buffer`, read from a function
  defined in our own `SOKOL_GFX_IMPL` unit) is nil before the first pass,
  non-nil between `sg_end_pass` and `sg_commit` on every one of 2,000
  frames, and nil after `sg_commit`. A completion handler added in that
  window ran once per frame, 2,000 of 2,000, all
  `MTLCommandBufferStatusCompleted`, in frame order, on the queue
  `com.Metal.CompletionQueueDispatch`: a Metal thread that is neither the
  worker nor main. The same under `MTL_DEBUG_LAYER=1
  MTL_SHADER_VALIDATION=1` with no message.
- Four empty `sg_commit()` calls return the id pool's free count to its
  value before one image and two views were created and destroyed
  (`_sg.mtl.idpool.free_queue_top`). Those three resources took two pool
  slots, not three.
- `objc_autoreleasePoolPush` / `objc_autoreleasePoolPop`, declared by hand,
  compile and link from an ARC unit. Under `OBJC_DEBUG_MISSING_POOLS=YES`
  the count of objects autoreleased with no pool was 4,095 with no pools,
  100 with a pool per frame only (all from setup, resource creation and
  shutdown, which run outside the frame loop), and **zero** with a pool per
  frame plus one around every other backend entry on the worker.
- Sokol's layer is a `CAMetalLayer` with `framebufferOnly` YES as created;
  clearing it in `init_cb` reads back NO. With it cleared, a blit encoder on
  Sokol's own command buffer (the accessor) copying the drawable's texture
  to a shared `MTLBuffer` between `sg_end_pass` and `sg_commit` reads back
  the clear colour exactly on frames 1 and 100, with no validation message.
  `sg_commit` encodes `presentDrawable:` after that blit. The swapchain was
  valid from the first frame.
- **The drawable is `MTLPixelFormatBGRA8Unorm` (80), storage mode Managed.**
  A clear of (32, 64, 128, 255) reads back as bytes `80 40 20 ff`. Capture
  code must swap to the RGBA order the GL path and the tests use.
- With `framebufferOnly` left YES the copy aborts under validation with
  `-[MTLDebugBlitCommandEncoder validateCopyFromTexture:...]: failed
  assertion 'Copy From Texture Validation sourceTexture must not be a
  framebufferOnly texture.'`. That is the failure a future Sokol bump would
  reintroduce.

**Refuted.**

- `-[NSRunningApplication terminate]` from a second process does NOT reach
  `terminate:`. The quit Apple event is sent and is received by the target
  (visible with `AEDebugReceives=1`), and then reaches no handler at all:
  not the overridden `terminate:`, not `applicationShouldTerminate:`, not a
  handler installed for `kAEQuitApplication` through
  `NSAppleEventManager`. The same locked and unlocked, under the Regular
  and the Accessory policy, and in a minimal control application that does
  nothing else. **So `test/ui-terminate.js` drives `terminate:` in-process
  only** (`__driveWindow('terminate')`), which is the fallback Design (f)
  names. Whether a real Dock Quit or a Cmd-Q menu item reaches the override
  is not shown by any probe; it needs a person at the machine.

**A locked or sleeping machine.** With the console session locked and the
display asleep, a `sokol_app` program gets no frame at all: Sokol starts on
a `CADisplayLink`, which does not fire, and moves to its obscured timer only
on an occlusion change, which a window that was never visible never has. So
`init_cb` never runs. Forced onto the obscured path, every swapchain is
invalid. A bare `CAMetalLayer` still hands out drawables in that state. This
is why the `display` lit feature asks the system about the console session
before it opens anything (Task 16a, as built, below).

**Tools on that machine.** `pkill` has no `-s` option. `/usr/bin/timeout`
exists (Apple's). There is no `python`, only `python3`, and no `llvm-nm`,
only Apple's `nm`. `/usr/libexec/tmp_cleaner` deletes `/tmp` files whose
access, modification and change times are all more than three days old,
daily at midnight, and an open or locked file is not exempt.

---

## Task 16a as built: the contract for the Mac tasks

Task 16a is done on Linux. What follows is its implementer's statement of
every interface, file, definition, lit feature, substitution, check prefix
and environment variable a Metal implementer needs, checked against the
code by two reviews. Where it differs from the text of Task 16a above, this
section describes the tree.


C++ interfaces:

- `lib/compositor-gpu/backend.h`
  - `virtual void workerLoopTurn() = 0;` on `CompositorBackend`. Worker (JS) thread, once per loop turn, never while CLOSING or with a frame callback on the stack. Called from `onWorkerLoopTurn` (`tools/hermes-node-ui/ui_module.cpp`), which is a `uv_check_t` started right after `makeCurrentOnWorker()` succeeds.
  - `CompositorBackend *createCompositorBackend();`: exactly one backend file defines it.
  - `const char *compositorBackendName();`: must return `"metal"`, which is what `--version` prints.
  - `CompositorBackend::~CompositorBackend()` is still defined in `gl_backend.cpp`. **`metal_backend.mm` must define it too**, or move it to a shared file.
- `lib/compositor-gpu/present_constants.h`: `hermes::node_compat::kPresenterClearLevel` (32/255).
- `lib/compositor-gpu/sokol_gfx_ext.h`: `hnui_sg_mtl_command_buffer`, `hnui_sgp_mtl_command_buffer` (both `const void *(void)`, `__bridge`, not retained), `hnui_sg_mtl_idpool_free` / `hnui_sgp_mtl_idpool_free` (`int(void)`, `free_queue_top`) and `hnui_sg_mtl_idpool_pending` / `hnui_sgp_mtl_idpool_pending` (`int(void)`, `(front-back) mod num_slots`).
  - Defined by `lib/compositor-gpu/sokol_gfx_ext.inc` under `#if defined(SOKOL_METAL)`.
  - Included at the end of `sokol_gfx_impl.cpp` (`#define HNUI_SG_EXT(name) hnui_sg_##name`) and `sokol_gfx_present_impl.cpp` (`hnui_sgp_##name`).
  - UNVERIFIED: compiled only against a mock `_sg` on Linux, never against the real Metal `sokol_gfx.h`.
- `lib/compositor-gpu/sokol_config.h` `#error`s when `SOKOL_METAL` is defined without `__OBJC__`, or with `__OBJC__` but without ARC. It is included by `sokol_gfx_impl.cpp`, `sokol_gfx_present_impl.cpp`, `gl_*.cpp` and `tools/hermes-node-ui/sokol_app_impl.cpp`. **On macOS those Sokol units (including `sokol_app_impl.cpp`) must be compiled `LANGUAGE OBJCXX` with `-fobjc-arc`**, or must be `.mm` files.
- `tools/hermes-node-ui/platform_services.h` (namespace `hermes::node_compat`; platform thread only, every function):
  - `ClipboardResult uiPlatformReadClipboard(int timeoutMs, size_t maxBytes);`
  - `void uiPlatformWriteClipboard(const std::string &utf8);`
  - `std::string uiPlatformDescribeClipboardWrite(size_t bytesWritten);` returns the text after `command clipboard: `.
  - `bool uiPlatformWindowTitle(std::string &out);` returns false if no read-back; the trace is `command title: readback='...'` or `command title: no readback available`.
  - `std::string uiPlatformDescribeIcon();` returns the text after `command icon: `.
  - `void uiPlatformBeforeRun();` runs after `selectContextRoute` succeeds, before `sapp_run` (`uiRunPlatformLoop`).
  - `void uiPlatformAfterInit();` runs in `onInit` after `createSharedContext` succeeds, before `refreshTrackedState()`.
  - The X11 implementation is `platform_services_x11.cpp`. **macOS must provide `platform_services_mac.mm` (16e), or a stub, before 16b links the window files on macOS**: `ui_platform.cpp` calls all seven functions unconditionally.

CMake:

- Variables: `HNUI_SGP_PRODUCING_USER_OBJ`, `HNUI_SGP_PRESENTING_USER_OBJ` in `lib/compositor-gpu/CMakeLists.txt` (with PARENT_SCOPE), today `${SGP_ISOLATION_OBJ_DIR}/gl_backend.cpp.o` and `gl_present.cpp.o`. 16b sets them to `metal_backend.mm.o` / `metal_present.mm.o` inside `if(APPLE)`.
- Lit params: `sgp_producing_user`, `sgp_presenting_user`, appended in the top-level `CMakeLists.txt` under `if(TARGET hermesNodeCompositorGpu)`.
- The demo addons take `$<TARGET_PROPERTY:hermesNodeCompositorGpu,INTERFACE_COMPILE_DEFINITIONS>`, held in the variable `HNUI_ADDON_BACKEND_DEFINES`. Make `SOKOL_METAL` a PUBLIC define of `hermesNodeCompositorGpu` on macOS and the addons follow.
- The guard `if(NOT (UNIX AND NOT APPLE))` in `lib/compositor-gpu/CMakeLists.txt` and the `ui-gpu` predicate in `lit.cfg` are unchanged. 16b flips both in one commit.
- `tools/hermes-node-ui/CMakeLists.txt`: the X11 sources and `platform_services_x11.cpp` stay under `if(UNIX AND NOT APPLE)`. Add `platform_services_mac.mm` under `if(APPLE)`. The compile definition `HERMES_NODE_UI_HAVE_X11` no longer exists.
- `utils/check-sgp-isolation.py` takes `--producing-user` and `--presenting-user`, both repeatable.

`--version` (main.cpp): line 1 `hermes-node-ui <ver>`; line 2 `sokol_gfx validation: on|off` (window builds only); line 3 `gpu backend: gl|metal|none` (every build).

lit (`test/lit.cfg`):

- Features:
  - `ui-backend-gl` / `ui-backend-metal`, from `--version` line 3; a value outside {gl, metal, none} is read as none;
  - `display-expected` (ci and not darwin, or ambient `HERMES_NODE_UI_EXPECT_DISPLAY=1`);
  - `display` (now `(ok, reason)`; darwin in two stages).
- Substitutions:
  - `%ui-backend` (`gl` | `metal` | `none`);
  - `%ui-backend-expected` (`metal` on darwin, else `gl`; registered before `%ui-backend`);
  - `%gl-completion-poll` / `%gl-completion-block` (`HERMES_NODE_UI_GL_COMPLETION=poll|block` on gl, empty otherwise);
  - `%with-display` (`<python> <src>/utils/with-display.py`, used as `%with-display <seconds> CMD`);
  - `%timeout` (`timeout` on Linux, `<python> <src>/utils/timeout.py` on darwin; accepts `--foreground`);
  - `%display-reason`;
  - `%sgp_producing_user`, `%sgp_presenting_user`;
  - `%with-xvfb` is GONE.
- Check-prefix mechanism: `--check-prefixes=X,X-%ui-backend`. GL lines are under `X-gl`; the Mac agent adds `X-metal` (or `X-metal-SAME` / `-NEXT`). FileCheck errors if NONE of the selected prefixes has a directive, so every prefix the Mac run selects must have at least one line.
  - Files and prefixes now using it: `ui-present.js` (POLLERR, BLOCKERR, PROBEERR); `ui-route-refusal.js` (BOGUS, BOGUSERR, PB, PBERR, RET, RETERR); `ui-commands.js` (TRACE); `ui-abi-sizes.js` (APPLIED); `ui-conformance.js` (CHECK, TRACE).
  - For `ui-route-refusal`, a Metal `-metal-SAME` line follows the neutral `X: REJECTED` / `X: hermes-node-ui:` line.
- Script arguments: `test/ui-commands.js` takes `%ui-backend` as argv[2] (it skips the fullscreen pair on `metal`). `test/fixtures/hnui-demo/demo.js conformance <owner> <ppm> <gl|metal>` runs the mute-owner read only on gl; the Metal zero-deadline read is 16e's to add, along with its `CHECK-metal-NEXT` line in place of the gl one.
- The darwin display probe:
  - stage 1 is `utils/macos_console_session.py`'s `query()`: `ioreg -n Root -d1 -a`; the `IOConsoleUsers` entry whose `kCGSSessionUserIDKey` is `os.getuid()` (else the single on-console entry; ambiguity is refused with a reason); any truthy `CGSSessionScreenIsLocked` means locked (absent means unlocked); `kCGSSessionOnConsoleKey` must be truthy. The plist-to-list step is `_find_console_users()`, the session choice `_select_session()`;
  - stage 2 requires `ui-backend-metal` and runs `with-display.py 60 hermes-node-ui test/fixtures/hnui-demo/display_probe.js` with `HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI`, needing exit 0, `DISPLAY_PROBE_OK` on stdout and **`present: drawable acquired` on stderr**: 16d must emit exactly that trace line;
  - **UNVERIFIED on Linux: the real shape of `ioreg -a` output.** `classify()` looks for `IOConsoleUsers` at the top level of the root dict, under `IORegistryEntryChildren`, or inside a top-level array, up to 3 levels deep. The Mac round must check `python3 utils/macos_console_session.py` on an unlocked and on a locked session.
- The canary `test/ui-display-feature.js` (`REQUIRES: ui-gpu, display-expected`) goes red when either stage fails, and its FileCheck input line shows `%display-reason`.

Wrappers and environment:

- `utils/with-display.py SECONDS CMD...`: darwin takes the lock `/tmp/hermes-node-ui-display.lock`. It is machine-wide: every lit invocation, every worker, every hand run and every user of the machine shares it. `HERMES_NODE_UI_DISPLAY_LOCK` overrides the path, and only test/with-display-lock.js uses that. Two budgets (since fix round 2):
  - **SECONDS bounds only the command.** It starts once the lock is held, and expiry is 124 as before.
  - **The wait for the lock has its own budget**, `HERMES_NODE_UI_DISPLAY_LOCK_WAIT` seconds (default 1800). Exhausting it exits **125**, never 124, without running the command, and prints `with-display.py: gave up waiting for the display lock <path> after <N> s`. A non-numeric value is refused with 125 and a message.
  - Under `HERMES_NODE_DEBUG_NATIVE=UI` the wrapper prints `with-display.py: waited <t> s for the display lock` to its own stderr; nothing is printed on the normal path. The wrapper also sets `HERMES_NODE_UI_TEST_NO_ACTIVATE=1` and `HERMES_NODE_UI_TEST_PASTEBOARD=hnui-test-<digits>`, runs the command in its own process group, and exits 124 on expiry.
- `utils/timeout.py [--foreground] SECONDS CMD...`
- `utils/lit-run.sh <build dir> [lit options] <tests...>`
- Environment variables the Mac code must honour (it reads them; nothing in 16a consumes them on Linux): `HERMES_NODE_UI_TEST_NO_ACTIVATE`, `HERMES_NODE_UI_TEST_PASTEBOARD`, and `HERMES_NODE_UI_EXPECT_DISPLAY` (read by lit.cfg).

Window driver:

- `test/fixtures/hnui-demo/window_driver.js`: `startDriver({ui, driverPath, title})` resolves to `{send, onLost, stop}`; `closeOnce({ui, driverPath, title})` returns a boolean.
- The darwin branch assumes **`ui.__driveWindow(command)` is SYNCHRONOUS and returns a string**:
  - for `state` the string is `viewable=<0|1> <w>x<h>` (the wrapper prefixes `STATE `);
  - for an action, anything not starting with `ERROR`, which the wrapper turns into `SENT <verb>`;
  - a refusal starts with `ERROR` and is passed through.
- Commands: `key <c>`, `resize <w> <h>`, `close`, `state`, `miniaturize`, `deminiaturize`, `terminate`.
- This is a design choice made here (the brief left sync or async open). If 16d returns a Promise instead, only `driveLine()` in that file changes.

Trace lines added (exact): `worker loop: first turn`, `worker loop: turn skipped (closing)`, `worker loop: turn skipped (frame callback)`, each printed once at most, by the shared check callback. **`worker loop: turn skipped (frame callback)` is asserted by no test, and none can be written today.** The only nested `uv_run` under a frame callback is `process.exit()` -> `flushPendingWrites`, and that enters CLOSING first, which clears `inFrameCallback`, so the closing guard acts. Removing the frame-callback guard leaves the suite green. It is a second line of defence for Metal, and the comment at the guard says the same.

### Added in fix round 1 (each checked against the code)

- **Platform services vs. the backend-neutral checks.** A "not available" answer is acceptable only where nothing neutral checks it:
  - `uiPlatformWindowTitle`: REQUIRED. `test/ui-commands.js` `TRACE: command title: readback='commands test title'` and `test/ui-exit-paths.js` `COMMANDS-TRACE: command title: readback='early title'` are neutral, so returning false (`no readback available`) turns both red.
  - `uiPlatformReadClipboard` / `uiPlatformWriteClipboard`: REQUIRED to round-trip. `test/ui-conformance.js` neutral `CHECK-NEXT: hnui_read_clipboard_sync status=HNUI_OK text="hnui conformance"` and `TRACE: clipboard read sync #1: status=ok bytes=16`; `test/ui-exit-paths.js` READS/COMMANDS lines (these are CLOSING refusals, backend-independent). `ui-clipboard.js` is gl-only.
  - `uiPlatformDescribeClipboardWrite`: free text. Neutrally, `test/ui-abi-sizes.js` NCLIP counts exactly one `command clipboard` line, so it must print one line per write (any text). Every exact-text check is `-gl`.
  - `uiPlatformDescribeIcon`: free text. Every check on it is `-gl` (`ui-commands`). A Metal `-metal` line should pin the dock-tile read-back.
  - `uiPlatformBeforeRun` / `uiPlatformAfterInit`: no test observes them directly.
- **The FileCheck rule.** FileCheck exits 2 with `no check strings found with prefixes 'X:', 'X-metal:'` if NONE of the selected prefixes has a directive (verified with this tree's FileCheck). Every RUN whose neutral prefix has zero lines therefore needs a `-metal` line: **only `test/ui-present.js` PROBEERR**. The other per-backend prefixes all keep neutral lines: POLLERR and BLOCKERR (capture lines), BOGUS/PB/RET (`REJECTED`), BOGUSERR/PBERR (`hermes-node-ui:`), RETERR, ui-commands TRACE, ui-abi-sizes APPLIED, and ui-conformance CHECK/TRACE.
- **What `test/ui-route-refusal.js`'s neutral lines bind a backend to**, when `HERMES_NODE_UI_GL_ROUTE` is `bogus` or `pbuffer`:
  - createWindow rejects (`REJECTED`, with the message on the same line);
  - stderr carries a line starting `hermes-node-ui:` (the `fprintf` in `uiRunPlatformLoop`, printing `selectContextRoute`'s error);
  - the script's own exit statuses are 3 for bogus and pbuffer, and 5 in the return arm via `process.exitCode`, so the refusal must not exit the process itself;
  - in the return arm, `RAF 0` on the next line, and the three RETERR lines in order: `lifecycle: FAILED`, `lifecycle: event-loop handle released`, `lifecycle: runtime returned, exiting with status 5`;
  - `test/ui-exit-tty.js` additionally needs `exit status: 0` and a restored terminal under the same bogus route.
- **Placeholder arguments on macOS.** `%x11_selection_owner` and `%x11_window_driver` have no CMake target there, so lit registers `/nonexistent/x11_selection_owner-was-not-configured` (and the same for the driver). These reach `ui-conformance.js` (demo.js's owner argument), `ui-close.js` and `ui-real-events.js` (driverPath).
  - `demo.js conformance` requires a NON-EMPTY owner path and uses it only on `gl`, so the placeholder is fine as is. 16e should leave the RUN line alone, or give the Metal path its own argument.
  - The darwin branch of `window_driver.js` ignores `driverPath`, so the placeholder is harmless there.
  - Nothing should create those paths.
- **`desc.gl.major_version` / `minor_version`** stay in the shared `ui_platform.cpp`. sokol_app reads `desc.gl` only on its GL backends, so on Metal they are inert. Left in shared code deliberately rather than moved behind the seam.
- **Never run two lit invocations over one build directory at once.** They share `test_exec_root`, so each overwrites the other's `%t` files and both fail spuriously. The machine-wide display lock does NOT make that safe: it serialises windows, not lit's temporary files.
- **Exit 125 from `with-display.py` (darwin)** means one of these, and the stderr line names which:
  - `gave up waiting for the display lock <path> after <N> s` (the lock-wait budget ran out);
  - `refusing the display lock <path>: it is a symbolic link | is not a regular file | cannot be opened (...) | cannot be created (...)`;
  - `HERMES_NODE_UI_DISPLAY_LOCK_WAIT must be a finite, non-negative number of seconds, not '<v>'`.

  In none of these cases does the command run. `with-display.py --print-lock-path` prints the path the darwin branch would lock: the override if set, else `/tmp/hermes-node-ui-display.lock`.
- **The `display` probe** (`utils/display_probe.py`, called by `test/lit.cfg`) uses lock wait 120 s and a wrapper limit of 60 s. Its outer timeout is derived as `lock wait + limit + KILL_GRACE_SECONDS (5) + margin 15 = 200` s, so it never kills the wrapper while the command may be alive. If the outer timeout fires anyway, the whole session is killed: its members are enumerated in Python (see the kill order below). There is no `pkill`. Outcomes:
  - wrapper 125 after giving up the wait gives `display busy: another run held the display lock for 120 s`. That reason sets the feature `display-busy`, which makes `test/ui-display-busy.js` RED on any machine and makes the `display-expected` canary red too, with the reason printed;
  - 124 gives `display probe timed out after 60 s`;
  - the outer timeout gives `display probe killed after N s; the wrapper never returned`.
- **The probe's outer-timeout kill** (since fix round 4) goes in this order:
  1. SIGTERM to the wrapper alone, then a wait of `KILL_GRACE_SECONDS + 2`; the wrapper forwards the signal to the command's group;
  2. the session is enumerated in Python: `ps -A -o pid=` (bounded at 10 s), keeping each pid whose `os.getsid()` is the wrapper's, with `ProcessLookupError` and `PermissionError` ignored. The distinct `os.getpgid()` of the members are `killpg`ed, then each member is SIGKILLed, with ESRCH ignored. **No `pkill`**: macOS's has no `-s`, and `ps -o sess` prints 0 there;
  3. a reap of the wrapper bounded at 10 s;
  4. the session is enumerated again for up to 3 s.

  Members still alive then, or a `ps` that could not be run at all, append `the probe's session could not be killed; a window may survive` to the reason. The seam for the test is `display_probe.PS`. `%display-reason` is sanitised: anything outside `[A-Za-z0-9 _.,:;()/=+-]` becomes `?`.
- **Confirmed on the Mac (fix round 5, macOS 27.2):**
  - `O_NOFOLLOW` on a symlink gives ELOOP;
  - `O_CREAT|O_EXCL` on a dangling link gives EEXIST;
  - a non-blocking read-only open of a FIFO returns at once;
  - `flock` works on an `O_RDONLY` descriptor (EAGAIN for a second one);
  - a created file is mode 0666 under umask 077;
  - the session enumeration (`ps -A -o pid=` plus `os.getsid`) finds a leader and its `setpgrp`'d grandchild, and SIGKILLing them leaves the grandchild dead;
  - the classifier gives the right verdict on the real `ioreg` output, both unlocked and locked.
- **Still to verify on a Mac:**
  - that a second USER can lock the 0666 file created through `/tmp` (a link to `/private/tmp`);
  - how long lit's configuration takes when the display is busy (about 120 s expected);
  - that `CGDisplayIsAsleep(CGMainDisplayID())` returns nonzero for a display that really is asleep (UNVERIFIED: only False, with the display awake, was seen);
  - the tmp_cleaner refresh in practice: that `os.utime(fd)` on the held lock file updates all three times as the cleaner reads them.
- **macOS's `/usr/libexec/tmp_cleaner`** (macOS 14+, daily at midnight, `-atime +3 -mtime +3 -ctime +3` over `/tmp`) can unlink a lock file under its holder, since neither `open(O_RDONLY)` nor `flock` touches any of the three times. `with-display.py` defends in two ways:
  - after every successful `flock` it compares `fstat(fd)` with `lstat(path)` (`st_dev`/`st_ino`), and on a mismatch or a vanished path it closes, reopens and queues again, inside the same lock-wait budget and with the same refusals;
  - the holder refreshes the file's times with `os.utime(fd)` when it can (it created or owns the file). A read-only file owned by someone else is simply not refreshed: never a failure, never a chmod.

  A holder of the unlinked file and the creator of its replacement still overlap, which cannot be prevented once the file is gone; no third run joins them. Under `HERMES_NODE_DEBUG_NATIVE=UI` the wrapper also prints `waiting for the display lock <path>` once, and `the display lock <path> was replaced while waiting; reopening`, on its own stderr.
- **The real `ioreg -n Root -d1 -a` shape** (macOS 27.2): a top-level DICT with `IOConsoleUsers` directly in it (one entry) and `IOConsoleLocked` beside it (True when locked, False when unlocked).
  - In the entry, `CGSSessionScreenIsLocked` is ABSENT when unlocked and true when locked, together with `CGSSessionScreenLockedTime`.
  - The other keys seen are `CGSSessionUniqueSessionUUID`, `kCGSSessionAuditIDKey`, `kCGSSessionGroupIDKey`, `kCGSSessionIDKey`, `kCGSSessionLoginwindowSafeLogin`, `kCGSSessionOnConsoleKey`, `kCGSSessionUserIDKey`, `kCGSSessionUserNameKey`, plus `kCGSessionLoginDoneKey` and `kCGSessionLongUserNameKey`, both spelled with one S.
  - `IOConsoleLocked` is a cross-check: if it is true, the verdict is not usable with `console is locked`, whatever the entry says; if it is false it adds nothing.
- **Display-asleep signal** (`macos_console_session.display_asleep()`, combined by `combine()` only in `query()`, so `classify()` stays pure):
  - `ctypes.CDLL('/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics')`, then `CGDisplayIsAsleep(CGMainDisplayID())`, with `CGMainDisplayID` returning `uint32`, `CGDisplayIsAsleep` taking `uint32` and returning `int`;
  - nonzero turns a usable verdict into `display is asleep`;
  - any failure leaves the verdict unchanged;
  - the positive case is UNVERIFIED on a real Mac.
- **A `gpu backend: none` build** makes `display` false at once (`hermes-node-ui has no Metal backend`), before ioreg, CoreGraphics or any window is asked anything.
- **The lock's real scope**: machine-wide (see the wrapper entry above). The limit and the lock-wait budget are separate, and so are their exit statuses: 124 means the test timed out, 125 means it gave up waiting for the display. It is held by the wrapper process for exactly its lifetime and is never inherited by the command. An `flock` belongs to the open file, and the kernel drops it when the last descriptor closes, SIGKILL included, so it cannot be left stale. One consequence: if the wrapper itself is SIGKILLed while its command runs, the command (in its own process group) can outlive the lock.

Items in the brief marked UNVERIFIED that these seams depend on:

- P2: `objc_autoreleasePoolPush` / `objc_autoreleasePoolPop` via hand-written extern "C" under ARC. `workerLoopTurn` is the hook it needs.
- P1 / P4: the `HnuiApplication` subclass, and `[window orderFrontRegardless]` under no-activate. `uiPlatformBeforeRun` and `uiPlatformAfterInit` are those hooks.
- (b): the appended-accessor approach, which is not yet compiled against real Metal Sokol.

---

## Task 16b as built: what it measured, and what that corrects above

Task 16b is "Build the Metal toolchain and clear the window". Measured on
the Mac on 2026-11-20; where a statement above disagrees, this section is
right.

- **A window covered from the moment it opens keeps a valid swapchain.**
  Sokol marks the swapchain invalid only while its obscured timer runs,
  and starts that timer on an occlusion *change*. A window that was never
  visible has no change, so it gets a drawable every frame and its clear
  commits: measured on every one of 600 frames, in two runs, with
  another application's window on top throughout and `occlusionState`
  reporting not visible. So Design (f)'s "the covered one loses
  `NSWindowOcclusionStateVisible`, its swapchain goes invalid, and its
  capture never matches" is false for a window born covered, and the
  locked-screen wording in Design (e) holds only for a change that
  happens while the window exists. Serialising window tests stays, for
  the window that gets covered after it opened. Miniaturizing does
  produce a change, which is what Task 16d's occlusion test uses.
- **No test observes `orderFrontRegardless`.** With the call removed the
  window sits behind another application's and every check still passes.
  It is kept for whoever watches a run.
- **`NSApp.isActive` reads true in the real binary** while the frontmost
  application, by `lsappinfo front`, never changes. Sokol posts itself a
  synthetic application-activated event right after
  `makeKeyAndOrderFront:`, so AppKit believes it is active and the window
  server was never asked. Probe P1's "`isActive` false throughout" came
  from a probe without that event. Neither `isActive` nor key/focus state
  is evidence of anything in Tasks 16d and 16e; "did not take focus" is
  judged from `lsappinfo front` only.
- **`check-sgp-isolation` requires the producing object to reference at
  least one `sg_*` symbol**, so a producing half that only refuses fails
  it. Task 16b's worker half therefore calls `sg_isvalid()` before
  refusing. Task 16c replaces that with the real precondition in front of
  `sg_setup()`.
- **The device.** `selectContextRoute` takes
  `MTLCreateSystemDefaultDevice()` before AppKit is touched, so a machine
  with no device refuses before a window exists (Design (a) named
  `sapp_get_environment()`, which exists only after `sapp_run`). The
  presenter uses the device Sokol's own environment reports.
- **`present: first clear committed` is read back**: the frame's command
  buffer is held across the commit and the line is printed only when
  Sokol's own pointer is nil afterwards and the buffer's status is
  Committed or later.
- **The activation policy is read back** (`app: activation policy=...`,
  from `NSApp.activationPolicy` after Sokol's launch code ran); the
  `activate=` field of the install line is only the request. Nothing can
  read back the refusal of `activateIgnoringOtherApps:`.
- **A refused presenter setup leaves no presenter state**: `sokol_app`
  calls the frame callback straight after the init callback, so state
  left by an early return carried the first frame into a pass on an
  instance never set up (an assert in Debug).
- **Suite on the Mac between 16b and 16c: 17 window tests fail**, each on
  the worker half's `not built yet` rejection, because `display` is now
  true and nothing draws yet. That state must never be the head of a pull
  request: CI builds the UI option on macOS.

