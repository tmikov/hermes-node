# Design: a UI compositor for hermes-node

**Status:** Draft for review, 2026-09-18.

A native windowing and rendering layer for hermes-node, built on
[Sokol](https://github.com/floooh/sokol). The JavaScript runtime and libuv
move off the process main thread; the main thread owns the window and
presents finished frames. The two exchange input events in one direction and
a rendered surface in the other.

This is a new subsystem. It adds one field to `HermesNodeConfig` and changes
nothing else in hermes-node.

## The problem

A GUI toolkit and a Node program each want to own the thread. A window system
has an event loop that must run on a particular thread -- on macOS, the
process main thread, because AppKit requires it -- and libuv has an event
loop of its own that blocks in `epoll`/`kqueue`. hermes-node runs that loop on
the main thread today (`runHermesNode`, `uv_run(UV_RUN_DEFAULT)`).

The three ways this is normally resolved:

1. **Pump the GUI from a timer.** Rejected everywhere it has been tried.
   Electron's own write-up ("Electron Internals: Message Loop Integration",
   2016) says it "makes GUI interface response slow and occupies lots of CPU
   resources".
2. **Run both loops on one thread**, either by polling libuv's backend fd
   alongside the GUI's, or by rebuilding one loop on top of the other. NW.js
   does the second, with `MessagePumpUV` in a Chromium fork. It needs forks of
   both projects, and on macOS it still ends up with a helper thread.
3. **Keep the loops on separate threads.** Electron does this: an "embed
   thread" blocks on `uv_backend_fd()` and posts a task that runs
   `uv_run(UV_RUN_NOWAIT)` on the Chromium main thread, the two taking turns
   through a semaphore (`shell/common/node_bindings.cc`). Electron needs that
   shape because its JavaScript calls UI-thread-affine APIs (`BrowserWindow`,
   menus) and touches the DOM synchronously, so JS has to run *on* the UI
   thread.

Our constraint is different, and weaker: **nothing in the JS we run has
thread affinity to the window.** A UI toolkit like Dear ImGui is a pile of
data plus draw calls. So the loops can stay unmodified, each blocking in its
own native wait, with JS on a thread of its own.

That is the compositor model browsers use for `OffscreenCanvas`: a worker
renders whole frames, and the thread that owns the window presents them.

## The decision

- **The main thread owns the window** through `sokol_app`, forwards input,
  and presents.
- **A spawned thread runs `runHermesNode()`**, unmodified, with its own
  libuv loop, and owns every GPU call.
- **The JS thread renders into a triple-buffered offscreen surface.** The
  main thread blits the newest completed surface at every vsync.
