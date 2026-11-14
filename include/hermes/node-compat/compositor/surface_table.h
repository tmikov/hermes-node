/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_SURFACE_TABLE_H
#define HERMES_NODE_COMPAT_COMPOSITOR_SURFACE_TABLE_H

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace hermes {
namespace node_compat {

/// Index of a slot within its generation. Never meaningful on its own: a
/// slot is only identified together with the generation that owns it.
using SlotId = uint8_t;

/// Identifies one allocation of surfaces. A resize starts a new generation;
/// the value 0 is reserved for "no generation".
struct GenerationId {
  uint32_t value = 0;
};

inline bool operator==(GenerationId a, GenerationId b) {
  return a.value == b.value;
}
inline bool operator!=(GenerationId a, GenerationId b) {
  return a.value != b.value;
}

/// Generation-qualified. A GPU callback carries this, never a bare SlotId:
/// the slot may have been reused by the time the callback runs.
struct SurfaceRef {
  GenerationId gen;
  SlotId slot = 0;
};

inline bool operator==(SurfaceRef a, SurfaceRef b) {
  return a.gen == b.gen && a.slot == b.slot;
}
inline bool operator!=(SurfaceRef a, SurfaceRef b) {
  return !(a == b);
}

/// RETIRING is the state a displaced surface occupies: no longer the
/// presenting image, but blits that sampled it may still be executing. It is
/// neither reusable nor presentable, and it is the reason "stopped
/// presenting" and "reusable" are two different things.
enum class SlotState { kFree, kWriting, kReady, kPresenting, kRetiring };

/// Slots per generation: one presenting, one retired-but-still-being-read,
/// and one for the producer to write. A fourth would only improve throughput
/// under measured consumer latency; it is not a stronger guarantee.
constexpr int kSlotsPerGeneration = 3;

/// The protocol by which the producer thread and the presenting thread hand
/// surfaces back and forth without either writing one the other is reading.
///
/// **One mutex guards everything**, and every public method takes it for its
/// whole transition. This is deliberate, and it is a recorded deviation from
/// the design document's original "one atomic word plus a CAS retry loop":
/// the transitions here are compound. Displacement must test the outgoing
/// slot's read count and store its new state *together*, or a completion
/// landing between the two strands the slot in RETIRING with nobody left to
/// free it. Separate atomics cannot express that pair.
///
/// Lifetime is a per-generation reference count. Its owners are the
/// current-generation reference the table itself holds, each non-FREE slot,
/// each outstanding read, each wrapper, and each piece of in-flight producer
/// GPU work (which spans submission through the completion handler's last
/// access). A generation is destroyed when the count reaches zero, which
/// implies it is no longer current.
///
/// Threading: every method may be called from any thread. The destroy
/// function is invoked **with the lock released**, on whichever thread
/// dropped the last reference; it is expected to post the work to the JS
/// owner thread rather than to free textures inline.
class SurfaceTable {
 public:
  /// Proof that the caller holds the lock. Only SurfaceTable can make one,
  /// so a *Locked helper cannot be reached without going through withLock().
  ///
  /// It carries the table it was issued for and a validity flag that
  /// withLock() clears before it releases the lock. Neither is needed for a
  /// caller that plays by the rules; both exist so that one that does not --
  /// stashing the token and using it after the transaction, or passing it to
  /// a second table -- fails an assertion instead of silently running a
  /// transition with no lock held.
  class LockToken {
   public:
    LockToken(const LockToken &) = delete;
    LockToken &operator=(const LockToken &) = delete;

   private:
    explicit LockToken(const SurfaceTable *owner) : owner_(owner) {}

    const SurfaceTable *owner_ = nullptr;
    bool valid_ = true;
    friend class SurfaceTable;
  };

  /// \param destroyOnOwnerThread invoked when a generation's last reference
  ///   is dropped, never with the lock held and never inline from within a
  ///   transition. It must not throw.
  explicit SurfaceTable(std::function<void(GenerationId)> destroyOnOwnerThread);
  ~SurfaceTable();

  SurfaceTable(const SurfaceTable &) = delete;
  SurfaceTable &operator=(const SurfaceTable &) = delete;

  /// Allocate a generation of kSlotsPerGeneration slots and make it current,
  /// retiring the previous one. Retiring is exactly the act of dropping the
  /// current-generation reference, so a generation that never presented
  /// anything is destroyed here.
  GenerationId newGeneration(int width, int height);

  /// FREE -> WRITING, in the current generation only. Empty when no slot is
  /// free, which is the backpressure that paces rendering: the frame tick is
  /// withheld rather than queued.
  ///
  /// Refused while closing, for the same reason publish() is: CLOSING
  /// refuses further frame callbacks, and acquiring a slot is the first
  /// thing a frame callback does.
  std::optional<SurfaceRef> acquire();

  /// WRITING -> READY. False if the generation has retired or the window is
  /// closing, in which case the caller must reclaim() the slot.
  ///
  /// A READY frame nobody has taken yet is superseded and its slot is
  /// released: it was never presented, so no blit can be sampling it, and
  /// the newer frame is the one worth showing. The slot returns to FREE only
  /// if the producer's own GPU work has completed as well -- otherwise it
  /// waits in RETIRING, exactly as reclaim() does. The tick machine keeps
  /// one frame in flight, so this is reachable only from a producer that
  /// does not pace itself against take().
  bool publish(SurfaceRef ref);

  /// WRITING -> FREE, for a tick that drew nothing and for a publication
  /// that was refused.
  ///
  /// **Reclamation waits for the producer's own GPU work exactly as a
  /// publication would** (spec: "Reclaim"). A slot whose beginGpuWork() has
  /// not been balanced goes to RETIRING rather than FREE, and endGpuWork()
  /// frees it -- otherwise the slot is handed straight back to acquire()
  /// while a submitted command buffer is still writing into it.
  void reclaim(SurfaceRef ref);

  /// READY -> PRESENTING, displacing the outgoing surface in the same
  /// critical section. The successful take is the linearization point that
  /// defines "newest".
  ///
  /// A retired generation's READY frame is still taken. It was published
  /// before the resize, the presenter can show it (it stretches), and
  /// refusing it would throw away work the producer had already finished for
  /// no gain. That decision is why wrappersReleasable() has to count READY
  /// as well as PRESENTING and RETIRING: the wrapper for such a frame is
  /// needed after its generation stops being current.
  std::optional<SurfaceRef> take();

  /// Take a read reference for a blit about to be submitted. Permitted on a
  /// retired generation: retirement forbids new *producer* ownership only,
  /// and a retired surface keeps being re-presented until a current-
  /// generation surface replaces it.
  void addRead(SurfaceRef ref);

  /// Release a read reference when its blit has completed on the GPU. Frees
  /// the slot only from RETIRING: it never frees one that is still
  /// PRESENTING.
  void releaseRead(SurfaceRef ref);

  /// The presenting instance registers one wrapper per surface of a
  /// generation, and calls releaseWrappers() when it has destroyed them.
  void addWrapper(GenerationId gen);
  void releaseWrappers(GenerationId gen);

  /// Producer GPU work, acquired BEFORE submission and released when the
  /// completion handler has made its last access. The ref identifies which
  /// surface the work belongs to, so a reused slot cannot be mistaken for
  /// it; the reference it holds is on the generation, so it outlives the
  /// slot returning to FREE.
  ///
  /// One reference, not two. The design lists "producer GPU work" and "a
  /// pending completion handler" as separate owners, but they span a single
  /// contiguous interval -- submission through the handler's last access --
  /// with no point between them where one could be released without the
  /// other, so a second reference would have an identical lifetime.
  ///
  /// Outstanding work also **gates reuse of the slot**, not only the
  /// lifetime of the generation: a slot released while work is in flight
  /// becomes RETIRING, and endGpuWork() is what finally frees it -- the
  /// same rule, and the same code path, as an outstanding read.
  void beginGpuWork(SurfaceRef ref);
  void endGpuWork(SurfaceRef ref);

  /// Whether the presenting instance may destroy its wrappers for \p gen:
  /// the generation has stopped being current *and* has no READY,
  /// PRESENTING or RETIRING slot left. Waiting for the reference count to
  /// reach zero instead would deadlock, since wrappers are themselves
  /// counted owners.
  bool wrappersReleasable(GenerationId gen) const;

  /// Refuse further acquisition and publication. Every exit path enters
  /// this state before tearing the window down.
  void beginClosing();

  /// Whether the current generation had a slot the producer could acquire
  /// **at the instant this ran**. A retired generation's free slots are not
  /// acquirable and do not count, and neither does anything once closing has
  /// begun.
  ///
  /// The answer is a snapshot, not a promise: another thread may acquire the
  /// last free slot before this call returns. A caller that gates the frame
  /// tick must therefore not call this and then move tick state in a second
  /// critical section -- TickMachine::shouldSendTick(SurfaceTable &) exists
  /// so that the test and the transition compose under one lock, exactly as
  /// publishFrame() and takeFrame() do. This overload remains for callers
  /// that want the bare reading, and for tests.
  bool anyFreeSlot() const;

  /// For tests and diagnostics. A slot of a generation that has already been
  /// destroyed reports kFree: it owns nothing and cannot be sampled.
  SlotState state(SurfaceRef ref) const;

  /// Run several transitions as one critical section, so that a compound
  /// operation built out of two of them cannot be interleaved. The callback
  /// receives a token proving the lock is held; it must call only the
  /// *Locked helpers with that token. Calling a public method from inside
  /// would re-enter the non-recursive mutex and deadlock.
  ///
  /// Generations that died during the transaction are handed to the destroy
  /// function after the lock is released, exactly as for a single call.
  template <typename Fn>
  auto withLock(Fn &&fn) -> decltype(fn(std::declval<LockToken &>())) {
    // The token is declared BEFORE the scope, so that it outlives it.
    // Locals are destroyed in reverse, so ~LockedScope invalidates a token
    // that is still alive, and does so before releasing the lock -- which is
    // the point: a token that outlives the transaction must not authorise
    // one. Declared the other way round, that write lands on an object whose
    // scope has ended, which is what ASAN caught here.
    LockToken token(this);
    LockedScope scope(*this);
    scope.invalidateOnExit(token);
    return fn(token);
  }

  /// The transitions above, assuming the lock is already held. Each public
  /// method is a thin wrapper over the matching helper, so composing two of
  /// them under withLock() runs exactly the code a single call would.
  GenerationId newGenerationLocked(LockToken &token, int width, int height);
  std::optional<SurfaceRef> acquireLocked(LockToken &token);
  bool publishLocked(LockToken &token, SurfaceRef ref);
  void reclaimLocked(LockToken &token, SurfaceRef ref);
  std::optional<SurfaceRef> takeLocked(LockToken &token);
  void addReadLocked(LockToken &token, SurfaceRef ref);
  void releaseReadLocked(LockToken &token, SurfaceRef ref);
  void addWrapperLocked(LockToken &token, GenerationId gen);
  void releaseWrappersLocked(LockToken &token, GenerationId gen);
  void beginGpuWorkLocked(LockToken &token, SurfaceRef ref);
  void endGpuWorkLocked(LockToken &token, SurfaceRef ref);
  bool wrappersReleasableLocked(LockToken &token, GenerationId gen) const;
  void beginClosingLocked(LockToken &token);
  bool anyFreeSlotLocked(LockToken &token) const;
  SlotState stateLocked(LockToken &token, SurfaceRef ref) const;

 private:
  struct Slot {
    SlotState state = SlotState::kFree;
    /// Blits submitted against this surface that have not yet completed.
    /// Non-zero **gates reuse**: a slot released with reads outstanding
    /// becomes RETIRING rather than FREE.
    int reads = 0;
    /// Producer GPU work submitted against this surface and not yet
    /// completed. Non-zero **gates reuse in exactly the same way** as
    /// `reads` above: a reclaimed or displaced slot with work still in
    /// flight becomes RETIRING, and endGpuWork() frees it. The two counts
    /// are symmetric on purpose -- freeing a slot the producer's own
    /// command buffer is still writing into is the same defect as freeing
    /// one a blit is still sampling.
    int gpuWork = 0;
  };

  struct Generation {
    int width = 0;
    int height = 0;
    Slot slots[kSlotsPerGeneration];
    /// Every owner listed in the class comment. Zero means destroy.
    int refs = 0;
    bool current = false;
    int wrappers = 0;
  };

  /// Holds the lock for one transaction and, on destruction, releases it
  /// and only then hands any generations that died to the destroy function.
  /// Extract under the lock, unlock, then invoke -- never inline.
  class LockedScope {
   public:
    explicit LockedScope(SurfaceTable &table);
    ~LockedScope();

    LockedScope(const LockedScope &) = delete;
    LockedScope &operator=(const LockedScope &) = delete;

    /// Invalidate \p token on the way out, before the lock is released.
    void invalidateOnExit(LockToken &token) {
      token_ = &token;
    }

   private:
    SurfaceTable &table_;
    std::unique_lock<std::mutex> lock_;
    LockToken *token_ = nullptr;
  };

  static void invalidateToken(LockToken &token) {
    token.valid_ = false;
  }

  /// Whether \p token authorises a transition on *this* table right now.
  /// Asserted by every *Locked helper.
  bool tokenHeld(const LockToken &token) const {
    return token.valid_ && token.owner_ == this;
  }

  /// Null when the generation has already been destroyed.
  Generation *findGeneration(GenerationId gen);
  const Generation *findGeneration(GenerationId gen) const;

  /// Drop \p count references. The generation may be destroyed here, so no
  /// reference or pointer to it survives this call.
  void releaseGeneration(GenerationId gen, int count);

  /// A slot stops being owned by the producer or the presenter: it becomes
  /// reusable only if no blit is still sampling it **and no producer GPU
  /// work of its own is still in flight**, and RETIRING otherwise. This is
  /// the test-and-store pair that must not be split.
  void releaseSlotOwnership(SurfaceRef ref);

  mutable std::mutex mutex_;
  std::function<void(GenerationId)> destroy_;
  /// Keyed by GenerationId::value. Ids increase monotonically from 1, so a
  /// stale ref can never name a live generation.
  ///
  /// std::map rather than a vector or a hash map because its **node
  /// stability** is load-bearing: several transitions hold a Generation *
  /// across a call that can erase a *different* generation (publish()
  /// superseding a READY frame, take() displacing the outgoing surface), and
  /// std::map invalidates references only to the element actually erased.
  std::map<uint32_t, Generation> generations_;
  uint32_t nextGenerationId_ = 1;
  GenerationId currentGeneration_{};
  std::optional<SurfaceRef> ready_;
  std::optional<SurfaceRef> presenting_;
  bool closing_ = false;
  /// Generations whose last reference was dropped during the transaction in
  /// progress, to be handed over once the lock is released.
  std::vector<GenerationId> pendingDestroy_;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_SURFACE_TABLE_H
