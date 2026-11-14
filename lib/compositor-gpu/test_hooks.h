/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_TEST_HOOKS_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_TEST_HOOKS_H

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/surface_table.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace hermes {
namespace node_compat {

/// Whether HERMES_NODE_UI_TEST_HOOKS is set.
///
/// Task 9 added these hooks **before** the code that calls into them
/// (frame capture reached the window in Task 10; `CompletionGate` is also
/// exercised directly against `SurfaceTable`). Event injection used to
/// live here too and moved to `tools/hermes-node-ui/event_injector.h` in
/// Task 11b, where it injects platform events -- the converter's input --
/// rather than the `UiEvent`s it produces.
/// Every future call site that could route through a hook is expected to
/// check this first and take the ordinary, hook-free path when it is
/// false -- the same shape `uiTracingEnabled()` (`ui_trace.h`) already
/// uses, and for the same reason: these sit on paths a production run
/// takes every frame, and "compiled always, enabled only by an
/// environment variable" (the plan's Global Constraints) means the cost of
/// being off is one cached bool check, never a preprocessor guard that a
/// build configuration could omit entirely.
///
/// Read **once**, into a function-local static: several threads reach
/// these hooks (the JS producer, the platform thread, and whichever thread
/// a GPU completion lands on), so a plain global initialised lazily by
/// hand would be a data race, and a mechanism that could change its own
/// enablement mid-run would be one more thing a test would have to reason
/// about.
///
/// **Deliberately a variable of its own, not `HERMES_NODE_DEBUG_NATIVE`.**
/// That variable enables *observation* (tracing) and this one enables
/// *behaviour changes* (holding a completion, injecting an event, matching
/// a capture request) -- conflating them would mean a developer who sets
/// `HERMES_NODE_DEBUG_NATIVE=UI` to watch a production run's trace output
/// also, silently, makes that run capturable and event-injectable.
///
/// **Testable only through `uiTestHooksEnabledFor()` and a forked child,
/// never by asserting on this function directly from a test's own
/// process.** The cache above means this function can be observed at most
/// once per process; a test that calls it in the *parent* process either
/// depends on that process's ambient environment (measured: a developer's
/// `HERMES_NODE_UI_TEST_HOOKS=1`, exported to drive a real UI test, made
/// the unit suite fail with no explanation -- `unittests/lit.cfg` runs
/// `lit.formats.GoogleTest`, which does not scrub the environment the way
/// `test/lit.cfg`'s ShTest does) or primes the cache for good, so every
/// later fork in the same process -- including a death test meant to check
/// the opposite answer -- would silently inherit that first answer instead
/// of evaluating fresh.
bool uiTestHooksEnabled();

/// The value-parsing half of `uiTestHooksEnabled()`, factored out so its
/// logic is unit-testable without touching the process environment at all
/// -- see that function's comment for why calling it directly from a test
/// is unsafe. \p value is what `getenv("HERMES_NODE_UI_TEST_HOOKS")`
/// returned; nullptr means unset. Empty or exactly `"0"` is disabled,
/// anything else enabled -- matching the convention
/// `HERMES_NODE_DISABLE_COMPILE_CACHE` already uses
/// (`lib/runtime/hermes_node_runtime.cpp`).
bool uiTestHooksEnabledFor(const char *value);

//===========================================================================
// Controllable completion
//===========================================================================

/// Lets a test hold and release a producer's or a consumer's GPU-work
/// completion deterministically, with no GPU and no timing.
///
/// **Deliberately a queue of closures, not a wrapper around two specific
/// `SurfaceTable` methods.** A real completion callback does not only call
/// `endGpuWork()`/`releaseRead()`: it calls that and then acts on the
/// result. **On the producer side that action is `TickMachine::
/// publishFrame()` (if the generation is still current) or
/// `publicationDropped()` (if it is not) -- never a bare
/// `SurfaceTable::publish()`/`reclaim()` call**: `TickMachine` leaves
/// `kGpuPending` only inside those two composed entry points
/// (`tick_machine.h`), and a completion handler that published straight
/// through `SurfaceTable` would leave the machine parked in `kGpuPending`
/// forever, stalling every tick after the first. (A consumer completion
/// may be the trigger that makes a generation's wrappers releasable.)
/// Hardcoding the gate to one `SurfaceTable` call each would only let a
/// test hold the accounting and not the decision built on top of it -- so
/// callers pass **what completion does**, built out of the real API (a
/// `SurfaceTable`-only test may reasonably call `SurfaceTable`'s own
/// `beginGpuWork()`/`endGpuWork()`, `addRead()`/`releaseRead()`, per the
/// design's "Surfaces" section, exactly as `TestHooksTest.cpp` does;
/// production code calls `TickMachine`'s composed entry points instead),
/// and the gate controls only *when* that closure runs. This is what plan
/// Task 9's brief means by "use beginGpuWork/endGpuWork and
/// addRead/releaseRead rather than inventing a parallel mechanism": the
/// mechanism here is generic; the calls inside a closure are real ones.
///
/// **Held is not the default.** A gate nobody has held runs every
/// completion immediately and inline, in `completeProducer()`/
/// `completeConsumer()` itself -- which is what makes it safe to sit on a
/// production completion path unconditionally once a future task wires it
/// in behind `uiTestHooksEnabled()`: an unused gate costs one mutex
/// lock/unlock and adds no delay.
///
/// **Queued completions run in submission order** on release, holding no
/// lock of this class's own while each closure runs -- a closure that
/// calls into `SurfaceTable` takes that table's own lock, and running it
/// while `mutex_` below is held would nest two unrelated locks for no
/// reason and risk a lock-order inversion against any other path that
/// takes them the other way round.
///
/// **"Run in submission order" describes the queue, not the channel as a
/// whole, and is not an ordering barrier against a concurrent
/// `completeX()`.** `releaseX()` clears the held flag and swaps the
/// pending queue out from under `mutex_`, then runs the drained closures
/// with no lock held. A `completeX()` that observes the held flag already
/// false during that window -- which is possible for a call arriving from
/// another thread, such as a real GPU driver's completion thread in a
/// future task -- runs its own closure immediately, and nothing here
/// orders it against the closures `releaseX()` is still draining. Every
/// test in this file is single-threaded, so this is latent today; it
/// matters the moment a real multi-threaded completion source is wired in
/// (Task 10).
///
/// Threading: every method may be called from any thread; `holdX()`/
/// `releaseX()` are typically called from the test's own thread,
/// `completeX()` from whichever thread would have carried the real
/// completion.
class CompletionGate {
 public:
  CompletionGate() = default;

  CompletionGate(const CompletionGate &) = delete;
  CompletionGate &operator=(const CompletionGate &) = delete;

  /// Hold producer completions from this point on: a `completeProducer()`
  /// call queues its action rather than running it.
  void holdProducer();

  /// Stop holding, and run every queued producer action, in the order
  /// `completeProducer()` queued them. A no-op, other than clearing the
  /// held flag, if nothing was queued.
  void releaseProducer();

  /// A producer completion. Run immediately, inline, unless the gate is
  /// currently held, in which case \p action is queued for the matching
  /// `releaseProducer()`.
  void completeProducer(std::function<void()> action);

  /// The consumer-side equivalents.
  void holdConsumer();
  void releaseConsumer();
  void completeConsumer(std::function<void()> action);

 private:
  std::mutex mutex_;
  bool producerHeld_ = false;
  bool consumerHeld_ = false;
  std::vector<std::function<void()>> pendingProducer_;
  std::vector<std::function<void()>> pendingConsumer_;
};

//===========================================================================
// Frame capture
//===========================================================================

/// What a capture request names, and what the presenter's observation of
/// the frame it actually drew must agree with before a capture is taken
/// (plan Task 9, Step 1; spec "Testing": "A fixed pattern is not by itself
/// enough to make captured pixels deterministic").
///
/// `targetWidth`/`targetHeight` are the surface's own size -- the
/// generation's dimensions, what the producer drew into.
/// `framebufferWidth`/`framebufferHeight` are the window's actual
/// framebuffer size at present time, which can differ from the target
/// during a resize; the blit stretches with linear filtering when they do,
/// which is exactly the case a capture must not silently accept as if it
/// were the steady-state pattern.
///
/// **There is no input-state field, and that is deliberate.** The spec asks
/// that a capture agree with "the input state the frame was drawn from"
/// because presentation happens later than drawing, and a capture of the
/// right pixels from the wrong frame is still the wrong frame. The
/// publication id below already identifies exactly one drawn frame, and a
/// test that injects events and then draws knows which publication followed
/// which events -- test/ui-demo-visual.js's KEY case is that argument. An
/// input serial was carried here until Slice C, permanently 0 on both
/// sides: a second key that always agrees adds nothing but the appearance
/// of a check.
///
/// **`publicationId` (spec "Testing": "Capture is keyed to a publication id
/// and acknowledged, never to an absolute frame number, because blits can
/// precede any publication").** A `GenerationId` alone is not that key: a
/// generation spans many publications, so two different publications of
/// the same generation are otherwise indistinguishable, and a re-blit of
/// the still-presenting surface (no new publication at all) would report
/// an identical tuple without one.
/// That is benign for a fixed-pattern demo, where every publication draws
/// the same pixels -- but the spec's binding rule is a property of the
/// matching *mechanism*, not of any one demo's content, so it is not
/// optional here. The presenter reports the presenting surface's id
/// (`TickMachine::presentingPublicationId()`, read in ui_module.cpp); this
/// module only compares it.
struct CaptureRequest {
  GenerationId generation;
  uint64_t publicationId = 0;
  int32_t targetWidth = 0;
  int32_t targetHeight = 0;
  int32_t framebufferWidth = 0;
  int32_t framebufferHeight = 0;
  double dpiScale = 0.0;
};

inline bool operator==(const CaptureRequest &a, const CaptureRequest &b) {
  return a.generation == b.generation && a.publicationId == b.publicationId &&
      a.targetWidth == b.targetWidth && a.targetHeight == b.targetHeight &&
      a.framebufferWidth == b.framebufferWidth &&
      a.framebufferHeight == b.framebufferHeight && a.dpiScale == b.dpiScale;
}
inline bool operator!=(const CaptureRequest &a, const CaptureRequest &b) {
  return !(a == b);
}

/// Holds at most one outstanding capture request and matches it against
/// what the presenter actually observes, once per presented frame.
///
/// **Matching, not polling.** The presenter does not ask "is a capture
/// pending"; it reports what it just drew via `tryMatch()`, every frame,
/// and finds out in the same call whether that frame was the one asked
/// for. This is the shape that makes "taken only when generation,
/// publication id, target size, framebuffer size, DPI scale and serial all
/// agree" a property of one comparison rather than of some ordering
/// between a poll and a present.
///
/// **Acknowledged, exactly once, by clearing the request.** A `tryMatch()`
/// that agrees clears the pending request before returning true, so a
/// steady stream of identical frames captures the first of them and no
/// more; a caller that wants another capture must `request()` again. What
/// happens with the matched frame's pixels -- reading them back, handing
/// them to whoever asked -- is the presenter's job (Task 10, which has the
/// GPU backend this module deliberately does not); this class only decides
/// *when*.
///
/// Threading: `request()` is called from the thread issuing the request
/// (a test driver); `tryMatch()`/`pending()` from the presenter's thread.
/// One mutex guards the single pending request.
class FrameCapture {
 public:
  FrameCapture() = default;

  FrameCapture(const FrameCapture &) = delete;
  FrameCapture &operator=(const FrameCapture &) = delete;

  /// Install \p want as the pending request, replacing any request that
  /// was never matched.
  void request(const CaptureRequest &want);

  /// Cancel the pending request, if any, without it ever having matched.
  void cancel();

  /// \return true if a capture is currently pending -- for diagnostics and
  ///   tests; the presenter itself should prefer `tryMatch()`, which folds
  ///   this check into the comparison it has to make anyway.
  bool pending() const;

  /// Report what the presenter actually drew this frame. If a request is
  /// pending and \p observed equals it field for field, the request is
  /// cleared (the acknowledgment) and this returns true. Otherwise
  /// (nothing pending, or a disagreement) returns false and leaves the
  /// pending state exactly as it was -- a disagreement is not an error,
  /// only "not yet".
  bool tryMatch(const CaptureRequest &observed);

 private:
  mutable std::mutex mutex_;
  bool pending_ = false;
  CaptureRequest want_{};
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_TEST_HOOKS_H
