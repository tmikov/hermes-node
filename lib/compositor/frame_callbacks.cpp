/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/frame_callbacks.h>

#include <utility>

namespace hermes {
namespace node_compat {

int FrameCallbackList::add(FrameCallback::Kind kind, void *fn, void *user) {
  Entry entry;
  entry.callback.id = nextId_++;
  entry.callback.kind = kind;
  entry.callback.fn = fn;
  entry.callback.user = user;
  pending_.push_back(entry);
  return entry.callback.id;
}

bool FrameCallbackList::cancel(int id, FrameCallback *removed) {
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->callback.id == id) {
      *removed = it->callback;
      pending_.erase(it);
      return true;
    }
  }
  // Only the part of the batch not yet handed out. Everything before
  // batchPos_ has run or is running -- including the caller itself, when a
  // callback cancels its own id -- and cannot be prevented any more.
  for (size_t i = batchPos_; i < batch_.size(); ++i) {
    Entry &entry = batch_[i];
    if (entry.callback.id == id) {
      if (entry.cancelled)
        return false;
      entry.cancelled = true;
      *removed = entry.callback;
      return true;
    }
  }
  return false;
}

void FrameCallbackList::beginBatch() {
  // Unrun leftovers first, then the new registrations; see the header.
  std::vector<Entry> batch(
      batch_.begin() + static_cast<std::ptrdiff_t>(batchPos_), batch_.end());
  batch.insert(batch.end(), pending_.begin(), pending_.end());
  pending_.clear();
  batch_ = std::move(batch);
  batchPos_ = 0;
}

bool FrameCallbackList::next(FrameCallback *out) {
  while (batchPos_ < batch_.size()) {
    const Entry &entry = batch_[batchPos_++];
    if (entry.cancelled)
      continue;
    *out = entry.callback;
    return true;
  }
  batch_.clear();
  batchPos_ = 0;
  return false;
}

bool FrameCallbackList::pending() const {
  return !pending_.empty();
}

void FrameCallbackList::releaseAll(
    const std::function<void(const FrameCallback &)> &release) {
  std::vector<Entry> batch;
  batch.swap(batch_);
  const size_t from = batchPos_;
  batchPos_ = 0;
  std::vector<Entry> pending;
  pending.swap(pending_);
  // Emptied before any release runs, so a release that re-enters the list
  // finds it consistent.
  for (size_t i = from; i < batch.size(); ++i) {
    if (!batch[i].cancelled)
      release(batch[i].callback);
  }
  for (const Entry &entry : pending)
    release(entry.callback);
}

} // namespace node_compat
} // namespace hermes
