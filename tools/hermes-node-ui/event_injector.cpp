/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "event_injector.h"

#include <utility>

namespace hermes {
namespace node_compat {

void PlatformEventInjector::inject(InjectedPlatformEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(std::move(event));
}

bool PlatformEventInjector::takeNext(InjectedPlatformEvent *out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty())
    return false;
  // FIFO: an injected sequence must reach the converter in the order it
  // was injected, the same order InputQueue keeps downstream.
  *out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

} // namespace node_compat
} // namespace hermes
