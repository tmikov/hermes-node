/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_BACKEND_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_BACKEND_H

#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/ui/hnui_abi.h>

#include <cstdint>
#include <string>

namespace hermes {
namespace node_compat {

/// What the compositor needs from a graphics API, and **all** of it.
///
/// Defined before either implementation exists, on purpose. The previous
/// plan had a "Metal task" that turned out to implement only completion,
/// because nothing had ever written down the rest of the surface it owed
/// (finding 15); a pure-virtual interface is the form of that list which a
/// compiler checks. GL fills it in over Tasks 8 and 10, Metal in Task 16,
/// and neither may quietly answer a question the other answers differently
/// -- where the two genuinely must differ, the difference is named in a
/// method comment here rather than discovered in an implementation.
///
/// **Nothing backend-specific appears in this header.** No GL type, no
/// Objective-C type, no `sokol_gfx.h` include: `hnui_gpu_view` carries a
/// Sokol handle across as a plain id (see `hnui_abi.h`, and the
/// `static_assert`s in `gl_backend.cpp` that tie it to the real `sg_view`).
/// That is what lets `tools/hermes-node-ui/` hold a pointer to one of these
/// without inheriting a platform's headers.
///
/// # Threading, which is the part that bites
///
/// Two threads call into this object and they call **different** methods.
/// `sokol_gfx` keeps one instance per process and every call must come from
/// the thread owning that instance's context (floooh, sokol#91), so the
/// compositor runs two instances -- and an implementation that serviced the
/// wrong method on the wrong thread would corrupt the other instance's
/// state rather than fail. Every method below therefore says which thread
/// may call it, and that annotation is part of the contract, not a note.
///
///  - **Worker** means the JS thread, which owns the producing `sg_*`
///    instance and every drawing call.
///  - **Platform** means the process main thread, which owns `sokol_app`
///    and the presenting `sgp_*` instance.
class CompositorBackend {
 public:
  virtual ~CompositorBackend();

  CompositorBackend(const CompositorBackend &) = delete;
  CompositorBackend &operator=(const CompositorBackend &) = delete;

  //=========================================================================
  // Startup, in the two stages the spec requires
  //=========================================================================

  /// **Stage 1 (spec: Lifecycle). Platform thread, BEFORE `sapp_run()`.**
  ///
  /// Decide the context route -- on GL, whether the worker's context goes
  /// current surfaceless or against a pbuffer. This cannot be deferred to
  /// `createSharedContext()` below, and that is the whole reason it is a
  /// separate call: Sokol ranks its framebuffer config and creates its
  /// context inside `sapp_run()` (`sokol_app.h:14272-14274`), long before
  /// `init_cb` first runs, so a route that needs a different config must be
  /// chosen while the desc is still being built.
  ///
  /// **A route this build cannot deliver is refused here, with a message.**
  /// Never degraded into another one: the two spike rounds measured
  /// surfaceless working on llvmpipe and on AMD radeonsi, and a driver that
  /// refuses it is a driver nobody here has met -- silently substituting a
  /// route would turn that into a rendering bug found much later instead of
  /// a sentence printed at startup.
  ///
  /// This is the one method not named in Task 8's brief. It is here because
  /// Step 5 requires stage 1 to be a decision taken before `sapp_run()`, and
  /// there is no other method on this interface that runs then --
  /// `createSharedContext()` is already too late by construction.
  ///
  /// \return false with \p error set; the caller reports and fails startup.
  virtual bool selectContextRoute(std::string &error) = 0;

  /// **Stage 2a. Platform thread, inside `init_cb`,** with the platform's
  /// own context current -- which on GLX it is, since
  /// `_sapp_glx_create_context()` ends in `_sapp_glx_make_current()`
  /// (`sokol_app.h:12935`) and `init_cb` runs later, from the frame path.
  ///
  /// Create whatever the worker will share: a GL context sharing with
  /// `sokol_app`'s, or (Metal) nothing at all, the `MTLDevice` being already
  /// shared. Must not be called before `sapp_run()` has started: on Linux
  /// `XInitThreads()` runs a few statements into `_sapp_linux_run`
  /// (`sokol_app.h:14253`), so Xlib is not thread-safe until then.
  ///
  /// **Failure here rejects `createWindow`'s Promise rather than
  /// terminating** (spec: "Failure splits in two"). Sokol's own window and
  /// context creation failing is the fatal half, and it never reaches this
  /// method -- it panics from inside `sapp_run()` before `init_cb`.
  virtual bool createSharedContext(std::string &error) = 0;

  /// **Stage 2b. Worker thread, once,** before any `sg_*` call it makes.
  ///
  /// Bind the context `createSharedContext()` produced to this thread, by
  /// the route stage 1 selected. Separate from the call above because the
  /// two run on different threads: a GL context can only be made current by
  /// the thread that will use it.
  virtual bool makeCurrentOnWorker(std::string &error) = 0;

