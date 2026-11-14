/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/event_dispatcher.h>
#include <hermes/node-compat/compositor/frame_callbacks.h>
#include <hermes/node-compat/compositor/hnui_event_convert.h>
#include <hermes/node-compat/compositor/owner_dispatch.h>
#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>

#include <uv.h>

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace hermes::node_compat;

namespace {

//===========================================================================
// A per-test watchdog.
//
// Several invariants in this file can only fail by deadlocking -- holding
// the lock across the destroy function, or calling a public method from
// inside withLock -- and two of the mutations these tests are verified
// against do exactly that. A deadlock with no timeout hangs the suite
// instead of failing it, which reports nothing at all. gtest has no
// per-test timeout, and this repo's lit configuration (unittests/lit.cfg,
// lit.formats.GoogleTest) sets none, so the binary carries its own.
//
// The bound is far above the slowest test here (the sweeps, ~2.5 s
// together), so it can only fire on a hang -- and far enough above it to
// let a failing ASAN report finish symbolizing. Measured: a report took
// longer than 30 seconds, and a watchdog set there aborted in the middle of
// it, throwing away the diagnosis of a real bug.
//===========================================================================

constexpr int kTestTimeoutSeconds = 60;

using Clock = std::chrono::steady_clock;

class TestWatchdog : public ::testing::EmptyTestEventListener {
 public:
  void OnTestStart(const ::testing::TestInfo &info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    name_ = info.name();
    deadline_ = Clock::now() + std::chrono::seconds(kTestTimeoutSeconds);
    if (!thread_.joinable())
      thread_ = std::thread([this] { run(); });
  }

  void OnTestEnd(const ::testing::TestInfo &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    deadline_ = Clock::time_point::max();
  }

  void OnTestProgramEnd(const ::testing::UnitTest &) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    if (thread_.joinable())
      thread_.join();
  }

 private:
  void run() {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_)
        return;
      if (Clock::now() <= deadline_)
        continue;
      fprintf(
          stderr,
          "watchdog: %s exceeded %d seconds -- deadlocked?\n",
          name_.c_str(),
          kTestTimeoutSeconds);
      fflush(stderr);
      std::abort();
    }
  }

  std::mutex mutex_;
  std::thread thread_;
  std::string name_;
  Clock::time_point deadline_ = Clock::time_point::max();
  bool stop_ = false;
};

const bool kWatchdogInstalled = [] {
  ::testing::UnitTest::GetInstance()->listeners().Append(new TestWatchdog());
  return true;
}();

/// Dereferencing the optional acquire() returns is undefined behaviour when
/// it is empty, and reads back whatever the storage happened to hold --
/// quite possibly a plausible value, which would let a mutation that breaks
/// acquisition look like a pass. Every acquisition in these tests that
/// expects to succeed goes through here.
SurfaceRef mustAcquire(SurfaceTable &table) {
  auto ref = table.acquire();
  EXPECT_TRUE(ref.has_value()) << "acquire() unexpectedly returned nothing";
  return ref.value_or(SurfaceRef{});
}

//===========================================================================
// OwnerDispatch test support
//===========================================================================

/// A real uv_loop_t, torn down safely even though OwnerDispatch::close()
/// deliberately does not wait for its async handle's uv_close() callback:
/// that callback can still be pending when this loop needs to go away, and
/// uv_loop_close() reports UV_EBUSY until every handle has actually
/// finished closing. Pumping the loop until it succeeds is what flushes
/// that callback.
struct TestLoop {
  uv_loop_t loop{};

  TestLoop() {
    EXPECT_EQ(uv_loop_init(&loop), 0);
  }

  ~TestLoop() {
    while (uv_loop_close(&loop) == UV_EBUSY)
      uv_run(&loop, UV_RUN_DEFAULT);
  }

  TestLoop(const TestLoop &) = delete;
  TestLoop &operator=(const TestLoop &) = delete;
};

} // namespace

TEST(CompositorScaffold, TargetBuildsAndRuns) {
  EXPECT_TRUE(kWatchdogInstalled);
}

//===========================================================================
// The surface protocol.
//
// Every test here names, in a comment, the mutation that must break it. A
// test that survives its mutation is not a test.
//===========================================================================

TEST(SurfaceTableTest, DisplacementAndLastReadCompletionCannotStrandASlot) {
  // The previous design read the read-count and then stored RETIRING as two
  // separate atomic operations. Interleaving: displacement observes reads==1;
  // the completion handler decrements to 0 and fails its CAS against
  // PRESENTING; displacement then stores RETIRING, and no completion is left
  // to free it. One lock makes the pair indivisible.
  //
  // That window is a handful of instructions wide, so firing the two
  // operations at each other once and hoping is not a test: measured, a
  // single unsynchronized shot lands outside the window every time, and the
  // defect survives 1000 repeats of it. Round i therefore spins i times
  // before releasing, sweeping the completion across the window until some
  // round lands inside it.
  //
  // What that buys, measured rather than assumed: nothing dependable. The
  // sweep caught a split take() whose halves are separated by a yield in
  // one run and let the identical mutation through in the next, and it
  // never caught a BARE split -- the two critical sections back to back, a
  // window of tens of nanoseconds -- in 4000 rounds. Sweeping the main
  // thread's side instead is worse again, losing even the yield case. The
  // cause is the same each time: thread wake-up latency is orders of
  // magnitude larger than the window and swamps whichever offset is being
  // swept, so which side of the window a round lands on is luck rather than
  // aim.
  //
  // So this is kept as a cheap single-shot regression check -- it asserts a
  // real invariant and passes deterministically against correct code -- and
  // NOT as the thing that pins that invariant. What pins it is
  // DisplacementRacesCompletionUnderLoad below, which catches both forms of
  // the split. The round count is a fraction of what was measured above,
  // since more rounds were buying no more detection.
  //
  // Against correct code every round passes either way, because the pair is
  // indivisible and neither order can strand the slot.
  //
  // The round count is deliberately small. Rounds buy detection only if
  // sweeping works, and it does not; at 500 rounds this test and the
  // publication sweep below were together ~2.5 s of a ~3 s suite, spent on
  // two tests whose own comments say they pin nothing.
  constexpr int kRounds = 20;
  for (int round = 0; round < kRounds; ++round) {
    SurfaceTable table([](GenerationId) {});
    auto gen = table.newGeneration(64, 64);
    (void)gen;
    auto a = mustAcquire(table);
    table.publish(a);
    table.take();
    table.addRead(a);

    std::atomic<bool> go{false};
    std::thread completion([&] {
      while (!go.load())
        std::this_thread::yield();
      for (int spin = 0; spin < round; ++spin)
        std::atomic_signal_fence(std::memory_order_seq_cst);
      table.releaseRead(a);
    });
    auto b = mustAcquire(table);
    table.publish(b);
    go.store(true);
    table.take(); // displaces a, concurrently with release
    completion.join();

    // Whichever order won, a must end FREE and be acquirable again.
    ASSERT_EQ(table.state(a), SlotState::kFree) << "round " << round;
    auto reacquired = table.acquire();
    ASSERT_TRUE(reacquired.has_value()) << "round " << round;
  }
}
// No mutation reliably breaks this test, measured both ways -- see the
// comment above. The invariant it describes is pinned by
// DisplacementRacesCompletionUnderLoad, immediately below.

TEST(SurfaceTableTest, DisplacementRacesCompletionUnderLoad) {
  // The sweep above aims one release at one displacement, and can only
  // position them to within thread wake-up latency -- orders of magnitude
  // wider than the window it is trying to hit, which is why it only catches
  // an artificially widened split. This test gives up on positioning
  // altogether and lets three threads drift against each other for as long
  // as it takes. That is how a window of tens of nanoseconds is actually
  // reached: every iteration is an independent draw, and there are hundreds
  // of thousands of them.
  //
  // It needs no timing assertion, and that is the point. A stranded slot is
  // PERMANENT -- displaced with a read outstanding, then abandoned by the
  // completion that found it still PRESENTING -- so the pool loses it for
  // good. The end state therefore says everything: once every completion
  // has run, every slot but the presenting one must be acquirable again.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);

  std::atomic<bool> stop{false};
  std::atomic<bool> drained{false};
  std::atomic<int> taken{0};
  std::mutex handoffMutex;
  std::vector<SurfaceRef> handoff;
  size_t head = 0;

  std::thread producer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (auto w = table.acquire()) {
        if (!table.publish(*w))
          table.reclaim(*w);
      }
    }
  });

  // The presenter hands every read it takes to a third thread, so the
  // completion runs concurrently with the NEXT take -- which is exactly the
  // pair that must not be split.
  std::thread presenter([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (auto t = table.take()) {
        table.addRead(*t);
        taken.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(handoffMutex);
        handoff.push_back(*t);
      }
    }
  });

  std::thread completion([&] {
    for (;;) {
      SurfaceRef ref{};
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(handoffMutex);
        if (head < handoff.size()) {
          ref = handoff[head++];
          have = true;
          if (head == handoff.size()) {
            handoff.clear();
            head = 0;
          }
        }
      }
      if (have)
        table.releaseRead(ref);
      else if (drained.load(std::memory_order_acquire))
        return;
    }
  });

  const auto deadline = Clock::now() + std::chrono::milliseconds(750);
  while (Clock::now() < deadline)
    std::this_thread::yield();
  stop.store(true, std::memory_order_release);
  producer.join();
  presenter.join();
  drained.store(true, std::memory_order_release);
  completion.join();

  ASSERT_GT(taken.load(), 1000) << "the threads must have raced for real";

  // Consume whatever the producer published last, so that the only slot
  // still owned is the presenting one.
  table.take();

  int free = 0;
  while (table.acquire())
    ++free;
  EXPECT_EQ(free, kSlotsPerGeneration - 1)
      << "a slot was stranded: displaced with a read outstanding, and the "
         "completion that should have freed it found it still PRESENTING";
}
// Mutation that must break it: split the take()/releaseRead() critical
// sections into separate locks -- with or without a yield between them.

TEST(SurfaceTableTest, PublishRefusesARetiredGeneration) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  table.newGeneration(128, 128); // retires a's generation
  EXPECT_FALSE(table.publish(a)) << "a retired generation may not publish";
  table.reclaim(a);
}
// Mutation: drop the generation check in publish(). Test fails.

