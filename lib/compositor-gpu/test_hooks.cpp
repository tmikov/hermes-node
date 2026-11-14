/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "test_hooks.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace hermes {
namespace node_compat {

bool uiTestHooksEnabledFor(const char *value) {
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool uiTestHooksEnabled() {
  // Function-local static: see the header comment for why this is read
  // once rather than on every call.
  static const bool enabled =
      uiTestHooksEnabledFor(::getenv("HERMES_NODE_UI_TEST_HOOKS"));
  return enabled;
}

//===========================================================================
// CompletionGate
//===========================================================================

void CompletionGate::holdProducer() {
  std::lock_guard<std::mutex> lock(mutex_);
  producerHeld_ = true;
}

void CompletionGate::releaseProducer() {
  std::vector<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    producerHeld_ = false;
    ready.swap(pendingProducer_);
  }
  // Run with the lock released: a queued action calls into SurfaceTable,
  // which takes its own lock and can run its destroy callback -- neither of
  // which may happen while mutex_ above is held. Same "extract, unlock,
  // invoke" discipline SurfaceTable::LockedScope itself follows.
  for (auto &action : ready)
    action();
}

void CompletionGate::completeProducer(std::function<void()> action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producerHeld_) {
      pendingProducer_.push_back(std::move(action));
      return;
    }
  }
  action();
}

void CompletionGate::holdConsumer() {
  std::lock_guard<std::mutex> lock(mutex_);
  consumerHeld_ = true;
}

void CompletionGate::releaseConsumer() {
  std::vector<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    consumerHeld_ = false;
    ready.swap(pendingConsumer_);
  }
  for (auto &action : ready)
    action();
}

void CompletionGate::completeConsumer(std::function<void()> action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (consumerHeld_) {
      pendingConsumer_.push_back(std::move(action));
      return;
    }
  }
  action();
}

//===========================================================================
// FrameCapture
//===========================================================================

void FrameCapture::request(const CaptureRequest &want) {
  std::lock_guard<std::mutex> lock(mutex_);
  want_ = want;
  pending_ = true;
}

void FrameCapture::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_ = false;
}

bool FrameCapture::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_;
}

bool FrameCapture::tryMatch(const CaptureRequest &observed) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_ || want_ != observed)
    return false;
  pending_ = false;
  return true;
}

} // namespace node_compat
} // namespace hermes
