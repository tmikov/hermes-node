/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/surface_table.h>

#include <cassert>

namespace hermes {
namespace node_compat {

namespace {

/// A SurfaceRef reaching this table comes back from a GPU completion on
/// another thread, so its slot index is worth checking rather than trusting:
/// every other contract violation in this file asserts, and an out-of-range
/// index would otherwise read past a generation's slot array. Debug builds
/// abort; release builds treat the ref as naming nothing.
bool slotInRange(SurfaceRef ref) {
  assert(ref.slot < kSlotsPerGeneration && "slot index out of range");
  return ref.slot < kSlotsPerGeneration;
}

} // namespace

//===========================================================================
// LockedScope
//===========================================================================

SurfaceTable::LockedScope::LockedScope(SurfaceTable &table)
    : table_(table), lock_(table.mutex_) {}

SurfaceTable::LockedScope::~LockedScope() {
  // Extract the work under the lock, release the lock, then invoke. A
  // destroy function frees GPU textures and posts to another thread; running
  // it inline would both hold the lock across unbounded work and deadlock
  // any dispatcher that comes back through the table.
  std::vector<GenerationId> dead;
  dead.swap(table_.pendingDestroy_);
  // Before the lock goes, not after: a token that outlives this scope must
  // never authorise a transition.
  if (token_)
    SurfaceTable::invalidateToken(*token_);
  lock_.unlock();

  if (!table_.destroy_)
    return;
  for (GenerationId gen : dead)
    table_.destroy_(gen);
}

//===========================================================================
// Construction
//===========================================================================

SurfaceTable::SurfaceTable(
    std::function<void(GenerationId)> destroyOnOwnerThread)
    : destroy_(std::move(destroyOnOwnerThread)) {}

SurfaceTable::~SurfaceTable() {
  // Deliberately does not hand the surviving generations to the destroy
  // function. The table is torn down by its owner, on the very thread that
  // function posts to, and that owner's teardown path frees the textures
  // itself; calling back into a dispatcher mid-destruction would be the
  // riskier of the two.
}

//===========================================================================
// Private helpers. All of these assume the lock is held.
//===========================================================================

SurfaceTable::Generation *SurfaceTable::findGeneration(GenerationId gen) {
  auto it = generations_.find(gen.value);
  return it == generations_.end() ? nullptr : &it->second;
}

const SurfaceTable::Generation *SurfaceTable::findGeneration(
    GenerationId gen) const {
  auto it = generations_.find(gen.value);
  return it == generations_.end() ? nullptr : &it->second;
}

void SurfaceTable::releaseGeneration(GenerationId gen, int count) {
  assert(count > 0);
  auto it = generations_.find(gen.value);
  assert(it != generations_.end() && "releasing a destroyed generation");
  if (it == generations_.end())
    return;

  assert(it->second.refs >= count && "generation reference count underflow");
  it->second.refs -= count;
  if (it->second.refs > 0)
    return;

  assert(!it->second.current && "a current generation still holds a reference");
  assert(it->second.wrappers == 0 && "a wrapper still holds a reference");
  // Erase before the destroy function can run: the generation is gone from
  // this table's point of view the moment its count reaches zero, and the
  // handover happens once the lock is released.
  //
  // Callers may hold a Generation * to a *different* generation across this
  // call -- publish() and take() both do. That is safe because generations_
  // is a std::map, whose erase invalidates references only to the element
  // erased; it would not be safe over a vector or after a rehash.
  generations_.erase(it);
  pendingDestroy_.push_back(gen);
}

void SurfaceTable::releaseSlotOwnership(SurfaceRef ref) {
  Generation *generation = findGeneration(ref.gen);
  assert(generation && "releasing a slot of a destroyed generation");
  if (!generation || !slotInRange(ref))
    return;

  Slot &slot = generation->slots[ref.slot];
  assert(slot.state != SlotState::kFree && "slot was not owned");

  // The pair that must be indivisible. Reading the counts and storing the
  // new state in one critical section is what stops a completion landing
  // between them: either it has already decremented, in which case the slot
  // goes straight to FREE, or it has not, in which case it will find
  // RETIRING and free the slot itself.
  //
  // gpuWork counts for this exactly as reads does, and the spec says so of
  // reclamation in as many words: "Reclamation waits for the producer's own
  // GPU work to complete, exactly as a publication would." Consulting only
  // `reads` sent a reclaimed slot whose command buffer was still executing
  // straight to FREE, where the very next acquire() handed it back out --
  // the producer then writing into a surface the GPU had not finished
  // writing into. Both completions come back through the same RETIRING ->
  // FREE door below, and whichever of them is last through it frees the
  // slot.
  if (slot.reads > 0 || slot.gpuWork > 0) {
    slot.state = SlotState::kRetiring;
    return;
  }

  slot.state = SlotState::kFree;
  releaseGeneration(ref.gen, 1);
}

//===========================================================================
// Transitions, assuming the lock is held.
//===========================================================================

GenerationId
SurfaceTable::newGenerationLocked(LockToken &token, int width, int height) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  if (currentGeneration_.value != 0) {
    GenerationId previous = currentGeneration_;
    Generation *generation = findGeneration(previous);
    assert(generation && "the current generation cannot have been destroyed");
    generation->current = false;
    currentGeneration_ = GenerationId{};
    // Retiring a generation is exactly the act of dropping the current-
    // generation reference. One that never presented anything dies here.
    releaseGeneration(previous, 1);
  }