TEST(SurfaceTableTest, PublicationRacingRetirementIsOneDecision) {
  // "Publication is one synchronized decision with retirement and closing":
  // a completion handler cannot test "is my generation still live?" and then
  // publish, because the generation can retire in between. The test above
  // checks the outcome single-threaded, which a split test-then-store would
  // also produce; this one races the two, sweeping the retirement across
  // the publication. Both sweep directions were measured and neither
  // catches a split publish(), with or without a yield between its halves,
  // for the reason spelled out in the displacement sweep above: wake-up
  // latency swamps the window. It is kept as a single-shot check that the
  // outcome is self-consistent whichever side wins, while the invariant
  // itself is pinned by WrapperTriggerNeverGoesBackwardsUnderResizeLoad
  // below.
  //
  // What it watches is the wrapper trigger, because that is what a split
  // decision corrupts: once a retired generation reports its wrappers
  // releasable the presenting instance destroys them, so that answer must
  // never go back to false. A publication landing after retirement makes it
  // do exactly that.
  //
  // Twenty rounds, not two thousand, for the reason given on the sweep
  // above: rounds are only worth their runtime if aiming works.
  constexpr int kRounds = 20;
  for (int round = 0; round < kRounds; ++round) {
    SurfaceTable table([](GenerationId) {});
    auto gen1 = table.newGeneration(64, 64);
    auto a = mustAcquire(table);

    std::atomic<bool> go{false};
    std::atomic<bool> producerDone{false};
    std::atomic<bool> published{false};
    std::thread producer([&] {
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      published.store(table.publish(a), std::memory_order_relaxed);
      producerDone.store(true, std::memory_order_release);
    });

    go.store(true, std::memory_order_release);
    for (int spin = 0; spin < round; ++spin)
      std::atomic_signal_fence(std::memory_order_seq_cst);
    table.newGeneration(128, 128); // retires gen1, racing the publish

    bool sawReleasable = false;
    while (!producerDone.load(std::memory_order_acquire)) {
      if (table.wrappersReleasable(gen1))
        sawReleasable = true;
    }
    producer.join();

    if (sawReleasable) {
      ASSERT_TRUE(table.wrappersReleasable(gen1))
          << "round " << round
          << ": wrappers were releasable and then were not";
    }
    if (published.load(std::memory_order_relaxed)) {
      // It won the race, so the frame belongs to the generation that was
      // current when it was published, and is still showable.
      auto shown = table.take();
      ASSERT_TRUE(shown.has_value()) << "round " << round;
      ASSERT_EQ(shown->gen.value, gen1.value) << "round " << round;
    } else {
      // It lost, so the slot is untouched and the caller reclaims it.
      ASSERT_EQ(table.state(a), SlotState::kWriting) << "round " << round;
      table.reclaim(a);
    }
  }
}
// Mutation: split publish() into two critical sections, testing the
// generation in one and storing READY in the other. Measured: this test
// does NOT catch that, in either sweep direction and whether or not the
// halves are separated by a yield -- the window is too narrow to aim at.
// The load test below is the one that catches it.

TEST(SurfaceTableTest, WrapperTriggerNeverGoesBackwardsUnderResizeLoad) {
  // The paired load test for publication racing retirement, built the same
  // way as the displacement load test: stop aiming, and assert an end-to-end
  // property while three threads drift against each other.
  //
  // Publishing into an already-retired generation leaves nothing behind to
  // find afterwards -- the frame simply gets shown -- so the assertion has
  // to be the property the presenting instance actually relies on: once a
  // retired generation reports its wrappers releasable it has destroyed
  // them, so that answer may never go back to false. A publication that
  // lands after its generation retired makes it do exactly that, which is
  // the wrapper being resurrected after the textures behind it are gone.
  SurfaceTable table([](GenerationId) {});
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> newest{0};
  std::atomic<int> violations{0};
  std::atomic<int> resizes{0};

  newest.store(table.newGeneration(64, 64).value, std::memory_order_release);

  std::thread producer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (auto w = table.acquire()) {
        if (!table.publish(*w))
          table.reclaim(*w);
      }
    }
  });

  std::thread resizer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      newest.store(
          table.newGeneration(64, 64).value, std::memory_order_release);
      resizes.fetch_add(1, std::memory_order_relaxed);
      // Resizing flat out starves the producer: every publish finds its
      // generation already retired and refuses at its first check, so the
      // interleaving this test looks for never arises at all. Leaving room
      // between resizes is what lets a publication get far enough in for a
      // retirement to land in the middle of it.
      for (int spin = 0; spin < 300; ++spin)
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
  });

  std::thread observer([&] {
    std::set<uint32_t> seenReleasable;
    while (!stop.load(std::memory_order_acquire)) {
      const uint32_t top = newest.load(std::memory_order_acquire);
      // A narrow window on purpose: the flap this looks for happens on the
      // generation that has just retired, and polling fewer ids means
      // polling each of them far more often.
      const uint32_t oldest = top > 2 ? top - 2 : 1;
      seenReleasable.erase(
          seenReleasable.begin(), seenReleasable.lower_bound(oldest));
      for (uint32_t id = oldest; id <= top; ++id) {
        if (table.wrappersReleasable(GenerationId{id}))
          seenReleasable.insert(id);
        else if (seenReleasable.count(id) != 0)
          violations.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  const auto deadline = Clock::now() + std::chrono::milliseconds(750);
  while (Clock::now() < deadline)
    std::this_thread::yield();
  stop.store(true, std::memory_order_release);
  producer.join();
  resizer.join();
  observer.join();

  EXPECT_GT(resizes.load(), 1000) << "the threads must have raced for real";
  EXPECT_EQ(violations.load(), 0)
      << "a generation's wrappers became releasable and then did not: a "
         "frame was published into it after it had already retired";
}
// Mutation that must break it: split publish() into two critical sections,
// testing the generation in one and storing READY in the other -- with or
// without a yield between them.

TEST(SurfaceTableTest, PublishRefusesWhileClosing) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  table.beginClosing();
  EXPECT_FALSE(table.publish(a));
  EXPECT_FALSE(table.acquire().has_value())
      << "closing refuses new producer ownership too";
  EXPECT_FALSE(table.anyFreeSlot())
      << "a tick gated on anyFreeSlot() must not be told yes and then "
         "handed an empty acquire()";
}
// Mutation: drop the closing check in publish(), in acquire(), or in
// anyFreeSlot(). Test fails.

TEST(SurfaceTableTest, RetiredGenerationStillTakesNewReads) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  table.publish(a);
  table.take();
  table.newGeneration(128, 128); // retires, but a is still on screen
  table.addRead(a); // the main thread re-presents it
  EXPECT_EQ(table.state(a), SlotState::kPresenting);
  table.releaseRead(a);
  EXPECT_EQ(table.state(a), SlotState::kPresenting)
      << "releasing a read never frees a PRESENTING slot";

  // The read must be *counted*, not merely tolerated. Refusing it leaves no
  // trace in the slot's own state, so the two assertions above hold either
  // way; what a refused read really costs is visible one step later, when
  // displacement frees a surface a blit is still sampling.
  table.addRead(a);
  auto b = mustAcquire(table);
  table.publish(b);
  table.take(); // displaces a, which still has one outstanding read
  EXPECT_EQ(table.state(a), SlotState::kRetiring)
      << "a displaced slot with an outstanding read is not reusable yet";
  table.releaseRead(a); // the blit completes; the old generation dies here
}
// Mutation: make retirement forbid addRead. Test fails, and so would the
// screen -- it would go blank at every resize.

TEST(SurfaceTableTest, WrappersHoldUntilRetiringReadsDrain) {
  SurfaceTable table([](GenerationId) {});
  auto gen = table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  table.publish(a);
  table.take();
  table.addRead(a); // outstanding blit on the old surface
  table.newGeneration(128, 128);
  auto b = mustAcquire(table);
  table.publish(b);
  table.take(); // a -> RETIRING with one read
  EXPECT_FALSE(table.wrappersReleasable(gen))
      << "a RETIRING slot still needs its wrapper";
  table.releaseRead(a);
  EXPECT_TRUE(table.wrappersReleasable(gen));
}
// Mutation: count only PRESENTING, not RETIRING. Test fails -- this is the
// defect finding 7 caught.

TEST(SurfaceTableTest, ARetiredGenerationsReadyFrameKeepsItsWrappers) {
  // take() deliberately still shows a frame that was published just before a
  // resize: the producer's work is already done and the presenter stretches
  // it, so dropping it would cost a frame for nothing. That decision is what
  // makes this rule necessary. If the wrapper trigger looked only at
  // PRESENTING and RETIRING, the presenting instance would destroy gen1's
  // wrappers while a gen1 frame was still waiting to be shown, and the next
  // take() would promote a slot with no wrapper behind it.
  SurfaceTable table([](GenerationId) {});
  auto gen1 = table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  ASSERT_TRUE(table.publish(a));
  table.newGeneration(128, 128); // retires gen1 with its frame unconsumed

  EXPECT_FALSE(table.wrappersReleasable(gen1))
      << "a retired generation holding a READY frame still needs its wrapper";

  auto shown = table.take();
  ASSERT_TRUE(shown.has_value())
      << "a frame published before the resize is still shown";
  EXPECT_EQ(shown->gen.value, gen1.value);
  EXPECT_EQ(table.state(a), SlotState::kPresenting);
}
// Two mutations must each break it: stop counting kReady in
// wrappersReleasableLocked (the wrapper is released too early), and make
// takeLocked refuse a non-current generation's READY frame (the frame is
// dropped instead of shown).

TEST(SurfaceTableTest, DestructionHappensWhenTheLastReferenceIsDropped) {
  // Named for what it checks. It was called DestructionIsPostedNotCalledInline,
  // which names a contract the header explicitly denies: the destroy function
  // runs "with the lock released, on whichever thread dropped the last
  // reference" -- inline on that thread, and it is the function itself that
  // is expected to post. Where it runs relative to the lock is pinned by
  // DestructionRunsAfterTheLockIsReleased below.
  std::thread::id destroyThread;
  SurfaceTable table(
      [&](GenerationId) { destroyThread = std::this_thread::get_id(); });
  auto gen = table.newGeneration(64, 64);
  (void)gen;
  auto a = mustAcquire(table);
  table.publish(a);
  table.take();
  table.newGeneration(128, 128);
  std::thread other([&] {
    auto b = mustAcquire(table);
    table.publish(b);
    table.take(); // drops the last reference to gen
  });
  other.join();
  EXPECT_NE(destroyThread, std::thread::id{})
      << "destruction must be handed to the owner dispatcher, not skipped";
}
// Mutation: drop the generation instead of handing it to the destroy
// function when its reference count reaches zero. Test fails.

TEST(SurfaceTableTest, DestructionRunsAfterTheLockIsReleased) {
  // The test above proves destruction happens; this one proves *where*. The
  // destroy function is handed the generation with the lock released, so a
  // dispatcher that re-enters the table -- and the real one will, since it
  // runs arbitrary owner-thread work -- does not deadlock on a non-recursive
  // mutex.
  std::unique_ptr<SurfaceTable> table;
  bool reentered = false;
  table = std::unique_ptr<SurfaceTable>(new SurfaceTable([&](GenerationId) {
    (void)table->anyFreeSlot(); // would deadlock if called under the lock
    reentered = true;
  }));
  table->newGeneration(64, 64);
  table->newGeneration(128, 128); // retires the first, which owns nothing
  EXPECT_TRUE(reentered);
}
// Mutation: invoke the destroy function inside the critical section. The
// test then deadlocks rather than failing, and the watchdog above turns that
// into an abort.

TEST(SurfaceTableTest, ThreeSlotsExhaustAndAcquireReportsIt) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  table.publish(a);
  table.take();
  table.addRead(a);
  auto b = mustAcquire(table);
  table.publish(b);
  auto c = table.acquire();
  ASSERT_TRUE(c.has_value());
  EXPECT_FALSE(table.acquire().has_value());
  EXPECT_FALSE(table.anyFreeSlot()) << "the tick must be withheld here";
}
// Mutation: make anyFreeSlot() ignore slot state, so it answers yes with
// every slot occupied -- the tick is sent and acquire() then returns
// nothing. A second mutation, four slots per generation instead of three,
// breaks it too.

TEST(SurfaceTableTest, GpuWorkKeepsAGenerationAliveUntilTheCompletionRuns) {
  // This test's subject is generation lifetime: a completion handler that
  // has not run yet is a counted owner, whatever became of the slot it was
  // submitted against.
  std::vector<GenerationId> destroyed;
  SurfaceTable table([&](GenerationId g) { destroyed.push_back(g); });
  auto gen = table.newGeneration(64, 64);
  (void)gen;
  auto a = mustAcquire(table);
  table.beginGpuWork(a); // submission happens here
  table.reclaim(a); // the tick drew nothing
  EXPECT_EQ(table.state(a), SlotState::kRetiring)
      << "reclamation waits for the producer's own GPU work, exactly as a "
         "publication would";
  table.newGeneration(128, 128); // retires gen while GPU work is pending
  EXPECT_TRUE(destroyed.empty())
      << "a pending completion handler still owns this generation";
  table.endGpuWork(a);
  EXPECT_EQ(destroyed.size(), 1u);
  // The weaker half of the pair, and deliberately so: state() reports kFree
  // for a destroyed generation as well as for a free slot, and this
  // generation is destroyed by the line above. That the slot genuinely goes
  // RETIRING -> FREE is pinned by the test below, whose generation is still
  // alive when it asks.
  EXPECT_EQ(table.state(a), SlotState::kFree);
}
// Mutation that must break it: make endGpuWork a no-op, or infer GPU
// ownership from slot state. The generation is destroyed early and the
// completion handler then touches freed textures.

