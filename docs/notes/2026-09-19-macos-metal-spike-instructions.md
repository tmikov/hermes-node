# macOS/Metal spike instructions for the UI compositor

**Written 2026-09-19, for a session running on a Mac.** The Linux half of
these spikes runs elsewhere; everything here needs Metal and AppKit and
cannot be answered on Linux.

You are executing throwaway probes, not building a feature. The output is an
**answer per probe**, plus the exact versions you measured on. Keep every
file you write under a scratch directory (`/tmp/hnui-spike` is fine). **Do
not modify the hermes-node repository**; nothing here needs it.

## Why these questions matter

The design being validated is
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md` in the
hermes-node repo (branch `work-imgui`). Read it first if it is available to
you; if not, this brief is self-contained.

In that design, `sokol_app` owns the process main thread and presents, while
a second thread runs a JavaScript runtime and does all the drawing into
offscreen surfaces that the main thread then blits. Two `sokol_gfx`
instances exist in one process: the drawing one on the worker thread, and a
**symbol-renamed** copy on the main thread that only presents. A surface is
reusable only after (a) the producer's GPU work completed, and (b) every
blit that sampled it completed. Both of those completion signals are
implemented with a **marker command buffer**: an empty `MTLCommandBuffer`
committed after the real work, whose completion handler is taken as proof
the earlier work finished.

**Probe 1 is the load-bearing one.** If command buffers committed in order
on one queue do not complete in order, the marker trick is invalid for both
the producer and the consumer, and the design needs a Sokol patch exposing
Sokol's own frame command buffer — `sg_mtl_command_queue()` returns the
queue only, not the current buffer.

## Setup

- Xcode command line tools (`xcode-select --install`).
- Sokol headers: `git clone https://github.com/floooh/sokol /tmp/hnui-spike/sokol`.
  **Pin the same commit the design was written against: `c0db757`** (`git -C
  /tmp/hnui-spike/sokol checkout c0db757`). If that commit is gone, use
  master and say so in your report.
- Build Objective-C++ (`.mm`) with
  `clang++ -std=c++17 -ObjC++ -fobjc-arc -framework Metal -framework MetalKit
  -framework Cocoa -framework QuartzCore`.
- Run everything **twice**: once normally, once with Metal API validation on
  (`METAL_DEVICE_WRAPPER_TYPE=1`, and Metal Validation enabled in the
  environment). Validation errors are results, not obstacles.
- Record and report: macOS version, Xcode version, chip (Apple silicon model
  or Intel), and GPU family.

## Probe 1 — command buffer completion ordering (CRITICAL)

**Question:** on a single `MTLCommandQueue`, if buffer A is committed before
empty buffer B, is A's completion handler guaranteed to have run before B's?

Write a pure Metal program, no Sokol:

1. One device, one `MTLCommandQueue`.
2. Buffer A: submit genuinely slow GPU work — for example a compute kernel
   over a large buffer, or several hundred blit-encoder copies of a few MB —
   tuned so A takes at least ~10 ms of GPU time. Add a completion handler
   that records a monotonically increasing sequence number under a lock.
3. Buffer B: no encoders at all. Commit immediately after A. Its completion
   handler records the next sequence number.
4. Assert B's recorded order is always after A's. **Repeat at least 1000
   iterations** and report any inversion, with its iteration number.
5. Repeat the whole run three ways:
   - `commandBuffer` (retained references);
   - `commandBufferWithUnretainedReferences` — **this is what Sokol uses by
     default**, so it is the configuration that matters most;
   - A committed, then B `enqueue`d before A's commit and committed after
     (i.e. enqueue ordering differing from commit ordering).

**Report:** PASS only if there were zero inversions across all three
variants. Any inversion is a FAIL and changes the design.

Also state, from Apple's documentation rather than from the experiment,
whether this ordering is a *documented guarantee* or merely observed
behaviour. An experiment that passes 1000 times on one machine is weaker
evidence than a documented contract, and the design should say which it is
relying on.

## Probe 2 — two `sokol_gfx` instances, one process, two threads

**Question:** can a symbol-renamed second copy of `sokol_gfx` run on the main
thread while the original runs on a worker thread, both Metal, sharing one
`MTLDevice`?