  GenerationId id{nextGenerationId_++};
  Generation generation;
  generation.width = width;
  generation.height = height;
  generation.current = true;
  generation.refs = 1; // the current-generation reference
  generations_.emplace(id.value, generation);
  currentGeneration_ = id;
  return id;
}

std::optional<SurfaceRef> SurfaceTable::acquireLocked(LockToken &token) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  // Refused while closing, matching publish(): CLOSING refuses further frame
  // callbacks, and this is the first thing one does.
  if (closing_)
    return std::nullopt;

  Generation *generation = findGeneration(currentGeneration_);
  if (!generation)
    return std::nullopt;

  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    Slot &slot = generation->slots[i];
    if (slot.state != SlotState::kFree)
      continue;
    slot.state = SlotState::kWriting;
    ++generation->refs; // CPU writing ownership
    return SurfaceRef{currentGeneration_, static_cast<SlotId>(i)};
  }
  return std::nullopt;
}

bool SurfaceTable::publishLocked(LockToken &token, SurfaceRef ref) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  // The test and the state change happen together: a generation cannot
  // retire, and the window cannot start closing, between them.
  if (closing_)
    return false;
  if (ref.gen != currentGeneration_)
    return false;
  if (!slotInRange(ref))
    return false;

  Generation *generation = findGeneration(ref.gen);
  assert(generation && "the current generation cannot have been destroyed");
  if (!generation)
    return false;

  Slot &slot = generation->slots[ref.slot];
  assert(slot.state == SlotState::kWriting && "publishing an unowned slot");
  if (slot.state != SlotState::kWriting)
    return false;

  // A READY frame nobody took is superseded. It was never presented, so no
  // blit can be sampling it; the newer frame is the one worth showing.
  // releaseSlotOwnership still holds the slot in RETIRING if the producer's
  // own GPU work is outstanding, so this is a release, not a straight return
  // to FREE. Unreachable when a tick machine paces the producer, which keeps
  // one frame in flight.
  if (ready_ && *ready_ != ref)
    releaseSlotOwnership(*ready_);

  slot.state = SlotState::kReady;
  ready_ = ref;
  return true;
}

void SurfaceTable::reclaimLocked(LockToken &token, SurfaceRef ref) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(ref.gen);
  assert(generation && "reclaiming a slot of a destroyed generation");
  if (!generation || !slotInRange(ref))
    return;

  Slot &slot = generation->slots[ref.slot];
  assert(slot.state == SlotState::kWriting && "reclaiming an unowned slot");
  if (slot.state != SlotState::kWriting)
    return;

  releaseSlotOwnership(ref);
}

std::optional<SurfaceRef> SurfaceTable::takeLocked(LockToken &token) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  if (!ready_)
    return std::nullopt;

  SurfaceRef taken = *ready_;
  ready_.reset();

  // Deliberately not conditioned on the generation still being current. A
  // frame published just before a resize is shown rather than thrown away:
  // the presenter stretches it, and the producer's work is already done.
  // wrappersReleasableLocked() counts kReady for exactly this reason.
  Generation *generation = findGeneration(taken.gen);
  assert(generation && "a READY slot holds its generation alive");
  if (!generation)
    return std::nullopt;

  Slot &slot = generation->slots[taken.slot];
  assert(slot.state == SlotState::kReady);
  slot.state = SlotState::kPresenting;

  // Displacement happens in the same critical section as the take. Splitting
  // them is what stranded a slot in the design this replaced.
  if (presenting_ && *presenting_ != taken)
    releaseSlotOwnership(*presenting_);
  presenting_ = taken;
  return taken;
}