TEST(SurfaceTableTest, ReclaimWaitsForTheProducersOwnGpuWork) {
  // spec:390 -- "Reclaim. WRITING -> FREE ... Reclamation waits for the
  // producer's own GPU work to complete, exactly as a publication would."
  //
  // The rule is about REUSE, not about lifetime: the generation stays alive
  // either way, because beginGpuWork() holds a reference on it. What a slot
  // freed too early costs is the surface itself -- acquire() hands it
  // straight back to the producer, which begins writing into a texture the
  // GPU is still writing into. That is why this test drains the pool rather
  // than only reading state().
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = mustAcquire(table);
  table.beginGpuWork(a); // submitted, not yet complete
  table.reclaim(a); // the tick drew nothing

  EXPECT_EQ(table.state(a), SlotState::kRetiring)
      << "a slot with producer GPU work in flight is not reusable, for the "
         "same reason one with an outstanding read is not";

  auto b = mustAcquire(table);
  auto c = mustAcquire(table);
  EXPECT_NE(b.slot, a.slot);
  EXPECT_NE(c.slot, a.slot);
  EXPECT_FALSE(table.acquire().has_value())
      << "the slot whose GPU work is outstanding was handed back out";
  EXPECT_FALSE(table.anyFreeSlot())
      << "and the frame tick must be withheld while it is";

  table.endGpuWork(a); // the completion handler runs
  EXPECT_EQ(table.state(a), SlotState::kFree);
  auto again = table.acquire();
  ASSERT_TRUE(again.has_value())
      << "the completion frees the slot, exactly as a read release does";
  EXPECT_EQ(again->slot, a.slot);
}
// Mutation that must break it: make releaseSlotOwnership ignore gpuWork --
// consult slot.reads alone, as it did before. reclaim() then sends the slot
// straight to kFree, state(a) fails its first assertion, and the third
// acquire() succeeds by handing `a` back out. A second mutation breaks it
// from the other end: drop the RETIRING -> FREE transition from
// endGpuWorkLocked, and the slot is never reusable again.

TEST(SurfaceTableTest, WrappersKeepAGenerationAliveUntilReleased) {
  // The wrapper half of the same rule: a wrapper is a counted owner, so a
  // generation with wrappers outstanding is not destroyed even once every
  // slot is free -- otherwise the presenting instance would hold texture
  // wrappers over destroyed surfaces.
  std::vector<GenerationId> destroyed;
  SurfaceTable table([&](GenerationId g) { destroyed.push_back(g); });
  auto gen = table.newGeneration(64, 64);
  table.addWrapper(gen);
  table.newGeneration(128, 128); // retires gen; every slot is FREE
  EXPECT_TRUE(destroyed.empty()) << "a live wrapper still owns this generation";
  EXPECT_TRUE(table.wrappersReleasable(gen))
      << "no READY, PRESENTING or RETIRING slot remains, so the trigger has "
         "fired";
  table.releaseWrappers(gen);
  EXPECT_EQ(destroyed.size(), 1u);
}
// Mutation: make addWrapper() not take a reference (or releaseWrappers()
// not drop one). The generation is destroyed while wrappers still name its
// textures, and the first EXPECT_TRUE fails.

TEST(SurfaceTableTest, WrappersAreNotReleasableForAGenerationNeverIssued) {
  // "Releasable" is an instruction to destroy wrappers, so the answer for an
  // id this table never issued has to be no. Answering yes for anything it
  // does not recognise conflates two opposite situations: a generation that
  // came and went, whose wrappers genuinely were released, and an id that
  // means nothing at all -- which includes a default-constructed
  // GenerationId{} and every id about to be issued. A caller polling ids
  // around the current one reads yes for a generation and then no the
  // moment it becomes real, and that flap is exactly what the resize load
  // test above treats as a defect.
  SurfaceTable table([](GenerationId) {});
  EXPECT_FALSE(table.wrappersReleasable(GenerationId{}))
      << "the reserved 'no generation' id names nothing to release";

  auto gen = table.newGeneration(64, 64);
  EXPECT_FALSE(table.wrappersReleasable(GenerationId{gen.value + 1}))
      << "an id that has not been issued yet names nothing to release";
  EXPECT_FALSE(table.wrappersReleasable(gen))
      << "a current generation is never releasable";

  table.newGeneration(128, 128); // retires gen, which owns nothing else
  EXPECT_TRUE(table.wrappersReleasable(gen))
      << "an issued generation that has since been destroyed released its "
         "wrappers long ago";
}
// Mutation that must break it: drop the `gen.value == 0 || gen.value >=
// nextGenerationId_` guard from wrappersReleasableLocked. An unknown id then
// falls through to "not in the table, therefore already destroyed,
// therefore releasable", and both expectations on unissued ids fail.

TEST(SurfaceTableTest, OldAndNewGenerationSlotsCoexistAcrossAResize) {
  SurfaceTable table([](GenerationId) {});
  auto oldGen = table.newGeneration(64, 64);
  (void)oldGen;
  auto a = mustAcquire(table);
  table.publish(a);
  table.take();
  table.addRead(a); // old surface is on screen
  auto newGen = table.newGeneration(128, 128);
  EXPECT_NE(a.gen.value, newGen.value);
  // Three slots of the NEW generation are available while the old one is
  // still presenting -- a flat Slot[3] could not represent this.
  auto b = table.acquire();
  auto c = table.acquire();
  auto d = table.acquire();
  // has_value() rather than dereferencing straight through: the mutation
  // this test exists to catch makes the third acquire come back empty, and
  // `*` on an empty optional is undefined behaviour that can read back a
  // plausible value instead of failing.
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(c.has_value());
  ASSERT_TRUE(d.has_value())
      << "three slots of the new generation must be available at once";
  EXPECT_EQ(b->gen.value, newGen.value);
  EXPECT_EQ(d->gen.value, newGen.value);
  EXPECT_EQ(table.state(a), SlotState::kPresenting);
}
// Mutation that must break it: store one Slot[3] shared across generations.
// The third acquire fails because the old presenting slot occupies a slot.

TEST(SurfaceTableTest, WithLockComposesTransitionsWithoutReentering) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto ref = table.withLock([&](SurfaceTable::LockToken &tok) {
    auto w = table.acquireLocked(tok);
    // has_value() rather than dereferencing straight through, for the reason
    // mustAcquire() exists: `*` on an empty optional is undefined behaviour
    // that can read back a plausible value instead of failing. ASSERT_ cannot
    // be used inside a callback that returns a value, so the check is an
    // EXPECT_ plus an early return.
    EXPECT_TRUE(w.has_value()) << "acquire() unexpectedly returned nothing";
    if (!w)
      return std::optional<SurfaceRef>();
    table.publishLocked(tok, *w);
    return table.takeLocked(tok);
  });
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(table.state(*ref), SlotState::kPresenting);
}
// Mutation: have withLock's callback call the public publish(). It
// deadlocks, which the watchdog turns into an abort. This says nothing
// about atomicity -- only that the mutex is non-recursive, so a callback
// must use the *Locked helpers. The test below is the one that pins what
// withLock is for.

TEST(SurfaceTableTest, WithLockHoldsTheLockAcrossTheWholeCallback) {
  // Composing two transitions is only worth anything if no other thread can
  // observe the state between them -- that is the whole property Task 4
  // depends on, and asserting the *result* of a transaction does not test
  // it: a withLock that took no lock at all produces the same result. So
  // watch from another thread instead.
  SurfaceTable table([](GenerationId) {});
  auto gen = table.newGeneration(64, 64);
  const SurfaceRef probe{gen, 0}; // acquire() hands out slot 0 first
  std::atomic<bool> done{false};
  std::atomic<bool> sawIntermediate{false};

  std::thread observer([&] {
    while (!done.load(std::memory_order_acquire)) {
      if (table.state(probe) == SlotState::kWriting)
        sawIntermediate.store(true, std::memory_order_relaxed);
    }
  });

  table.withLock([&](SurfaceTable::LockToken &tok) {
    auto w = table.acquireLocked(tok); // slot 0 -> WRITING
    ASSERT_TRUE(w.has_value());
    // Hold the intermediate state long enough that an observer which could
    // see it, would. Time-based on purpose: waiting for a handshake from a
    // thread that needs the lock would deadlock the correct implementation,
    // which is the one thing this test must not do.
    const auto deadline = Clock::now() + std::chrono::milliseconds(20);
    while (Clock::now() < deadline) {
    }
    table.reclaimLocked(tok, *w); // slot 0 -> FREE
  });

  done.store(true, std::memory_order_release);
  observer.join();
  EXPECT_FALSE(sawIntermediate.load(std::memory_order_relaxed))
      << "another thread observed a slot mid-transaction";
}
// Mutation that must break it: have withLock hand out a token without
// taking the lock. The observer then sees the slot WRITING between the two
// helpers -- exactly the interleaving a composed transition must not have.

TEST(SurfaceTableTest, ProducerConsumerStressCompletesRealCycles) {
  // The previous stress test could pass having done almost nothing. Count
  // completed cycles and require progress.
  //
  // Both of this test's original `violations` checks were unable to fire
  // against the mutation it names. Each read the slot's state back one
  // instruction after the call that set it, so a mutated acquire() handing
  // out a PRESENTING slot was observed as kWriting -- the state that same
  // mutated acquire() had just written over it. The check could only ever
  // have fired on an interleaving, and the interleaving it needed was one
  // the mutation removes.
  //
  // What replaces them is a test-side ownership flag per slot, maintained
  // INSIDE the table's own transactions. The consumer sets a slot's flag in
  // the same critical section as the take-and-read that makes it the
  // consumer's, and clears it in the same critical section as the read
  // release that ends that ownership; the producer's check shares a critical
  // section with the acquire it is checking. A slot handed to the producer
  // while the consumer still holds a read is then caught by construction
  // rather than by luck.
  //
  // It cannot false-positive: consumerOwns[s] is true only while slot s has
  // a consumer read outstanding, a slot with a read outstanding is never
  // FREE, and a correct acquire() returns only FREE slots.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  std::atomic<bool> stop{false};
  std::atomic<int> taken{0}, violations{0};
  // Not atomic, deliberately: every read and write below happens under the
  // table's own lock, which is the whole point of the arrangement.
  bool consumerOwns[kSlotsPerGeneration] = {};

  auto releaseHeld = [&](SurfaceRef held) {
    table.withLock([&](SurfaceTable::LockToken &tok) {
      table.releaseReadLocked(tok, held);
      consumerOwns[held.slot] = false;
    });
  };

  std::thread consumer([&] {
    std::optional<SurfaceRef> held;
    while (!stop.load(std::memory_order_acquire)) {
      auto t = table.withLock(
          [&](SurfaceTable::LockToken &tok) -> std::optional<SurfaceRef> {
            auto got = table.takeLocked(tok);
            if (got) {
              table.addReadLocked(tok, *got);
              consumerOwns[got->slot] = true;
            }
            return got;
          });
      if (!t)
        continue;
      taken.fetch_add(1);
      // Hold the read across the NEXT take, which is what exercises
      // displacement concurrent with completion.
      if (held)
        releaseHeld(*held);
      held = t;
    }
    if (held)
      releaseHeld(*held);
  });

  int published = 0;
  for (int i = 0; i < 200000; ++i) {
    auto w = table.withLock(
        [&](SurfaceTable::LockToken &tok) -> std::optional<SurfaceRef> {
          auto got = table.acquireLocked(tok);
          if (got && consumerOwns[got->slot])
            violations.fetch_add(1);
          return got;
        });
    if (w && table.publish(*w))
      ++published;
  }
  stop.store(true, std::memory_order_release);
  consumer.join();
  EXPECT_EQ(violations.load(), 0)
      << "acquire() handed the producer a surface the consumer was still "
         "reading";
  EXPECT_GT(published, 1000) << "the producer must have made real progress";
  EXPECT_GT(taken.load(), 100) << "the consumer must have taken real frames";
}
// Mutation: let acquire() hand out a slot that is not FREE (drop the
// `slot.state != SlotState::kFree` continue from acquireLocked). The
// producer is then handed the surface the consumer is presenting, the flag
// check above fires in the same critical section as the acquire, and
// `violations` is non-zero.