  //=========================================================================
  // Surfaces (worker thread)
  //=========================================================================

  /// Allocate one generation's worth of surfaces -- `kSlotsPerGeneration`
  /// color textures of \p width x \p height in the swapchain's color
  /// format, each usable as **both** a color and a resolve attachment.
  ///
  /// **On GL these must use immutable storage** (`glTexStorage2D`). Sokol
  /// turns on `gl_texture_views` for GL 4.3 and above
  /// (`sokol_gfx.h:10505`) and its view path then calls `glTextureView()`,
  /// which requires an immutable source; a `glTexImage2D` texture trips an
  /// assert inside Sokol instead of failing here. Measured 2026-09-19
  /// (`docs/notes/2026-09-19-linux-spike-results.md`, section 4).
  virtual bool createGenerationSurfaces(
      GenerationId gen,
      int width,
      int height,
      std::string &error) = 0;

  /// Free \p gen's textures. **Worker thread only** -- which is the point of
  /// `OwnerDispatch`: a generation's last reference is dropped by whichever
  /// thread ran the completion that dropped it, and `SurfaceTable`'s destroy
  /// callback posts to the worker rather than freeing inline.
  ///
  /// Not one of the brief's eight either, and for a duller reason than
  /// `selectContextRoute()`: `createGenerationSurfaces()` has to have an
  /// opposite, and leaving it unnamed would mean every caller freeing
  /// textures through some backend-specific side door.
  virtual void destroyGenerationSurfaces(GenerationId gen) = 0;

  /// Fill \p out for the surface \p ref, for `hnui_frame_target`: both view
  /// handles, the pixel size, the color format, the sample count and the
  /// generation. The views are **borrowed for the current frame callback
  /// only**.
  ///
  /// \return false if \p ref names a surface this backend does not have,
  ///   which is how a stale `SurfaceRef` from a destroyed generation is
  ///   refused rather than answered with a dangling handle.
  virtual bool frameTarget(SurfaceRef ref, hnui_target *out) = 0;

  /// The pixel size \p gen's surfaces were allocated at. **Either thread**
  /// -- the one method in this section that is not the worker's alone.
  ///
  /// It exists for the platform thread, which must learn a presented
  /// surface's size from that surface's own `SurfaceRef::gen` and never
  /// from a variable the JS thread rewrites on every resize. The answer is
  /// read under the same lock that guards allocation and destruction, and
  /// the platform thread asks only about a generation it holds wrappers
  /// for, which therefore cannot be destroyed while it asks.
  ///
  /// \return false if \p gen has no surfaces (never allocated, or already
  ///   destroyed).
  virtual bool generationSize(GenerationId gen, int *width, int *height) = 0;

  /// The producer has finished issuing draw calls for \p ref. Commit, and
  /// arrange for \p onComplete to run **when the GPU has finished**, not
  /// when submission returns.
  ///
  /// The two backends reach that differently and both are correct: GL
  /// creates a `glFenceSync` and `glFlush`es, transferring the fence to the
  /// platform thread, which polls it with timeout 0; Metal installs a
  /// handler on Sokol's own frame command buffer before `sg_commit()` nils
  /// it. What the interface fixes is only that the callback means
  /// *completion* -- CPU submission, an atomic store or elapsed frames
  /// establish nothing (spec: "The full ordering chain").
  ///
  /// The callback may run on **any** thread, including a driver's.
  ///
  /// **Test hooks (`test_hooks.h`, Task 9).** Neither this interface nor
  /// either implementation routes through `CompletionGate` itself -- that
  /// is a decision for whichever call site installs \p onComplete, made by
  /// checking `uiTestHooksEnabled()` there and, if so, wrapping its own
  /// completion logic in a closure passed to
  /// `CompletionGate::completeProducer()`. **That logic is
  /// `TickMachine::publishFrame()` or `publicationDropped()`, never a bare
  /// `SurfaceTable::publish()`/`reclaim()` call**: `TickMachine` leaves
  /// `kGpuPending` only inside those two composed entry points
  /// (`tick_machine.h`), and a completion handler that published straight
  /// through `SurfaceTable` would leave the machine parked in
  /// `kGpuPending` forever, stalling every tick after the first. None of
  /// today's call sites exist yet (presenting is Task 10). `CompletionGate`
  /// is what lets a future test hold and release that delivery
  /// deterministically, with no GPU and no timing; see its class comment
  /// for exactly what it controls, and `TestHooksTest.cpp` for the
  /// `SurfaceTable`-only proof (deliberately below `TickMachine`, which
  /// this library does not depend on) that holding and releasing composes
  /// correctly with acquire/publish/take.
  virtual void submitAndTrackCompletion(
      SurfaceRef ref,
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user) = 0;

