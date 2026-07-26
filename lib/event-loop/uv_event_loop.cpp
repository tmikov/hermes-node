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
/// data needed by hermes_napi_host::post_work.
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
  hermes_napi_host host{};

  /// Mutex protecting the task queue and loopRefs (accessed from multiple
  /// threads).
  uv_mutex_t taskMutex{};
  /// Singly-linked list of pending tasks (LIFO push, reversed on drain).
  TaskEntry *taskHead = nullptr;
  /// Number of outstanding long-lived loop references taken via ref_loop().
  /// Each napi_env sharing this host contributes at most one, but distinct
  /// envs may each hold one concurrently -- see the contract documented on
  /// hermes_napi_host::ref_loop.
  int loopRefs = 0;
  /// Set by close(), after which the loop and its handles are gone. The Impl
  /// itself is kept alive until ~UvEventLoop because the host vtable can
  /// still be invoked afterwards: napi_env teardown runs when the Runtime is
  /// destroyed, which happens after close(), and releases the loop refs held
  /// by any surviving thread-safe function. Callbacks that would touch the
  /// closed loop become no-ops.
  bool closed = false;
  /// The thread that owns the loop, captured in init(). Handle state may
  /// only be touched here; post_task and post_work accept callers from other
  /// threads. See the threading note on the class.
  uv_thread_t loopThread{};

  /// Whether the caller is on the thread that owns the loop. Only that
  /// thread may touch handle state.
  bool onLoopThread() const {
    uv_thread_t self = uv_thread_self();
    // uv_thread_equal takes non-const pointers, but does not modify.
    return uv_thread_equal(const_cast<uv_thread_t *>(&loopThread), &self) != 0;
  }

  //=========================================================================
  // hermes_napi_host vtable implementations (static)
  //=========================================================================

  static void postWork(
      void *loop_data,
      void *work_data,
      void (*execute)(void *work_data),
      void (*complete)(void *work_data, napi_status status)) {
    auto *impl = static_cast<Impl *>(loop_data);

    // Deliberately no loop-thread assert. Node restricts only the
    // thread-safe function ref/unref pair to the main thread;
    // napi_queue_async_work() carries no such requirement and goes straight
    // to uv_queue_work(), so an addon may submit work from a thread it
    // spawned. Asserting here would abort on an addon Node accepts.
    //
    // Deferring an off-thread submission to the loop thread would be worse
    // than the narrow race it removes: uv_queue_work() bumps the loop's
    // active request count, and that is what keeps the loop alive for work
    // submitted from elsewhere. A deferred submission could find nothing
    // draining the task queue, leaving the work unqueued and its complete
    // callback never called.
    uv_mutex_lock(&impl->taskMutex);
    bool isClosed = impl->closed;
    uv_mutex_unlock(&impl->taskMutex);
    if (isClosed) {
      // No loop left to queue on. Report cancellation, which keeps the
      // contract that complete is always called exactly once.
      complete(work_data, napi_cancelled);
      return;
    }

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
    if (impl->closed) {
      // The loop is gone, so the task can never run. Drop it rather than
      // queueing something nothing will drain.
      uv_mutex_unlock(&impl->taskMutex);
      delete entry;
      return;
    }
    // Push to head of the singly-linked list (LIFO, reversed on drain).
    entry->next = impl->taskHead;
    impl->taskHead = entry;
    uv_mutex_unlock(&impl->taskMutex);

    // Ref the async handle so the loop stays alive to process this task;
    // drainTasks() unrefs it once all tasks are processed.
    //
    // Only possible when posting from the loop thread: uv_ref() mutates
    // handle flags and the loop's active-handle count without synchronizing
    // against the running loop. Refing from a producer thread would also be
    // pointless -- by the time we get here the loop may already have made
    // its liveness decision and returned from uv_run(). A cross-thread
    // producer that needs the loop kept alive says so with ref_loop(), which
    // is what a referenced thread-safe function does; an unreferenced one
    // deliberately does not hold the loop open, matching Node. Either way,
    // the next drainTasks() re-evaluates the ref on the loop thread.
    if (impl->onLoopThread())
      impl->updateAsyncRef();

    // Wake up the event loop. uv_async_send is thread-safe and coalescing.
    uv_async_send(&impl->async);
  }

  static void refLoop(void *loop_data) {
    auto *impl = static_cast<Impl *>(loop_data);
    assert(
        impl->onLoopThread() && "ref_loop must be called on the loop thread");
    uv_mutex_lock(&impl->taskMutex);
    ++impl->loopRefs;
    uv_mutex_unlock(&impl->taskMutex);
    impl->updateAsyncRef();
  }

  static void unrefLoop(void *loop_data) {
    auto *impl = static_cast<Impl *>(loop_data);
    assert(
        impl->onLoopThread() && "unref_loop must be called on the loop thread");
    uv_mutex_lock(&impl->taskMutex);
    assert(impl->loopRefs > 0 && "unref_loop without a matching ref_loop");
    --impl->loopRefs;
    uv_mutex_unlock(&impl->taskMutex);
    impl->updateAsyncRef();
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

  /// Ref the async handle while something needs the loop to stay alive --
  /// pending tasks or an outstanding ref_loop() -- and unref it otherwise so
  /// an idle loop can exit. uv_ref/uv_unref use a flag (not a count), so
  /// repeated calls are idempotent and this may be called from anywhere the
  /// two conditions change -- on the loop thread, which is the only thread
  /// that may touch handle state.
  void updateAsyncRef() {
    assert(onLoopThread() && "async handle refs are loop-thread only");

    uv_mutex_lock(&taskMutex);
    bool alive = taskHead != nullptr || loopRefs > 0;
    bool isClosed = closed;
    uv_mutex_unlock(&taskMutex);

    // After close() the async handle is closed and the loop destroyed;
    // touching either would be a use-after-free.
    if (isClosed)
      return;

    if (alive)
      uv_ref(reinterpret_cast<uv_handle_t *>(&async));
    else
      uv_unref(reinterpret_cast<uv_handle_t *>(&async));
  }

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

    // Tasks may have been enqueued during execution, and a loop ref may be
    // outstanding; re-evaluate whether the loop still needs to stay alive.
    updateAsyncRef();
  }
};

