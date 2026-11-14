/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_OWNER_DISPATCH_H
#define HERMES_NODE_COMPAT_COMPOSITOR_OWNER_DISPATCH_H

#include <uv.h>

#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace hermes {
namespace node_compat {

/// Gets work back onto the thread that owns a uv_loop_t, from any thread.
///
/// Used so that work which may only run on the loop thread -- GPU resource
/// destruction is the motivating case -- can be requested from wherever a
/// reference count happens to reach zero, which is not necessarily that
/// thread. `post()` is the one operation callable from anywhere; everything
/// else (construction, `close()`, the actual running of a posted function)
/// happens on the loop thread.
///
/// Threading: `post()` may be called from any thread, including the loop
/// thread itself. The constructor and `close()` must both be called on the
/// thread that will drive the loop (`uv_run`) -- the "owner thread" -- and
/// `close()` asserts that in debug builds; a posted function always runs
/// there too, whether via a real loop turn or via `close()`'s own drain.
///
/// One mutex (`mutex_`) guards the queue, the closed flag, and the async
/// handle pointer together, matching SurfaceTable's "one mutex guards
/// everything" approach: the transitions here are exactly as compound. In
/// particular, `post()`'s enqueue-then-wake and `close()`'s
/// stop-accepting-then-detach-the-handle must each be indivisible, or a
/// `post()` racing `close()` could call `uv_async_send` on a handle that is
/// already being closed.
///
/// Loop liveness: an idle OwnerDispatch does not keep the loop open by
/// itself. `uv_async_init` leaves a handle referenced, and a referenced
/// active handle alone makes `uv_run(UV_RUN_DEFAULT)` never return, so the
/// handle is unrefed whenever the queue is empty and refed while it is not
/// -- the same policy as, and directly modeled on,
/// `UvEventLoop::Impl::updateAsyncRef` (`lib/event-loop/uv_event_loop.cpp`).
/// That update only ever runs on the loop thread, since `uv_ref`/`uv_unref`
/// mutate loop-internal bookkeeping without synchronizing against a
/// concurrently running loop; a `post()` from another thread does not ref
/// the handle itself for the same reason `postTask` does not -- the next
/// drain re-evaluates.
///
/// Ordering: `post()`ed functions run in FIFO order *within one drain
/// pass*. A function posted while a pass is already running (including a
/// cascading post from a function that pass is executing) runs in a later
/// pass, so ordering is not globally FIFO across a cascade. Nothing in this
/// class or its current callers depends on global ordering.
class OwnerDispatch {
 public:
  /// \p loop must outlive this object. Must be called on the thread that
  /// will own this dispatcher -- the same thread `close()` is later
  /// required to run on.
  explicit OwnerDispatch(uv_loop_t *loop);

  /// Calls close() first if it has not already run, so that a dispatcher
  /// which still has posted work outstanding never simply drops it. That
  /// call requires the owner thread, exactly as an explicit close() call
  /// does -- so destroying an unclosed OwnerDispatch off the owner thread
  /// is exactly as much a misuse as calling close() there directly would
  /// be. If close() was already called explicitly, this is a no-op: no
  /// thread-affine state remains to enforce, so nothing here checks the
  /// calling thread in that case.
  ~OwnerDispatch();

  OwnerDispatch(const OwnerDispatch &) = delete;
  OwnerDispatch &operator=(const OwnerDispatch &) = delete;

  /// Queue \p fn to run on the loop thread and wake the loop with
  /// uv_async_send -- the one libuv call that is safe to make from any
  /// thread. Callable from any thread, including the loop thread. See the
  /// class comment for the ordering guarantee this does and does not make.
  ///
  /// Returns false, without enqueueing \p fn, once close() has run: a
  /// caller whose posted work is destruction-critical must be able to find
  /// out that it will not run rather than have it silently dropped.
  bool post(std::function<void()> fn);

  /// Test-only: run whatever is currently queued, synchronously, on the
  /// calling thread -- without going through the async handle or a real
  /// loop turn. Lets a test drive one drain pass deterministically, e.g. to
  /// exercise a queued function that itself calls post() again (see the
  /// extraction-rule test), without needing to pump an actual uv_loop_t.
  /// Owner-thread only (asserted), exactly like close(): it runs queued
  /// functions here, which may include GPU-resource destruction.
  void drainForTest();

  /// Owner-thread only (asserted against the thread that constructed this
  /// object). Stops accepting new work -- post() returns false from this
  /// point on -- and drains the queue to a fixpoint before returning:
  /// repeatedly runs whatever is queued until a pass leaves it empty, so a
  /// function that itself posts another (a cascading generation
  /// destruction, the motivating case) is not refused just because it ran
  /// during this call. Nothing queued, including anything queued by a
  /// cascade, is dropped: the work is GPU-resource destruction, and
  /// discarding a closure would leak. May be called at most once (asserted
  /// in debug builds; a second call is a harmless no-op in a release build,
  /// where the assert is compiled out). The destructor calls this
  /// automatically if it was not already called explicitly.
  ///
  /// Handle lifetime: uv_close() is asynchronous, and its callback runs
  /// later, on a subsequent turn of the loop -- possibly long after this
  /// call, and possibly after this OwnerDispatch object itself is gone.
  /// The uv_async_t is therefore heap-allocated on its own, independently
  /// of this object, in the constructor; close() detaches it here (so
  /// nothing after this point touches `this` through it) and the close
  /// callback frees that heap block itself once it finally runs. That is
  /// the "keep the loop running until the callback fires" alternative's
  /// opposite number: this dispatcher does not require the loop, or this
  /// object, to stay alive for the close callback to be *safe*. It does
  /// still need the loop to turn at least once more after close() returns
  /// for the callback to actually run and the detached handle to actually
  /// be freed -- close() only makes that safe to defer, not unnecessary.
  /// A loop that never runs again after close() leaks that one handle.
  void close();

 private:
  /// Steal the queue under the lock, release the lock, then run what was
  /// stolen -- the extraction rule: a queued function that itself calls
  /// post() must not deadlock on a lock this thread is still holding, and a
  /// generation destruction can cascade into another one. Shared by the
  /// real async callback, drainForTest(), and close()'s fixpoint loop, all
  /// of which only ever call it on the owner thread.
  void drainQueue();

  /// Ref the async handle while the queue is non-empty, unref it otherwise,
  /// so an idle OwnerDispatch does not by itself keep
  /// uv_run(UV_RUN_DEFAULT) from returning. See the class comment.
  /// Owner-thread only (asserted): touching handle ref state off the loop
  /// thread is not safe, matching UvEventLoop::Impl::updateAsyncRef.
  void updateAsyncRef();

  static void onAsync(uv_async_t *handle);
  static void onAsyncClosed(uv_handle_t *handle);

  const std::thread::id ownerThread_;

  std::mutex mutex_;
  std::deque<std::function<void()>> queue_;
  bool closed_ = false;
  /// Heap-allocated independently of this object; see close()'s comment.
  /// Null once close() has detached it.
  uv_async_t *async_;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_OWNER_DISPATCH_H