  //=========================================================================
  // Presenting (platform thread)
  //=========================================================================

  /// Give the presenting instance its own handles for \p gen's surfaces,
  /// once per generation, by texture injection (`gl_textures[0]` /
  /// `mtl_textures[0]`). Each wrapper is a counted owner of the generation
  /// (`SurfaceTable::addWrapper`).
  virtual bool wrapForPresenting(GenerationId gen, std::string &error) = 0;

  /// **Platform thread, once per frame callback, before `blit()`,
  /// `presentClear()` or `presentInvalid()`.** Acquire this frame's
  /// swapchain.
  ///
  /// Not one of the brief's eight, and it is here because of a contract
  /// `sokol_app` states and nothing else could satisfy:
  /// `sapp_acquire_swapchain()` "must be called exactly once per frame"
  /// (`sokol_app.h:1935`), and on Metal it is what advances to the next
  /// drawable. So exactly one call site may exist, it must be per frame,
  /// and the swapchain it yields is needed by whichever of the three
  /// presenting calls below runs. Leaving it inside `blit()` would mean no
  /// call on a frame that clears instead, and two calls on a frame that
  /// did both.
  ///
  /// It is also how the caller learns the answer `presentInvalid()`
  /// exists for **without naming a backend type**: on macOS an obscured
  /// window returns `sapp_swapchain.invalid` (`sokol_app.h:14872`), and
  /// the shared `frame_cb` has to handle that identically on both
  /// backends without knowing what a swapchain is.
  ///
  /// \return false when this frame has no usable swapchain, in which case
  ///   the caller must call `presentInvalid()` and nothing else.
  virtual bool acquireFrameSwapchain() = 0;

  /// Present a frame that shows no surface: fill the swapchain with the
  /// compositor's clear colour and commit.
  ///
  /// **This is the "never published" state, and it is a real one rather
  /// than a transient** (spec: The lock protocol -- "while that flag is
  /// set the main thread presents a clear colour and samples nothing,
  /// because attachment contents are initially undefined,
  /// `sokol_gfx.h:3714`"). A window exists from the moment `sapp_run()`
  /// starts; the first publication cannot arrive before the JS thread has
  /// bound its context, allocated a generation and run a frame callback,
  /// and every frame in between still has to put something on screen.
  ///
  /// Not one of the brief's eight either. Presenting nothing is not the
  /// same as `presentInvalid()`, which draws nothing at all because there
  /// is nowhere to draw: here there is a swapchain and it must be filled,
  /// since `_sapp_linux_frame` swaps unconditionally after `frame_cb`
  /// returns (`sokol_app.h:14232`) and would otherwise present undefined
  /// back-buffer contents.
  virtual void presentClear() = 0;

  /// Draw \p ref into the swapchain -- a full-screen triangle with linear
  /// filtering, stretching when the sizes differ during a resize. The read
  /// reference is taken by the caller **before** this is called and released
  /// when the blit completes on the GPU, never when this returns.
  ///
  /// The consumer-side half of the same test-hook note above:
  /// `CompletionGate::completeConsumer()` is what a future caller uses to
  /// control when that release reaches `SurfaceTable::releaseRead()`.
  virtual void blit(SurfaceRef ref) = 0;

  /// Install the sink a **completed blit** reports to: \p onComplete runs
  /// once per `blit()`, when that blit has finished on the GPU, and its
  /// job is to release the read reference the caller took before the blit
  /// (`SurfaceTable::releaseRead`, reached through
  /// `CompletionGate::completeConsumer()` when the test hooks are on).
  ///
  /// Not one of the brief's eight, and it is a **setter** rather than a
  /// parameter of `blit()` because of the shape `blit()` already has:
  /// `blit()` returns void and takes only a ref, so a backend that
  /// finishes a blit has no way to tell anyone. The producer side has the
  /// matching argument on `submitAndTrackCompletion()`; the consumer side
  /// needs the same edge and this is where it comes from. Installed once,
  /// before the first `blit()`, rather than per frame: the sink is a
  /// property of the module that owns the surface table, not of any one
  /// frame.
  ///
  /// \p onComplete may run on **any** thread, exactly as the producer's
  /// may. `ok` false means the blit's completion could not be
  /// established -- `GL_WAIT_FAILED` -- which is terminal, and the
  /// callee decides what that means rather than this interface.
  virtual void setConsumerCompletion(
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user) = 0;

  /// Destroy the presenting instance's wrappers for \p gen. Called only when
  /// `SurfaceTable::wrappersReleasable(gen)` is true -- waiting for the
  /// reference count to reach zero instead would deadlock, since the
  /// wrappers are themselves counted owners (spec: "Obsolete wrappers are
  /// released on a trigger, not by waiting").
  virtual void releaseWrappers(GenerationId gen) = 0;

