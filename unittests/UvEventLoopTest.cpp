/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/event-loop/uv_event_loop.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace hermes::node_compat;

//===========================================================================
// Basic lifecycle tests
//===========================================================================

TEST(UvEventLoop, InitAndClose) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);
  // No work posted — loop should return immediately.
  ASSERT_EQ(loop.run(), 0);
  ASSERT_EQ(loop.close(), 0);
}

TEST(UvEventLoop, GetLoop) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);
  EXPECT_NE(loop.getLoop(), nullptr);
  ASSERT_EQ(loop.run(), 0);
  ASSERT_EQ(loop.close(), 0);
}

TEST(UvEventLoop, GetEventLoop) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);
  hermes_napi_event_loop *el = loop.getEventLoop();
  ASSERT_NE(el, nullptr);
  EXPECT_NE(el->post_work, nullptr);
  EXPECT_NE(el->cancel_work, nullptr);
  EXPECT_NE(el->post_task, nullptr);
  EXPECT_NE(el->data, nullptr);
  ASSERT_EQ(loop.run(), 0);
  ASSERT_EQ(loop.close(), 0);
}

//===========================================================================
// post_work tests
//===========================================================================

namespace {

struct WorkTestData {
  std::atomic<bool> executed{false};
  std::atomic<bool> completed{false};
  napi_status completionStatus{napi_generic_failure};
  std::thread::id executeThreadId{};
  std::thread::id completeThreadId{};
};

void workExecute(void *data) {
  auto *td = static_cast<WorkTestData *>(data);
  td->executed.store(true);
  td->executeThreadId = std::this_thread::get_id();
}

void workComplete(void *data, napi_status status) {
  auto *td = static_cast<WorkTestData *>(data);
  td->completed.store(true);
  td->completionStatus = status;
  td->completeThreadId = std::this_thread::get_id();
}

} // namespace

TEST(UvEventLoop, PostWorkExecutesAndCompletes) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  WorkTestData td;
  hermes_napi_event_loop *el = loop.getEventLoop();
  el->post_work(el->data, &td, workExecute, workComplete);

  // Run the loop — it will process the work and its completion.
  loop.run();

  EXPECT_TRUE(td.executed.load());
  EXPECT_TRUE(td.completed.load());
  EXPECT_EQ(td.completionStatus, napi_ok);
  // Execute runs on a worker thread (different from main thread).
  EXPECT_NE(td.executeThreadId, std::thread::id{});
  // Complete runs on the main thread.
  EXPECT_EQ(td.completeThreadId, std::this_thread::get_id());
  // Execute and complete should be on different threads.
  EXPECT_NE(td.executeThreadId, td.completeThreadId);

  ASSERT_EQ(loop.close(), 0);
}

TEST(UvEventLoop, PostWorkMultiple) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  constexpr int kCount = 5;
  WorkTestData td[kCount];

  hermes_napi_event_loop *el = loop.getEventLoop();
  for (int i = 0; i < kCount; ++i) {
    el->post_work(el->data, &td[i], workExecute, workComplete);
  }

  loop.run();

  for (int i = 0; i < kCount; ++i) {
    EXPECT_TRUE(td[i].executed.load()) << "work " << i << " not executed";
    EXPECT_TRUE(td[i].completed.load()) << "work " << i << " not completed";
    EXPECT_EQ(td[i].completionStatus, napi_ok)
        << "work " << i << " bad status";
  }

  ASSERT_EQ(loop.close(), 0);
}

//===========================================================================
// cancel_work tests
//===========================================================================

TEST(UvEventLoop, CancelWorkReturnsFalse) {
  // Our implementation always returns false (cancellation not supported).
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  hermes_napi_event_loop *el = loop.getEventLoop();

  // cancel_work with an arbitrary pointer should return false.
  int dummy = 0;
  EXPECT_FALSE(el->cancel_work(el->data, &dummy));

  ASSERT_EQ(loop.run(), 0);
  ASSERT_EQ(loop.close(), 0);
}

