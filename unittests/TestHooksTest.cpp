/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// test_hooks.{h,cpp} (lib/compositor-gpu/, plan Task 9) added its hooks
// before the consumers that use them: frame capture reached real code in
// Task 10 (event injection, once here too, moved to tools/hermes-node-ui/
// event_injector.h in Task 11b), and CompletionGate is testable against
// SurfaceTable alone, with no GPU. That is the point of this
// file -- it is the GPU-ordering coverage the design's Testing section asks
// for ("the backend tests use controllable producer and consumer
// completion"), obtained without a GPU (Task 9's brief).
//
// Every test below names, in a comment, the mutation that must break it. A
// test that survives its mutation is not a test.

#include "test_hooks.h"

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/surface_table.h>

#include "gtest/gtest.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

using namespace hermes::node_compat;

namespace {

//===========================================================================
// A per-test watchdog, copied from CompositorTest.cpp's TestWatchdog -- see
// there for the full rationale. Short version: gtest has no per-test
// timeout, and neither test/lit.cfg nor unittests/lit.cfg sets one, so a
// hung test hangs the whole suite instead of failing it.
//===========================================================================

// Every test here is single-threaded and deterministic -- no wait, no
// condition variable, no real GPU -- so 20s is already a wide margin over
// what any of them should ever take; it exists only to turn a deadlock
// introduced by a future edit into a FAILURE rather than a hang.
constexpr int kTestTimeoutSeconds = 20;

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
/// it is empty, matching CompositorTest.cpp's own mustAcquire().
SurfaceRef mustAcquire(SurfaceTable &table) {
  auto ref = table.acquire();
  EXPECT_TRUE(ref.has_value()) << "acquire() unexpectedly returned nothing";
  return ref.value_or(SurfaceRef{});
}

} // namespace

TEST(TestHooksScaffold, TargetBuildsAndRuns) {
  EXPECT_TRUE(kWatchdogInstalled);
}

//===========================================================================
// uiTestHooksEnabled() / uiTestHooksEnabledFor()
//
// Task 9 review finding F4: a plain top-level EXPECT_FALSE(
// uiTestHooksEnabled()) proves only that the flag is not hardwired ON --
// hardwiring it OFF, or misspelling HERMES_NODE_UI_TEST_HOOKS inside the
// getenv() call, passed every test in the repository. It also depends on
// this process's ambient environment, which is real: measured,
// HERMES_NODE_UI_TEST_HOOKS=1 cmake-build-asan/unittests/TestHooksTest
// --gtest_filter=UiTestHooksEnabledTest.* used to FAIL, because
// unittests/lit.cfg's lit.formats.GoogleTest does not scrub the
// environment the way test/lit.cfg's ShTest does -- so a developer who
// exports that variable to drive a real UI test got a red unit suite with
// no explanation.
//
// The fix is two-layered. uiTestHooksEnabledForTest below tests the
// parsing *logic* directly, with no process environment touched at all --
// this is what closes the "hardwired in either direction" hole, and it
// cannot depend on any shell state because it never calls getenv().
// UiTestHooksEnabledDeathTest below additionally proves the *wiring* --
// that uiTestHooksEnabled() reads the documented variable name and feeds
// it through that same logic -- each case in a forked child (EXPECT_EXIT)
// that sets its OWN environment before the one call that matters. That is
// required, not merely tidy: uiTestHooksEnabled()'s answer is cached in a
// function-local static for the life of the process, so calling it even
// once from THIS test file's own top-level code would prime that cache for
// every later test in the same binary run, including these death tests --
// a fork after that point would inherit the already-decided answer instead
// of evaluating fresh. Nothing in this file may call uiTestHooksEnabled()
// outside a forked child, for exactly that reason.
//===========================================================================

TEST(UiTestHooksEnabledForTest, RecognizesEnabledAndDisabledValues) {
  // Mutation: change either comparison in uiTestHooksEnabledFor() -- e.g.
  // drop the std::strcmp(value, "0") != 0 term, or flip a && to ||. Every
  // case below is independent enough that almost any such change flips at
  // least one of them.
  EXPECT_FALSE(uiTestHooksEnabledFor(nullptr));
  EXPECT_FALSE(uiTestHooksEnabledFor(""));
  EXPECT_FALSE(uiTestHooksEnabledFor("0"));
  EXPECT_TRUE(uiTestHooksEnabledFor("1"));
  EXPECT_TRUE(uiTestHooksEnabledFor("UI"));
}