- **Frames follow browser `requestAnimationFrame` semantics.**
- The compositor is **a separate embedder binary**, `hermes-node-ui`, which
  links hermes-node's runtime library the way `tools/hermes-node/
  bundle_main.cpp` already does.

### Why Sokol

It is the only lightweight abstraction covering every major graphics API
(GL, GLES3, Metal, D3D11, WebGPU, Vulkan) and every major OS. GLFW covers
windowing but no graphics API. Patching Sokol is acceptable, though the plan
below needs no patch for v1.

### Why not put rendering on the main thread

The alternative to handing over a surface is handing over *draw data*: the JS
thread builds a frame description and the main thread renders it. Dear ImGui
supports exactly that, and ships the helper for it
(`ocornut/imgui_club/imgui_threaded_rendering/imgui_threaded_rendering.h`:
`ImDrawDataSnapshot` plus `ImTextureQueue`, from imgui issues #1860 and
#8597). Rejected because:

- it is ImGui-specific, where a surface is universal;
- GPU resources (user textures, `ImGui::Image`) would then be created on one
  thread and used on another;
- the JS thread would still need a GPU context for anything else.

Rendering on the JS thread and presenting on the main thread keeps every GPU
API of a renderer on one thread, and makes the compositor's contract
"a finished image", which any renderer can satisfy.

## What Sokol gives us, and what it does not

Facts below were read from `floooh/sokol` master, commit `c0db757`
(2026-09-13). They drive several decisions, so they are recorded here.

- **`sokol_gfx` is one instance per process** (`static _sg_state_t _sg`), and
  every call must come from the thread owning the GPU context. floooh,
  sokol#91: "It's safe to run sokol-gfx on a different thread as long as all
  calls happen from that thread, and it's the same thread where the backend
  3D context/device lives." The per-context API (`sg_context_desc`,
  `sg_activate_context`) was removed.
- **`sokol_app` calls `frame_cb` continuously.** X11 and Win32 poll and run a
  frame every loop iteration; macOS drives frames from a `CADisplayLink`.
  There is no on-demand mode (sokol#301, open; `sapp_input_wait`, PR #640,
  not merged) and **no thread-safe way to wake the loop** -- there is no
  `glfwPostEmptyEvent` equivalent.
- **`sapp_run()` never returns on macOS** (`[NSApp run]`), and does return on
  Linux and Windows.
- **No share-context hook.** GLX, EGL and WGL contexts are all created with a
  null share context. The Metal device is public through
  `sapp_get_environment().metal.device`.
- **One window per process**, and multi-window is "not in the immediate
  future" (sokol#437).
- **Window geometry is fixed at `sapp_run()`**: width, height, `sample_count`,
  `srgb`, `high_dpi`, `fullscreen`. Only the title can change afterwards
  (`sapp_set_window_title`).
- **Linux is X11 only** (the Wayland PR, #425, was declined), Linux high-DPI
  is a documented TODO, and there is **no IME** on any platform (floooh,
  sokol#1398: "sokol_app.h is entirely 'IME neutral'").
- **Clipboard** access must be enabled (`enable_clipboard`, `clipboard_size`,
  default 8 KB, longer content "silently clipped"). On X11 a read waits for
  `SelectionNotify` for up to 100 ms.
- **Texture injection exists**: `sg_image_desc.gl_textures[]`,
  `.mtl_textures[]`, `.d3d11_texture`, `.wgpu_texture`, with native handles
  readable back through `sg_gl_query_image_info` and friends. There is no
  Vulkan injection field.
- **Nobody has shipped `sokol_app` on one thread with `sokol_gfx` on
  another.** floooh, sokol#1257: "the event loop and render loop is sort-of
  hardwired to the same thread". Our design never calls `sokol_gfx` from the
  `sokol_app` thread, which is what avoids that.

### Two copies of `sokol_gfx`

The main thread has to present, and presenting is a GPU operation, so it
needs `sokol_gfx` too. Since `sokol_gfx` is one instance per process, the
main thread gets **a second copy compiled under renamed public symbols**
(`sg_setup` -> `sgp_setup`, and so on).

This needs no patch. Compiling `sokol_gfx.h` with `SOKOL_GFX_IMPL` and the
dummy backend produces **exactly 156 external symbols, all `sg_*` functions**
(verified with `nm`); everything else, `_sg` included, is `static`. Across
every backend the only other global is a `static const` D3D11 GUID, and there
are no Objective-C classes. The rename is therefore a generated header of
`#define`s, and a build-time `nm` check enforces the property.

The alternative -- one `sokol_gfx` on the JS thread plus a hand-written blit
per backend on the main thread -- was rejected because it reintroduces
per-backend code in the one place Sokol was chosen to avoid it.

## Architecture

### Threads

**Main thread (`sokol_app`)**

- `main()` spawns the JS thread and waits on a condition variable. It does
  *not* call `sapp_run()` yet; see "Startup".