//===========================================================================
// OwnerDispatch
//===========================================================================

TEST(OwnerDispatchTest, PostRunsFunctionsOnLoopThreadNotInline) {
  TestLoop loop;
  OwnerDispatch dispatch(&loop.loop);
  const std::thread::id loopThreadId = std::this_thread::get_id();

  constexpr int kPosters = 3;
  std::mutex m;
  int completed = 0;
  std::vector<std::thread::id> postingThreadIds(kPosters);
  std::vector<std::thread::id> ranOnThreadIds(kPosters);

  std::vector<std::thread> posters;
  for (int i = 0; i < kPosters; ++i) {
    posters.emplace_back([&, i] {
      postingThreadIds[i] = std::this_thread::get_id();
      bool accepted = dispatch.post([&, i] {
        std::lock_guard<std::mutex> lock(m);
        ranOnThreadIds[i] = std::this_thread::get_id();
        ++completed;
      });
      EXPECT_TRUE(accepted);
    });
  }

  // Join before pumping the loop at all. Correct code only enqueues and
  // calls uv_async_send from post(); nothing runs until this thread
  // actually pumps the loop below, so `completed` must still be 0 here no
  // matter how the scheduler interleaved the three posting threads.
  for (auto &t : posters)
    t.join();
  {
    std::lock_guard<std::mutex> lock(m);
    EXPECT_EQ(completed, 0)
        << "post() must not run its function before returning";
  }

  // OwnerDispatch's own handle is unrefed while its queue is empty (see
  // AnIdleDispatcherDoesNotPinTheLoopOpen), and this loop otherwise has
  // nothing else in it -- so uv_run would see uv__loop_alive() == false
  // and return without polling anything at all, which is not something an
  // off-thread uv_async_send can do anything about. A cheap repeating
  // timer keeps the loop genuinely polling for this part of the test,
  // standing in for whatever else keeps a real program's loop alive; it is
  // not part of what this test checks.
  uv_timer_t keepAlive{};
  ASSERT_EQ(uv_timer_init(&loop.loop, &keepAlive), 0);
  int timerStartRc = uv_timer_start(&keepAlive, [](uv_timer_t *) {}, 1, 1);
  ASSERT_EQ(timerStartRc, 0);

  // Now pump real loop turns -- not a fixed sleep -- until every post has
  // actually run. uv_run(UV_RUN_ONCE) blocks on real readiness of the
  // async handle's wakeup, so this cannot pass on luck the way a timed
  // sleep could.
  {
    std::unique_lock<std::mutex> lock(m);
    while (completed < kPosters) {
      lock.unlock();
      uv_run(&loop.loop, UV_RUN_ONCE);
      lock.lock();
    }
  }

  // Flush the timer's close synchronously, here, rather than leaving it to
  // TestLoop's destructor: keepAlive is a stack local of this function, and
  // uv_close()'s callback runs on a later loop turn (see OwnerDispatch's
  // own close() for the general point) -- one that must happen before this
  // stack frame's local goes away, not after. TestLoop's own destructor
  // flushes OwnerDispatch's heap-allocated handle, which has no such
  // constraint; it must not be the thing relied on to flush this one.
  uv_timer_stop(&keepAlive);
  uv_close(reinterpret_cast<uv_handle_t *>(&keepAlive), nullptr);
  uv_run(&loop.loop, UV_RUN_DEFAULT);

  for (int i = 0; i < kPosters; ++i) {
    EXPECT_EQ(ranOnThreadIds[i], loopThreadId)
        << "posted function " << i << " did not run on the loop thread";
    EXPECT_NE(ranOnThreadIds[i], postingThreadIds[i])
        << "posted function " << i << " ran inline on the posting thread";
  }

  dispatch.close();
}
// Mutation that must break it: call fn() directly inside post(). Each
// posted function then runs synchronously inside post(), on the posting
// thread, before that thread's lambda returns -- so `completed` is already
// 3 by the time every poster has been joined, the pump loop below never
// even runs, and EXPECT_EQ(completed, 0) fails by assertion (no hang, no
// watchdog needed for this one).

TEST(OwnerDispatchTest, AnIdleDispatcherDoesNotPinTheLoopOpen) {
  // uv_async_init leaves a handle referenced by default, and a referenced
  // active handle alone keeps uv__loop_alive() true forever -- so
  // uv_run(UV_RUN_DEFAULT) would never return while an OwnerDispatch with
  // nothing posted exists, unless the idle handle is unrefed. This matters
  // in production, not just in principle: the JS thread runs exactly
  // uv_run(UV_RUN_DEFAULT), so the first real dispatcher would wedge
  // shutdown. Guarded by the file's TestWatchdog: under the mutation this
  // hangs rather than failing an assertion.
  TestLoop loop;
  OwnerDispatch dispatch(&loop.loop);

  EXPECT_EQ(uv_run(&loop.loop, UV_RUN_DEFAULT), 0)
      << "an idle OwnerDispatch must not keep the loop alive by itself";

  dispatch.close();
}
// Mutation: never unref the handle (e.g. drop the updateAsyncRef() call
// from the constructor). uv_run(UV_RUN_DEFAULT) then blocks forever polling
// a referenced, otherwise-idle async handle nothing will ever send --
// caught by TestWatchdog.

TEST(OwnerDispatchTest, CloseDrainsACascadingPostToo) {
  // SurfaceTable's own motivation for the extraction rule is that
  // generation destruction can cascade: a function run during close()'s
  // drain posts another one. Both must run, not just the first.
  TestLoop loop;
  OwnerDispatch d(&loop.loop);
  int firstRan = 0;
  int secondRan = 0;

  EXPECT_TRUE(d.post([&] {
    ++firstRan;
    // A cascading post, made from inside a function close() is draining.
    EXPECT_TRUE(d.post([&] { ++secondRan; }));
  }));

  d.close();

  EXPECT_EQ(firstRan, 1);
  EXPECT_EQ(secondRan, 1)
      << "a post() made while close() is draining must still run, not be "
         "refused";
}
// Mutation: set closed_ = true before draining (close() drains one pass,
// marks itself closed, and never revisits the queue). The cascading post()
// inside the first function then sees closed_ already set, returns false,
// and secondRan stays 0.

TEST(OwnerDispatchTest, ConcurrentPostAndCloseNeverDropsAnAcceptedPost) {
  // Exercises the post()/close() race the shared mutex_ exists to avoid --
  // several threads hammer post() while the owner thread calls close()
  // concurrently, rather than only after joining them. Every post() that
  // returns true must have its function actually run: nothing accepted may
  // be silently dropped, and ASAN must see no use-after-free on the
  // detached handle across the race.
  TestLoop loop;
  OwnerDispatch dispatch(&loop.loop);

  constexpr int kPosters = 4;
  // Bounded, not literally infinite, so a pathological scheduler cannot
  // hang this test forever; in practice close() wins the race within a
  // handful of iterations, well under this cap. TestWatchdog is the real
  // backstop either way.
  constexpr int kMaxAttemptsPerPoster = 200000;
  std::atomic<int> accepted{0};
  std::atomic<int> ran{0};

  std::vector<std::thread> posters;
  for (int p = 0; p < kPosters; ++p) {
    posters.emplace_back([&] {
      for (int i = 0; i < kMaxAttemptsPerPoster; ++i) {
        bool ok =
            dispatch.post([&] { ran.fetch_add(1, std::memory_order_relaxed); });
        if (ok)
          accepted.fetch_add(1, std::memory_order_relaxed);
        else
          break;
      }
    });
  }

  // Deliberately not joined first: close() races the still-running
  // posters. close()'s fixpoint drain (Important 2's fix) keeps absorbing
  // and running whatever lands in the queue until closed_ is set, so this
  // both closes the dispatcher and runs everything accepted up to that
  // point.
  dispatch.close();

  for (auto &t : posters)
    t.join();

  EXPECT_EQ(ran.load(), accepted.load())
      << "a post() that returned true must always have its function run";
}
// Mutation: read/null the async handle pointer and set closed_ in two
// separate critical sections in close() (splitting what is currently one
// lock_guard), or drop post()'s check-and-enqueue out of its own single
// critical section. Either lets a post() land after close() has already
// stopped accepting (dropped: accepted != ran) or lets a post() call
// uv_async_send on a handle already handed to uv_close() (ASAN: use-after-free
// / heap-use-after-free under this test's concurrent load).

TEST(OwnerDispatchTest, CloseDrainsRatherThanDiscards) {
  // Queued work is GPU-resource destruction. Dropping it leaks.
  TestLoop loop;
  int ran = 0;
  {
    OwnerDispatch d(&loop.loop);
    EXPECT_TRUE(d.post([&] { ++ran; }));
    EXPECT_TRUE(d.post([&] { ++ran; }));
    d.close();
  }
  EXPECT_EQ(ran, 2);
}
// Mutation: make close() clear the queue without running it. Test fails
// (ran stays 0).

TEST(OwnerDispatchTest, PostAfterCloseIsRefusedNotDropped) {
  TestLoop loop;
  OwnerDispatch d(&loop.loop);
  d.close();
  EXPECT_FALSE(d.post([] {}));
}
// Mutation: return true (and drop fn) after close. Test fails.

TEST(OwnerDispatchTest, AQueuedFunctionMayPostAgainWithoutDeadlock) {
  // The extraction rule, tested directly: the drain must not hold the
  // queue lock while running a function that re-enters post(). Guarded by
  // the file's TestWatchdog -- the mutation this pins is a deadlock, which
  // hangs rather than fails without it.
  TestLoop loop;
  OwnerDispatch d(&loop.loop);
  bool inner = false;
  EXPECT_TRUE(d.post([&] { d.post([&] { inner = true; }); }));
  d.drainForTest();
  EXPECT_FALSE(inner) << "the re-post should not have run yet";
  d.drainForTest();
  EXPECT_TRUE(inner);
  d.close();
}
// Mutation: run the queue under its own lock (drainQueue invoking fn()
// while mutex_ is still held). The inner post() call then tries to
// re-acquire mutex_ from the same thread and deadlocks -- caught by
// TestWatchdog rather than hanging the suite.

//===========================================================================
// TickMachine
//
// The state machine that paces requestAnimationFrame. Every test here names
// the mutation it is verified against.
//===========================================================================

TEST(TickMachineTest, IdleAppReceivesNoTicks) {
  TickMachine m;
  EXPECT_FALSE(m.shouldSendTick(/*aFreeSlotExists=*/true));
}
// Mutation: return true when no frame was requested. Fails.

