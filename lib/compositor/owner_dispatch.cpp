/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/owner_dispatch.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace hermes {
namespace node_compat {

OwnerDispatch::OwnerDispatch(uv_loop_t *loop)
    : ownerThread_(std::this_thread::get_id()) {
  // Heap-allocated on its own, deliberately not a member: close() detaches
  // it from this object before uv_close()'s callback can run, and that
  // callback frees this block itself. See close()'s comment.
  async_ = new uv_async_t();
  async_->data = this;
  int rc = uv_async_init(loop, async_, &OwnerDispatch::onAsync);
  if (rc != 0) {
    // uv_async_init can fail under ordinary fd exhaustion --
    // uv__async_start creates an eventfd internally, and that can return
    // EMFILE/ENFILE -- not only when the loop itself is fundamentally
    // broken. There is no failure channel through the
    // OwnerDispatch(uv_loop_t*) interface this must match, and limping on
    // with a half-initialized handle would crash later, inside
    // uv_async_send or uv_close, somewhere that does not name the cause.
    // Fail loudly and immediately, in every build configuration -- abort(),
    // not assert(), so this does not vanish under NDEBUG.
    fprintf(
        stderr, "OwnerDispatch: uv_async_init failed: %s\n", uv_strerror(rc));
    abort();
  }
  // uv_async_init leaves the handle referenced; the queue is empty at this
  // point, so this unrefs it immediately. Without it, a live OwnerDispatch
  // with nothing posted keeps uv_run(UV_RUN_DEFAULT) from ever returning,
  // since an active referenced handle alone is enough to do that.
  updateAsyncRef();
}

OwnerDispatch::~OwnerDispatch() {
  // A dispatcher that still has work queued when it is destroyed must not
  // simply drop it -- close() is what guarantees that, so run it here if
  // the caller has not already. If close() already ran, this is a no-op:
  // no thread-affine state remains to enforce, so this does not itself
  // check the calling thread.
  //
  // Read under the lock like every other access to closed_ (post(),
  // updateAsyncRef(), close()). An unsynchronized read of a mutex-guarded
  // field is a data race whatever the practical odds -- and the odds here
  // are not zero: a post() from another thread can be inside its critical
  // section while this runs, which is exactly the report a TSAN run would
  // open with.
  bool alreadyClosed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    alreadyClosed = closed_;
  }
  if (!alreadyClosed)
    close();
}

void OwnerDispatch::onAsync(uv_async_t *handle) {
  auto *self = static_cast<OwnerDispatch *>(handle->data);
  self->drainQueue();
}

void OwnerDispatch::onAsyncClosed(uv_handle_t *handle) {
  // Deliberately does not touch handle->data / the OwnerDispatch that
  // created this handle: by the time this runs, that object may already be
  // gone. This callback's only job is to free the heap storage close()
  // detached.
  delete reinterpret_cast<uv_async_t *>(handle);
}

void OwnerDispatch::updateAsyncRef() {
  // Handle state may only be touched on the loop thread: uv_ref/uv_unref
  // mutate the loop's active-handle bookkeeping without synchronizing
  // against a concurrently running loop. Callers are the constructor, the
  // end of drainQueue() (always the loop thread -- see its own comment),
  // and post() when the poster happens to be the loop thread; an
  // off-thread post() does not call this, for the same reason
  // UvEventLoop::Impl::postTask does not ref from an off-thread caller --
  // by the time it could, the loop may already have made its liveness
  // decision, and the next drain re-evaluates anyway. Precedent:
  // UvEventLoop::Impl::updateAsyncRef (lib/event-loop/uv_event_loop.cpp).
  assert(
      std::this_thread::get_id() == ownerThread_ &&
      "async handle refs are loop-thread only");

  uv_async_t *handle;
  bool alive;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Already closed, or closing: the handle is being (or about to be)
    // handed to uv_close(), which does not care about ref state, and
    // async_ may already be null.
    if (closed_)
      return;
    handle = async_;
    alive = !queue_.empty();
  }
  if (alive)
    uv_ref(reinterpret_cast<uv_handle_t *>(handle));
  else
    uv_unref(reinterpret_cast<uv_handle_t *>(handle));
}

