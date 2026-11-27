/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// UiHost's wait predicate -- "a window was requested, OR the runtime
// finished" (tools/hermes-node-ui/ui_host.cpp) -- has two ways to regress
// into a hang, and neither is safe to reproduce through test/ui-windowless.js
// alone: dropping the runtime-finished half hangs that lit test inside the
// 16-way parallel suite with nothing bounding it (unittests/lit.cfg and
// test/lit.cfg both configure no per-test timeout), and the
// windowed-but-unfinished branch used to join a JS thread that, by
// construction, would never finish -- a real deadlock hazard, not just a
// slow test. Both are pinned here instead (RULING 41), each bounded so a
// regression is a FAILURE rather than a hang: the first by this file's own
// TestWatchdog, copied from CompositorTest.cpp's pattern (see there for the
// full rationale); the second by construction, since the fix for that
// branch is to _exit() rather than join, which EXPECT_EXIT observes
// directly without this test process itself ever blocking on a join.

#include "named_addons.h"
#include "ui_host.h"

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

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
// hung test hangs the whole suite (and, in CI, the whole job's budget)
// instead of failing it.
//===========================================================================

// Lower than CompositorTest.cpp's 60s: that file's bound accounts for
// multi-second sweeps and ASAN symbolication of a real crash report, neither
// of which applies here -- both tests below do at most one runHermesNode()
// boot, and the one path expected to actually run to completion
// (WindowlessPathReturnsOnceTheRuntimeFinishes) is measured well under a
// second. 30s keeps a wide margin over that for a loaded ASAN + 16-way
// parallel run while keeping a genuine hang's cost to observe reasonable.
constexpr int kTestTimeoutSeconds = 30;

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

/// A config that boots, runs a trivial script, and finishes on its own --
/// for tests that expect run() to return without any window ever being
/// requested. disableCompileCache so this never touches a real developer's
/// or CI machine's on-disk cache from inside a unit test.
HermesNodeConfig quickFinishingConfig() {
  HermesNodeConfig config;
  config.process.disableCompileCache = true;
  config.argv.push_back("hermes-node-ui-test");
  config.evalCode = "process.exitCode = 7;";
  return config;
}

/// A config whose script keeps the event loop alive indefinitely -- a
/// repeating interval with nothing to ever clear it -- so a test using it
/// can prove run() does not wait for the runtime to finish once a window
/// has been requested.
HermesNodeConfig neverFinishingConfig() {
  HermesNodeConfig config;
  config.process.disableCompileCache = true;
  config.argv.push_back("hermes-node-ui-test");
  config.evalCode = "setInterval(function () {}, 1000);";
  return config;
}

} // namespace

// Pins the OR's runtime-finished half: with no window ever requested, run()
// must still return once the script finishes, with the runtime's own exit
// code.
//
// MUTATION: dropping "|| !runtimeFinished_" from the wait predicate's while
// condition in ui_host.cpp's run() (leaving only "!windowRequested_") makes
// this test hang instead of returning. Caught here by the TestWatchdog
// above -- an abort within kTestTimeoutSeconds -- rather than by
// test/ui-windowless.js, which has nothing bounding it inside the 16-way
// parallel lit suite (that lit test's own RUN line would simply never
// finish).
TEST(UiHostTest, WindowlessPathReturnsOnceTheRuntimeFinishes) {
  UiHost host(quickFinishingConfig());
  EXPECT_EQ(host.run(), 7);
}

// Pins the predicate's window-requested half together with the bounded-exit
// fix for that branch: requestWindow() is called from another thread while
// the runtime is deliberately still running (an interval with nothing to
// clear it), and run() must not wait for the runtime to finish. The old
// implementation joined the JS thread on this path before returning, which
// -- for a script with nothing to end it -- never returns, wedging the
// process instead of failing; the fix makes that branch _exit(1) instead,
// which is what makes this a death test: EXPECT_EXIT observes the exit code
// and stderr message directly, and this test process never itself blocks on
// the join the old code attempted.
//
// MUTATION: reverting ui_host.cpp's windowRequested-and-not-finished branch
// to call uv_thread_join(&jsThread_) before returning reproduces the exact
// hang this test exists to catch. Verified by hand (this binary, filtered
// to just this test, run under an external `timeout`, with the orphaned
// child cleaned up afterward) rather than by a plain build-and-run here:
// a real hang in a death test blocks the parent's waitpid() with nothing in
// this process bounding it either -- the watchdog above governs this
// process's own test thread, not a forked child stuck in a blocking libuv
// call, and gtest death tests fork rather than exec, so the child carries
// no watchdog thread of its own (fork() only continues the calling thread).
TEST(
    UiHostDeathTest,
    WindowRequestedWithoutFinishingExitsInsteadOfJoiningForever) {
  EXPECT_EXIT(
      {
        UiHost host(neverFinishingConfig());
        std::thread requester([&host] { host.requestWindow(); });
        requester.join();
        host.run();
      },
      ::testing::ExitedWithCode(1),
      "no window support");
}

TEST(NamedAddonsTest, AddonPathIsBesideTheExecutable) {
  EXPECT_EQ(
      pathBesideExecutable("/opt/app/bin/hermes-node-ui", "x.node"),
      "/opt/app/bin/x.node");
  // An executable directly in the root keeps the root, with no doubled
  // separator.
  EXPECT_EQ(pathBesideExecutable("/hermes-node-ui", "x.node"), "/x.node");
  EXPECT_EQ(pathBesideExecutable("hermes-node-ui", "x.node"), "./x.node");
}
// Mutation: answer "" when the separator is at position 0, which is what
// the empty directory substr(0, 0) gave did before. Fails on the root case.