//===========================================================================
// UvEventLoop public API
//===========================================================================

UvEventLoop::UvEventLoop() = default;

UvEventLoop::~UvEventLoop() {
  if (impl_) {
    // The mutex is live whenever impl_ is set: init() only publishes impl_
    // after uv_mutex_init() succeeds, and close() deliberately leaves it
    // usable for the post-close unref_loop calls.
    uv_mutex_destroy(&impl_->taskMutex);
    delete impl_;
  }
}

int UvEventLoop::init() {
  assert(!impl_ && "UvEventLoop::init() called twice");

  auto *impl = new Impl();
  impl->loopThread = uv_thread_self();

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

  // Wire up the hermes_napi_host vtable.
  impl->host.post_work = Impl::postWork;
  impl->host.cancel_work = Impl::cancelWork;
  impl->host.post_task = Impl::postTask;
  impl->host.ref_loop = Impl::refLoop;
  impl->host.unref_loop = Impl::unrefLoop;
  impl->host.data = impl;
  impl->host.uv_loop = &impl->loop;

  impl_ = impl;
  return 0;
}

int UvEventLoop::run() {
  assert(impl_ && "UvEventLoop::run() called before init()");
  assert(
      impl_->onLoopThread() &&
      "UvEventLoop::run() must be called on the thread that called init()");
  return uv_run(&impl_->loop, UV_RUN_DEFAULT);
}

int UvEventLoop::runOnce() {
  assert(impl_ && "UvEventLoop::runOnce() called before init()");
  assert(
      impl_->onLoopThread() &&
      "UvEventLoop::runOnce() must be called on the thread that called init()");
  return uv_run(&impl_->loop, UV_RUN_ONCE);
}

int UvEventLoop::close() {
  assert(impl_ && "UvEventLoop::close() called before init()");
  assert(
      impl_->onLoopThread() &&
      "UvEventLoop::close() must be called on the thread that called init()");
  assert(!impl_->closed && "UvEventLoop::close() called twice");

  // Mark the host vtable inert up front. Once the async handle below is
  // closed nothing would drain a newly posted task anyway, and doing this
  // first means another thread can never queue onto a loop we are tearing
  // down. The Impl itself stays alive: the napi_env is torn down when the
  // Runtime is destroyed, which happens after close(), and that teardown
  // still calls unref_loop for any thread-safe function holding a loop ref.
  // The mutex stays valid for those calls and is destroyed in ~UvEventLoop.
  uv_mutex_lock(&impl_->taskMutex);
  impl_->closed = true;
  uv_mutex_unlock(&impl_->taskMutex);

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

  return uv_loop_close(&impl_->loop);
}

hermes_napi_host *UvEventLoop::getHost() {
  assert(impl_ && "UvEventLoop::getHost() called before init()");
  return &impl_->host;
}

uv_loop_t *UvEventLoop::getLoop() {
  assert(impl_ && "UvEventLoop::getLoop() called before init()");
  assert(!impl_->closed && "UvEventLoop::getLoop() called after close()");
  return &impl_->loop;
}

} // namespace node_compat
} // namespace hermes
