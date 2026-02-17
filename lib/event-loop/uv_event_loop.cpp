/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/event-loop/uv_event_loop.h>

#include <cassert>

namespace hermes {
namespace node_compat {

/// Internal struct wrapping a uv_work_t request with the callbacks and
/// data needed by hermes_napi_event_loop::post_work.
struct WorkRequest {
  uv_work_t req;
  void *work_data;
  void (*execute)(void *work_data);
  void (*complete)(void *work_data, napi_status status);
};

/// Internal task queue entry for post_task.
struct TaskEntry {
  void *task_data;
  void (*callback)(void *task_data);
  TaskEntry *next;
};

//===========================================================================
// UvEventLoop::Impl
//===========================================================================

struct UvEventLoop::Impl {
  uv_loop_t loop{};
  uv_async_t async{};
  hermes_napi_event_loop eventLoop{};

  /// Mutex protecting the task queue (accessed from multiple threads).
  uv_mutex_t taskMutex{};
  /// Singly-linked list of pending tasks (LIFO push, reversed on drain).
  TaskEntry *taskHead = nullptr;

  //=========================================================================
  // hermes_napi_event_loop vtable implementations (static)
  //=========================================================================

  static void postWork(
      void *loop_data,
      void *work_data,
      void (*execute)(void *work_data),
      void (*complete)(void *work_data, napi_status status)) {
    auto *impl = static_cast<Impl *>(loop_data);

    auto *wr = new WorkRequest{};
    wr->req.data = wr;
    wr->work_data = work_data;
    wr->execute = execute;
    wr->complete = complete;

    int rc = uv_queue_work(&impl->loop, &wr->req, onWork, onWorkDone);
    if (rc != 0) {
      // If queuing failed, call complete with cancellation status on the
      // current thread. This matches the contract that complete is always
      // called exactly once.
      complete(work_data, napi_cancelled);
      delete wr;
    }
  }

  static bool cancelWork(void * /*loop_data*/, void * /*work_data*/) {
    // Cancellation is not supported in this implementation. The NAPI spec
    // allows cancel_work to fail, and napi_cancel_async_work will return
    // napi_generic_failure.
    return false;
  }

  static void postTask(
      void *loop_data,
      void *task_data,
      void (*callback)(void *task_data)) {
    auto *impl = static_cast<Impl *>(loop_data);

    auto *entry = new TaskEntry{task_data, callback, nullptr};

    uv_mutex_lock(&impl->taskMutex);
    // Push to head of the singly-linked list (LIFO, reversed on drain).
    entry->next = impl->taskHead;
    impl->taskHead = entry;
    uv_mutex_unlock(&impl->taskMutex);

    // Ref the async handle so the loop stays alive to process this task.
    // drainTasks() will unref it once all tasks are processed.
    // uv_ref uses a flag (not a count), so multiple refs are idempotent.
    uv_ref(reinterpret_cast<uv_handle_t *>(&impl->async));

    // Wake up the event loop. uv_async_send is thread-safe and coalescing.
    uv_async_send(&impl->async);
  }

  //=========================================================================
  // libuv callbacks (static)
  //=========================================================================

  static void onWork(uv_work_t *req) {
    auto *wr = static_cast<WorkRequest *>(req->data);
    wr->execute(wr->work_data);
  }

  static void onWorkDone(uv_work_t *req, int status) {
    auto *wr = static_cast<WorkRequest *>(req->data);
    napi_status nstatus = (status == UV_ECANCELED) ? napi_cancelled : napi_ok;
    wr->complete(wr->work_data, nstatus);
    delete wr;
  }

  static void onAsync(uv_async_t *handle) {
    auto *impl = static_cast<Impl *>(handle->data);
    impl->drainTasks();
  }

  static void onAsyncClose(uv_handle_t * /*handle*/) {
    // Nothing to do — the Impl owns the async handle by value.
  }

  //=========================================================================
  // Task queue
  //=========================================================================