TEST(TickMachineTest, RequestsCoalesce) {
  TickMachine m;
  m.requestFrame();
  m.requestFrame();
  m.requestFrame();
  EXPECT_TRUE(m.shouldSendTick(true));
  // Back to kIdle before probing again, so the second probe is answered by
  // the request bit rather than by the state gate. Without this the test
  // passes against a machine that keeps a backlog of requests -- the very
  // thing it is named for.
  m.beginCallbacks();
  m.endCallbacksDrew(false);
  EXPECT_FALSE(m.shouldSendTick(true)) << "three requests, one tick";
}
// Mutation: keep a count instead of a flag. The second probe finds a
// backlog entry left by the other two requests and returns true; fails.

TEST(TickMachineTest, TickWithheldWhenNoSlotIsFree) {
  TickMachine m;
  m.requestFrame();
  EXPECT_FALSE(m.shouldSendTick(/*aFreeSlotExists=*/false));
  EXPECT_TRUE(m.shouldSendTick(/*aFreeSlotExists=*/true));
}
// Mutation: ignore the parameter. First expectation fails. This is the
// presentation backpressure the spec calls a deliberate deviation from
// browser rAF -- it must not be quietly dropped.

TEST(TickMachineTest, TheComposedTickGateReadsTheTableUnderOneLock) {
  // The production form of the gate. Every other test here calls
  // shouldSendTick(bool), which is the decomposed form: it cannot compose
  // the free-slot reading with the tick transition, because by the time it
  // is called the reading has already been taken. Production takes the
  // overload that does both under the table's lock, the same way
  // publishFrame() and takeFrame() do -- and nothing tested that overload at
  // all until this.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  TickMachine m;

  // Occupy every slot: the table can honour no tick at all.
  SurfaceRef a = mustAcquire(table);
  SurfaceRef b = mustAcquire(table);
  SurfaceRef c = mustAcquire(table);
  ASSERT_FALSE(table.acquire().has_value());

  m.requestFrame();
  EXPECT_FALSE(m.shouldSendTick(table))
      << "a tick delivered here reaches a producer that cannot acquire "
         "anything to draw into";
  EXPECT_EQ(m.state(), TickState::kIdle);

  // Withheld, not serviced: the request survives, and the next frame
  // callback that finds a free slot delivers it.
  table.reclaim(c);
  EXPECT_TRUE(m.shouldSendTick(table));
  EXPECT_EQ(m.state(), TickState::kTickQueued);

  table.reclaim(a);
  table.reclaim(b);
}
// Mutation that must break it: have the composed overload gate on nothing
// (return shouldSendTickLocked(true)). The exhausted table still sends a
// tick and the first expectation fails. A second mutation breaks it
// differently and is worth knowing about: read the free-slot state through
// the PUBLIC anyFreeSlot() from inside the withLock body. The table's mutex
// is not recursive, so that deadlocks and TestWatchdog aborts the run --
// which is how this test pins that the reading really is taken inside the
// table's transaction rather than beside it.
//
// What it does NOT catch, stated rather than implied: splitting the call
// back into `table.anyFreeSlot()` followed by `shouldSendTick(free)`. The
// two answer identically except when another thread changes the table
// between them, a window a few instructions wide -- and this suite's
// measured conclusion (see the sweep tests above) is that it cannot aim at
// such a window. The composition is here because the gap is unnecessary,
// not because a test can catch it.

TEST(TickMachineTest, EmptyTickReturnsToIdleImmediately) {
  TickMachine m;
  m.requestFrame();
  m.shouldSendTick(true);
  m.beginCallbacks();
  m.endCallbacksDrew(false);
  EXPECT_EQ(m.state(), TickState::kIdle);
}
// Mutation: always go to kGpuPending. Non-rendering rAF chains stall; fails.

TEST(TickMachineTest, DroppedPublicationReturnsToIdleNotAwaitingTake) {
  TickMachine m;
  m.requestFrame();
  m.shouldSendTick(true);
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  EXPECT_EQ(m.state(), TickState::kGpuPending);
  m.publicationDropped();
  EXPECT_EQ(m.state(), TickState::kIdle);
}
// Mutation: leave the state at kGpuPending or move to
// kPublishedAwaitingTake. The machine waits forever for a take that cannot
// come; fails.

TEST(TickMachineTest, ARefusedPublicationEndsTheFrameLikeADroppedOne) {
  // The same transition as the test above, reached through the other door.
  // A publication the TABLE refuses -- here because the generation retired
  // between the acquire and the publish -- ends the frame just as an
  // explicit publicationDropped() does. Without this the refusal branch in
  // publishFrame() can be deleted outright and every other test still
  // passes.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  TickMachine m;
  m.requestFrame();
  m.shouldSendTick(true);
  m.beginCallbacks();
  SurfaceRef ref = mustAcquire(table);
  m.endCallbacksDrew(true);
  ASSERT_EQ(m.state(), TickState::kGpuPending);

  table.newGeneration(64, 64); // retires the generation ref belongs to

  EXPECT_FALSE(m.publishFrame(table, ref))
      << "the table must refuse a publication into a retired generation";
  EXPECT_EQ(m.state(), TickState::kIdle)
      << "a refused publication ends the frame; leaving the machine at "
         "kGpuPending waits for a take that cannot come";
}
// Mutation: delete the `else if (state_ == kGpuPending) state_ = kIdle;`
// branch from publishFrame(). The machine stays at kGpuPending; fails.

TEST(TickMachineTest, ACallbackRegisteredInsideACallbackRunsNextTick) {
  TickMachine m;
  m.requestFrame();
  m.shouldSendTick(true);
  m.beginCallbacks();
  m.requestFrame(); // registered from inside
  m.endCallbacksDrew(false);
  EXPECT_TRUE(m.shouldSendTick(true));
}
// Mutation: ignore requestFrame() while state is kCallbacksRunning. Fails.
// (The addendum named "clear pending requests in beginCallbacks()", which
// cannot kill this test: beginCallbacks() runs BEFORE the inner request, so
// the clear lands on an already-clear flag.)

TEST(TickMachineTest, NoTicksWhileMinimized) {
  TickMachine m;
  m.setMinimized(true);
  m.requestFrame();
  EXPECT_FALSE(m.shouldSendTick(true));
  m.setMinimized(false);
  EXPECT_TRUE(m.shouldSendTick(true));
}
// Mutation: ignore the minimized flag. Fails. (rAF stops in a hidden tab.)

TEST(TickMachineTest, RequestDuringGpuPendingSurvivesADroppedPublication) {
  TickMachine m;
  m.requestFrame();
  m.shouldSendTick(true);
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  m.requestFrame(); // arrives while GPU work pending
  m.publicationDropped();
  EXPECT_TRUE(m.shouldSendTick(true));
}
// Mutation: clear pending requests in publicationDropped(). Fails.

TEST(TickMachineTest, RequestsSurviveTickQueuedAndGpuPendingAlike) {
  for (int phase = 0; phase < 2; ++phase) {
    TickMachine m;
    m.requestFrame();
    m.shouldSendTick(true); // -> kTickQueued
    if (phase == 0)
      m.requestFrame(); // during kTickQueued
    m.beginCallbacks();
    m.endCallbacksDrew(true); // -> kGpuPending
    if (phase == 1)
      m.requestFrame(); // during kGpuPending
    m.publicationDropped();
    EXPECT_TRUE(m.shouldSendTick(true)) << "phase " << phase;
  }
}
// The previous revision covered only kCallbacksRunning. Phase 0 is what
// forces the pending flag to be cleared when the tick is SENT rather than at
// beginCallbacks(): a clear there would swallow this request.

TEST(TickMachineTest, PublishThenImmediateTakeNeverStrandsAwaitingTake) {
  // The race the integration rule exists for: the main thread can take the
  // surface before the producer's bookkeeping runs. Because publishFrame and
  // takeFrame each compose with the table's lock, the machine must never be
  // left waiting for a take that already happened.
  //
  // DEVIATION from the task addendum, which asserted
  //   ASSERT_NE(m.state(), TickState::kPublishedAwaitingTake)
  // unconditionally. That assertion is unsound. Nothing orders the consumer
  // against publishFrame, so the consumer can run to completion FIRST, find
  // nothing ready, return nullopt and leave the machine in kGpuPending --
  // after which publishFrame legitimately sets kPublishedAwaitingTake with
  // the frame still waiting for the next frame_cb. Nothing is stranded
  // there, yet the original assertion fails it. The main thread wins that
  // race almost always, so it would have passed constantly in development
  // and failed rarely under ASAN and 16-way parallel load.
  //
  // The real invariant is the conjunction: never kPublishedAwaitingTake AND
  // no un-taken ready frame left. Both orderings are pinned below, so the
  // assertion is total and still fails the named mutation.
  for (int i = 0; i < 2000; ++i) {
    SurfaceTable table([](GenerationId) {});
    table.newGeneration(64, 64);
    TickMachine m;
    m.requestFrame();
    m.shouldSendTick(true);
    m.beginCallbacks();
    m.endCallbacksDrew(true);
    SurfaceRef ref = mustAcquire(table);
    std::optional<SurfaceRef> taken;
    std::thread consumer([&] { taken = m.takeFrame(table); });
    m.publishFrame(table, ref);
    consumer.join();

    if (taken.has_value()) {
      ASSERT_EQ(m.state(), TickState::kIdle)
          << "the take happened, so the frame is complete -- iteration " << i;
    } else {
      ASSERT_EQ(m.state(), TickState::kPublishedAwaitingTake)
          << "iteration " << i;
      ASSERT_EQ(table.state(ref), SlotState::kReady)
          << "the published frame must still be there for the next frame_cb "
             "-- iteration "
          << i;
    }
  }
}
// NOTE: this test does NOT catch the split-publish mutation, and measuring
// that is the reason the test below exists. The window between
// table.publish() returning and a separate state store is a few
// instructions, while std::thread construction costs tens of microseconds,
// so the consumer reliably arrives after BOTH halves and takes the frame
// from a machine that already reads kPublishedAwaitingTake. Verified: the
// mutation was applied and all 2000 iterations passed. What this test is
// worth keeping for is the end-state invariant itself, over both orderings;
// what pins the composition rule is the deterministic test below.

TEST(TickMachineTest, PublishFrameUpdatesTheMachineWithinTheTablesTransaction) {
  // The composition rule, pinned without a race.
  //
  // SurfaceTable hands a dead generation to the destroy function at the END
  // of a transaction -- after the withLock body has run, before withLock
  // returns (see SurfaceTable's class comment and withLock()). That makes
  // the callback a deterministic observation point sitting exactly where a
  // split implementation would leave its gap:
  //
  //   composed:  publishLocked + state store, THEN destroy -> the callback
  //              sees kPublishedAwaitingTake
  //   split:     table.publish() runs its own transaction and fires destroy
  //              BEFORE the state store -> the callback sees kGpuPending
  //
  // A publication destroys a generation when it supersedes a READY frame
  // belonging to an already-retired one, which is what the setup below
  // arranges.
  TickMachine m;
  std::optional<TickState> observed;
  SurfaceTable table([&](GenerationId) {
    // Exactly one hand-over is expected, from the publication below:
    // ~SurfaceTable deliberately never calls the destroy function, so the
    // table's own teardown fires nothing. The guard keeps the FIRST
    // observation rather than the last, so that an unexpected second one
    // cannot quietly overwrite the reading under test.
    if (!observed.has_value())
      observed = m.state();
  });

  // A READY frame in a generation that is then retired. Its slot is the
  // generation's last owner, so superseding it destroys the generation.
  table.newGeneration(64, 64);
  SurfaceRef stale = mustAcquire(table);
  ASSERT_TRUE(table.publish(stale));
  table.newGeneration(64, 64); // retires the first generation

  // Drive the machine to kGpuPending, the state a publication completes.
  m.requestFrame();
  m.shouldSendTick(true);
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  ASSERT_EQ(m.state(), TickState::kGpuPending);

  SurfaceRef fresh = mustAcquire(table);
  EXPECT_TRUE(m.publishFrame(table, fresh));

  ASSERT_TRUE(observed.has_value())
      << "the superseded generation should have been destroyed by the "
         "publication -- the observation point never ran";
  EXPECT_EQ(*observed, TickState::kPublishedAwaitingTake)
      << "the machine's state must already be updated when the table's "
         "transaction ends; seeing kGpuPending here means publication and "
         "the tick transition are two critical sections, not one";
}
// Mutation: implement publishFrame as table.publish() followed by a separate
// state store. The destroy callback then runs between the two and observes
// kGpuPending; fails deterministically, with no threads involved.

