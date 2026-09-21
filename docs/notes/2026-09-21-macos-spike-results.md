# macOS/Metal spike results for the UI compositor

**Run 2026-09-21.** The five probes in
`docs/notes/2026-09-19-macos-metal-spike-instructions.md`, answering the Metal
half of `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`. Throwaway
code, kept outside the repo (`/tmp/hnui-spike`); what matters is the answers.

**Machine:** macOS 26.6.2 (25G83), Xcode 26.4 / Apple clang 21.0.0, **Apple
M4 Max** (40-core GPU, highest supported family `MTLGPUFamilyApple9`,
`supportsFamily(Metal3)` yes, system reports "Metal 4"). Sokol at `c0db757`,
the pinned commit, which still exists.

Every probe ran twice, plain and under
`METAL_DEVICE_WRAPPER_TYPE=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` (both
"Metal API Validation Enabled" and "Metal GPU Validation Enabled" confirmed in
the logs). **No probe emitted a single validation message**, in any
configuration.

**Verdict: no blocker found.** All five pass. Three things the design should
say that it does not say yet are in "What this forces" at the bottom.

## 1. Command buffer completion ordering -- PASS, with a warning label

The load-bearing question. Buffer A ran a compute kernel over 1,048,576
floats, auto-calibrated to **12.10 ms** of GPU time; buffer B had no encoders.
Completion handlers recorded a sequence number under a lock. 1000 iterations
per variant.

| Variant | Inversions | handler gap tB-tA, ms (min/avg/max) |
|---|---|---|
| `commandBuffer` (retained), commit A then B | **0 / 1000** | +0.003 / +0.006 / +0.022 |
| `commandBufferWithUnretainedReferences`, commit A then B | **0 / 1000** | +0.003 / +0.006 / +0.016 |
| B `enqueue`d before A's commit, B committed after | **1000 / 1000** | -12.558 / -12.211 / -12.111 |

Identical verdicts under validation (0/1000, 0/1000, 1000/1000).

**The marker trick is valid for buffers committed in order, including the
unretained form Sokol uses by default.** The gap is consistently positive and
tiny, which is what you would see if handlers are dispatched in completion
order rather than racing.

**The third variant is not a failure of the design, it is a constraint on it.**
`enqueue` reserves a place in the queue when it is called, so a buffer enqueued
early runs early no matter when it is committed -- B completed a full
A-duration *before* A, deterministically, every time. That matters here
because **Sokol `enqueue`s its own frame command buffer**, in
`_sg_mtl_begin_pass` at the first pass of each frame:

```c
// sokol_gfx.h:17167
if (nil == _sg.mtl.cmd_buffer) {
    dispatch_semaphore_wait(_sg.mtl.sem, DISPATCH_TIME_FOREVER);
    if (_sg.desc.metal.use_command_buffer_with_retained_references) {
        _sg.mtl.cmd_buffer = [_sg.mtl.cmd_queue commandBuffer];
    } else {
        _sg.mtl.cmd_buffer = [_sg.mtl.cmd_queue commandBufferWithUnretainedReferences];
    }
    [_sg.mtl.cmd_buffer enqueue];
```

(That line also confirms the brief's premise: the unretained form is the
default, and the retained one is opt-in through
`sg_desc.metal.use_command_buffer_with_retained_references`.)

The ordinary sequence is safe because the marker is created and committed
after `sg_commit()`, on the same thread, and so always takes a queue slot after
the frame buffer enqueued at `begin_pass`. Pre-creating or pooling markers
would invert 100% of the time.

### Documented guarantee, or observed behaviour?

**Half and half, and the design leans on the undocumented half.** Web access
was unavailable in this session, so this rests on the macOS 26.6 SDK headers,
which are a primary Apple source. Verbatim:

- `MTLCommandQueue.h:18` -- "A serial queue of command buffers to be executed
  by the device."
- `MTLCommandBuffer.h:298` (`enqueue`) -- "Append this command buffer to the
  end of its MTLCommandQueue."
- `MTLCommandBuffer.h:304` (`commit`) -- "Commit a command buffer so it can be
  executed as soon as possible."
- `MTLCommandBuffer.h:346` (`addCompletedHandler:`) -- "Add a block to be
  called when this command buffer has completed execution."

So **execution order is documented**. What is documented nowhere in the SDK is
that the completion handlers of two *distinct* command buffers are invoked in
the order the buffers completed -- `addCompletedHandler:` says only when a
given buffer's own block fires, never how it is ordered against another's.
Grepping every Metal header for "in the order" / "order in which" / "serial
queue" / "in-order" turns up only the `MTLCommandQueue` line above; Metal 4's
`MTL4CommitFeedback.h`, the modern equivalent, says nothing about ordering
either.

This is cheap to stop depending on: read `A.status == MTLCommandBufferStatusCompleted`
(or A's `GPUEndTime`, which the header documents as returning zero until the
CPU has the completion notification) inside the marker's handler, which turns
the assumption into a check for the price of retaining A.