//===========================================================================
// post_task tests
//===========================================================================

namespace {

struct TaskTestData {
  bool called = false;
  std::thread::id threadId{};
  int value = 0;
};

void taskCallback(void *data) {
  auto *td = static_cast<TaskTestData *>(data);
  td->called = true;
  td->threadId = std::this_thread::get_id();
}

void taskCallbackWithValue(void *data) {
  auto *td = static_cast<TaskTestData *>(data);
  td->called = true;
  td->value = 42;
  td->threadId = std::this_thread::get_id();
}

} // namespace

TEST(UvEventLoop, PostTaskFiresOnMainThread) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  TaskTestData td;
  hermes_napi_event_loop *el = loop.getEventLoop();
  el->post_task(el->data, &td, taskCallback);

  // Run one iteration — the async handle should fire and drain the task.
  loop.run();

  EXPECT_TRUE(td.called);
  EXPECT_EQ(td.threadId, std::this_thread::get_id());

  ASSERT_EQ(loop.close(), 0);
}

TEST(UvEventLoop, PostTaskFromAnotherThread) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  TaskTestData td;
  hermes_napi_event_loop *el = loop.getEventLoop();

  // Use a timer to keep the loop alive while the background thread posts.
  uv_timer_t timer;
  uv_timer_init(loop.getLoop(), &timer);
  uv_timer_start(
      &timer, [](uv_timer_t *t) { uv_close((uv_handle_t *)t, nullptr); },
      200, 0);

  // Post the task from another thread.
  std::thread t([&]() {
    // Small delay to ensure the main thread is in uv_run.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    el->post_task(el->data, &td, taskCallbackWithValue);
  });

  loop.run();
  t.join();

  EXPECT_TRUE(td.called);
  EXPECT_EQ(td.value, 42);
  // Task callback should run on the main thread, not the posting thread.
  EXPECT_EQ(td.threadId, std::this_thread::get_id());

  ASSERT_EQ(loop.close(), 0);
}

TEST(UvEventLoop, PostTaskMultiple) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  constexpr int kCount = 5;
  TaskTestData td[kCount];

  hermes_napi_event_loop *el = loop.getEventLoop();
  for (int i = 0; i < kCount; ++i) {
    el->post_task(el->data, &td[i], taskCallback);
  }

  loop.run();

  for (int i = 0; i < kCount; ++i) {
    EXPECT_TRUE(td[i].called) << "task " << i << " not called";
  }

  ASSERT_EQ(loop.close(), 0);
}

//===========================================================================
// Combined tests
//===========================================================================

namespace {

struct CombinedTestData {
  std::atomic<int> order{0};
  int workOrder = -1;
  int taskOrder = -1;
};

void combinedWorkExecute(void *data) {
  // Simulate some work.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void combinedWorkComplete(void *data, napi_status status) {
  auto *td = static_cast<CombinedTestData *>(data);
  td->workOrder = td->order.fetch_add(1);
}

void combinedTaskCallback(void *data) {
  auto *td = static_cast<CombinedTestData *>(data);
  td->taskOrder = td->order.fetch_add(1);
}

} // namespace

TEST(UvEventLoop, PostWorkAndPostTaskBothFire) {
  UvEventLoop loop;
  ASSERT_EQ(loop.init(), 0);

  CombinedTestData td;
  hermes_napi_event_loop *el = loop.getEventLoop();

  el->post_work(el->data, &td, combinedWorkExecute, combinedWorkComplete);
  el->post_task(el->data, &td, combinedTaskCallback);

  loop.run();

  // Both should have fired.
  EXPECT_GE(td.workOrder, 0);
  EXPECT_GE(td.taskOrder, 0);
  EXPECT_EQ(td.order.load(), 2);

  ASSERT_EQ(loop.close(), 0);
}
