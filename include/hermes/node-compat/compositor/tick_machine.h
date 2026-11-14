/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_TICK_MACHINE_H
#define HERMES_NODE_COMPAT_COMPOSITOR_TICK_MACHINE_H

#include <hermes/node-compat/compositor/surface_table.h>

#include <mutex>
#include <optional>

namespace hermes {
namespace node_compat {

/// Where one frame has got to.
///
/// kPublishedAwaitingTake is not an error state: it is the ordinary state
/// between a publication and the take that presents it. What must never
/// happen is being in it with no un-taken frame left -- waiting for a take
/// that already happened -- which is what the composition rule below exists
/// to prevent.
enum class TickState {
  kIdle,
  kTickQueued,
  kCallbacksRunning,
  kGpuPending,
  kPublishedAwaitingTake,
};

/// Paces requestAnimationFrame: decides when a frame tick is delivered to
/// JavaScript, and tracks the frame it started until its surface has been
/// handed to the presenter.
///
/// **Deliberate deviation from browser rAF: presentation backpressure.** A
/// tick is withheld -- not queued -- while no surface slot is free, so a
/// producer that outruns the presenter is slowed at the source rather than
/// building a backlog of frames nobody will ever see. That is what the
/// `aFreeSlotExists` parameter of shouldSendTick() is for.
///
/// **A request is cleared when the tick is SENT, not when callbacks begin.**
/// Browser rAF swaps out the callback list at the start of the batch, which
/// would be a clear in beginCallbacks(); doing that here would swallow a
/// request that arrived while the tick was already queued but had not yet
/// started running, and such a request must be serviced by the *next* tick
/// rather than dropped. Pinned by
/// TickMachineTest.RequestsSurviveTickQueuedAndGpuPendingAlike, phase 0.
///
/// Threading: every method may be called from any thread. requestFrame()
/// comes from the JS thread, shouldSendTick()/takeFrame() from the thread
/// driving presentation, publishFrame() from the producer. One mutex guards
/// the whole of the small state.
///
/// **Lock ordering:** the composed entry points -- publishFrame(),
/// takeFrame() and shouldSendTick(SurfaceTable &) -- take the SurfaceTable's
/// lock and then this machine's, in that order. Nothing here ever takes them
/// the other way round, which is what keeps them deadlock-free.
class TickMachine {
 public:
  TickMachine() = default;

  TickMachine(const TickMachine &) = delete;
  TickMachine &operator=(const TickMachine &) = delete;

  /// Ask for a frame. Repeated requests coalesce: the machine carries one
  /// bit, not a count, because n requests before a tick still mean one tick.
  void requestFrame();

  /// **The production form.** Called from the platform's frame callback:
  /// reads \p table's free-slot state and moves the tick state **in one
  /// critical section**, under the table's own lock. True means deliver a
  /// tick to JavaScript now, and moves to kTickQueued.
  ///
  /// This exists for the same reason publishFrame() and takeFrame() do.
  /// Written as `table.anyFreeSlot()` followed by `shouldSendTick(free)`,
  /// the gate is two calls under two locks and the answer can go stale
  /// between them -- the shape SurfaceTable's own header now warns about,
  /// and the one the whole withLock()/LockToken apparatus exists to make
  /// unnecessary.
  bool shouldSendTick(SurfaceTable &table);

  /// The decomposed form, for tests and for a caller that already has the
  /// free-slot reading in hand. Production code should prefer the overload
  /// above: this one cannot compose the reading with the transition,
  /// because by the time it is called the reading has already been taken.
  ///
  /// \param aFreeSlotExists whether the producer could acquire a surface to
  ///   draw into. False withholds the tick and *keeps* the request, so the
  ///   next callback with a free slot delivers it.
  bool shouldSendTick(bool aFreeSlotExists);

  /// Reject a pointer argument at compile time. `shouldSendTick(&table)` is
  /// the shape that gets this wrong: a pointer converts to bool, so it binds
  /// to the decomposed overload above and silently means "a free slot always
  /// exists" -- withholding nothing, and defeating the presentation
  /// backpressure the composed form exists to enforce. Slice B is the first
  /// code that will write this call.
  template <typename T>
  bool shouldSendTick(T *) = delete;

  /// The tick's callbacks are about to run. Requests made from inside them
  /// belong to the next frame and are left alone.
  void beginCallbacks();

  /// The tick's callbacks have finished. \p drew false means nothing was
  /// rendered, so the frame is over and the machine returns to kIdle
  /// immediately -- a rAF chain that only mutates state must not stall
  /// waiting for a publication that will never come.
  void endCallbacksDrew(bool drew);

  /// The frame's publication did not happen: the generation retired, the
  /// window began closing, or the GPU work failed. The frame is over.
  /// Pending requests survive, so an animation continues across a resize.
  void publicationDropped();

  /// A minimized (or otherwise hidden) window receives no ticks, matching
  /// rAF in a hidden tab. Requests still accumulate and are delivered when
  /// the window comes back.
  void setMinimized(bool minimized);

  TickState state() const;

  /// Publish \p ref and record the transition **in one critical section**,
  /// under the table's own lock.
  ///
  /// This is the whole reason published()/taken() are not exposed as
  /// free-standing calls. Split into `table.publish(ref)` followed by a
  /// separate state store, the presenting thread can take the surface
  /// between the two, and the state store then overwrites kIdle with
  /// kPublishedAwaitingTake -- leaving the machine waiting forever for a
  /// take that already happened, and the animation dead.
  ///
  /// \return false if the table refused the publication, in which case the
  ///   frame is treated as dropped and the caller must reclaim() the slot.
  bool publishFrame(SurfaceTable &table, SurfaceRef ref);

  /// Take the ready surface and record the transition, likewise in one
  /// critical section. Empty when no frame is ready, which leaves the
  /// machine's state alone: the producer has not published yet, and the next
  /// frame callback will find the frame.
  std::optional<SurfaceRef> takeFrame(SurfaceTable &table);

 private:
  /// The tick decision itself, with mutex_ already held. The one copy both
  /// public overloads run, so the composed form and the decomposed form
  /// cannot drift apart in what they decide -- only in how the free-slot
  /// reading reached them.
  bool shouldSendTickLocked(bool aFreeSlotExists);

  mutable std::mutex mutex_;
  TickState state_ = TickState::kIdle;
  /// One bit rather than a count: requests coalesce.
  bool pending_ = false;
  bool minimized_ = false;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_TICK_MACHINE_H