1. Compile `sokol_gfx.h` twice. In the second translation unit, include a
   generated header of `#define sg_xxx sgp_xxx` for **all 156 public
   functions** *before* including `sokol_gfx.h` at all — before its
   declarations, not merely before `SOKOL_GFX_IMPL`. The header defines C++
   inline overloads (`inline void sg_setup(const sg_desc&)`, around
   `sokol_gfx.h:5824`) which otherwise bind to the wrong copy.
   Generate the rename list from the `SOKOL_API_IMPL` definitions in the
   header; do not hand-write it.
2. Verify with `nm` that neither object defines or references the other's
   symbols. Repeat under `-flto`.
3. `sokol_app` on the main thread with `SOKOL_NO_ENTRY`; get the device from
   `sapp_get_environment().metal.device`. Set up the renamed instance on the
   main thread and the ordinary instance on a spawned thread against the same
   device.
4. Worker: render a recognisable pattern into an offscreen color image each
   frame, `sg_commit()`, then the marker buffer from probe 1.
5. Main: wrap the worker's texture — `sg_mtl_query_image_info()` on the
   worker side gives the native handle; inject it on the main side with
   `sg_image_desc.mtl_textures[0]` — and blit it full-screen into the
   swapchain pass.
6. Run for at least 30 seconds, resize the window repeatedly during it.

**Report:** PASS/FAIL, any assertion or validation message, and whether the
image on screen is correct (not torn, not black, not stale). Note explicitly
whether a **single** injected texture is accepted for a render-target image,
or whether Sokol demands `SG_NUM_INFLIGHT_FRAMES` (2) of them — the design
assumes one is enough and says so as an open question.

## Probe 3 — offscreen-only frames

**Question:** does a `sokol_gfx` frame consisting *only* of offscreen passes,
with no swapchain pass ever, work indefinitely?

The worker instance never presents anything; it only renders to textures. The
header says a frame "must have at least one swapchain render pass", and I
found no validation enforcing it. Sokol's Metal backend waits on
`dispatch_semaphore_wait(_sg.mtl.sem)` at the first pass of a frame and
signals it from the frame command buffer's completion handler.

Run 10,000 offscreen-only frames on the worker instance. **Report:** whether
it runs cleanly, asserts, or deadlocks, and how throughput behaves — in
particular whether the semaphore limits you to `SG_NUM_INFLIGHT_FRAMES`
frames in flight, which is expected and fine, or stalls entirely, which is
not.

## Probe 4 — autorelease pools on the worker thread

**Question:** how much memory leaks without a pool, and does a per-frame pool
fix it?

Sokol wraps its own macOS frame callback in `@autoreleasepool`
(`sokol_app.h:6177`) and its Metal backend creates autoreleased command
buffers and encoders. Our worker thread has no such wrapper.

1. Run 10,000 worker frames **without** any `@autoreleasepool`, sampling RSS
   every 1000 frames.
2. Repeat **with** `@autoreleasepool` around each frame.
3. Repeat with resource creation (images, buffers, pipelines) happening
   outside a frame callback, to check whether setup-time work needs a pool
   too.

**Report:** the RSS curves and whether a per-frame pool bounds growth. This
is expected to confirm the design's requirement, not to overturn it.

## Probe 5 — main-thread ownership

**Question:** does AppKit object to the design's thread layout?

`main()` spawns the worker thread and *waits*; only when the worker asks for
a window does the main thread call `sapp_run()`. Verify:

1. Waiting on a condition variable on the main thread before `[NSApp run]`
   causes no problem.
2. `sapp_run()` never returns on macOS, so the process exits via the
   worker's own exit path (`_exit()` in our design) — confirm that exiting
   from a secondary thread while the main thread is inside `[NSApp run]`
   terminates cleanly, with no crash log, and with the window removed.
3. Metal work on the worker thread produces no main-thread-only complaints
   under validation.

**Report:** PASS/FAIL per item, with any console output AppKit produces.

## What to send back

For each probe: **PASS / FAIL / INCONCLUSIVE**, the evidence (numbers,
messages), and for any FAIL your reading of what it forces the design to do.
Include the versions and hardware asked for in Setup, and attach the probe
sources.

The single most important line in your report is probe 1's verdict, and
whether it rests on a documented Apple guarantee or on observed behaviour.