- `init_cb`: create what the JS thread will share (a GL context sharing with
  `sokol_app`'s, or the `MTLDevice`), `sgp_setup()` the presenting copy,
  signal the JS thread.
- `event_cb`: copy the event, append to the input queue, `uv_async_send`.
- `frame_cb`, once per vsync:
  1. take the newest completed surface if there is one, else keep the
     previous;
  2. blit it into the swapchain pass;
  3. if a frame was requested and none is in flight, send the tick;
  4. drain the command queue.

**JS thread (`runHermesNode()`)**

- An unmodified hermes-node runtime with its own libuv loop. `runHermesNode`
  is already documented as callable from any thread, one independent runtime
  per call.
- The embedder supplies one N-API module, loaded as `require('hermes-node:ui')`.
- On window creation the module calls `sg_setup()` against the shared context
  or device, owns the surfaces, and implements `requestAnimationFrame`.

### The four channels

| Direction | What | Mechanism |
|---|---|---|
| Main -> JS | Input and window events | Mutex-protected queue + `uv_async` |
| Main -> JS | Frame tick, with timestamp | `uv_async` |
| JS -> main | Frame requested; surface published | Atomics, polled in `frame_cb` |
| JS -> main | Platform commands | Mutex-protected queue, drained in `frame_cb` |

Nothing else crosses. **Once the window exists, the main thread never blocks
on the JS thread**, which is what makes the one synchronous JS-side call
(clipboard read) deadlock-free. The single exception is before that: at
startup the main thread waits for the JS thread to ask for a window (see
"Lifecycle"), and no JS-side call can be outstanding then, because the
window it would need does not yet exist.

### Code layout

| Path | Contents |
|---|---|
| `lib/compositor/` | Triple buffer, queues, tick rule. No GPU, no VM, unit-testable. |
| `lib/compositor-gpu/` | Per-backend sharing and completion; both `sokol_gfx` copies. `.mm` on macOS. |
| `tools/hermes-node-ui/` | `main()`, `sokol_app` callbacks, the `hermes-node:ui` module; exports `hnui_*` and `sg_*`. |
| `external/sokol/sokol` | Vendored upstream, unmodified, pinned. |

## The surface

**Format.** Three single-sample color textures, sized to the framebuffer in
pixels, in the swapchain's own color format
(`sapp_get_environment().defaults.color_format`), so the blit converts
nothing. Depth and MSAA belong to the renderer and are never shared: a
renderer wanting MSAA resolves into the surface before publishing.

**Triple buffer.** Slots are *writing* (JS), *ready*, *presenting* (main).
The three indices and a `fresh` bit live in one atomic word.

- *Publish* (JS): swap writing/ready, set `fresh`, one compare-and-swap.
- *Take* (main, in `frame_cb`): if `fresh`, swap ready/presenting, clear it.

No locks, and no surface is ever written and sampled at the same time.

**Publishing waits for the GPU.**

| Backend | Mechanism |
|---|---|
| Metal | After `sg_commit()`, enqueue an empty `MTLCommandBuffer` on `sg_mtl_command_queue()` with `addCompletedHandler`. Command buffers on one queue complete in order, so the handler runs after the frame. It publishes from Metal's callback thread; nothing blocks. |
| GL | `glFenceSync` after the frame. *Preferred:* the main thread polls `glClientWaitSync` with timeout 0 in `frame_cb` and treats the frame as unpublished until it signals -- this assumes sync objects are shared within a share group, which the spike must confirm. *Fallback:* the JS thread waits on the fence before publishing, blocking only for that frame's GPU time. |

**Presenting.** The main copy wraps each surface's native texture once, via
texture injection (`gl_textures[0]` / `mtl_textures[0]`), and draws a
full-screen triangle into the swapchain pass with linear filtering. When
sizes differ -- during a resize -- it stretches.

**Resize and texture lifetime.** On `RESIZED` the JS side allocates a new
*generation* of three surfaces before its next frame; each surface carries
its generation. The ownership rule is:

> The JS thread creates and destroys textures. It destroys a generation only
> after the main thread reports it is done with that generation.

When the main thread first takes a surface of generation N+1 it destroys its
wrappers for N and queues "released N"; the JS side then deletes those
textures. Until the new generation arrives, the old surface keeps being
stretched.

## Frames: `requestAnimationFrame`

The main thread cannot know when a frame is needed; only JS can, because a
redraw follows a state change JS made. Redraws start from input events, JS
state changes (timers, sockets, promises), animation, or a resize.

**The rule is browser rAF semantics, deliberately, so nothing surprises
anyone who has written for the web:**

- `requestAnimationFrame(cb)` sets a flag the main thread reads in
  `frame_cb`. An idle app receives no ticks at all.
- The tick is sent only if a frame was requested **and** the previous
  published surface has already been taken. If JS is still rendering at a
  vsync, that tick is skipped, exactly as browsers drop frames when their
  main thread is busy.
- All callbacks in a tick receive **the same timestamp**.
- A callback registered *inside* a callback runs on the **next** tick.
- `cancelAnimationFrame` works as on the web.
- No ticks while the window is minimized (`ICONIFIED`/`SUSPENDED` until
  `RESTORED`/`RESUMED`), like rAF in a hidden tab.
- **Rendering happens only inside a frame callback.** After the last callback
  of a tick, the module calls `sg_commit()`, arranges completion, and
  publishes. A tick in which nothing drew publishes nothing.

An earlier variant ran the callback immediately when nothing was in flight,
saving up to one refresh of latency on the first frame after input. It was
rejected: it breaks the `rAF(() => rAF(fn))` idiom for "wait one frame", and
its timestamps are not frame times.

## Input and platform services

**Input.** `event_cb` copies each `sapp_event` into our own fixed-layout
struct -- `sapp_event` itself is never exposed, so Sokol's ABI cannot break
ours. Data readable only on the main thread is attached there:
`CLIPBOARD_PASTED` carries the text, `FILES_DROPPED` the paths. Events keep
their order; consecutive mouse moves coalesce with `dx`/`dy` summed, as
browsers coalesce pointer moves. On the JS thread they are delivered to
native listeners first (a C callback, so an ImGui addon needs no JS call per
mouse move), then emitted as JS events.

**Window state** -- size, framebuffer size, DPI scale, focused, fullscreen,
minimized -- travels with the events that change it and is mirrored on the JS
side, so JS reads it synchronously and never waits. A DPI change arrives as
`RESIZED`, as in Sokol.

**Commands** (JS -> main, fire-and-forget, applied in the next `frame_cb`):
title, fullscreen toggle, icon, cursor shape, cursor visibility, pointer
lock, custom cursor images, clipboard write, quit.

**Clipboard read** is the only request needing an answer.

- JS: `readClipboard()` returns a Promise, answered through the input queue
  after the next `frame_cb`, matching `navigator.clipboard.readText()`.
- Native: a **synchronous** read, because ImGui's `GetClipboardTextFn` must
  return a value. It blocks the JS thread for up to one refresh plus, on X11,
  up to Sokol's 100 ms `SelectionNotify` wait. It cannot deadlock, because
  the main thread never waits on JS.
- `clipboard_size` is set well above Sokol's 8 KB default, which silently
  clips.

**Closing.** On `QUIT_REQUESTED` the main thread calls `sapp_cancel_quit()`
immediately and forwards a cancelable `close` event. Without a
`preventDefault()`, the module quits (see Lifecycle).

**Inherited limits, documented not fixed:** no IME, no Wayland, Linux DPI
scale read once at startup.

## Lifecycle

**Startup: JS creates the window.** Since geometry is fixed at `sapp_run()`
and a script cannot run before it, the main thread waits instead: `main()`
spawns the JS thread and blocks on a condition variable. Waiting on the main
thread before `[NSApp run]` is fine on macOS.

`createWindow({...})` sends the options to the main thread, which builds
`sapp_desc` and calls `sapp_run()`. `init_cb` prepares sharing and signals
back; the JS thread calls `sg_setup()`; the Promise resolves.

- **One window per process.** A second `createWindow`, or one after the
  window closed, throws: `sapp_run()` runs once and never returns on macOS.
- **Setup failure** rejects the Promise with the error and quits Sokol.
- **A script that never creates a window** runs normally; when
  `runHermesNode()` returns, the main thread wakes and exits with its code.

**While the window is open** the module holds a referenced libuv handle, so
the loop cannot drain, exactly as an open server socket keeps Node alive.

**Closing the window ends the process.** With no `preventDefault()`, or on
`win.close()`, the module goes through hermes-node's normal `process.exit()`
path: `'exit'` listeners run, `process.exitCode` is honoured, stdio is
flushed, the terminal is restored, then `_exit()` ends both threads.
`cleanup_cb` never runs, as with any `process.exit()` today.

The alternative -- unreference the handle and let Node's rules decide --
would leave a process running with no window whenever a timer or socket is
still active, and `sokol_app` cannot hide a window to cover that case.

## The JS API

```js
const ui = require('hermes-node:ui');
const win = await ui.createWindow({ width: 1280, height: 800, title: 'App',
                                    highDpi: true, fullscreen: false });
```

The name carries a colon, so it can never collide with an npm package.

- **Frames:** the module exports `requestAnimationFrame` /
  `cancelAnimationFrame` and, once a window exists, installs them as globals,
  because libraries test `typeof requestAnimationFrame`.
- **Events** (`win` is an EventEmitter): `keydown`, `keyup`, `char`;
  `mousedown`, `mouseup`, `mousemove`, `wheel`, `mouseenter`, `mouseleave`;
  `touchstart`, `touchmove`, `touchend`, `touchcancel`; `resize`, `focus`,
  `blur`, `minimize`, `restore`; `paste`, `drop`; `close` (cancelable).
- **Field names follow the DOM where the meaning is identical**: `code` with
  `KeyboardEvent.code` names, `repeat`, `shiftKey`/`ctrlKey`/`altKey`/
  `metaKey`, DOM button numbering. **Where Sokol differs, Sokol's shape
  wins**: a separate `char` event, and no `key` property. Faking DOM
  behaviour Sokol cannot deliver would be the larger surprise.
- **State:** `win.width`, `.height`, `.framebufferWidth`,
  `.framebufferHeight`, `.dpiScale`, `.focused`, `.fullscreen`,
  `.minimized`.
- **Commands:** `win.title = ...`, `setFullscreen`, `setIcon`, `close`,
  `setCursor` (CSS cursor names where they map), `setCursorImage`,
  `showCursor`, `lockPointer`; `ui.writeClipboard`, `ui.readClipboard`.
- **JS never calls `sokol_gfx`.** Rendering is native, through addons. A
  JS-level graphics API would be just another addon.

## The native C API

`include/hermes/node-compat/ui/hnui.h`, C ABI, prefix `hnui_`. The embedder
exports `hnui_*` and the JS thread's `sg_*` -- as hermes-node already exports
NAPI -- and ships `sokol_gfx.h` (declarations only) beside it. Addons link
against neither and resolve at `dlopen`.

**Every call takes an explicit window handle.** Several runtimes can exist in
one process (`--inspect` already makes two), only one can own the window, and
multi-window should later be an addition rather than a break. A handle also
carries its owner thread, so the thread check is "this window's thread".

```c
int   hnui_api_version(void);
void  hnui_free(void *p);

hnui_window *hnui_get_window(napi_env env);   /* NULL if this env has none */

bool  hnui_frame_target(hnui_window *w, hnui_target *out);
int   hnui_request_animation_frame(hnui_window *w, hnui_frame_cb cb, void *user);
void  hnui_cancel_animation_frame(hnui_window *w, int id);

int   hnui_add_event_listener(hnui_window *w, hnui_event_cb cb, void *user);
void  hnui_remove_event_listener(hnui_window *w, int id);
void  hnui_window_state(hnui_window *w, hnui_window_state *out);

void  hnui_set_cursor(hnui_window *w, hnui_cursor c);
void  hnui_show_cursor(hnui_window *w, bool show);
void  hnui_lock_pointer(hnui_window *w, bool lock);
void  hnui_write_clipboard(hnui_window *w, const char *utf8);
char *hnui_read_clipboard_sync(hnui_window *w);
```

- **`hnui_frame_target`** works only inside a frame callback, JS or native,
  and returns false elsewhere. It yields the surface's color attachment as an
  `sg_view`, its size, color format, `sample_count` (always 1) and
  generation, and marks the frame as drawn. The addon wraps its drawing in
  `sg_begin_pass`/`sg_end_pass` against that view.
- **Addons never call `sg_setup`, `sg_shutdown` or `sg_commit`** -- the
  module owns all three. Several renderers may draw one frame in turn; the
  first clears and the rest load, which the application arranges.
- `sg_setup` uses environment defaults of the surface color format,
  `depth_format = NONE`, `sample_count = 1`, so default pipelines in
  `sokol_imgui` and `sokol_gl` match the surface with no configuration. A
  renderer needing depth creates its own depth image and recreates it when
  the generation changes.
- **Native frame callbacks** share the tick and timestamp with JS ones and
  run in registration order among them, so an ImGui addon can keep drawing
  for a few frames after input without JS driving it.
- Native event listeners run before the JS `emit` for the same event.
- `hnui_event` and `hnui_window_state` begin with a `size` field so they can
  grow compatibly.
- Addons own every `sg_*` resource they create; the module destroys none.

**Version check.** Sokol has no ABI stability, so `hnui.h` bakes in the
vendored Sokol commit and the `hnui` API version, and
`HNUI_CHECK_VERSION(env)` in an addon's init throws a JS error naming both on
a mismatch. An `hnui` addon loaded into plain hermes-node fails to `dlopen`
with an undefined-symbol error; that is documented, not wrapped.

## Build and integration

- **`HERMES_NODE_ENABLE_UI`, default OFF**, so a plain checkout needs no
  X11/GL development packages. One CI job enables it. Linux needs X11, Xi,
  Xcursor and GL.
- **Symbol rename:** a build step generates `sgp_rename.h` from the
  `SOKOL_API_IMPL` definitions in the vendored header. A build-time `nm`
  check fails if the renamed object defines any `sg_*` symbol, or if the two
  objects share any external symbol -- catching, for instance, a new
  upstream global.
- **Runtime hook:** `HermesNodeConfig` gains
  `std::vector<EmbedderModule> embedderModules`, each `{name,
  napi_addon_register_func}`. The loader answers those names before looking
  anywhere else. Plain hermes-node leaves the list empty.
- **Command line:** `hermes-node-ui <script> [args]` and
  `hermes-node-ui --bundle=<file> [args]`, plus hermes-node's environment
  variables. hermes-node's parser lives in `hermes-node.cpp` rather than a
  library; copying it would drift, so the full flag set waits until that
  parser is factored out.
- **Bundles:** `--build-bundle` treats any `hermes-node:*` specifier as
  embedder-provided: not packaged, left to the run-time loader. Running such
  a bundle under plain hermes-node throws `MODULE_NOT_FOUND`.

## Errors

- Setup failures (GL version, context creation, sharing) reject
  `createWindow`.
- Misuse of the C API aborts with a message naming the function, in release
  builds too: wrong thread, or a frame target requested outside a frame
  callback. Corrupting GPU state silently is worse than a crash.
- A frame the GPU never completes is never published: the last surface stays
  on screen and ticks are withheld. No timeout in v1.

## Testing

**Unit (GTest, no GPU, no VM).** The triple buffer: publish/take sequences,
generation release, and a two-thread stress test asserting a surface is never
taken while being written. The tick rule: skip while in flight, request
coalescing, callbacks registered inside callbacks deferred to the next tick,
cancel, no ticks while minimized. The queues: mouse-move coalescing, command
order, clipboard request/response. Vsync is driven by the test, never by
wall-clock time.

**End-to-end (lit).** Linux CI under Xvfb with Mesa `llvmpipe`. Two lit
features gate them: `ui` (decided by running the binary, as the `wasm`
feature already is) and `display`. Two test-only hooks, enabled by
environment variables: injecting synthetic `sapp_event`s into `event_cb`, and
capturing presented frame N with `glReadPixels` after the blit, since
`sokol_gfx` has no readback. A small test addon draws a known pattern; tests
assert pixels, event order, resize generations, and `close` -> `'exit'` ->
exit code. One test drives real X input through XTest; the rest use
injection.

**Ticks, publishes, takes and generation releases are asserted from
`HERMES_NODE_DEBUG_NATIVE=UI` tracing, never from timing** -- the suite runs
16-way parallel, and both known flaky tests got that way through timing.

macOS is tested locally; whether CI macOS runners expose Metal is unverified.

## Spike, before the implementation plan

Throwaway code, go/no-go on five unknowns:

1. **GLX:** a context sharing with `sokol_app`'s (display from
   `sapp_x11_get_display()`, share from `glXGetCurrentContext()` inside
   `init_cb`, config via `glXQueryContext(GLX_FBCONFIG_ID)`), made current on
   another thread with no drawable or a 1x1 pbuffer. And whether sync objects
   are shared within a share group.