void OwnerDispatch::drainQueue() {
  // Extract under the lock, unlock, then invoke -- never call a queued
  // function with the lock held. A function that calls post() again would
  // otherwise deadlock on this same mutex, and that is not hypothetical:
  // generation destruction can cascade into another one.
  std::deque<std::function<void()>> ran;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ran.swap(queue_);
  }
  for (auto &fn : ran)
    fn();

  // A function just run (or a racing post() from another thread) may have
  // queued more work, or the queue may now be empty; re-evaluate. Always
  // runs on the loop thread here: the real async callback (onAsync),
  // drainForTest() (asserted) and close()'s fixpoint loop are the only
  // callers.
  updateAsyncRef();
}

bool OwnerDispatch::post(std::function<void()> fn) {
  // The enqueue and the wake happen in the same critical section as
  // close()'s stop-accepting-then-detach, so the two can never interleave:
  // either this runs first and close()'s drain picks up what was just
  // queued, or close() has already run and this sees closed_ set. Without
  // that, a post() that read async_ before close() nulled it could call
  // uv_async_send on a handle that uv_close() has already been given to.
  bool accepted;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      accepted = false;
    } else {
      queue_.push_back(std::move(fn));
      uv_async_send(async_);
      accepted = true;
    }
  }

  // Ref the handle so the loop does not exit before this runs -- but only
  // when posting from the loop thread itself; see updateAsyncRef()'s
  // comment for why an off-thread poster does not do this.
  if (accepted && std::this_thread::get_id() == ownerThread_)
    updateAsyncRef();

  return accepted;
}

void OwnerDispatch::drainForTest() {
  assert(
      std::this_thread::get_id() == ownerThread_ &&
      "OwnerDispatch::drainForTest() must be called on the owner thread");
  drainQueue();
}

void OwnerDispatch::close() {
  assert(
      std::this_thread::get_id() == ownerThread_ &&
      "OwnerDispatch::close() must be called on the thread that "
      "constructed it");
  assert(!closed_ && "OwnerDispatch::close() called twice");
  // Both asserts above vanish under NDEBUG; this is the release-build guard
  // against a second call, which would otherwise uv_close() a handle
  // already handed to it (async_ is already null by then).
  if (closed_)
    return;

  // Drain to a fixpoint rather than one pass: a function run here may
  // itself call post() again (a cascading generation destruction is the
  // addendum's own motivating example for the extraction rule), and that
  // post() must still succeed while draining is still in progress --
  // closed_ is not set until a pass leaves the queue empty, so a cascading
  // post() lands in the queue normally and the next pass picks it up. The
  // cascade is finite (a generation graph has finite depth), so this
  // terminates.
  //
  // The empty check and setting closed_ (+ detaching the handle) happen in
  // the SAME critical section, not two in a row: checking empty and then
  // separately marking closed_ would leave a window, after the check and
  // before the mark, where a racing post() on another thread could still
  // see closed_ == false, enqueue successfully, and return true for
  // something this loop has already decided it will never look at again --
  // accepted without ever running. Measured, not hypothesized: an earlier
  // two-critical-section version of this dropped 2 of ~4840 posts under
  // concurrent load, roughly 1 run in 15 of a tight repeat loop.
  uv_async_t *handle = nullptr;
  for (;;) {
    drainQueue();
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      closed_ = true;
      handle = async_;
      async_ = nullptr;
      break;
    }
  }

  // Detach the handle rather than wait for its close callback: see the
  // class comment and this method's own doc comment on close-callback
  // timing. onAsyncClosed() frees `handle` itself and never touches `this`.
  // Deliberately outside the lock above: uv__async_close() calls
  // uv__async_spin(), which must not run with mutex_ held.
  uv_close(
      reinterpret_cast<uv_handle_t *>(handle), &OwnerDispatch::onAsyncClosed);
}

} // namespace node_compat
} // namespace hermes