//===========================================================================
// Publication ids (spec "Testing"; plan Ruling 49)
//
// Capture is keyed to a publication id, never to a frame number, "because
// blits can precede any publication". These three tests are what stop that
// key from shipping as a constant zero -- the failure mode being silent,
// since 0 == 0 matches and every capture test stays green.
//===========================================================================

TEST(TickMachineTest, PublicationIdsStartAtOneAndAdvanceOncePerPublication) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  TickMachine m;

  // Nothing published yet. Zero is reserved for exactly this, so that a
  // caller who forgets to propagate the id is comparing "never" against
  // "never" rather than a real value against itself.
  EXPECT_EQ(m.readyPublicationId(), 0u);
  EXPECT_EQ(m.presentingPublicationId(), 0u);

  for (uint64_t expected = 1; expected <= 3; ++expected) {
    m.requestFrame();
    ASSERT_TRUE(m.shouldSendTick(true));
    m.beginCallbacks();
    m.endCallbacksDrew(true);
    SurfaceRef ref = mustAcquire(table);
    ASSERT_TRUE(m.publishFrame(table, ref));
    EXPECT_EQ(m.readyPublicationId(), expected)
        << "ids must advance by exactly one per successful publication";
    ASSERT_TRUE(m.takeFrame(table).has_value());
  }
}
// Mutation: start the counter so the first id is 0 (readyPublicationId_ =
// publicationCounter_++), or stop incrementing it altogether. Either fails
// the first iteration.

TEST(TickMachineTest, ARefusedPublicationAssignsNoPublicationId) {
  // The id is assigned in publishFrame's SUCCESS branch only. A refused
  // publication -- here, one against a generation that has retired -- is
  // not a publication, and must not consume an id: if it did, the next
  // real publication would be numbered for a frame nobody can ever see.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  TickMachine m;

  m.requestFrame();
  ASSERT_TRUE(m.shouldSendTick(true));
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  SurfaceRef ref = mustAcquire(table);
  table.newGeneration(64, 64); // retires ref's generation

  EXPECT_FALSE(m.publishFrame(table, ref));
  EXPECT_EQ(m.readyPublicationId(), 0u)
      << "a refused publication must not have been given an id";
  table.reclaim(ref);

  // And the next real one is still id 1, not 2.
  m.requestFrame();
  ASSERT_TRUE(m.shouldSendTick(true));
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  SurfaceRef good = mustAcquire(table);
  ASSERT_TRUE(m.publishFrame(table, good));
  EXPECT_EQ(m.readyPublicationId(), 1u);
}
// Mutation: move the increment above publishFrame's `if (published)`, so
// every attempt consumes an id. Both assertions fail.

TEST(TickMachineTest, AReblitDoesNotSatisfyARequestForTheNextPublication) {
  // **The case the field was added for** (spec: capture "is keyed to a
  // publication id and acknowledged, never to an absolute frame number,
  // because blits can precede any publication").
  //
  // The presenter re-blits the still-presenting surface on every frame
  // callback, publication or not. Each of those frames reports the SAME
  // publication id -- the one it is re-showing -- so a capture request
  // naming the NEXT publication cannot be satisfied by any of them. This
  // test is the unit-level half of that property; test/ui-present.js
  // asserts it end to end, through the real presenter.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  TickMachine m;

  m.requestFrame();
  ASSERT_TRUE(m.shouldSendTick(true));
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  SurfaceRef first = mustAcquire(table);
  ASSERT_TRUE(m.publishFrame(table, first));
  ASSERT_TRUE(m.takeFrame(table).has_value());
  ASSERT_EQ(m.presentingPublicationId(), 1u);

  // Ten frame callbacks in which the producer published nothing. Each one
  // takes nothing and re-blits what is already presenting.
  for (int frame = 0; frame < 10; ++frame) {
    EXPECT_FALSE(m.takeFrame(table).has_value());
    EXPECT_EQ(m.presentingPublicationId(), 1u)
        << "a re-blit shows the same publication and must keep reporting "
           "its id -- frame "
        << frame;
    EXPECT_NE(m.presentingPublicationId(), 2u)
        << "a re-blit must never answer for the next publication";
  }

  // Only a real publication, taken, advances what the presenter reports.
  m.requestFrame();
  ASSERT_TRUE(m.shouldSendTick(true));
  m.beginCallbacks();
  m.endCallbacksDrew(true);
  SurfaceRef second = mustAcquire(table);
  ASSERT_TRUE(m.publishFrame(table, second));
  EXPECT_EQ(m.presentingPublicationId(), 1u)
      << "published but not yet taken: the presenter is still showing the "
         "first frame";
  ASSERT_TRUE(m.takeFrame(table).has_value());
  EXPECT_EQ(m.presentingPublicationId(), 2u);
}
// Mutation: assign presentingPublicationId_ unconditionally in takeFrame
// (outside `if (taken)`), or set it from a frame counter incremented per
// frame callback. The loop then reports a changing id for a re-blit.

//===========================================================================
// Channels
//===========================================================================

TEST(ChannelsTest, ConsecutiveMouseMovesCoalesceWithDeltasSummed) {
  InputQueue q;
  UiEvent a{};
  a.type = UiEventType::kMouseMove;
  a.dx = 3;
  a.dy = 1;
  a.x = 13;
  a.y = 11;
  UiEvent b{};
  b.type = UiEventType::kMouseMove;
  b.dx = 4;
  b.dy = 2;
  b.x = 17;
  b.y = 13;
  q.push(a);
  q.push(b);
  std::vector<UiEvent> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].dx, 7);
  EXPECT_EQ(out[0].dy, 3);
  EXPECT_EQ(out[0].x, 17) << "position is the latest, deltas are summed";
  EXPECT_EQ(out[0].y, 13) << "both coordinates take the latest sample";
}
// Mutation: keep the first position instead of the latest. Fails.

TEST(InputQueueTest, CoalescedMoveKeepsTheLatestWindowSnapshot) {
  // Every event carries a WindowState snapshot, and a move that absorbs an
  // earlier one must carry the snapshot of the LATER one: the mirror on the
  // JS side is assigned from whatever event is delivered, so a stale one
  // would roll it back past a change the dropped move never described.
  InputQueue q;
  UiEvent a{};
  a.type = UiEventType::kMouseMove;
  a.window.width = 100;
  a.window.focused = false;
  UiEvent b{};
  b.type = UiEventType::kMouseMove;
  b.window.width = 200;
  b.window.focused = true;
  q.push(a);
  q.push(b);
  std::vector<UiEvent> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].window.width, 200);
  EXPECT_TRUE(out[0].window.focused);
}
// Mutation: in InputQueue::push, save back.window before `back = event` and
// restore it afterwards (keep the first snapshot). Fails.

TEST(ChannelsTest, NonMoveEventsBreakCoalescing) {
  InputQueue q;
  UiEvent move{};
  move.type = UiEventType::kMouseMove;
  move.dx = 1;
  UiEvent down{};
  down.type = UiEventType::kMouseDown;
  q.push(move);
  q.push(down);
  q.push(move);
  std::vector<UiEvent> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[1].type, UiEventType::kMouseDown);
}
// Mutation: coalesce across non-move events. Ordering is destroyed; fails.

TEST(ChannelsTest, ADefaultConstructedEventDoesNotCoalesceIntoAMove) {
  // UiEventType's zero value is kInvalid rather than kMouseMove, and this is
  // what that buys. A default-constructed or half-filled event -- one a
  // platform callback forgot to finish, or a value-initialized element of a
  // vector -- must not present itself as a mouse move: it would coalesce
  // into the move ahead of it and overwrite that move's position with
  // zeroes, destroying a real event rather than merely adding a bogus one,
  // and doing it silently.
  InputQueue q;
  UiEvent move{};
  move.type = UiEventType::kMouseMove;
  move.x = 17;
  move.y = 13;
  q.push(move);
  q.push(UiEvent{}); // never filled in

  std::vector<UiEvent> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 2u) << "an unfilled event coalesced into a real move";
  EXPECT_EQ(out[0].type, UiEventType::kMouseMove);
  EXPECT_EQ(out[0].x, 17) << "and carried the real move's position away";
  EXPECT_EQ(out[0].y, 13);
  EXPECT_EQ(out[1].type, UiEventType::kInvalid);
}
// Mutation that must break it: default UiEvent::type to kMouseMove again
// (equivalently, make kMouseMove the enum's zero value once more). The
// unfilled event coalesces, out.size() is 1, and the one surviving event
// reports position (0, 0).

TEST(ChannelsTest, ClipboardRequestIsAnsweredAcrossThreads) {
  // Race-free by construction rather than by timing: wait() returns an
  // answer that already arrived, so it does not matter whether the responder
  // thread runs before or after the waiter blocks.
  ClipboardRequests reqs;
  auto slot = reqs.create();
  std::thread responder([&] {
    reqs.answer(
        slot->id(),
        ClipboardResult{ClipboardStatus::kOk, "from the other thread"});
  });
  ClipboardResult got = slot->wait(/*timeoutMs=*/5000);
  responder.join();
  EXPECT_EQ(got.status, ClipboardStatus::kOk);
  EXPECT_EQ(got.text, "from the other thread");
}
// Mutation: fulfill() stores nothing, so the answer never reaches the
// waiter. Fails by timeout. (Deliberately not "wait() blocks even when the
// answer already arrived": that cannot be made to fail deterministically,
// since the waiter still returns the stored value when its deadline
// expires.)

TEST(ChannelsTest, ClipboardRequestTimesOutRatherThanHanging) {
  ClipboardRequests reqs;
  auto slot = reqs.create();
  const auto start = Clock::now();
  EXPECT_EQ(slot->wait(/*timeoutMs=*/20).status, ClipboardStatus::kTimeout);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - start);
  // A lower bound, not an upper one: wait_for() with a predicate cannot
  // return before its deadline, and load can only lengthen this. Without it
  // an implementation that never blocks at all passes.
  EXPECT_GE(elapsed.count(), 20)
      << "wait() must block until its deadline, not return immediately";
}
// Mutation: wait without a deadline. The main thread can block in
// presentation, so an unbounded wait hangs the JS thread; fails by timeout
// (caught by this file's TestWatchdog rather than hanging the suite).