## 2. Two `sokol_gfx` instances, one process, two threads -- PASS

Layout as the design describes it: `sokol_app` on the main thread with
`SOKOL_NO_ENTRY`, device from `sapp_get_environment().metal.device`, the
renamed instance presenting on the main thread and the ordinary instance
rendering offscreen on a spawned thread against that same device. Both report
`backend=4` (`SG_BACKEND_METAL_MACOS`).

The rename list is generated from the header's own `SOKOL_API_IMPL`
definitions -- **156**, matching the brief -- and included ahead of
`sokol_gfx.h` entirely.

```
worker.o      defines 156 _sg_*,  0 _sgp_*,  references 0 _sgp_*
mainrender.o  defines 156 _sgp_*, 0 _sg_*,   references 0 _sg_*
```

Identical under `-flto`, and the LTO link succeeds. The C++ overload hazard is
real, and in this configuration it is louder than feared: with the rename
header *after* `sokol_gfx.h` the reference-form call does not silently bind to
the other copy, it fails to compile, because the macro renames the call site
while the inline overload keeps its old name. With the correct order `nm`
shows the overload itself renamed (`T __Z9sgp_setupRK7sg_desc`,
`U _sgp_setup`). Follow the rule anyway.

Worth recording, because it is what keeps the rename list down to the public
surface: sokol's state is `static _sg_state_t _sg;` (`sokol_gfx.h:7756`) and
`_SOKOL_PRIVATE` expands to `static`, so per-TU state separation is automatic.

The worker rendered a rotating triangle over a clear colour **encoding its
frame counter** into one of two 512x512 surfaces, committed an empty marker,
and published on its completion. The main thread wrapped those textures via
`sg_mtl_query_image_info()` -> `sg_image_desc.mtl_textures[0]`, blitted the
published one full-screen, and in its own marker read back one pixel of the
source and **checked it against the counter the worker said it published** --
which replaces "looks right" with a number. 30 seconds, resized every 90
frames through five window sizes:

| | plain | validation |
|---|---|---|
| worker frames (offscreen only) | 28,682 | 14,819 |
| main frames / blits | 3,441 / 3,439 | 3,599 / 3,589 |
| **readback OK** | **3,439** | **3,589** |
| **readback MISMATCH / BLACK** | **0 / 0** | **0 / 0** |
| resize events | 38 | 39 |

No sokol assertion, no validation message, clean shutdown, and the screenshot
taken at the 30-second mark shows the worker's content correctly: not torn,
not black, not stale.

**A single injected texture is accepted, for a render-target image as well as
a sampled one.** Sokol does not demand `SG_NUM_INFLIGHT_FRAMES` of them, so the
design's assumption holds and that open question closes. The mechanism is
`cmn->num_slots = desc->usage.immutable ? 1 : SG_NUM_INFLIGHT_FRAMES`
(`sokol_gfx.h:8594`) together with `_sg_image_usage_defaults()` (`:25803`),
which sets `immutable` whenever none of `immutable` / `write_transient` /
`dynamic_update` is: an attachment-usage image therefore resolves to one slot,
and `_sg_mtl_create_image` only asserts `mtl_textures[slot]` for
`slot < num_slots`. Measured both ways -- sampled-only and
`usage.color_attachment = true` both come back VALID with `tex[1]` null. The
caveat is that this holds *because* the image is immutable; one created
`dynamic_update` or `write_transient` needs two and asserts on the second.

Same answer as the Linux round reached for GL, by a different mechanism.

## 3. Offscreen-only frames -- PASS

A standalone instance with no `sokol_app` and no swapchain anywhere in the
process:

| frames | wall | throughput | max in flight | worst frame |
|---|---|---|---|---|
| 10,000 | 0.69 s | 14,565 fps | 3 | 7.43 ms (frame 2) |
| 100,000 | 6.71 s | 14,900 fps | 3 | 6.85 ms (frame 2) |
| 10,000 under validation | 2.33 s | 4,296 fps | 3 | 7.83 ms (frame 2) |

No assert, no deadlock, no validation message. Throughput is flat across the
whole run -- the per-1000-frame rate stays between 14.4k and 15.5k fps with no
downward drift over 100,000 frames -- and the worst frame is always frame 2,
first-use warmup.

The semaphore bounds frames in flight rather than stalling, which is the
outcome the brief called expected and fine. The counter reads 3 rather than 2
because it is decremented by the probe's *marker* handler, one command buffer
behind the frame buffer whose handler signals `_sg.mtl.sem`, so sokol admits
the next frame before the counter drops. Sokol's own limit is
`SG_NUM_INFLIGHT_FRAMES` = 2 and the reading is consistent with that plus a
marker's lag.

Probe 2's worker is an independent second demonstration under more realistic
conditions: 28,682 offscreen-only frames beside a live presenting instance.
The header's "a frame must have at least one swapchain render pass" is neither
enforced nor true.

## 4. Autorelease pools -- requirement confirmed, but not where expected