  /// This frame has no usable swapchain, so nothing is drawn. On macOS an
  /// obscured window returns `sapp_swapchain.invalid`
  /// (`sokol_app.h:14872`); such a frame presents nothing and its read
  /// references are released **without** a blit.
  ///
  /// Kept on the interface even though GL never reports an invalid
  /// swapchain, because the *caller* -- the shared `frame_cb` -- must handle
  /// the case identically on both backends, and a GL implementation that
  /// simply does nothing here states that in code.
  virtual void presentInvalid() = 0;

  /// Advance whatever asynchronous completion this backend tracks, without
  /// blocking. Called once per platform frame callback.
  ///
  /// On GL this is where the transferred fences are polled with timeout 0
  /// and deleted; `GL_WAIT_FAILED` does not establish completion and is
  /// terminal (spec: Errors). On Metal it does nothing -- completion
  /// arrives on handlers the driver runs. The asymmetry is why this is on
  /// the interface rather than inside `blit()`: only one backend has
  /// anything to poll, and the frame callback must not know which.
  virtual void pollCompletions() = 0;

  /// **Platform thread, inside the frame callback, after the frame's
  /// `blit()` and before returning.** Read the pixels just presented, as
  /// tightly packed 8-bit RGBA, **top row first**, into \p out, which must
  /// hold at least `width * height * 4` bytes for the swapchain size
  /// `acquireFrameSwapchain()` reported.
  ///
  /// Only the frame-capture test hook calls this (`test_hooks.h`,
  /// `FrameCapture`), and only on the one frame whose agreement fields
  /// matched the pending request -- never on an ordinary frame, because
  /// reading pixels back synchronises the pipeline. That is also why it is
  /// on this interface rather than implemented once over "the framebuffer":
  /// how one reads a presented frame back is exactly as backend-specific
  /// as how one draws it, and the alternative -- exposing the GL default
  /// framebuffer to the caller -- would put a GL concept in the shared
  /// frame callback.
  ///
  /// **Top row first is this interface's choice, not the API's.**
  /// `glReadPixels` yields bottom-up rows; an implementation owes the
  /// caller the flip, so that a capture written to a file has the
  /// orientation a person looking at the window would describe, and so
  /// that a vertically mirrored image is a test failure rather than a
  /// convention mismatch.
  ///
  /// \return false if the read failed or the backend cannot do one.
  virtual bool readPresentedPixels(int width, int height, uint8_t *out) = 0;

  //=========================================================================
  // The worker's event loop
  //=========================================================================

  /// Worker thread, once per event-loop turn, with no frame callback on the
  /// stack and not once the window is CLOSING. GL: nothing. Metal: drain
  /// and replace the worker's autorelease pool, then run maintenance if due.
  ///
  /// Called from a `uv_check_t` the shared code starts on the JS loop once
  /// `makeCurrentOnWorker()` has succeeded (ui_module.cpp). The check phase
  /// follows the poll phase, so whatever one turn's callbacks left behind
  /// is dealt with in the turn that created it. The two exclusions are the
  /// caller's, not the implementation's: `process.exit()` inside a frame
  /// callback reaches a nested `uv_run` (`flushPendingWrites`), which runs
  /// the check phase under an open pass -- where neither a pool may be
  /// popped nor a maintenance commit issued.
  virtual void workerLoopTurn() = 0;

  /// Worker thread, once, before the first `workerLoopTurn()`: \p wake
  /// makes the worker's loop turn once more, so that `workerLoopTurn()`
  /// runs again even in a program that is otherwise idle. \p wake may be
  /// called from **any** thread, and does nothing once the window is
  /// closing.
  ///
  /// GL: unused. Metal: called from the producer's completion handler when
  /// `workerLoopTurn()` put maintenance off because a frame was in flight
  /// -- nothing else would turn an idle loop, and the retired generation's
  /// textures would stay until something unrelated did.
  virtual void setWorkerWake(void (*wake)(void *user), void *user) = 0;

 protected:
  CompositorBackend() = default;
};

/// The backend this binary was built with: GL (`gl_backend.cpp`) on Linux,
/// Metal on macOS (Task 16). Exactly one implementation file defines this
/// and `compositorBackendName()`, which is what makes the choice a link-time
/// fact rather than a run-time one.
///
/// Replaces createGlBackend(). Null, having printed why, if none -- rather
/// than an object in a state no method can be called on.
CompositorBackend *createCompositorBackend();

/// "gl" or "metal": the backend this binary was built with. Printed by
/// `hermes-node-ui --version` as `gpu backend: <name>`, which is what
/// test/lit.cfg reads for `%ui-backend` and the `ui-backend-*` features.
const char *compositorBackendName();

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_BACKEND_H