TEST(ClipboardRequestTest, AnswerCarriesItsStatus) {
  // Two requests answered out of order, each with a status the other does
  // not have. A failed read is a status, not an empty string: "" is what an
  // empty clipboard reads as, and a failure must not look like one.
  ClipboardRequests reqs;
  auto a = reqs.create();
  auto b = reqs.create();
  EXPECT_TRUE(
      reqs.answer(b->id(), ClipboardResult{ClipboardStatus::kTooLarge, ""}));
  EXPECT_TRUE(
      reqs.answer(a->id(), ClipboardResult{ClipboardStatus::kOk, "for A"}));

  ClipboardResult gotA = a->wait(1000);
  EXPECT_EQ(gotA.status, ClipboardStatus::kOk);
  EXPECT_EQ(gotA.text, "for A");
  ClipboardResult gotB = b->wait(1000);
  EXPECT_EQ(gotB.status, ClipboardStatus::kTooLarge);
  EXPECT_EQ(gotB.text, "");

  // Every status survives the trip unchanged.
  for (ClipboardStatus status :
       {ClipboardStatus::kOk,
        ClipboardStatus::kUnavailable,
        ClipboardStatus::kTimeout,
        ClipboardStatus::kTooLarge,
        ClipboardStatus::kError}) {
    auto slot = reqs.create();
    reqs.answer(slot->id(), ClipboardResult{status, "t"});
    EXPECT_EQ(slot->wait(1000).status, status)
        << "status " << static_cast<int>(status);
  }
}
// Mutation: make answer() ignore the id and fulfil the oldest outstanding
// request. A is then handed B's kTooLarge; fails.

TEST(ClipboardRequestTest, WaitReportsTimeoutAsAStatus) {
  ClipboardRequests reqs;
  auto slot = reqs.create();
  ClipboardResult got = slot->wait(/*timeoutMs=*/0);
  EXPECT_EQ(got.status, ClipboardStatus::kTimeout);
  EXPECT_TRUE(got.text.empty())
      << "a timed-out read carries no text -- in particular none from an "
         "earlier read";
}
// Mutation: return a default-constructed result (kError) on expiry. Fails:
// a caller could not tell a deadline from a broken read.

TEST(
    ClipboardRequestTest,
    FailAllCompletesEveryWaiterAndALaterAnswerIsRefused) {
  ClipboardRequests reqs;
  auto a = reqs.create();
  auto b = reqs.create();

  // One waiter that may already be blocked when failAll() runs, one that
  // asks only afterwards. Race-free: wait() returns a stored answer whether
  // it arrived before or after the call, and a 10 s deadline that expired
  // would read as kTimeout rather than as a pass.
  ClipboardResult gotA;
  std::thread waiter([&] { gotA = a->wait(10000); });
  reqs.failAll(ClipboardStatus::kError);
  waiter.join();
  EXPECT_EQ(gotA.status, ClipboardStatus::kError);
  EXPECT_EQ(b->wait(10000).status, ClipboardStatus::kError);

  // What failAll() completed stays completed: a reply that was in flight
  // when it ran is refused, not delivered over the top of the error.
  EXPECT_FALSE(
      reqs.answer(a->id(), ClipboardResult{ClipboardStatus::kOk, "late"}));
  ClipboardResult again = a->wait(0);
  EXPECT_EQ(again.status, ClipboardStatus::kError);
  EXPECT_TRUE(again.text.empty());

  // And the registry still serves requests made after it.
  auto c = reqs.create();
  EXPECT_TRUE(reqs.answer(c->id(), ClipboardResult{ClipboardStatus::kOk, "c"}));
  EXPECT_EQ(c->wait(1000).text, "c");
}
// Mutation: make failAll() a no-op. Fails: gotA is kTimeout after 10 s,
// and the late answer is accepted.

TEST(ChannelsTest, ALateReplyToATimedOutRequestCannotSatisfyTheNextOne) {
  // This is the defect the retained per-request slot exists to prevent.
  ClipboardRequests reqs;
  auto a = reqs.create();
  const uint64_t aId = a->id();
  EXPECT_EQ(a->wait(20).status, ClipboardStatus::kTimeout); // A times out
  auto b = reqs.create();

  // A's waiter gives up and releases its handle while A's entry is still in
  // the registry. This is what makes the reference counting load-bearing:
  // answer() must take a strong reference under the registry lock and find
  // it already gone, rather than writing through a dangling pointer.
  a.reset();
  EXPECT_FALSE(reqs.answer(
      aId, ClipboardResult{ClipboardStatus::kOk, "late reply for A"}))
      << "a reply to an abandoned request is discarded, not delivered";
  EXPECT_EQ(b->wait(20).status, ClipboardStatus::kTimeout) // B NOT satisfied
      << "a reply carries the id of the request it answers";
  reqs.answer(b->id(), ClipboardResult{ClipboardStatus::kOk, "for B"});
  ClipboardResult got = b->wait(1000);
  EXPECT_EQ(got.status, ClipboardStatus::kOk);
  EXPECT_EQ(got.text, "for B");
}
// Mutation: keep a single "current request" instead of a map keyed by id, so
// answer() satisfies whatever is outstanding. B is then handed A's late
// reply; fails. Run under ASAN: a late reply must not touch freed storage,
// which is why the slot is reference-counted and outlives its waiter.

TEST(ChannelsTest, AClipboardReplyBreaksAMouseMoveRun) {
  // A reply is not an input event, but it travels in the input queue, and
  // it must not be folded into a move: it would vanish, and the Promise
  // waiting for it would never settle.
  InputQueue q;
  UiEvent move;
  move.type = UiEventType::kMouseMove;
  move.dx = 1;
  q.push(move);
  UiEvent reply;
  reply.type = UiEventType::kClipboardReply;
  reply.requestId = 7;
  q.push(reply);
  q.push(move);

  std::vector<UiEvent> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[1].type, UiEventType::kClipboardReply);
  EXPECT_EQ(out[1].requestId, 7u);
}

//===========================================================================
// Ownership of command payloads.
//
// Both tests below must fail by VALUE, not only under a sanitizer. Their
// mutation -- keeping a pointer or a string_view into the caller's storage
// -- is a use-after-free that ASAN reports, and this suite's development
// configuration is an ASAN one; but the release configuration and CI's
// release job run the same code with no sanitizer, where reading a freed
// block usually returns the original bytes and both tests would pass
// against the defect they exist to catch.
//
// So each payload is big enough to live in a heap block of its own rather
// than inside the object (defeating the small-string optimization for the
// title), and each dead scope is followed by allocations of exactly that
// size filled with a different byte. A non-sanitizing allocator satisfies
// one of them from the block just freed, so a command that kept a pointer
// reads back the scribble and the comparison fails on content. Under ASAN
// nothing is reused -- quarantine sees to that -- and the same mutation is
// caught by the use-after-free report instead. Either way the test fails,
// which is the property being bought here.
//===========================================================================

TEST(ChannelsTest, CommandQueuePayloadsAreOwned) {
  // A command's payload must not alias caller storage: the main thread
  // drains it later, on another thread.
  CommandQueue q;
  const std::string expected(64, 'h');
  {
    std::string title = expected;
    q.push(Command::setTitle(title));
  } // both title and setTitle's by-value parameter are freed by here
  std::vector<std::string> scribble;
  for (int i = 0; i < 16; ++i)
    scribble.emplace_back(64, 'X');

  std::vector<Command> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].title(), expected);
}
// Mutation: store a const char * or a string_view instead of copying. Under
// ASAN, a heap-use-after-free report; without a sanitizer, the scribble has
// taken the block over and the comparison fails on 'X's.

TEST(ChannelsTest, CursorImagePayloadIsOwned) {
  // The other payload-carrying command, and the one the brief names: the
  // RGBA pixels must be owned as surely as the title string is.
  CommandQueue q;
  constexpr int kWidth = 32;
  constexpr int kHeight = 32;
  constexpr size_t kBytes = static_cast<size_t>(kWidth) * kHeight * 4;
  {
    std::vector<uint8_t> rgba(kBytes, 0xAB);
    q.push(Command::setCursorImage(rgba, kWidth, kHeight, 1, 1));
  }
  std::vector<std::vector<uint8_t>> scribble;
  for (int i = 0; i < 16; ++i)
    scribble.emplace_back(kBytes, 0x00);

  std::vector<Command> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].kind(), Command::Kind::kSetCursorImage);
  ASSERT_EQ(out[0].cursorRgba().size(), kBytes);
  EXPECT_EQ(out[0].cursorRgba()[0], 0xAB);
  EXPECT_EQ(out[0].cursorRgba()[kBytes - 1], 0xAB)
      << "the last byte as well as the first: a payload copied only as far "
         "as its first bytes is not owned either";
  EXPECT_EQ(out[0].width(), kWidth);
  EXPECT_EQ(out[0].height(), kHeight);
  EXPECT_EQ(out[0].hotspotX(), 1);
  EXPECT_EQ(out[0].hotspotY(), 1);
}
// Mutation: store a pointer to the caller's vector. Under ASAN, a
// heap-use-after-free report; without a sanitizer, the scribble has zeroed
// the block and the 0xAB comparisons fail. Also the only coverage of five
// of Command's accessors.

TEST(CommandTest, EachKindOwnsItsPayload) {
  // The six kinds Task 11c adds, each built from a temporary that is
  // destroyed before the command is read back. The scribble allocations
  // after each scope play the part described above ChannelsTest.
  // CommandQueuePayloadsAreOwned: a command that kept a pointer reads back
  // the scribble, or trips ASAN.
  std::vector<Command> cmds;
  const std::string text(64, 't');
  constexpr size_t kIconBytes = 4 * 4 * 4;
  {
    std::vector<uint8_t> icon(kIconBytes, 0xCD);
    std::string clip = text;
    cmds.push_back(Command::setFullscreen(true));
    cmds.push_back(Command::setIcon(icon, 4, 4));
    cmds.push_back(Command::setCursor(7));
    cmds.push_back(Command::showCursor(false));
    cmds.push_back(Command::lockPointer(true));
    cmds.push_back(Command::writeClipboard(clip));
  }
  std::vector<std::string> scribble;
  std::vector<std::vector<uint8_t>> scribbleBytes;
  for (int i = 0; i < 16; ++i) {
    scribble.emplace_back(64, 'X');
    scribbleBytes.emplace_back(kIconBytes, 0x00);
  }

  ASSERT_EQ(cmds.size(), 6u);
  EXPECT_EQ(cmds[0].kind(), Command::Kind::kSetFullscreen);
  EXPECT_TRUE(cmds[0].flag());

  EXPECT_EQ(cmds[1].kind(), Command::Kind::kSetIcon);
  ASSERT_EQ(cmds[1].cursorRgba().size(), kIconBytes);
  EXPECT_EQ(cmds[1].cursorRgba().front(), 0xCD);
  EXPECT_EQ(cmds[1].cursorRgba().back(), 0xCD);
  EXPECT_EQ(cmds[1].width(), 4);
  EXPECT_EQ(cmds[1].height(), 4);

  EXPECT_EQ(cmds[2].kind(), Command::Kind::kSetCursor);
  EXPECT_EQ(cmds[2].cursor(), 7);

  EXPECT_EQ(cmds[3].kind(), Command::Kind::kShowCursor);
  EXPECT_FALSE(cmds[3].flag());

  EXPECT_EQ(cmds[4].kind(), Command::Kind::kLockPointer);
  EXPECT_TRUE(cmds[4].flag());

  EXPECT_EQ(cmds[5].kind(), Command::Kind::kWriteClipboard);
  EXPECT_EQ(cmds[5].text(), text);
}
// Mutation: make writeClipboard's or setIcon's payload a view of the
// caller's storage. ASAN reports a use-after-free; without a sanitizer the
// scribble fails the content comparison.

//===========================================================================
// EventDispatcher
//===========================================================================

namespace {

/// A key event whose key code identifies it. Key events, because mouse
/// moves coalesce in the queue and a test counting deliveries must not
/// have two of its events merge into one.
UiEvent numberedKey(int n) {
  UiEvent e;
  e.type = UiEventType::kKeyDown;
  e.keyCode = n;
  return e;
}

} // namespace

