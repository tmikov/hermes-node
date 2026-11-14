# Design: a UI compositor for hermes-node

**Status:** Draft, revised 2026-09-18 after external review.

A native windowing and rendering layer for hermes-node, built on
[Sokol](https://github.com/floooh/sokol). The JavaScript runtime and libuv
move off the process main thread; the main thread owns the window and
presents finished frames. The two exchange input events in one direction and
a rendered surface in the other.

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
- **A spawned thread runs `runHermesNode()`** with its own libuv loop, and
  owns every GPU call that draws.
- **The JS thread renders into an offscreen surface** drawn from a pool of
  three; the main thread blits the newest completed surface at every frame
  callback.
- **Frames follow browser `requestAnimationFrame` semantics**, with two
  deliberate deviations recorded below.
- The compositor is **a separate embedder binary**, `hermes-node-ui`, which
  links hermes-node's runtime library the way `tools/hermes-node/
  bundle_main.cpp` already does.

### Why Sokol

It is the only lightweight abstraction covering every major graphics API
(GL, GLES3, Metal, D3D11, WebGPU, Vulkan) and every major OS. GLFW covers
windowing but no graphics API. Patching Sokol is acceptable; two of the
routes below need one.

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

## What Sokol gives us, and what it does not

Read from `floooh/sokol` master, commit `c0db757` (2026-09-13). These drive
several decisions, so they are recorded here.

- **`sokol_gfx` is one instance per process** (`static _sg_state_t _sg`), and
  every call must come from the thread owning the GPU context. floooh,
  sokol#91: "It's safe to run sokol-gfx on a different thread as long as all
  calls happen from that thread, and it's the same thread where the backend
  3D context/device lives."
- **`sokol_app` calls `frame_cb` continuously.** X11 and Win32 poll and run a
  frame every loop iteration; macOS drives frames from a `CADisplayLink`.
  There is no on-demand mode (sokol#301, open) and **no thread-safe way to
  wake the loop**.
- **Frame pacing is not guaranteed.** `_sapp_glx_swapinterval` does nothing
  when neither `EXT_swap_control` nor `MESA_swap_control` is present
  (`sokol_app.h:12953`), so the loop can run **unpaced**, not merely
  imperfectly paced; an obscured macOS Metal window is driven by an `NSTimer`
  instead (`sokol_app.h:5485`). Both matter for idle CPU and for tests.
- **`sapp_run()` never returns on macOS** (`[NSApp run]`), and does return on
  Linux and Windows.
- **Window creation failures panic before `init_cb`.** `_SAPP_PANIC` routes
  to `_sapp_log` at level 0 (`sokol_app.h:3382`), from inside `sapp_run()`
  (`sokol_app.h:12916-12930`). There is no recovery path.
- **No share-context hook.** GLX, EGL and WGL contexts are created with a
  null share context. Sokol's GLX config filter requires `GLX_WINDOW_BIT` and
  never `GLX_PBUFFER_BIT` (`sokol_app.h:12829`). `XInitThreads()` runs inside
  `_sapp_linux_run` (`sokol_app.h:14253`), i.e. only once `sapp_run()` is
  under way.
- **The Metal device is public** through `sapp_get_environment().metal.device`,
  and `sg_mtl_command_queue()` exposes **the queue only, not the frame's
  command buffer** (`sokol_gfx.h:28324`).
- **One window per process**; multi-window is "not in the immediate future"
  (sokol#437).
- **Startup-only window settings**, fixed at `sapp_run()`: width, height,
  `sample_count`, `srgb`, `hdr`, `high_dpi`, `depth_format`,
  `composite_mode`, `swap_interval`. Mutable afterwards: the title
  (`sapp_set_window_title`), fullscreen (`sapp_toggle_fullscreen`) and the
  icon (`sapp_set_icon`).
- **Linux is X11 only** (the Wayland PR, #425, was declined), Linux high-DPI
  is a documented TODO, and there is **no IME** on any platform.
- **The X11 clipboard read reports no status.** On timeout, an INCR transfer
  or oversized content it returns the *previous* buffer contents
  (`sokol_app.h:13233` onward), through the same `const char*` a success
  returns. Stale text is therefore indistinguishable from current text
  without a patch.
- **Drag-and-drop is off by default**; `max_dropped_files` defaults to 1,
  extra files are silently ignored, and one path longer than
  `max_dropped_file_path_length` (default 2048) **discards the whole drop**
  (`sokol_app.h:670-678`).
- **Texture injection exists**: `sg_image_desc.gl_textures[]`,
  `.mtl_textures[]`, `.d3d11_texture`, `.wgpu_texture`, with native handles
  readable back through `sg_gl_query_image_info` and friends.
- **GL `sg_commit()` does not flush**; `_sg_gl_commit` only clears cached
  bindings (`sokol_gfx.h:12796`).
- **Nobody has shipped `sokol_app` on one thread with `sokol_gfx` on
  another.** floooh, sokol#1257: "the event loop and render loop is sort-of
  hardwired to the same thread". Our design never calls **the JS thread's
  `sokol_gfx` instance** from the `sokol_app` thread, which is what avoids
  that; the main thread has a second instance of its own.

### Two copies of `sokol_gfx`

The main thread has to present, and presenting is a GPU operation. Since
`sokol_gfx` is one instance per process, the main thread gets **a second copy
compiled under renamed public symbols** (`sg_setup` -> `sgp_setup`).

Compiling `sokol_gfx.h` with `SOKOL_GFX_IMPL` and the dummy backend produces
exactly 156 external symbols, all `sg_*` functions (verified with `nm`);
everything else, `_sg` included, is `static`. Across every backend the only
other global is a `static const` D3D11 GUID, and there are no Objective-C
classes.

**The rename header must be included before `sokol_gfx.h`'s declarations**,
not merely before the implementation: the header also defines external C++
inline overloads (`inline void sg_setup(const sg_desc&)`, `sokol_gfx.h:5824`
onward) which would otherwise bind to the other copy and create ODR
problems. `sokol_glue.h` defines `sglue_*` globals and is compiled exactly
once, in the presenting unit, or not at all.

## Architecture

### Threads

**Main thread (`sokol_app`)**

- `main()` spawns the JS thread and waits; it calls `sapp_run()` only when JS
  asks for a window (see "Lifecycle").
- `init_cb`: create what the JS thread will share (a GL context sharing with
  `sokol_app`'s, or the `MTLDevice`), `sgp_setup()` the presenting copy,
  signal the JS thread.
- `event_cb`: copy the event, append to the input queue, `uv_async_send`.
- `frame_cb`: take a newly published surface if there is one, blit it or the
  previous one, release completed consumer reads, send the frame tick if one
  is due, drain the command queue.

**JS thread (`runHermesNode()`)**

- A hermes-node runtime with its own libuv loop, plus one embedder-supplied
  N-API module loaded as `require('hermes-node:ui')`.
- On window creation the module calls `sg_setup()` against the shared context
  or device, owns the surfaces, and implements `requestAnimationFrame`.
- **UI mode does not support `--inspect`.** `processExiting` and `exitLoop`
  are file-static in `lib/process/node_process.cpp` (529, 538) and
  `setProcessExitLoop` is last-writer-wins, so a second runtime in the
  process would leave `process.exit()` running the wrong loop. That defect is
  pre-existing and independent of this work, and **still needs a tracker
  issue of its own**; until it is fixed, a UI process runs exactly one
  runtime, and the design relies on that contract rather than on the header's
  "one independent runtime per call".

### The channels

| Direction | What | Mechanism |
|---|---|---|
| Main -> JS | Input and window events | Mutex-protected queue + `uv_async` |
| Main -> JS | Frame tick, with timestamp | `uv_async` |
| Main -> JS | Synchronous clipboard reply | Per-request slot, mutex + condition variable |
| JS -> main | Frame requested; surface published; slot state | The mutex-guarded surface table, polled in `frame_cb` |
| JS -> main | Platform commands | Mutex-protected queue, drained in `frame_cb` |

**No queue lock is ever held while dispatching a native or JS listener, or
while waiting for a clipboard reply.** Once the window exists the main thread
never waits on the JS thread; before it exists it waits only for the window
request, when no JS-side call can be outstanding.

### Code layout

| Path | Contents |
|---|---|
| `lib/compositor/` | Slot pool, generations, tick state machine, queues. No GPU, no VM, unit-testable. |
| `lib/compositor-gpu/` | Per-backend sharing, completion and blitting; both `sokol_gfx` copies. `.mm` on macOS. |
| `tools/hermes-node-ui/` | `main()`, `sokol_app` callbacks, the `hermes-node:ui` module; exports `hnui_*` and `sg_*`. |
| `external/sokol/sokol` | Vendored upstream, pinned, plus our patches (see Build). |
| `test/fixtures/hnui-demo/` | The reference addon and its entry script; built when `HERMES_NODE_ENABLE_UI` is on. |

## Surfaces

**Format.** Three single-sample color textures per generation, sized to the
framebuffer in pixels, in the swapchain's color format
(`sapp_get_environment().defaults.color_format`), created with **both**
`color_attachment` and `resolve_attachment` usage. Depth belongs to the
renderer. A renderer doing MSAA draws into its own multisampled image and
resolves into the surface.

**On GL, surface textures must use immutable storage** (`glTexStorage2D`),
and so must any texture injected into a `sokol_gfx` image that will be
sampled. Sokol turns on `gl_texture_views` for GL 4.3 and above
(`sokol_gfx.h:10505`) and its texture-view path then calls `glTextureView()`,
which requires an immutable source; a `glTexImage2D` texture trips an assert
in `_sg_gl_create_view` instead. Images sokol allocates itself are already
immutable (`sokol_gfx.h:11217`), so this is a rule for the *injected*
textures the presenting side wraps. Measured 2026-09-19; see
`docs/notes/2026-09-19-linux-spike-results.md`.

**On Metal, completion is a handler installed on Sokol's own frame command
buffer, before `sg_commit()`.** Both instances do this. The handler carries
the slot and generation it belongs to, captured before submission, and it
inspects **that same buffer's** `status`: `MTLCommandBufferStatusCompleted`
publishes (producer) or releases the read reference (consumer), and
`MTLCommandBufferStatusError` is a terminal graphics failure carrying the
buffer's `error`. Nothing waits, retries or polls.

**This requires a Sokol patch, and it is the design's only required one.**
`sg_mtl_command_queue()` exposes the queue and not the current buffer
(`sokol_gfx.h:28324`), and `_sg_mtl_commit` nils `_sg.mtl.cmd_buffer` as it
commits (`sokol_gfx.h:17218`), so the buffer is reachable only through a new
accessor called while it still exists. The patch is a few lines; see Build.

**An earlier design used a second, empty "marker" command buffer committed
after `sg_commit()`, and it is recorded here because the reason it was
dropped is a trap.** A marker infers completion of *another* buffer from its
own, which Apple documents nowhere: the SDK documents that a queue executes
buffers in order, never that two buffers' completion handlers are dispatched
in that order. Worse, `enqueue` reserves a queue slot when called rather than
when committed, and Sokol `enqueue`s its frame buffer at `begin_pass`
(`sokol_gfx.h:17167`) -- so a marker enqueued early inverts, **measured 1000
times out of 1000**, completing a full frame's GPU work before the work it
was meant to follow. A handler on the real buffer removes the second buffer,
the ordering assumption and the retry machinery together.

**A commit with no passes has no command buffer at all** (`_sg_mtl_commit`
commits only `if (_sg.mtl.cmd_buffer)`), so the maintenance commits below
install no handler and complete nothing. That case is distinguished
explicitly rather than treated as a failed completion.

**All worker-thread Metal work runs inside `@autoreleasepool`, not only
frames.** Per-frame rendering leaks nothing measurable -- but only because the
driver recycles the two rotating uniform buffers, and there *is* exactly one
un-pooled autorelease per frame. Resource creation is the case that bites:
without a pool, RSS grew linearly at ~1.2 KB per create/destroy round with no
sign of levelling.

**Resource churn needs admission control, not just commits.** Sokol defers
each Metal release to `frame_index + SG_NUM_INFLIGHT_FRAMES + 1` and collects
only inside `sg_commit()`, while `_sg.frame_index` advances only there
(`sokol_gfx.h:27506`). Creating and destroying without committing therefore
exhausts the id-pool, and exhaustion is an assert
(`_sg_mtl_alloc_pool_slot`, `sokol_gfx.h:15876`) -- "bounded by the pool
size" means bounded by a crash.

**This is a Metal-only obligation, and the rest of this subsection describes
the Metal path.** The GL backend destroys immediately -- `_sg_gl_discard_image`
calls `glDeleteTextures` (`sokol_gfx.h:11470`), and the header has no GL
release queue or id-pool anywhere -- so nothing accumulates and nothing needs
reclaiming there.

**Maintenance is internal to the module. There is no public API for it in
v1**, and that is a deliberate cut: the obligation belongs to the module's
own generation churn, which it controls, and exposing it to addons required
admission rules, an asynchronous callback and shutdown interactions with no
consumer to justify them. See "What this does not do".

- **Module-owned resources** (surfaces, views, wrappers) are created and
  destroyed only at defined points: window creation, generation change,
  shutdown. A generation's textures are released when its reference count
  reaches **zero**, which is destruction and not retirement -- retirement is
  only the act of dropping the pool's current-generation reference. Every GPU
  use has therefore finished before any release is scheduled, including reads
  taken while that generation's surface was still presenting.
- **Addons create resources at init or inside a frame callback**, where a
  commit follows. Creating and destroying at other times is **not supported
  in v1**: the module cannot intercept the exported `sg_*` calls, so an addon
  that churns outside those points will exhaust Sokol's Metal id-pool and
  assert inside Sokol. This is a contract, not a mechanism, and the C API
  section states it as one.
- **One commit reclaims nothing**, which is why the module's own generation
  churn cannot simply commit once and proceed. A release is scheduled for
  `frame_index + SG_NUM_INFLIGHT_FRAMES + 1` and `_sg.frame_index` advances
  once per `sg_commit()`, so the module issues safe commits until the frame
  index has passed that threshold -- at least `SG_NUM_INFLIGHT_FRAMES + 1`
  after the last release -- and **does not begin a further generation change
  until the previous one's releases have been collected**.
- **Progress is guaranteed, not waited for.** While maintenance is pending
  the module **suspends the submissions that prevent its safe point**: the
  main thread stops issuing new blits, keeps servicing events and platform
  commands throughout, lets outstanding completions drain, runs the
  maintenance commits, then resumes. Without that rule a visible window
  starves maintenance forever, because `frame_cb` re-presents unconditionally
  and submits its blit *before* releasing completed reads -- so a read
  reference is outstanding on every pass and "no read reference held" never
  becomes true. **PRESENTING ownership is held across the pause**: submission
  stops, presentation does not, and the last frame stays on screen.
- **The suspension is Metal-only and must not be applied on GL**, where it is
  unnecessary (destruction is immediate, per the backend split above) *and*
  would not work: `_sapp_linux_frame` swaps unconditionally after `frame_cb`
  returns (`sokol_app.h:14232`), so a frame callback that skipped its blit
  would present undefined back-buffer contents rather than preserving the
  last image. Anything that later wants to skip presenting on GL -- an idle
  optimisation, say -- has to suppress the swap itself, which `sokol_app`
  offers no way to do.
- Maintenance stays pending indefinitely only when GPU work never completes
  at all, which is a driver or hardware failure rather than something the
  compositor is itself replenishing.
- **A maintenance commit runs only at a safe point for that instance**: no
  producer GPU work outstanding, no read reference held, no completion
  handler pending, and **no open pass**. Empty commits pass no back-pressure
  -- the throttling semaphore is waited on in `begin_pass` alone -- so
  advancing the release clock is safe only when nothing is in flight to
  outrun. Maintenance serialises with that instance's own submissions, and
  **the module reaches that safe point rather than waiting for one to occur**
  -- by suspending submissions, per the rule above. On the presenting
  instance that means pausing blits; on the producer it means withholding the
  frame tick, which the tick machine already does.
- **The rule is per instance**, each having its own pool and queue, and it
  does **not** license freeing anything a still-PRESENTING surface may yet be
  blitted from: a momentary zero read count is not permission, because the
  next blit can take a new reference. **Retirement forbids new *producer*
  ownership, never new reads** -- a retired generation keeps taking read
  references for as long as its surface is the presenting one. What makes a
  generation collectable is its reference count reaching zero, which cannot
  happen while its surface is PRESENTING or RETIRING.
- The retirement sequence itself is unchanged: dropping the current-
  generation reference starts retirement, obsolete wrappers release their
  references on their trigger, and zero permits destruction. Making wrappers
  wait for zero would restore the deadlock removed earlier.

Both measured 2026-09-21 on an Apple M4 Max under API and GPU validation; see
`docs/notes/2026-09-21-macos-spike-results.md`.

### Slot states and the reuse rule

This is the core of the design; everything else is built around it.

Each slot is in exactly one of **FREE**, **WRITING**, **READY**,
**PRESENTING**, **RETIRING**, and additionally carries a **read-reference
count** for blits that have been submitted but not yet completed on the GPU.

RETIRING is the state a displaced surface occupies: it is no longer the
presenting image, but blits that sampled it may still be executing. It is
neither reusable nor presentable, and it is the reason "stopped presenting"
and "reusable" are two different things.

- **Acquire.** The producer takes a FREE slot. **If none is FREE the frame
  tick is withheld** -- this is the backpressure that paces rendering.
- **Publish.** WRITING -> READY, after the producer's GPU work has completed
  (below). The slot and its generation are captured before submission, so an
  asynchronous completion publishes exactly what it rendered.
- **Take.** READY -> PRESENTING in `frame_cb`. The successful take is the
  linearization point that defines "newest".
- **Blit.** Every blit, including a re-present of an unchanged surface,
  **acquires a read reference before submission** and releases it when that
  blit completes on the GPU.
- **Retire.** When a take installs a replacement, the outgoing slot goes
  PRESENTING -> RETIRING if it has outstanding reads, or straight to FREE if
  it has none.
- **Release.** RETIRING -> FREE when the **read-reference count reaches
  zero**. A completion handler releases a read reference; it never frees a
  slot outright, and it never frees one that is still PRESENTING.
- **Reclaim.** WRITING -> FREE covers the two cases where a frame produces no
  publication: a tick that drew nothing, and a publication dropped because
  its generation retired or the window is closing. Reclamation waits for the
  producer's own GPU work to complete, exactly as a publication would.

The full ordering chain is:

> producer GPU completion -> consumer sampling -> consumer GPU completion ->
> producer reuse

CPU-observed completion of the producer before the consumer submits its blit
establishes the first link, so no GPU-side wait is required. CPU submission,
atomic publication or elapsed frames establish nothing.

Three slots accommodate one presenting, one retired-but-still-being-read, and
one producer slot. A fourth would only improve throughput under measured
consumer latency; it is not a stronger correctness guarantee.

### Completion, per backend

| Backend | Producer completion | Consumer completion |
|---|---|---|
| Metal | A completion handler installed on Sokol's own frame command buffer, reached through the accessor patch **before** `sg_commit()` nils it, carrying the slot and generation captured before submission. `Completed` publishes; `Error` is a terminal graphics failure. | The same, on the presenting instance's frame command buffer: the handler releases the read reference. No second command buffer and no cross-buffer ordering assumption on either side -- see the Surfaces rules above for why the marker design was abandoned. |
| GL | `glFenceSync` followed by `glFlush` on the producer context. The fence is created by the producer and **ownership transfers to the main thread**, which polls `glClientWaitSync` with timeout 0 in `frame_cb` and deletes it; the producer never touches it again. | A fence inserted after the blit, polled the same way. |

`GL_WAIT_FAILED` **does not establish completion**: the slot is quarantined,
never returned to FREE, and the graphics session is terminated with an error
rather than risking a write into a texture still being read. The blocking
fallback -- the producer waiting on its own fence before publishing -- uses
the identical flush rule, and in it the producer creates, waits on and
deletes its own fence while the main thread never touches one.

**Which mode runs is decided by a startup probe, not by driver name.** After
both instances exist, the module creates a fence on the producer context
following a trivial GPU operation and polls it with timeout 0 from the main
thread. Signalled within a bounded time selects polling; a timeout or
`GL_WAIT_FAILED` selects the blocking fallback for the life of the session.
The outcome is recorded in window state, traced under
`HERMES_NODE_DEBUG_NATIVE=UI`, and forceable either way by an environment
variable for debugging and for drivers nobody here has met. `GL_WAIT_FAILED`
*during the probe* selects the fallback; during normal operation it stays
terminal, as above. This probe decides the completion mode only -- never the
context route, which is settled earlier (see Lifecycle).

Sync objects were visible across the share group on **llvmpipe and on AMD
radeonsi** (Mesa 25.2.8), so polling is expected to be the usual outcome. The
AMD measurement is the one that counts: llvmpipe returned
`GL_ALREADY_SIGNALED` on the first poll -- which exercised cross-context
access to an already-signalled sync object, but never an unsignalled-to-
signalled transition -- while radeonsi took **94,603 timeout-0 polls over
14.57 ms** before signalling, which is the behaviour this path depends on.
**NVIDIA's proprietary driver is untested and is the most likely of any to
differ**; the probe exists so that it need not be. Measured 2026-09-19 and
2026-09-21; see `docs/notes/2026-09-21-linux-hardware-spike-results.md`.

### The lock protocol

Slot states, the ready and presenting references, the per-generation
reference counts and the closing flag are guarded by **one mutex**, and every
operation holds it for the whole of its transition. The initial state is: all
slots FREE, no presenting image, "never published" set -- while that flag is
set the main thread presents a clear colour and samples nothing, because
attachment contents are initially undefined (`sokol_gfx.h:3714`).

This replaces an earlier version of this section, which put slot states and
the published index in one atomic word and moved them with a CAS retry loop;
the change is recorded as a deviation in
`docs/superpowers/plans/2026-09-21-ui-compositor.md`. What defeats separate
atomics is that the transitions are **compound**. Displacement has to read
the outgoing slot's read count and store its new state *together*: read them
apart, and a completion landing in between decrements the count to zero,
finds the slot still PRESENTING rather than RETIRING, and so frees nothing --
after which displacement stores RETIRING over a slot that has no reads left
and nobody to free it. One word could hold both fields, but only by packing
read counts into bitfields, which trades a rule that can be stated for one
that has to be maintained.

**The cost is real, and it is not "one uncontended lock".** Three threads
reach this table -- the JS producer, the presenting thread, and whichever
thread a GPU completion handler runs on -- so these are several short but
genuinely contended critical sections per frame per thread, not a lock taken
by one thread at a time. What is bounded is their content: each is a handful
of field updates over a fixed-size table, with no allocation, no I/O and no
GPU call inside, and generation destruction is handed to the owner dispatcher
**after** the lock is released rather than run under it.

**Publication is one synchronized decision with retirement and closing.** A
completion handler cannot test "is my generation still live?" and then
publish, because the generation can retire in between. The test and the state
change happen under the same lock; a publication that loses drops the frame
and releases its captured ownership through the normal completion path,
exactly as a successful one would.

An operation spanning two of these transitions -- publishing a frame and
moving the tick state machine together, say -- runs inside a single critical
section through a transaction entry point, rather than by calling two
operations in turn, which would reopen exactly the gap this section exists to
close.

### Generations and texture lifetime

A resize allocates a new *generation* of three slots. Lifetime is a
**per-generation reference count**, whose owners are:

- CPU writing ownership, acquired at slot acquisition;
- producer GPU work, acquired **before** submission;
- a pending completion handler, from before submission until its last access;
- the READY slot;
- the PRESENTING slot;
- each outstanding consumer read;
- each wrapper held by the presenting copy.

There is one further owner: **the current-generation reference**, which the
pool itself holds. It exists so the count is never zero for a live
generation: a freshly
allocated generation has all slots FREE and none of the other owners, so
without it the first acquisition would be forbidden by the rule below.
Retiring a generation is exactly the act of dropping that reference.

A reference is acquired before work is scheduled and held until the callback
finishes or transfers its reference onward -- a completion handler transfers
its reference to READY ownership before releasing its own, so the count
cannot reach zero while a handler is still running.

**Retirement forbids new producer ownership, not existing readers.** A
retired generation accepts no new slot acquisition and no new publication,
but its still-presenting surface keeps being blitted, and each of those blits
takes a read reference as usual, until a surface of the current generation
replaces it. Without that distinction the screen would have to go blank at
every resize.

**Obsolete wrappers are released on a trigger, not by waiting.** The
presenting instance destroys its wrappers for a generation when that
generation stops being current *and* has no PRESENTING or RETIRING slot left;
waiting for the reference count to reach zero first would deadlock, since the
wrappers are themselves counted owners.

A generation is destroyed when its count reaches zero, which now implies it
is no longer current; that covers a generation created during a burst of
resizes and never presented. **Destruction always runs on the JS owner
thread**, whichever thread dropped the last reference; the main thread and
Metal callback threads post it there.

### Presenting

The main copy wraps each surface's native texture once per generation, via
texture injection (`gl_textures[0]` / `mtl_textures[0]`), and draws a
full-screen triangle into the swapchain pass with linear filtering. When
sizes differ -- during a resize -- it stretches. On macOS an obscured window
returns `sapp_swapchain.invalid` (`sokol_app.h:14872`); such a frame presents
nothing, and its read references are released without a blit.

## Frames: `requestAnimationFrame`

Only JS can know a frame is needed, because a redraw follows a state change
JS made. Redraws start from input events, JS state changes, animation, or a
resize.

**The tick state machine:** IDLE -> TICK_QUEUED -> CALLBACKS_RUNNING ->
GPU_PENDING -> PUBLISHED_AWAITING_TAKE -> IDLE.

- `requestAnimationFrame(cb)` sets a flag the main thread reads in
  `frame_cb`. An idle app receives no ticks.
- A tick is sent only when a frame is requested, the machine is IDLE, and a
  FREE slot exists.
- A tick in which nothing drew, or whose callbacks were all cancelled,
  returns to IDLE immediately, so non-rendering rAF chains never stall.
- **GPU_PENDING -> IDLE without publication** is a defined transition, taken
  when a publication is dropped (retired generation, or CLOSING). Otherwise a
  dropped frame would wait in PUBLISHED_AWAITING_TAKE for a take that can
  never come. Requests recorded while it was pending survive the transition,
  so the next eligible frame callback still runs.
- Requests made in any state are recorded and served at the next eligible
  frame callback.
- **An exception never short-circuits GPU bookkeeping.** If drawing was
  already submitted when a callback throws, the frame's GPU work completes,
  or is explicitly discarded, before ownership is released; the machine never
  jumps straight to IDLE.
- All callbacks in a tick receive the same timestamp: monotonic milliseconds
  sharing `process.uptime()`'s origin. Not `performance.now()`'s: this
  runtime has no `performance` global at all, and `process.uptime()` is
  the monotonic clock it does have with a defined origin, so a callback can
  compare its timestamp against `process.uptime() * 1000` directly.
- A callback registered inside a callback runs on the next tick. Cancelling a
  later callback in the current batch prevents it.
- No ticks while the window is minimized.
- **Rendering happens only inside a frame callback.** After the last callback
  of a tick the module calls `sg_commit()` and arranges completion.

**Exceptions follow the runtime's existing rule, which is fatal by default.**
`process._fatalException` (`libjs/process-events.js:82`) emits `'exit'` and
answers false when no listener takes the error, and
`triggerUncaughtException` (`lib/bindings/node_errors.cpp:55`) then exits. So
"the remaining callbacks still run" applies **only** when a listener handled
the exception and the runtime stays open; otherwise the process is going away
and the batch ends. Exceptions from the microtask checkpoints between
callbacks take the same path.

**Two deliberate deviations from browser rAF**, stated as such:

1. Ticks are paced by the main thread signalling that it took a surface,
   rather than by the rendering thread being the presenting thread.
2. **Presentation backpressure:** a tick is withheld while no slot is FREE.
   A slow GPU therefore delays even rAF callbacks that would not have drawn.
   Browsers have no equivalent rule.

Not a deviation, but worth recording: the first frame after idle waits for a
tick rather than running immediately. That is what browsers do. Running it
immediately would save up to one refresh of latency and was rejected, because
it breaks `rAF(() => rAF(fn))` as "wait one frame" and gives that callback a
timestamp that is not a frame time.

## Input and platform services

**Input.** `event_cb` copies each `sapp_event` into our own fixed-layout
struct -- `sapp_event` is never exposed, so Sokol's ABI cannot break ours.
Data readable only on the main thread is attached there: `CLIPBOARD_PASTED`
carries the text, `FILES_DROPPED` the paths. Events keep their order;
consecutive mouse moves coalesce with `dx`/`dy` summed. On the JS thread they
reach native listeners first (a C callback, so an ImGui addon needs no JS
call per mouse move), then the JS `emit`.

**Window state** -- size, framebuffer size, DPI scale, focused, fullscreen,
minimized -- travels with the events that change it and is mirrored on the JS
side. A DPI change arrives as `RESIZED`.

**Commands** (JS -> main, fire-and-forget, applied in the next `frame_cb`):
title, fullscreen toggle, icon, cursor shape, cursor visibility, pointer
lock, custom cursor images, clipboard write, quit.

**Clipboard read.**

- Sokol's X11 getter cannot distinguish failure from success and returns its
  previous buffer on timeout, INCR or oversize. **v1 therefore either patches
  `sokol_app` to return a status, or performs its own X11 read.** Returning
  possibly-stale text as the current clipboard is not acceptable, and a
  request sequence number cannot detect it -- sequence numbers only prevent
  misassociating a reply.
- JS: `readClipboard()` returns a Promise, answered through the input queue.
- Native: a **synchronous** read, because ImGui's `GetClipboardTextFn` must
  return a value. It uses its own per-request slot with a mutex and condition
  variable signalled directly by the main thread; a `uv_async` reply could
  never be serviced by the blocked JS thread.
- **It takes a deadline and can return a timeout error.** The main thread can
  itself block in presentation (Metal waits on a semaphore), so an
  unqualified "interruptible" promise would have no one left to deliver the
  interruption. A reply that arrives after the waiter has given up is
  **discarded**, not delivered. The registry holds its slots weakly, so an
  abandoned slot does not stay addressable; what makes a late reply safe is
  that the reply path takes a strong reference under the registry lock
  before writing, so it either lands in storage that is still alive or is
  dropped without touching anything. Nothing downstream may assume a
  cancelled request's slot is still there.
- Shutdown and setup failure complete outstanding requests with an error.
- `clipboard_size` is set well above Sokol's 8 KB default.

**Drag-and-drop** is enabled explicitly, and the public contract states the
file-count limit, the path-length limit, and that one oversized path discards
the entire drop.

**Closing.** On `QUIT_REQUESTED` the main thread calls `sapp_cancel_quit()`
and forwards a cancelable `close` event.

**Inherited limits, documented not fixed:** no IME, no Wayland, Linux DPI
scale read once at startup.

## Lifecycle

**Startup states:** REQUESTED -> RUNNING, or REQUESTED -> FAILED; RUNNING ->
CLOSING -> CLOSED.

- `main()` spawns the JS thread and waits on a condition variable. Waiting
  before `[NSApp run]` is fine on macOS.
- **The wait predicate is "a window was requested **or** the runtime
  finished".** A script that never loads the UI module runs to completion
  like any hermes-node program; the JS thread signals the same condition
  variable on its way out, and `main()` exits with the runtime's code without
  ever calling `sapp_run()`.
- **The referenced libuv handle is taken when `createWindow` is called**, not
  when the window opens, so the runtime cannot drain during creation.
- `createWindow({...})` sends the options; the main thread builds `sapp_desc`
  and calls `sapp_run()`. `init_cb` prepares sharing and signals back; the JS
  thread calls `sg_setup()`; the Promise resolves.
- **Two decisions are staged, and neither may fall back into the other.**
  *Stage 1, before `sapp_run()`:* the **context route** -- surfaceless by
  default, with the pbuffer route and the config-filter patch as
  contingencies. It cannot be decided later: Sokol ranks its framebuffer
  config and creates its context inside `sapp_run()`
  (`sokol_app.h:14272-14274`), long before `init_cb` is first called from the
  frame path. So the route is a compiled-in choice plus a startup option, and
  a configuration for which the built route fails is **refused with a
  message**, not silently degraded. *Stage 2, after both instances exist:*
  the **GL completion mode**, by the probe described under Surfaces. A fence
  failure never changes the drawable route, and a drawable failure never
  selects blocking completion.
- **One window per process.** A second `createWindow`, or one after the
  window closed, throws.

**Failure splits in two:**

- **Sokol's own window or context creation failing is fatal by contract.** It
  panics from inside `sapp_run()`, before `init_cb`, with no recovery path.
  We install a logger that prints the item and **terminates with a non-zero
  status**; returning from a logger does not make Sokol stop.
- **Our additional shared-context creation failing rejects the Promise.** The
  main thread posts the rejection and keeps running its loop, quitting only
  when JS asks, so macOS termination cannot pre-empt delivery.

**Abnormal return of the runtime.** The entry script can throw after calling
`createWindow`: step 14 of `runHermesNode` runs the loop only when
`exitCode == 0`, so cleanup proceeds with referenced handles outstanding.
Waiting for `runHermesNode()` to return is too late, and may never happen --
`UvEventLoop::close()` runs `uv_run(UV_RUN_DEFAULT)`
(`lib/event-loop/uv_event_loop.cpp:354`), which a live UI handle would keep
alive forever. So the module registers a **lifecycle hook that runs before
runtime cleanup**: it revokes cross-thread access with a synchronized sender
shutdown, closes the UI handle, and tells the main thread to tear the window
down and exit with the runtime's code.

**CLOSING gates every exit path.** Entering CLOSING refuses further frame
callbacks and event dispatch, fails pending synchronous clipboard requests,
and makes platform commands unavailable to `'exit'` listeners. It is entered
on **all three** paths: `win.close()` (or an uncancelled `close` event), an
explicit `process.exit()`, and the fatal-exception path. This is required
because `flushPendingWrites` runs `uv_run(UV_RUN_NOWAIT)` up to 64 times
(`lib/process/node_process.cpp:544`) and would otherwise dispatch UI work
while `'exit'` listeners are tearing down addon state.

**Closing the window ends the process** through hermes-node's normal
`process.exit()` path: `'exit'` listeners run, `process.exitCode` is
honoured, stdio is flushed, the terminal is restored, then `_exit()` ends both
threads. Outstanding Promises and native operations are abandoned by that
`_exit()`; `cleanup_cb` never runs.

## The JS API

```js
const ui = require('hermes-node:ui');
const win = await ui.createWindow({ width: 1280, height: 800, title: 'App',
                                    highDpi: true, fullscreen: false });
```

The name carries a colon, so it cannot collide with an npm package.

- **Frames:** the module exports `requestAnimationFrame` /
  `cancelAnimationFrame` and, once a window exists, installs them as globals,
  because libraries test `typeof requestAnimationFrame`.
- **Events** (`win` is an EventEmitter): `keydown`, `keyup`, `char`;
  `mousedown`, `mouseup`, `mousemove`, `wheel`, `mouseenter`, `mouseleave`;
  `resize`, `focus`, `blur`, `minimize`, `restore`; `paste`, `drop`; `close`
  (cancelable).
- **Field names follow the DOM where the meaning is identical**: `code` with
  `KeyboardEvent.code` names, `repeat`, `shiftKey`/`ctrlKey`/`altKey`/
  `metaKey`, DOM button numbering. Where Sokol differs, Sokol's shape wins: a
  separate `char` event, and no `key` property.
- **State:** `win.width`, `.height`, `.framebufferWidth`,
  `.framebufferHeight`, `.dpiScale`, `.focused`, `.fullscreen`, `.minimized`.
- **Commands:** `win.title = ...`, `setFullscreen`, `setIcon`, `close`,
  `setCursor`, `setCursorImage`, `showCursor`, `lockPointer`;
  `ui.writeClipboard`, `ui.readClipboard`.
- **JS never calls `sokol_gfx`.** Rendering is native, through addons.

## The native C API

`include/hermes/node-compat/ui/hnui.h`, C ABI, prefix `hnui_`. The embedder
exports `hnui_*` and the JS thread's `sg_*` -- as hermes-node already exports
NAPI -- and ships `sokol_gfx.h` (declarations only) beside it.

**Every call takes an explicit window handle.** Several runtimes can exist in
a hermes-node process, only one can own a window, and multi-window should
later be an addition rather than a break. The handle carries its owner
thread.

```c
int   hnui_api_version(void);
const char *hnui_sokol_commit(void);          /* the host's vendored Sokol commit */
void  hnui_free(void *p);

hnui_window *hnui_get_window(napi_env env);   /* NULL if this env has none */

bool  hnui_frame_target(hnui_window *w, hnui_target *out);
int   hnui_request_animation_frame(hnui_window *w, hnui_frame_cb cb, void *user);
void  hnui_cancel_animation_frame(hnui_window *w, int id);

int   hnui_add_event_listener(hnui_window *w, hnui_event_cb cb, void *user);
void  hnui_remove_event_listener(hnui_window *w, int id);
bool  hnui_event_dispatch_quiescent(hnui_window *w);  /* no native listener on the stack */
void  hnui_get_window_state(hnui_window *w, hnui_window_state *out);

void  hnui_set_cursor(hnui_window *w, hnui_cursor c);
void  hnui_show_cursor(hnui_window *w, bool show);
void  hnui_lock_pointer(hnui_window *w, bool lock);
void  hnui_write_clipboard(hnui_window *w, const char *utf8);
int   hnui_read_clipboard_sync(hnui_window *w, int timeout_ms, char **out);
```

- **`hnui_frame_target`** returns false outside a frame callback; only a
  wrong-thread call aborts. It yields **both** a color-attachment view and a
  resolve-attachment view for the current slot, plus size, color format,
  `sample_count` and generation, and marks the frame as drawn. Sokol
  distinguishes the two view kinds (`sg_attachments.colors[]` versus
  `.resolves[]`), and an MSAA renderer needs the resolve view.
- **Borrowed views are valid only for the callback that obtained them.** They
  must not be retained, destroyed, or used after it returns.
- **Addons never call `sg_setup`, `sg_shutdown` or `sg_commit`**, and never
  call `sg_*` from another thread. Addons are trusted code: **the abort
  guarantee covers checked `hnui_*` entry points only**, and out-of-contract
  use of the exported `sg_*` symbols is undefined behaviour.
- Several renderers may draw one frame in turn; the first clears and the rest
  load.
- `sg_setup` uses environment defaults of the surface color format,
  `depth_format = NONE`, `sample_count = 1`, so default pipelines in
  `sokol_imgui` and `sokol_gl` match with no configuration.
- **Native frame callbacks** share the tick and timestamp with JS ones and
  run in registration order among them. Cancellation takes effect
  immediately, including for a later callback in the current batch.
- **Event-listener removal is deferred** to after the current dispatch
  completes -- which is not the same rule as frame-callback cancellation, and
  is stated separately. A listener removed during an event's delivery still
  receives that event and no later one (including the next of the same
  batch); removing an unknown id, or the same id twice, is a no-op. A removed
  listener's user data may be freed once `hnui_event_dispatch_quiescent`
  returns true: it is false inside a native listener and true inside a frame
  callback.
- **The four command entry points are `void`**, so a refused argument
  (`hnui_set_cursor` outside `HNUI_CURSOR_DEFAULT..HNUI_CURSOR_NOT_ALLOWED`;
  `hnui_write_clipboard` with a NULL string or more than
  `HNUI_CLIPBOARD_MAX_BYTES - 1` bytes) prints one line to stderr naming the
  function and the value, and pushes nothing.
- `hnui_event` and `hnui_window_state` begin with a `size` field set by the
  caller. A size below the stated minimum is **rejected**; otherwise the
  implementation writes at most `size` bytes and at least the minimum.
  `hnui_target` follows the same rule.
- `hnui_read_clipboard_sync` returns a status; the string is owned by the
  caller and freed with `hnui_free`.
- Addons own every `sg_*` resource they create, and **create and destroy them
  at init or inside a frame callback**, where a commit follows. Doing so at
  other times is out of contract in v1: on Metal it exhausts Sokol's id-pool,
  which asserts rather than failing gracefully, and the module cannot
  intercept the exported `sg_*` calls to prevent it. (On GL destruction is
  immediate and no pool exists, but the contract is written once for both
  backends so an addon needs one rule.) A public entry point for
  out-of-frame churn was specified and then cut; see "What this does not do",
  and the reasoning under Surfaces for why one commit reclaims nothing.

**Version check.** Sokol has no ABI stability, so `hnui.h` bakes in the
vendored Sokol commit and the `hnui` API version, and
`HNUI_CHECK_VERSION(env)` throws a JS error naming both pairs (the addon's
and the host's, from `hnui_api_version()` and `hnui_sokol_commit()`) on a
mismatch. The commit is written once, as `HNUI_SOKOL_COMMIT_PINNED` in
`hnui.h`; the CMake variable is read from it and `test/sokol-pin.js` checks
it against `external/sokol/README.md`. An
`hnui` addon loaded into plain hermes-node fails to `dlopen`.

## The reference addon

**`test/fixtures/hnui-demo/` ships in v1 and is part of the deliverable**,
not a sample written afterwards. Without it, the first thing anyone could
render would have to be written from scratch against an API that had never
had a consumer, and an API with no consumer is usually subtly unusable.

**It is a test fixture, not an `examples/` entry**, which is a deliberate
departure from where a demo would normally go. `examples/` here means an npm
project: `examples/run-examples.sh` skips any directory without a
`node_modules`, so a dependency-free addon placed there would be permanently
reported as not installed. The precedent it follows instead is `hello_addon`
(`CMakeLists.txt:128`), a `.node` addon built by our own CMake from
`test/fixtures/test-addon/`. Its build is conditional on
`HERMES_NODE_ENABLE_UI`, its graphical tests are gated on the `ui` and
`display` lit features, and the built addon's path is passed to the entry
script rather than resolved by `require()` from a package.

It is deliberately small -- on the order of 150 lines of C plus a short entry
script -- and uses **`sokol_gl` only, never ImGui**:

- `demo.js` loads `hermes-node:ui`, calls `createWindow`, loads the addon,
  and runs an ordinary JS `requestAnimationFrame` loop that calls into it.
  That is the common shape, so it is the one demonstrated by default.
- The addon registers itself, and on each frame calls `hnui_frame_target`,
  begins a pass against the returned color view, clears, and draws a rotating
  triangle sized from the target's reported dimensions. It never calls
  `sg_setup`, `sg_shutdown` or `sg_commit`.
- A key event, delivered through `hnui_add_event_listener`, changes the clear
  colour, which exercises the input path end to end.
- Resize needs no special handling: the target reports its size and
  generation every frame, which is the point of that being per-frame data.
- A flag switches it to drive frames through `hnui_request_animation_frame`
  instead, so the native callback path has a consumer too. **Lit runs both
  modes**, not just the default one.
- **A conformance mode** calls the entry points the visual demo does not, once
  each, and prints what it observed: `hnui_cancel_animation_frame`,
  `hnui_remove_event_listener` (including removal during dispatch, and when
  its user data becomes safe to free), `hnui_get_window_state`,
  `hnui_set_cursor`, `hnui_show_cursor`, `hnui_lock_pointer`,
  `hnui_write_clipboard`, `hnui_read_clipboard_sync` (a normal read and a
  timeout), and `hnui_free`. It also renders one frame through the
  **resolve view** with an MSAA image of its own, which is otherwise the one
  part of `hnui_frame_target` nothing exercises. This mode is textual and
  runs under lit; it is not part of what a person sees when running the demo
  by hand.

It has three jobs:

1. **A smoke test a person can run**:
   `hermes-node-ui test/fixtures/hnui-demo/demo.js <addon path>` shows a
   window with a moving triangle.
2. **The worked example for `hnui.h`.** It is written *during* implementation,
   before the C API is frozen: if the demo is awkward to write, the API is
   wrong and changes while it still can.
3. **The artifact the end-to-end tests capture.** The capture hook needs
   something whose output is known, and this is it.

**Not in v1: an ImGui addon.** That is the real target and the reason the
input, cursor and clipboard paths exist, but it brings ImGui vendoring and
its own build questions. With the conformance mode above, every `hnui_*`
entry point has a caller in v1 -- but "has a caller" is not "is pleasant to
build a real UI on". **The ergonomics of the C API stay unproven until an
ImGui addon exists**, and that is the risk accepted by deferring it.

## Build and integration

**Sokol** is vendored at `external/sokol/sokol`, pinned, with our patches
carried alongside; the `external/` convention expects unmodified upstream, so
the patch-carrying mechanism is settled by the first patch, which this design
requires rather than anticipates:

- **Required: a Metal command-buffer accessor.** The completion protocol
  installs its handler on Sokol's own frame command buffer, which nothing
  public exposes. A few lines, and it must also appear in the rename list so
  the presenting instance gets its own copy.
- **Contingent: the GLX framebuffer-config filter** (`GLX_PBUFFER_BIT`),
  needed only where the surfaceless route fails -- neither driver measured so
  far needs it, and stage 1 of startup decides.
- **Contingent: a status-returning clipboard read**, so a failed X11 read is
  distinguishable from a stale buffer.

**The rename check** generates `sgp_rename.h` from the `SOKOL_API_IMPL`
definitions and verifies with `nm` that neither copy references the other's
instance. It distinguishes forbidden references to the other instance from
legitimate shared imports and compiler-generated helpers, defines its
mangling rules, and runs on the **real GL and Objective-C++ builds** and
under LTO -- not only the dummy backend.

**CMake targets**

| Target | Contents |
|---|---|
| `hermesNodeCompositor` | Slot pool, generations, tick machine, queues. No GPU, no VM. |
| `hermesNodeCompositorGpu` | Per-backend sharing, completion, blitting; both `sokol_gfx` copies. |
| `hermes-node-ui` | `main()`, the `sokol_app` callbacks, the module; exports `hnui_*` and `sg_*`. |

Behind `HERMES_NODE_ENABLE_UI`, **default OFF**. Linux needs X11, Xi, Xcursor
and GL.

**Integration is more than one field.** The full list:

1. `HermesNodeConfig` gains `std::vector<EmbedderModule> embedderModules`
   (`{name, napi_addon_register_func}`).
2. The ordinary loader resolves those names before anything else.
   `BindingRegistry` backs `internalBinding`, not `require`, so this is a new
   route rather than a reuse.
3. The bundle producer's classifier must pass `hermes-node:*` through:
   `isBuiltinSpecifier` strips only `node:` and checks a fixed set
   (`lib/bundle/bundle_resolve.cpp:406`).
4. The bundle consumer intercepts builtins separately
   (`libjs/bundle-loader.js`) and otherwise enforces its closed world, so it
   needs the same passthrough.
5. `require.resolve()` behaviour and the error for a missing embedder module
   are defined alongside.

**Command line:** `hermes-node-ui <script> [args]` and
`hermes-node-ui --bundle=<file> [args]`, plus hermes-node's environment
variables. The full flag set waits until hermes-node's parser is factored out
of `hermes-node.cpp`.

## Errors

- Sokol window/context creation failure: fatal, non-zero exit.
- Our shared-context creation failure: rejects `createWindow`.
- `GL_WAIT_FAILED`: the slot is quarantined and the graphics session ends.
- Misuse of a checked `hnui_*` call from the wrong thread: abort naming the
  function. Out-of-frame target queries return false.
- A frame the GPU never completes is never published; the last surface stays
  on screen and ticks are withheld. No timeout in v1.

## Testing

**Unit (GTest, no GPU, no VM).** The slot pool: acquire/publish/take/release
sequences, read-reference accounting, publication racing retirement and
closing, and a two-thread stress test of the state machine. Generations:
reference transfer from completion handlers, abandonment of never-presented
generations, destruction on the owner thread. The tick machine: withheld
ticks, empty ticks, cancelled batches, exception paths, requests recorded
mid-state. The queues: coalescing, ordering, clipboard request/reply and
timeout. Vsync and GPU completion are both driven by the test.

**End-to-end (lit).** Linux CI under Xvfb with Mesa `llvmpipe`, which needs
its display and driver-selection environment propagated explicitly. Two lit
features gate these tests: `ui` and `display`. **This branch's `test/lit.cfg`
defines only `examples-installed` and `linker-available`**, so the capability
probe is written as part of this work rather than copied.

Test-only hooks, enabled by environment variables: injecting synthetic
`sapp_event`s into `event_cb` -- which exercises the input path only, since
it does not resize the real framebuffer -- and capturing a presented frame.
**Capture is keyed to a publication id and acknowledged**, never to an
absolute frame number, because blits can precede any publication.

**What draws in these tests is the reference addon**
(`test/fixtures/hnui-demo/`, above), in a mode that renders a fixed pattern
rather than a rotating one.

**A fixed pattern is not by itself enough to make captured pixels
deterministic**, because the same publication can be presented at a different
framebuffer size, and the blit stretches with linear filtering when the sizes
differ. So a capture request carries, and the capture is only taken when they
agree: the target generation, the target's dimensions, the actual framebuffer
dimensions and DPI scale, and the input state the frame was drawn from.
Transitional stretching is asserted separately, as its own case, rather than
being something the ordinary assertions have to tolerate.

Comparison is by policy, not by golden image: solid interior regions are
compared exactly, and rasterized or filtered edges with a tolerance, with the
pixel format and any colour conversion stated by the test. Naming `llvmpipe`
does not make a byte-for-byte golden image portable.

The end-to-end tests assert that output, the order of the addon's events,
that a resize produces a new generation, that both frame-driving modes work,
the conformance mode's printed results, and that closing the window runs
`'exit'` and yields the expected status.

GPU exclusion cannot be established by a CPU-level stress test, so the
backend tests use controllable producer and consumer completion, covering
empty ticks, abandoned generations, quarantined slots and shutdown.
`glReadPixels` synchronizes and can mask races, so it is a correctness check
of content, not of ordering. Ticks, publishes, takes and releases are
asserted from `HERMES_NODE_DEBUG_NATIVE=UI` tracing, never from timing.

macOS is tested locally; whether CI macOS runners expose Metal is unverified.

## Spike, before the implementation plan

Throwaway code, go/no-go. **Every item has been run at least in part, and
nothing found blocks the design**, but the coverage is uneven and item 3 is
split because of it. Linux, 2026-09-19, llvmpipe under Xvfb
(`docs/notes/2026-09-19-linux-spike-results.md`); Apple M4 Max under API and
GPU validation, 2026-09-21 (`docs/notes/2026-09-21-macos-spike-results.md`);
AMD Radeon 780M, 2026-09-21
(`docs/notes/2026-09-21-linux-hardware-spike-results.md`). What they changed
is recorded in the sections above, not here.

- **3a, symbol isolation** -- rename ordering, `nm` checks, LTO: done on both
  GL and Metal.
- **3b, both instances running concurrently on two threads: done on Metal
  only.** The Linux round covered compilation and linking, not concurrent
  execution, and the AMD probe uses raw GL rather than either instance. **The
  concurrent GL case is outstanding**, and the first implementation milestone
  is where it gets exercised.

**The two questions software rendering could not settle were answered on AMD
hardware** (Radeon 780M, Mesa radeonsi 25.2.8) on 2026-09-21 --
`docs/notes/2026-09-21-linux-hardware-spike-results.md`, run with the
committed probe `test/fixtures/hnui-spike/glx_share.c`:

- **A shared context goes current on a second thread with no drawable**, so
  the surfaceless route is what v1 implements and Sokol's config filter needs
  no patch. Two qualifications: the probe chose its own framebuffer config
  (`glXChooseFBConfig`, RGBA / window bit / depth 24 / double buffer) rather
  than the one `_sapp_gl_choose_fbconfig` ranks first against desired depth,
  stencil, samples and sRGB, so surfaceless sharing is **expected** to hold
  for Sokol's config rather than established for it -- sharing is a property
  of the context and driver more than of the config, but that is a hypothesis
  until the first milestone creates the real `init_cb` context. And the
  pbuffer question stayed unexercised: radeonsi's config advertises
  `GLX_PBUFFER_BIT` exactly as llvmpipe's did.
- **Sync objects are visible across the share group**, and a texture's
  *content* crosses with them, so the preferred completion path is the
  default.

**What is still untested, and should be named rather than generalised to
"hardware":** NVIDIA's proprietary driver, which is the likeliest of any to
differ on both questions; question 1 under native Xorg rather than the
XWayland session this ran in (low stakes, since surfaceless works); Intel GL;
and Intel Macs.

1. **GLX sharing.** **Decided: v1 implements the surfaceless route**, chosen
   at stage 1 of startup (see Lifecycle). The alternatives are contingencies
   for a driver that refuses it, not a preference order: a pbuffer drawable,
   or patching Sokol's config filter to require `GLX_PBUFFER_BIT` as well as
   `GLX_WINDOW_BIT`. **EGL and GLES3 are refused at configure time in v1**
   and can be lifted later. The original framing follows, since it explains
   what each route costs: **Routes (a) and (b) are
   GLX and must** resolve the queried `GLX_FBCONFIG_ID` back to a compatible
   `GLXFBConfig` handle and validate directness and drawable support. **Route
   (c) is EGL and has none of those objects**: a Sokol EGL build creates no
   GLX context (`sokol_app.h:2400`), so it validates its own `EGLConfig`,
   shared `EGLContext` and surfaceless or pbuffer drawable instead. The spike
   also delimits which configurations v1 supports: desktop GL through GLX,
   and whether forced-EGL and GLES3 Linux builds are supported or refused at
   configure time. Also: whether sync objects are visible across the share
   group. **Answered on llvmpipe and AMD radeonsi**: the surfaceless route
   works, so no patch; sync objects are shared, so polling works. The EGL and
   GLES3 delimitation is the one part of this item still undecided, and it
   needs no hardware.
2. **Xlib threading. Decided: no worker-thread or addon Xlib call happens
   before `init_cb` signals.** `XInitThreads()` runs a few statements into
   `_sapp_linux_run`, so "after `sapp_run()` starts" was never a safe rule.
   The rule binds *our* code; Sokol's own initialisation necessarily calls
   Xlib before `init_cb`, which is fine because it is the only thread running
   Xlib at that point.
3. **Both `sokol_gfx` copies** live on two threads at once, GL and Metal,
   including the rename-before-declarations rule and an LTO build.
4. **Offscreen-only frames** with no swapchain pass, and a render target
   wrapped from a single injected texture. **Both answered yes**: 100,000
   offscreen-only frames on Metal and 2,000 on GL ran clean, with the Metal
   semaphore bounding frames in flight rather than stalling; and one injected
   texture suffices for a render-target image on both backends, because an
   attachment-usage image resolves to immutable and therefore to a single
   slot (`sokol_gfx.h:8594`) rather than `SG_NUM_INFLIGHT_FRAMES` of them.
   The header's claim that a frame "must have at least one swapchain render
   pass" is neither enforced nor true.
5. **Metal ordering.** Whether a later-committed command buffer on one queue
   completes after earlier ones. **Answered yes for committed buffers**, 0
   inversions in 1000 against 12 ms of GPU work, in both the retained and the
   unretained form Sokol uses by default; **answered no for enqueued ones**,
   1000 in 1000. The design no longer depends on either answer: completion is
   now a handler on Sokol's own frame command buffer, so nothing infers one
   buffer's completion from another's. The measurement survives as the reason
   that design was abandoned, and `sg_mtl_command_queue()` exposing only the
   queue (`sokol_gfx.h:28324`) is precisely why the accessor patch exists.
   Autorelease pools were measured at the same time and their requirement
   widened; see the Surfaces section.
6. **`llvmpipe` under Xvfb** running all of the above.

## What this does not do

- **No `--inspect` in UI mode**, pending the `exitLoop`/`processExiting`
  fix.
- **No `--build-exe` for UI apps.** It needs a UI kit variant; its own round.
- **No touch events.** `sokol_app` produces them only on iOS, Android and
  Emscripten.
- **No layering.** One full-window surface.
- **No multi-window**, following `sokol_app`. The handle-based C API keeps
  that additive.
- **No IME, no Wayland**, following Sokol.
- **No JS-level graphics API.** Rendering is native. A Canvas-style API over
  a native renderer would be a separate piece of work, and is what an
  animated 2D program would need.
- **No ImGui addon in v1.** The reference addon (`sokol_gl`, above) is what
  ships; an ImGui one is the next round.
- **No public API for addon resource churn outside frame callbacks.** A
  `hnui_maintenance_frame()` entry point was specified in full -- admission
  rule, asynchronous completion callback, suspension of blocking submissions
  -- and then cut, because its only consumer was the conformance mode that
  existed to exercise it. The obligation underneath it is real and stays:
  the module still performs Metal maintenance for its own generation churn
  (see Surfaces). Addons that need out-of-frame churn are the reason to add
  the API back, and then it will have a caller.
- **No idle sleep.** The main thread wakes every frame callback and
  re-presents, because `sokol_app` has no on-demand mode.
- **Windows and D3D11 are not built.**