Each mode ran as its own process, on a spawned `std::thread` -- no run loop, no
pool anyone else drains.

**Per-frame rendering leaks nothing measurable, with or without a pool:**

| mode | frames | RSS baseline -> end |
|---|---|---|
| no pool | 200,000 | 13.50 -> 16.64 MB |
| per-frame pool | 200,000 | 13.48 -> 16.66 MB |

Both curves are flat -- RSS reaches ~16.5 MB by iteration 1000 and does not
move again through 200,000 frames.

That is not because nothing is autoreleased. Under
`OBJC_DEBUG_MISSING_POOLS=YES` there is **exactly one autorelease per frame
with no pool in place** (370 at 300 frames, 1070 at 1000, 3070 at 3000, all
`AGXG16XFamilyBuffer` plus a fixed ~70 at setup), and a per-frame pool removes
it. It costs nothing only because **the driver recycles the objects**: across
3000 frames there were **2 distinct pointers**, sokol's two rotating uniform
buffers. The missing pool pins an object that was staying alive anyway.

**Do not read that as "the worker does not need a pool."** Resource creation
is the case that bites -- an image, view, buffer, shader and pipeline created
and destroyed per round:

| mode | rounds | RSS baseline -> end | per round |
|---|---|---|---|
| no pool | 5,000 | 13.48 -> **19.34 MB**, linear | 1.2 KB |
| per-round pool | 5,000 | 13.52 -> 13.84 MB, flat after ~3000 | 0.1 KB |

The no-pool curve rises a steady ~1.15 MB per 1000 rounds with no sign of
levelling. So the design's requirement stands, but the accurate statement of
it is **wrap everything the worker thread does, not only its frames** -- the
render loop's apparent innocence is an artifact of the driver recycling two
objects, and any per-frame allocation of a label, descriptor, texture or
resized surface would accumulate in an implicit thread pool nobody drains.

One incidental finding, from a bug in the first version of this probe:
**sokol only advances its Metal id-pool release rotation inside
`sg_commit()`**, deferring releases by `SG_NUM_INFLIGHT_FRAMES`. Creating and
destroying resources without ever committing exhausts the pool and trips
`SOKOL_ASSERT(_sg.mtl.idpool.free_queue_top > 0)` (`sokol_gfx.h:15877`) after a
few hundred rounds. A worker that churns surfaces must keep committing frames.

## 5. Main-thread ownership -- PASS on all three

```
[main] pid 50914, main thread 0x1fbf52180
[worker] started on thread 0x16d1ab000; main thread is someone else (correct)
[worker] asked for a window at 0.302 s
[main] woke from the condition variable at 0.302 s; calling sapp_run()
[main] init_cb on main thread=yes, device=0x103355940
[worker] window is up at 0.562 s, device=0x103355940
[worker] sokol_gfx up on the worker thread: valid
[worker] WINDOW_NUMBER=62890
[worker] 4976 offscreen frames; main-thread frame callback ran: yes
[worker] calling _exit(0) from a secondary thread while the main thread is inside [NSApp run]
--- exit status: 0
```

1. **Waiting on a condition variable on the main thread before `[NSApp run]`
   causes no problem.** `main()` spawned the worker, blocked 302 ms, then
   called `sapp_run()`. AppKit said nothing -- not a warning, not a note.
   `init_cb` ran on the main thread and handed over the same device pointer.
2. **`_exit()` from a secondary thread while the main thread is inside
   `[NSApp run]` terminates cleanly.** Exit status 0. Crash reports in
   `~/Library/Logs/DiagnosticReports`: **51 before, 51 after**, on both runs.
   No lingering process. The window is gone -- a `CGWindowListCopyWindowInfo`
   query for window 62890 reports it absent from the on-screen list.
   `sapp_run()` never returned, as expected.
3. **Metal work on the worker thread draws no main-thread-only complaints.**
   Under API and GPU validation the run has the same shape (4,613 offscreen
   frames in 8 s, exit 0, 51 -> 51 crash reports, window gone) with zero
   validation output.

## What this forces

Nothing structural. Three things to write into the design:

1. **A marker must be committed immediately after the work it marks, and must
   never be `enqueue`d.** Sokol `enqueue`s its own frame command buffer at
   `begin_pass`, which is what makes the ordinary sequence safe; a pre-created
   or pooled marker inverts deterministically, 1000/1000 above.
2. **Say which guarantee the marker rests on.** Execution order is documented,
   completion-*handler* dispatch order is not. Checking
   `A.status == Completed` in the marker's handler turns the undocumented half
   into a check.
3. **Wrap all worker-thread work in `@autoreleasepool`, not just frames.**

And one open question closes: a single injected texture is accepted for a
render-target image.

## Still open

- **Hardware GL**, unchanged from the Linux round: the pbuffer question and
  cross-context sync visibility were measured on llvmpipe only. Nothing here
  touches them.
- **Intel Macs.** Everything above is one Apple-silicon machine.
