/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_EVENT_DISPATCHER_H
#define HERMES_NODE_COMPAT_COMPOSITOR_EVENT_DISPATCHER_H

#include <hermes/node-compat/compositor/channels.h>

#include <functional>
#include <vector>

namespace hermes {
namespace node_compat {

/// Drains an `InputQueue` on the owner thread and hands each event to a
/// sink, in push order.
///
/// **Why a class rather than a loop at the call site: re-entrancy.** A sink
/// runs listeners, and a listener can cause another dispatch -- a JS
/// listener calling `process.exit()` runs `flushPendingWrites`, which pumps
/// the event loop, which can run the posted dispatch function again. The
/// events being delivered sit in one reused buffer, and a native listener
/// is handed an `hnui_event` that *borrows* from the `UiEvent` in that
/// buffer (`hnui_event_convert.h`, ADDRESS STABILITY). A nested call that
/// drained into the same buffer would reallocate it under the outer call,
/// and a release-build addon would read freed small-string bytes with
/// nothing to say so. So a nested call **returns at once** and touches
/// nothing: the outer call is still looping, and its next pass picks up
/// whatever the nested one would have delivered, in order.
///
/// Threading: owner thread only. The queue carries its own lock; this
/// class has none, because only one thread ever calls into it.
class EventDispatcher {
 public:
  using Sink = std::function<void(const UiEvent &)>;

  /// \p queue must outlive this object.
  explicit EventDispatcher(InputQueue &queue);

  EventDispatcher(const EventDispatcher &) = delete;
  EventDispatcher &operator=(const EventDispatcher &) = delete;

  /// Deliver everything queued, in order, and keep going until the queue
  /// is empty -- so an event pushed while a sink runs is delivered by this
  /// same call. A call made while one is already running returns at once:
  /// the running one will pick the new events up.
  void dispatch(const Sink &sink);

  /// True exactly while a sink called by dispatch() is running.
  bool dispatching() const {
    return dispatching_;
  }

 private:
  InputQueue &queue_;
  /// Reused across passes and across calls, which is what makes the guard
  /// above load-bearing rather than tidy: it is this buffer a nested drain
  /// would reallocate.
  std::vector<UiEvent> buffer_;
  bool dispatching_ = false;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_EVENT_DISPATCHER_H