TEST(UiTestHooksEnabledDeathTest, DefaultsToFalseWithTheVariableUnset) {
  // unsetenv() inside the child, not a bare "nothing sets it": the whole
  // point is to not depend on what the ambient shell that launched this
  // binary happened to export.
  //
  // Mutation: hardwire uiTestHooksEnabled()'s cached static to `true`.
  // The child then exits 1 instead of 0.
  EXPECT_EXIT(
      {
        ::unsetenv("HERMES_NODE_UI_TEST_HOOKS");
        _exit(uiTestHooksEnabled() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0),
      "");
}

TEST(UiTestHooksEnabledDeathTest, ReadsTheDocumentedVariableName) {
  // Mutation: hardwire uiTestHooksEnabled()'s cached static to `false`, OR
  // misspell the string literal passed to getenv() in test_hooks.cpp (e.g.
  // "HERMES_NODE_UI_TEST_HOOK"). Either way this child's explicit setenv()
  // of the correctly-spelled name is never observed, and it exits 1
  // instead of 0.
  EXPECT_EXIT(
      {
        ::setenv("HERMES_NODE_UI_TEST_HOOKS", "1", 1);
        _exit(uiTestHooksEnabled() ? 0 : 1);
      },
      ::testing::ExitedWithCode(0),
      "");
}

//===========================================================================
// CompletionGate, standalone: the not-held default, which is what makes it
// safe to sit on a production completion path unconditionally.
//===========================================================================

TEST(CompletionGateTest, NotHeldRunsImmediately) {
  int calls = 0;
  CompletionGate gate;
  // Never held: completeProducer() must run its action inline, synchronously
  // -- not merely "eventually".
  gate.completeProducer([&] { ++calls; });
  // Mutation: make completeProducer() always queue, regardless of
  // producerHeld_. Then nothing has run yet and this fails at 0 != 1.
  EXPECT_EQ(calls, 1);

  // A subsequent releaseProducer(), with nothing queued, must not re-run it.
  gate.releaseProducer();
  EXPECT_EQ(calls, 1);
}

//===========================================================================
// The scenario the plan's brief names explicitly (Task 9, Step 2): hold
// producer completion, assert nothing publishes; release, assert
// publication; hold consumer completion across a displacement, assert
// RETIRING; release, assert FREE.
//
// Each closure below is built out of SurfaceTable's own real methods
// (beginGpuWork/endGpuWork, addRead/releaseRead), per the brief's own
// instruction not to invent a parallel mechanism; CompletionGate only
// controls when the closure runs.
//
// These closures call SurfaceTable::publish()/reclaim() directly, which is
// correct AT THIS LEVEL -- this file's subject is SurfaceTable's own state
// machine composed with CompletionGate, not TickMachine. Production code
// must not copy this shape: a real completion handler calls
// TickMachine::publishFrame()/publicationDropped() (tick_machine.h), never
// a bare SurfaceTable::publish()/reclaim(), because TickMachine leaves
// kGpuPending only inside those two composed entry points and a direct
// SurfaceTable::publish() would leave it parked there forever. See
// CompletionGate's own class comment (test_hooks.h) for the same point
// made where a production implementer will read it.
//===========================================================================

TEST(
    SurfaceTableControllableCompletionTest,
    HoldWithholdsPublicationReleaseDeliversIt) {
  // "Publish ... after the producer's GPU work has completed" (design spec,
  // Surfaces): the simulated completion handler balances beginGpuWork()
  // with endGpuWork() and then publishes, in that order -- the SurfaceTable
  // half of what a real completion handler does; see this section's header
  // comment for why the TickMachine half is deliberately not modelled here.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(4, 4);
  SurfaceRef ref = mustAcquire(table);
  table.beginGpuWork(ref);

  CompletionGate gate;
  bool published = false;
  auto onProducerComplete = [&] {
    table.endGpuWork(ref);
    published = table.publish(ref);
  };

  gate.holdProducer();
  gate.completeProducer(onProducerComplete);

  // Mutation: make completeProducer() run its action immediately regardless
  // of producerHeld_. Then onProducerComplete() already ran above, and this
  // fails: `published` is true and the slot is already READY.
  EXPECT_FALSE(published) << "nothing publishes while producer completion "
                             "is held";
  EXPECT_EQ(table.state(ref), SlotState::kWriting);

  // Mutation: make releaseProducer() clear the held flag without running
  // the queued action (e.g. drop the swapped vector instead of iterating
  // it). Then the closure above never runs, `published` stays false, and
  // this EXPECT fails.
  gate.releaseProducer();
  EXPECT_TRUE(published);
  EXPECT_EQ(table.state(ref), SlotState::kReady);
}

TEST(
    SurfaceTableControllableCompletionTest,
    ConsumerHoldAcrossDisplacementRetiresUntilReleased) {
  // "hold consumer completion across a displacement, assert RETIRING;
  // release, assert FREE" -- the brief's own words for this test.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(4, 4);

  SurfaceRef first = mustAcquire(table);
  ASSERT_TRUE(table.publish(first));
  SurfaceRef presenting = *table.take();
  ASSERT_EQ(presenting, first);

  // A blit is submitted against the presenting surface: addRead() before
  // submission, matching backend.h's own contract ("The read reference is
  // taken by the caller BEFORE this is called").
  table.addRead(presenting);

  CompletionGate gate;
  gate.holdConsumer();
  // The blit's completion arrives, but the gate is held: queued, not run.
  gate.completeConsumer([&] { table.releaseRead(presenting); });

  // A second frame is produced and published, then taken -- displacing the
  // first surface. PRESENTING -> RETIRING, because its read count is still
  // 1 (the completion above never reached releaseRead()).
  SurfaceRef second = mustAcquire(table);
  ASSERT_TRUE(table.publish(second));
  SurfaceRef newlyPresenting = *table.take();
  ASSERT_EQ(newlyPresenting, second);

  // Mutation: make completeConsumer() run its action immediately regardless
  // of consumerHeld_. Then the read was already released before the
  // displacement above ran, take() finds zero outstanding reads on `first`,
  // and this state is kFree instead of kRetiring.
  EXPECT_EQ(table.state(first), SlotState::kRetiring);

  // Release the held completion: the queued releaseRead() runs now, is the
  // last owner of `first`'s slot, and frees it.
  //
  // Mutation: make releaseConsumer() clear the held flag without running
  // the queued action. Then the read is never released and this stays
  // kRetiring instead of becoming kFree.
  gate.releaseConsumer();
  EXPECT_EQ(table.state(first), SlotState::kFree);
}

//===========================================================================
// FrameCapture
//===========================================================================

namespace {

CaptureRequest sampleRequest() {
  CaptureRequest r;
  r.generation = GenerationId{7};
  r.publicationId = 99;
  r.targetWidth = 320;
  r.targetHeight = 240;
  r.framebufferWidth = 320;
  r.framebufferHeight = 240;
  r.dpiScale = 1.0;
  r.inputSerial = 42;
  return r;
}

} // namespace

TEST(FrameCaptureTest, MatchesOnlyWhenEveryFieldAgrees) {
  // One sub-case per field of CaptureRequest, each mutating exactly that
  // field away from an otherwise-matching observation, plus one all-fields-
  // agree case at the end. The invariant this pins is the design's own rule
  // (spec "Testing": "a capture request carries, and the capture is only
  // taken when they agree: the target generation, the target's dimensions,
  // the actual framebuffer dimensions and DPI scale, and the input state
  // the frame was drawn from" -- plus the publication id, spec "Testing":
  // "keyed to a publication id ... because blits can precede any
  // publication"): dropping any single comparison from
  // tryMatch()/operator== makes exactly one of these sub-cases wrongly
  // report a match, which is why every field of the struct has its own
  // block below rather than a representative few -- an omitted field here
  // is an omitted comparison nothing else in this file would catch (finding
  // F1 of the Task 9 review).
  const CaptureRequest want = sampleRequest();

  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.generation = GenerationId{8};
    EXPECT_FALSE(capture.tryMatch(wrong));
    EXPECT_TRUE(capture.pending());
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.publicationId = 100;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.targetWidth = 321;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.targetHeight = 200;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.framebufferWidth = 300;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.framebufferHeight = 241;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.dpiScale = 2.0;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    CaptureRequest wrong = want;
    wrong.inputSerial = 43;
    EXPECT_FALSE(capture.tryMatch(wrong));
  }
  {
    FrameCapture capture;
    capture.request(want);
    EXPECT_TRUE(capture.tryMatch(want));
  }
}