void SurfaceTable::addReadLocked(LockToken &token, SurfaceRef ref) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(ref.gen);
  // A blit against a destroyed generation means a reference was dropped too
  // early -- the bug this whole protocol exists to prevent.
  assert(generation && "reading a destroyed generation");
  if (!generation || !slotInRange(ref))
    return;

  Slot &slot = generation->slots[ref.slot];
  // Retirement forbids new producer ownership, not new readers: a retired
  // surface keeps being re-presented until a current-generation surface
  // replaces it, and each of those blits takes a read reference as usual.
  assert(
      slot.state != SlotState::kFree && slot.state != SlotState::kWriting &&
      "reading a slot the producer owns");
  ++slot.reads;
  ++generation->refs;
}

void SurfaceTable::releaseReadLocked(LockToken &token, SurfaceRef ref) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(ref.gen);
  assert(generation && "an outstanding read holds its generation alive");
  if (!generation || !slotInRange(ref))
    return;

  Slot &slot = generation->slots[ref.slot];
  assert(slot.reads > 0 && "read reference count underflow");
  if (slot.reads == 0)
    return;
  --slot.reads;

  // Freeing happens only from RETIRING. A completion never frees a slot that
  // is still PRESENTING, and it never frees one outright.
  //
  // Both counts have to be zero, not just this one: a slot can be RETIRING
  // because a blit is sampling it, because the producer's own work is still
  // in flight, or both, and the last completion of either kind is the one
  // that frees it. endGpuWorkLocked() carries the mirror image of this
  // check.
  int references = 1; // the read's own
  if (slot.reads == 0 && slot.gpuWork == 0 &&
      slot.state == SlotState::kRetiring) {
    slot.state = SlotState::kFree;
    ++references; // and the slot's, which it held while RETIRING
  }
  releaseGeneration(ref.gen, references);
}

void SurfaceTable::addWrapperLocked(LockToken &token, GenerationId gen) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(gen);
  assert(generation && "wrapping a destroyed generation");
  if (!generation)
    return;
  ++generation->wrappers;
  ++generation->refs;
}

void SurfaceTable::releaseWrappersLocked(LockToken &token, GenerationId gen) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(gen);
  assert(generation && "releasing wrappers of a destroyed generation");
  if (!generation)
    return;

  int wrappers = generation->wrappers;
  if (wrappers == 0)
    return;
  generation->wrappers = 0;
  releaseGeneration(gen, wrappers);
}

void SurfaceTable::beginGpuWorkLocked(LockToken &token, SurfaceRef ref) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(ref.gen);
  assert(generation && "submitting work against a destroyed generation");
  if (!generation || !slotInRange(ref))
    return;
  ++generation->slots[ref.slot].gpuWork;
  ++generation->refs;
}

void SurfaceTable::endGpuWorkLocked(LockToken &token, SurfaceRef ref) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  Generation *generation = findGeneration(ref.gen);
  // The reference taken before submission is what makes this lookup safe,
  // however long the completion handler takes and whatever happened to the
  // slot in the meantime.
  assert(generation && "pending GPU work holds its generation alive");
  if (!generation || !slotInRange(ref))
    return;

  Slot &slot = generation->slots[ref.slot];
  assert(slot.gpuWork > 0 && "unbalanced endGpuWork");
  if (slot.gpuWork == 0)
    return;
  --slot.gpuWork;

  // The mirror of releaseReadLocked()'s check, and required for the same
  // reason: releaseSlotOwnership() leaves a slot RETIRING while this work
  // is outstanding, so this completion may be the last owner and must be
  // able to free it. Without this the slot stays RETIRING for good -- the
  // stranding defect, reached through the other door.
  int references = 1; // the GPU work's own
  if (slot.gpuWork == 0 && slot.reads == 0 &&
      slot.state == SlotState::kRetiring) {
    slot.state = SlotState::kFree;
    ++references; // and the slot's, which it held while RETIRING
  }
  releaseGeneration(ref.gen, references);
}