2. **Both `sokol_gfx` copies**, one renamed, live on two threads at once, GL
   and Metal.
3. **Offscreen-only frames** with no swapchain pass, and whether a render
   target wrapped from a single injected texture passes validation (Sokol
   asks for `SG_NUM_INFLIGHT_FRAMES` textures for non-immutable images).
4. **Metal:** the empty-command-buffer completion trick, and the main copy
   blitting the JS thread's texture.
5. **`llvmpipe` under Xvfb** running all of the above.

A failure changes a detail, not the design: GL completion falls back to
waiting on the JS thread, or `sokol_app` gets a share-context patch.

## What this does not do

- **No `--build-exe` for UI apps.** It needs a UI kit variant (`sokol_app`,
  the GPU backends, a UI `bundle_main`); its own round.
- **No layering.** One full-window surface. Stacked surfaces -- video under a
  UI, a cursor layer moving at vsync while JS is busy -- were considered and
  dropped as unneeded complexity.
- **No multi-window**, following `sokol_app`. The handle-based C API is what
  keeps that additive.
- **No IME, no Wayland**, following Sokol.
- **No JS-level graphics API.** Rendering is native.
- **No idle sleep.** The main thread wakes every vsync and re-blits, because
  `sokol_app` has no on-demand mode. A later patch (sokol#301's
  `sapp_skip_frame` plus a thread-safe wakeup) would change nothing above the
  compositor.
- **Windows and D3D11 are not built**, though nothing here is hostile to
  them.