  void drainTasks() {
    // Atomically steal the entire task list.
    uv_mutex_lock(&taskMutex);
    TaskEntry *head = taskHead;
    taskHead = nullptr;
    uv_mutex_unlock(&taskMutex);

    if (!head)
      return;

    // Reverse the list to get FIFO order.
    TaskEntry *reversed = nullptr;
    while (head) {
      TaskEntry *next = head->next;
      head->next = reversed;
      reversed = head;
      head = next;
    }

    // Execute all tasks.
    while (reversed) {
      TaskEntry *next = reversed->next;
      reversed->callback(reversed->task_data);
      delete reversed;
      reversed = next;
    }

    // Check if new tasks were enqueued during execution. If the queue is
    // now empty, unref the async handle so the loop can exit when idle.
    uv_mutex_lock(&taskMutex);
    bool empty = (taskHead == nullptr);
    uv_mutex_unlock(&taskMutex);
    if (empty)
      uv_unref(reinterpret_cast<uv_handle_t *>(&async));
  }
};

//===========================================================================
// UvEventLoop public API
//===========================================================================

UvEventLoop::UvEventLoop() = default;

UvEventLoop::~UvEventLoop() {
  delete impl_;
}

int UvEventLoop::init() {
  assert(!impl_ && "UvEventLoop::init() called twice");

  auto *impl = new Impl();

  int rc = uv_loop_init(&impl->loop);
  if (rc != 0) {
    delete impl;
    return rc;
  }

  rc = uv_mutex_init(&impl->taskMutex);
  if (rc != 0) {
    uv_loop_close(&impl->loop);
    delete impl;
    return rc;
  }

  // Initialize the async handle used for post_task. The callback drains
  // the task queue.
  rc = uv_async_init(&impl->loop, &impl->async, Impl::onAsync);
  if (rc != 0) {
    uv_mutex_destroy(&impl->taskMutex);
    uv_loop_close(&impl->loop);
    delete impl;
    return rc;
  }
  impl->async.data = impl;

  // The async handle keeps the loop alive even when idle. Unref it so
  // the loop can exit when there is no other work. post_task will re-ref
  // it transiently via uv_async_send.
  uv_unref(reinterpret_cast<uv_handle_t *>(&impl->async));

  // Wire up the hermes_napi_event_loop vtable.
  impl->eventLoop.post_work = Impl::postWork;
  impl->eventLoop.cancel_work = Impl::cancelWork;
  impl->eventLoop.post_task = Impl::postTask;
  impl->eventLoop.data = impl;

  impl_ = impl;
  return 0;
}

int UvEventLoop::run() {
  assert(impl_ && "UvEventLoop::run() called before init()");
  return uv_run(&impl_->loop, UV_RUN_DEFAULT);
}

int UvEventLoop::runOnce() {
  assert(impl_ && "UvEventLoop::runOnce() called before init()");
  return uv_run(&impl_->loop, UV_RUN_ONCE);
}

int UvEventLoop::close() {
  assert(impl_ && "UvEventLoop::close() called before init()");

  // Close the async handle. The actual cleanup happens in the close callback.
  uv_close(reinterpret_cast<uv_handle_t *>(&impl_->async), Impl::onAsyncClose);
  // Run the loop to process the close callback.
  uv_run(&impl_->loop, UV_RUN_DEFAULT);

  // Drain any remaining tasks that arrived after the loop stopped.
  impl_->drainTasks();

  // Force-close any remaining handles. This mirrors Node's
  // Environment::CleanupHandles() which walks handle_wrap_queue_ and closes
  // everything. We use uv_walk() since we don't maintain a separate registry.
  uv_walk(
      &impl_->loop,
      [](uv_handle_t *handle, void *) {
        if (!uv_is_closing(handle)) {
          uv_close(handle, nullptr);
        }
      },
      nullptr);
  // Run the loop until all close callbacks have been processed.
  uv_run(&impl_->loop, UV_RUN_DEFAULT);

  uv_mutex_destroy(&impl_->taskMutex);
  int rc = uv_loop_close(&impl_->loop);

  delete impl_;
  impl_ = nullptr;
  return rc;
}

hermes_napi_event_loop *UvEventLoop::getEventLoop() {
  assert(impl_ && "UvEventLoop::getEventLoop() called before init()");
  return &impl_->eventLoop;
}

uv_loop_t *UvEventLoop::getLoop() {
  assert(impl_ && "UvEventLoop::getLoop() called before init()");
  return &impl_->loop;
}

} // namespace node_compat
} // namespace hermes
