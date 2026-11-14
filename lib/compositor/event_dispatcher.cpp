/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/event_dispatcher.h>

namespace hermes {
namespace node_compat {

EventDispatcher::EventDispatcher(InputQueue &queue) : queue_(queue) {}

void EventDispatcher::dispatch(const Sink &sink) {
  // The guard the class comment explains. A nested call must not so much
  // as clear the buffer: the outer call is iterating it, and a native
  // listener up the stack may be holding an hnui_event that borrows from
  // one of its elements.
  if (dispatching_)
    return;
  dispatching_ = true;

  // Pass after pass until a drain comes back empty, so that an event
  // pushed while a sink ran -- by the platform thread, or by a nested
  // dispatch() that returned at once above -- is delivered by this call
  // rather than left for a post that may already have been consumed.
  for (;;) {
    buffer_.clear();
    queue_.drain(buffer_);
    if (buffer_.empty())
      break;
    for (const UiEvent &event : buffer_)
      sink(event);
  }

  buffer_.clear();
  dispatching_ = false;
}

} // namespace node_compat
} // namespace hermes