TEST(EventDispatcherTest, DeliversInPushOrder) {
  InputQueue queue;
  EventDispatcher dispatcher(queue);
  for (int i = 0; i < 3; ++i)
    queue.push(numberedKey(i));

  // An event pushed from inside a sink is delivered by the same call: the
  // dispatcher keeps going until the queue is empty.
  std::vector<int> seen;
  dispatcher.dispatch([&](const UiEvent &e) {
    seen.push_back(e.keyCode);
    if (e.keyCode == 1)
      queue.push(numberedKey(3));
  });

  EXPECT_EQ(seen, (std::vector<int>{0, 1, 2, 3}));
  EXPECT_TRUE(queue.empty());
}

TEST(EventDispatcherTest, NestedDispatchLeavesABorrowedEventInPlace) {
  // The address-stability obligation hnui_event_convert.h states (Ruling
  // 45). "a" is short, so its bytes live inside the UiEvent object itself
  // (small-string optimization) rather than on the heap: the borrowed
  // pointer is valid exactly as long as that UiEvent stays where it is.
  //
  // Mutation: delete the already-dispatching guard in dispatch(). The
  // nested call then drains 64 events into the buffer the outer call is
  // iterating, the buffer reallocates, and reading *p is a
  // heap-use-after-free under ASAN.
  InputQueue queue;
  EventDispatcher dispatcher(queue);

  UiEvent text;
  text.type = UiEventType::kTextInput;
  text.text = "a";
  text.keyCode = 0;
  queue.push(text);
  queue.push(numberedKey(1));

  std::vector<int> seen;
  bool nested = false;
  char borrowed = 0;
  EventDispatcher::Sink sink;
  sink = [&](const UiEvent &e) {
    seen.push_back(e.keyCode);
    if (e.type != UiEventType::kTextInput || nested)
      return;
    nested = true;
    HnuiEventConverter converter(e);
    const char *p = converter.event().text;
    for (int i = 2; i < 66; ++i)
      queue.push(numberedKey(i));
    dispatcher.dispatch(sink);
    borrowed = *p;
  };
  dispatcher.dispatch(sink);

  EXPECT_EQ(borrowed, 'a');
  std::vector<int> expected;
  for (int i = 0; i < 66; ++i)
    expected.push_back(i);
  EXPECT_EQ(seen, expected);
}

TEST(EventDispatcherTest, DispatchingIsTrueOnlyInsideASink) {
  InputQueue queue;
  EventDispatcher dispatcher(queue);
  EXPECT_FALSE(dispatcher.dispatching());

  queue.push(numberedKey(0));
  int calls = 0;
  bool insideSink = false;
  dispatcher.dispatch([&](const UiEvent &) {
    ++calls;
    insideSink = dispatcher.dispatching();
  });
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(insideSink);
  EXPECT_FALSE(dispatcher.dispatching());

  // And an empty queue is a dispatch that runs no sink and leaves the flag
  // clear.
  dispatcher.dispatch([&](const UiEvent &) { ++calls; });
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(dispatcher.dispatching());
}

//===========================================================================
// FrameCallbackList
//===========================================================================

namespace {

using Kind = FrameCallback::Kind;

/// A distinct, recognisable opaque pointer per registration. The list
/// never dereferences `fn` or `user`, so any value will do.
void *tagPointer(int tag) {
  return reinterpret_cast<void *>(static_cast<intptr_t>(0x1000 + tag));
}

/// Run one batch, calling \p during with each callback as it is handed out
/// -- the point at which the real tick would invoke it -- and return the ids
/// in the order they ran.
std::vector<int> runBatch(
    FrameCallbackList &list,
    const std::function<void(const FrameCallback &)> &during = nullptr) {
  std::vector<int> ran;
  list.beginBatch();
  FrameCallback cb;
  while (list.next(&cb)) {
    ran.push_back(cb.id);
    if (during)
      during(cb);
  }
  return ran;
}

} // namespace

TEST(FrameCallbackListTest, RunsInRegistrationOrderAcrossKinds) {
  FrameCallbackList list;
  const int js1 = list.add(Kind::kJs, tagPointer(1), nullptr);
  const int native1 = list.add(Kind::kNative, tagPointer(2), tagPointer(20));
  const int js2 = list.add(Kind::kJs, tagPointer(3), nullptr);
  const int native2 = list.add(Kind::kNative, tagPointer(4), tagPointer(40));

  std::vector<FrameCallback> seen;
  const std::vector<int> ran =
      runBatch(list, [&](const FrameCallback &cb) { seen.push_back(cb); });
  EXPECT_EQ(ran, (std::vector<int>{js1, native1, js2, native2}));

  // And each entry comes back exactly as it was registered: the list
  // carries the caller's opaque pointers and must not mix them up.
  ASSERT_EQ(seen.size(), 4u);
  EXPECT_EQ(seen[0].kind, Kind::kJs);
  EXPECT_EQ(seen[0].fn, tagPointer(1));
  EXPECT_EQ(seen[1].kind, Kind::kNative);
  EXPECT_EQ(seen[1].fn, tagPointer(2));
  EXPECT_EQ(seen[1].user, tagPointer(20));
  EXPECT_EQ(seen[3].user, tagPointer(40));

  // Run once each: the next batch is empty.
  EXPECT_TRUE(runBatch(list).empty());
}

TEST(FrameCallbackListTest, RegisteredDuringABatchRunsInTheNext) {
  FrameCallbackList list;
  const int first = list.add(Kind::kJs, tagPointer(1), nullptr);
  const int second = list.add(Kind::kNative, tagPointer(2), nullptr);
  EXPECT_TRUE(list.pending());

  int addedDuring = 0;
  const std::vector<int> ran = runBatch(list, [&](const FrameCallback &cb) {
    if (cb.id == first)
      addedDuring = list.add(Kind::kJs, tagPointer(3), nullptr);
  });
  // Not extended by the registration: `second` was the last of this batch.
  EXPECT_EQ(ran, (std::vector<int>{first, second}));
  EXPECT_TRUE(list.pending());

  EXPECT_EQ(runBatch(list), (std::vector<int>{addedDuring}));
  EXPECT_FALSE(list.pending());
}

TEST(
    FrameCallbackListTest,
    CancellingALaterCallbackOfTheRunningBatchPreventsIt) {
  FrameCallbackList list;
  const int a = list.add(Kind::kJs, tagPointer(1), nullptr);
  const int b = list.add(Kind::kNative, tagPointer(2), tagPointer(20));
  const int c = list.add(Kind::kJs, tagPointer(3), nullptr);

  bool cancelled = false;
  FrameCallback removed;
  const std::vector<int> ran = runBatch(list, [&](const FrameCallback &cb) {
    if (cb.id == a)
      cancelled = list.cancel(b, &removed);
  });
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(removed.id, b);
  EXPECT_EQ(removed.kind, Kind::kNative);
  EXPECT_EQ(removed.fn, tagPointer(2));
  EXPECT_EQ(removed.user, tagPointer(20));
  EXPECT_EQ(ran, (std::vector<int>{a, c}));

  // Cancelled once: a second cancel has nothing left to remove, and the
  // next batch does not resurrect it.
  EXPECT_FALSE(list.cancel(b, &removed));
  EXPECT_TRUE(runBatch(list).empty());
}

TEST(FrameCallbackListTest, CancellingOneThatAlreadyRanReportsFalse) {
  FrameCallbackList list;
  const int a = list.add(Kind::kJs, tagPointer(1), nullptr);
  const int b = list.add(Kind::kJs, tagPointer(2), nullptr);

  FrameCallback removed;
  removed.id = -1;
  bool cancelledEarlier = true;
  bool cancelledSelf = true;
  runBatch(list, [&](const FrameCallback &cb) {
    if (cb.id == b) {
      cancelledEarlier = list.cancel(a, &removed);
      // The callback now executing has already been taken from the batch.
      cancelledSelf = list.cancel(b, &removed);
    }
  });
  EXPECT_FALSE(cancelledEarlier);
  EXPECT_FALSE(cancelledSelf);
  // And after the batch, too.
  EXPECT_FALSE(list.cancel(a, &removed));
  // A refusal leaves the out-parameter alone.
  EXPECT_EQ(removed.id, -1);
  // Never issued.
  EXPECT_FALSE(list.cancel(0, &removed));
  EXPECT_FALSE(list.cancel(1000, &removed));
}

TEST(FrameCallbackListTest, CancellingANextBatchCallbackRemovesIt) {
  FrameCallbackList list;
  const int a = list.add(Kind::kJs, tagPointer(1), nullptr);
  const int b = list.add(Kind::kNative, tagPointer(2), nullptr);

  FrameCallback removed;
  EXPECT_TRUE(list.cancel(a, &removed));
  EXPECT_EQ(removed.id, a);
  EXPECT_EQ(removed.fn, tagPointer(1));
  EXPECT_EQ(runBatch(list), (std::vector<int>{b}));

  // Registered inside a callback and cancelled inside the same batch: it
  // is waiting for the next batch, and is removed from there.
  int later = 0;
  bool cancelledLater = false;
  list.add(Kind::kJs, tagPointer(3), nullptr);
  runBatch(list, [&](const FrameCallback &) {
    later = list.add(Kind::kJs, tagPointer(4), nullptr);
    cancelledLater = list.cancel(later, &removed);
  });
  EXPECT_TRUE(cancelledLater);
  EXPECT_EQ(removed.id, later);
  EXPECT_FALSE(list.pending());
  EXPECT_TRUE(runBatch(list).empty());
}

TEST(FrameCallbackListTest, IdsAreSharedAndNeverReused) {
  FrameCallbackList list;
  std::set<int> ids;
  FrameCallback removed;
  for (int round = 0; round < 4; ++round) {
    const int js = list.add(Kind::kJs, tagPointer(round), nullptr);
    const int native = list.add(Kind::kNative, tagPointer(round), nullptr);
    const int cancelled = list.add(Kind::kJs, tagPointer(round), nullptr);
    EXPECT_TRUE(list.cancel(cancelled, &removed));
    for (int id : {js, native, cancelled}) {
      EXPECT_GT(id, 0);
      // One space across both kinds, and nothing handed out twice --
      // including the ids of callbacks that ran or were cancelled.
      EXPECT_TRUE(ids.insert(id).second) << "id " << id << " reused";
    }
    runBatch(list);
  }
  EXPECT_EQ(*ids.begin(), 1);
}

TEST(FrameCallbackListTest, ReleaseAllHandsBackPendingAndUnrunEntries) {
  FrameCallbackList list;
  const int a = list.add(Kind::kJs, tagPointer(1), nullptr);
  const int b = list.add(Kind::kJs, tagPointer(2), nullptr);
  const int c = list.add(Kind::kNative, tagPointer(3), nullptr);

  // A batch abandoned after its first callback, with one more registered
  // for the next: everything not yet run comes back, the cancelled one does
  // not (cancel() already handed it over), and the list is left empty.
  list.beginBatch();
  FrameCallback cb;
  ASSERT_TRUE(list.next(&cb));
  EXPECT_EQ(cb.id, a);
  FrameCallback removed;
  EXPECT_TRUE(list.cancel(c, &removed));
  const int d = list.add(Kind::kJs, tagPointer(4), nullptr);

  std::vector<int> released;
  list.releaseAll(
      [&](const FrameCallback &entry) { released.push_back(entry.id); });
  EXPECT_EQ(released, (std::vector<int>{b, d}));
  EXPECT_FALSE(list.pending());
  EXPECT_TRUE(runBatch(list).empty());
}
