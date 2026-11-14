/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/tick_machine.h>

#include <cassert>

namespace hermes {
namespace node_compat {

void TickMachine::requestFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_ = true;
}

bool TickMachine::shouldSendTick(SurfaceTable &table) {
  // One critical section for the reading and the transition, exactly as
  // publishFrame() and takeFrame() do it. anyFreeSlotLocked() rather than
  // the public anyFreeSlot(): the table's mutex is not recursive, so
  // calling the public form from in here would deadlock rather than
  // compose.
  return table.withLock([&](SurfaceTable::LockToken &token) -> bool {
    const bool freeSlot = table.anyFreeSlotLocked(token);
    std::lock_guard<std::mutex> lock(mutex_);
    return shouldSendTickLocked(freeSlot);
  });
}

bool TickMachine::shouldSendTick(bool aFreeSlotExists) {
  std::lock_guard<std::mutex> lock(mutex_);
  return shouldSendTickLocked(aFreeSlotExists);
}

bool TickMachine::shouldSendTickLocked(bool aFreeSlotExists) {
  // A frame is already in flight. Its own completion is what re-opens the
  // machine; a second tick on top of it would race the first.
  if (state_ != TickState::kIdle)
    return false;
  if (!pending_)
    return false;
  // Both of the remaining refusals deliberately leave pending_ set: the
  // request has not been serviced, only postponed.
  if (minimized_)
    return false;
  if (!aFreeSlotExists)
    return false;

  pending_ = false;
  state_ = TickState::kTickQueued;
  return true;
}

void TickMachine::beginCallbacks() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Asserted rather than guarded: these two are driven by the frame
  // callback in a fixed order, so a wrong predecessor is a caller bug, not
  // a state this should absorb. The guarded methods below are the ones a
  // second source can legitimately reach.
  assert(state_ == TickState::kTickQueued && "callbacks began without a tick");
  state_ = TickState::kCallbacksRunning;
}

void TickMachine::endCallbacksDrew(bool drew) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(
      state_ == TickState::kCallbacksRunning &&
      "callbacks ended without having begun");
  // Nothing was rendered, so there is no publication to wait for and the
  // frame is already over.
  state_ = drew ? TickState::kGpuPending : TickState::kIdle;
}

void TickMachine::publicationDropped() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Guarded rather than an unconditional store: publishFrame() already
  // handles its own refusal, so a caller that reports the drop as well must
  // not clobber a tick that has since started. pending_ is untouched -- a
  // request made while the GPU work was in flight is still owed a frame.
  if (state_ == TickState::kGpuPending)
    state_ = TickState::kIdle;
}

void TickMachine::setMinimized(bool minimized) {
  std::lock_guard<std::mutex> lock(mutex_);
  minimized_ = minimized;
}

TickState TickMachine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

uint64_t TickMachine::readyPublicationId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return readyPublicationId_;
}

uint64_t TickMachine::presentingPublicationId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return presentingPublicationId_;
}

bool TickMachine::publishFrame(SurfaceTable &table, SurfaceRef ref) {
  // One critical section for both halves. See the header: splitting these is
  // precisely the defect the composed entry point exists to prevent.
  return table.withLock([&](SurfaceTable::LockToken &token) -> bool {
    const bool published = table.publishLocked(token, ref);
    std::lock_guard<std::mutex> lock(mutex_);
    if (published) {
      assert(
          state_ == TickState::kGpuPending &&
          "publishing a frame the machine did not start");
      // The successful publication is the linearization point: from here the
      // frame belongs to the presenter until it takes it.
      state_ = TickState::kPublishedAwaitingTake;
      // And it is therefore also the one place a publication id may be
      // assigned (see readyPublicationId()). Both locks are held here --
      // the table's, taken by withLock() above, and this machine's -- so
      // the id and the state move together with the publication itself.
      // Ids start at 1, leaving 0 to mean "nothing has ever been
      // published".
      readyPublicationId_ = ++publicationCounter_;
    } else if (state_ == TickState::kGpuPending) {
      // Refused -- retired generation, or the window is closing. The frame is
      // over, exactly as for publicationDropped().
      state_ = TickState::kIdle;
    }
    return published;
  });
}

std::optional<SurfaceRef> TickMachine::takeFrame(SurfaceTable &table) {
  return table.withLock(
      [&](SurfaceTable::LockToken &token) -> std::optional<SurfaceRef> {
        std::optional<SurfaceRef> taken = table.takeLocked(token);
        if (taken) {
          std::lock_guard<std::mutex> lock(mutex_);
          // Only a take that actually got the frame this machine published
          // completes the frame. A take during any other state belongs to a
          // frame this machine is not tracking.
          if (state_ == TickState::kPublishedAwaitingTake)
            state_ = TickState::kIdle;
          // The taken frame is now the presenting one, so its publication
          // id is what the presenter reports for every frame it shows --
          // including the re-blits that follow, which take nothing and so
          // leave this untouched. That is the whole point of the field:
          // see readyPublicationId().
          presentingPublicationId_ = readyPublicationId_;
        }
        // Nothing ready: the producer has not published yet. The state is
        // deliberately left alone -- the next frame callback finds the frame.
        return taken;
      });
}

} // namespace node_compat
} // namespace hermes