bool SurfaceTable::wrappersReleasableLocked(LockToken &token, GenerationId gen)
    const {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  // An id this table never issued names no generation at all -- a
  // default-constructed GenerationId{} (0), or one from a future the caller
  // is guessing at. "Yes, destroy your wrappers" is the wrong answer to a
  // question about a generation that does not exist: the honest answer is
  // that there is nothing to release, and it keeps a caller polling ids
  // around the current one from reading a spurious yes that turns into a no
  // the moment that id is actually issued.
  if (gen.value == 0 || gen.value >= nextGenerationId_)
    return false;

  const Generation *generation = findGeneration(gen);
  // Issued and since destroyed: its wrappers were released long ago, since a
  // wrapper is itself a counted owner.
  if (!generation)
    return true;
  if (generation->current)
    return false;

  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    SlotState state = generation->slots[i].state;
    // READY counts as well as PRESENTING and RETIRING. READY because take()
    // will still show a frame published just before the resize, so its
    // wrapper is needed after its generation stops being current; RETIRING
    // because a displaced surface is still being sampled by blits already
    // submitted. Counting only PRESENTING would let the presenting instance
    // destroy a wrapper the very next take() needs.
    if (state == SlotState::kReady || state == SlotState::kPresenting ||
        state == SlotState::kRetiring)
      return false;
  }
  return true;
}

void SurfaceTable::beginClosingLocked(LockToken &token) {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  closing_ = true;
}

bool SurfaceTable::anyFreeSlotLocked(LockToken &token) const {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  // Closing first, so that a caller gating the frame tick on this is never
  // told yes and then handed an empty acquire().
  if (closing_)
    return false;

  const Generation *generation = findGeneration(currentGeneration_);
  if (!generation)
    return false;
  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    if (generation->slots[i].state == SlotState::kFree)
      return true;
  }
  return false;
}

SlotState SurfaceTable::stateLocked(LockToken &token, SurfaceRef ref) const {
  assert(tokenHeld(token) && "stale or foreign lock token");
  (void)token;

  const Generation *generation = findGeneration(ref.gen);
  if (!generation || !slotInRange(ref))
    return SlotState::kFree;
  return generation->slots[ref.slot].state;
}

//===========================================================================
// Public entry points. Each one is the matching transition wrapped in the
// lock, so composing two of them under withLock() runs identical code.
//===========================================================================

GenerationId SurfaceTable::newGeneration(int width, int height) {
  LockedScope scope(*this);
  LockToken token(this);
  return newGenerationLocked(token, width, height);
}

std::optional<SurfaceRef> SurfaceTable::acquire() {
  LockedScope scope(*this);
  LockToken token(this);
  return acquireLocked(token);
}

bool SurfaceTable::publish(SurfaceRef ref) {
  LockedScope scope(*this);
  LockToken token(this);
  return publishLocked(token, ref);
}

void SurfaceTable::reclaim(SurfaceRef ref) {
  LockedScope scope(*this);
  LockToken token(this);
  reclaimLocked(token, ref);
}

std::optional<SurfaceRef> SurfaceTable::take() {
  LockedScope scope(*this);
  LockToken token(this);
  return takeLocked(token);
}

void SurfaceTable::addRead(SurfaceRef ref) {
  LockedScope scope(*this);
  LockToken token(this);
  addReadLocked(token, ref);
}

void SurfaceTable::releaseRead(SurfaceRef ref) {
  LockedScope scope(*this);
  LockToken token(this);
  releaseReadLocked(token, ref);
}

void SurfaceTable::addWrapper(GenerationId gen) {
  LockedScope scope(*this);
  LockToken token(this);
  addWrapperLocked(token, gen);
}

void SurfaceTable::releaseWrappers(GenerationId gen) {
  LockedScope scope(*this);
  LockToken token(this);
  releaseWrappersLocked(token, gen);
}

void SurfaceTable::beginGpuWork(SurfaceRef ref) {
  LockedScope scope(*this);
  LockToken token(this);
  beginGpuWorkLocked(token, ref);
}

void SurfaceTable::endGpuWork(SurfaceRef ref) {
  LockedScope scope(*this);
  LockToken token(this);
  endGpuWorkLocked(token, ref);
}

void SurfaceTable::beginClosing() {
  LockedScope scope(*this);
  LockToken token(this);
  beginClosingLocked(token);
}

// The three const accessors cannot start a destruction, so they take the
// lock directly rather than through LockedScope.

bool SurfaceTable::wrappersReleasable(GenerationId gen) const {
  std::lock_guard<std::mutex> lock(mutex_);
  LockToken token(this);
  return wrappersReleasableLocked(token, gen);
}

bool SurfaceTable::anyFreeSlot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  LockToken token(this);
  return anyFreeSlotLocked(token);
}

SlotState SurfaceTable::state(SurfaceRef ref) const {
  std::lock_guard<std::mutex> lock(mutex_);
  LockToken token(this);
  return stateLocked(token, ref);
}

} // namespace node_compat
} // namespace hermes