TEST(FrameCaptureTest, AcknowledgingClearsThePendingRequest) {
  FrameCapture capture;
  const CaptureRequest want = sampleRequest();
  capture.request(want);
  ASSERT_TRUE(capture.pending());

  ASSERT_TRUE(capture.tryMatch(want));
  // Mutation: have tryMatch() leave pending_ set on a match instead of
  // clearing it. Then a second identical frame -- entirely plausible, a
  // fixed-pattern demo presents the same content every frame -- would
  // match again, capturing more than the one frame that was asked for.
  EXPECT_FALSE(capture.pending());
  EXPECT_FALSE(capture.tryMatch(want));
}

TEST(FrameCaptureTest, CancelClearsAPendingRequestWithoutMatching) {
  // Task 9 review finding F5: cancel() had no test and no caller yet --
  // kept rather than dropped, since a test driver that decides not to wait
  // for a capture after all (a timeout, an early test failure) needs a way
  // to withdraw the request rather than leave a stale one for some later,
  // unrelated frame to match against.
  FrameCapture capture;
  const CaptureRequest want = sampleRequest();
  capture.request(want);
  ASSERT_TRUE(capture.pending());

  // Mutation: make cancel()'s body a no-op. Then pending() stays true and
  // the later frame below still (wrongly) matches.
  capture.cancel();
  EXPECT_FALSE(capture.pending());
  EXPECT_FALSE(capture.tryMatch(want));
}
