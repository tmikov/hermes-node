/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/channels.h>

#include <cassert>
#include <chrono>
#include <iterator>
#include <utility>

namespace hermes {
namespace node_compat {

//===========================================================================
// InputQueue
//===========================================================================

void InputQueue::push(const UiEvent &event) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Only a move directly behind another move coalesces. A non-move event
  // between them breaks the run, so a click keeps its place in the sequence
  // of motion around it.
  if (event.type == UiEventType::kMouseMove && !events_.empty() &&
      events_.back().type == UiEventType::kMouseMove) {
    UiEvent &back = events_.back();
    // Sum the deltas before overwriting: a delta is a quantity, so dropping
    // one loses movement that really happened.
    const double dx = back.dx + event.dx;
    const double dy = back.dy + event.dy;
    // Everything else -- position, modifiers -- is a sample of the current
    // state, and the latest sample is the accurate one.
    back = event;
    back.dx = dx;
    back.dy = dy;
    return;
  }

  events_.push_back(event);
}

void InputQueue::drain(std::vector<UiEvent> &out) {
  std::vector<UiEvent> taken;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    taken.swap(events_);
  }
  // Moved out from under the lock: the copies are the caller's cost, not a
  // reason to keep the platform thread waiting.
  out.insert(
      out.end(),
      std::make_move_iterator(taken.begin()),
      std::make_move_iterator(taken.end()));
}

bool InputQueue::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.empty();
}

//===========================================================================
// Command
//===========================================================================

Command Command::setTitle(std::string title) {
  Command command;
  command.kind_ = Kind::kSetTitle;
  // By value then moved: the command owns the text outright, whatever the
  // caller does with theirs afterwards.
  command.title_ = std::move(title);
  return command;
}

Command Command::setCursorImage(
    std::vector<uint8_t> rgba,
    int width,
    int height,
    int hotspotX,
    int hotspotY) {
  Command command;
  command.kind_ = Kind::kSetCursorImage;
  command.cursorRgba_ = std::move(rgba);
  command.width_ = width;
  command.height_ = height;
  command.hotspotX_ = hotspotX;
  command.hotspotY_ = hotspotY;
  return command;
}

Command Command::setFullscreen(bool on) {
  Command command;
  command.kind_ = Kind::kSetFullscreen;
  command.flag_ = on;
  return command;
}

Command Command::setIcon(std::vector<uint8_t> rgba, int width, int height) {
  Command command;
  command.kind_ = Kind::kSetIcon;
  command.cursorRgba_ = std::move(rgba);
  command.width_ = width;
  command.height_ = height;
  return command;
}

Command Command::setCursor(int cursor) {
  Command command;
  command.kind_ = Kind::kSetCursor;
  command.cursor_ = cursor;
  return command;
}

Command Command::showCursor(bool show) {
  Command command;
  command.kind_ = Kind::kShowCursor;
  command.flag_ = show;
  return command;
}

Command Command::lockPointer(bool lock) {
  Command command;
  command.kind_ = Kind::kLockPointer;
  command.flag_ = lock;
  return command;
}

Command Command::writeClipboard(std::string utf8) {
  Command command;
  command.kind_ = Kind::kWriteClipboard;
  command.text_ = std::move(utf8);
  return command;
}

Command Command::readClipboard(
    uint64_t requestId,
    bool replyThroughSlot,
    uint64_t deadlineNs) {
  Command command;
  command.kind_ = Kind::kReadClipboard;
  command.requestId_ = requestId;
  command.flag_ = replyThroughSlot;
  command.deadlineNs_ = deadlineNs;
  return command;
}

const std::string &Command::title() const {
  assert(kind_ == Kind::kSetTitle && "title() on a command that has none");
  return title_;
}

const std::vector<uint8_t> &Command::cursorRgba() const {
  assert(
      (kind_ == Kind::kSetCursorImage || kind_ == Kind::kSetIcon) &&
      "cursorRgba() on a command with no pixels");
  return cursorRgba_;
}

int Command::width() const {
  assert(
      (kind_ == Kind::kSetCursorImage || kind_ == Kind::kSetIcon) &&
      "width() on a command with no pixels");
  return width_;
}

int Command::height() const {
  assert(
      (kind_ == Kind::kSetCursorImage || kind_ == Kind::kSetIcon) &&
      "height() on a command with no pixels");
  return height_;
}

int Command::hotspotX() const {
  assert(
      kind_ == Kind::kSetCursorImage && "hotspotX() on a non-cursor command");
  return hotspotX_;
}

int Command::hotspotY() const {
  assert(
      kind_ == Kind::kSetCursorImage && "hotspotY() on a non-cursor command");
  return hotspotY_;
}

bool Command::flag() const {
  assert(
      (kind_ == Kind::kSetFullscreen || kind_ == Kind::kShowCursor ||
       kind_ == Kind::kLockPointer) &&
      "flag() on a command that has none");
  return flag_;
}

int Command::cursor() const {
  assert(kind_ == Kind::kSetCursor && "cursor() on a non-cursor command");
  return cursor_;
}

const std::string &Command::text() const {
  assert(kind_ == Kind::kWriteClipboard && "text() on a non-clipboard command");
  return text_;
}

uint64_t Command::requestId() const {
  assert(kind_ == Kind::kReadClipboard && "requestId() on a non-read command");
  return requestId_;
}

bool Command::replyThroughSlot() const {
  assert(
      kind_ == Kind::kReadClipboard &&
      "replyThroughSlot() on a non-read command");
  return flag_;
}

uint64_t Command::deadlineNs() const {
  assert(kind_ == Kind::kReadClipboard && "deadlineNs() on a non-read command");
  return deadlineNs_;
}

//===========================================================================
// CommandQueue
//===========================================================================

void CommandQueue::push(Command command) {
  std::lock_guard<std::mutex> lock(mutex_);
  commands_.push_back(std::move(command));
}

void CommandQueue::drain(std::vector<Command> &out) {
  std::vector<Command> taken;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    taken.swap(commands_);
  }
  out.insert(
      out.end(),
      std::make_move_iterator(taken.begin()),
      std::make_move_iterator(taken.end()));
}

bool CommandQueue::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commands_.empty();
}

//===========================================================================
// Clipboard
//===========================================================================

uint64_t clipboardClockNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

ClipboardResult ClipboardRequest::wait(int timeoutMs) {
  std::unique_lock<std::mutex> lock(mutex_);
  // The predicate form both survives spurious wakeups and returns
  // immediately when the answer arrived before this call -- which is what
  // makes a responder running ahead of its waiter harmless rather than a
  // lost reply.
  if (!value_.has_value()) {
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
      return value_.has_value();
    });
  }
  if (value_.has_value())
    return *value_;
  // Built fresh: a timeout carries no text at all, so there is nothing an
  // earlier read could leave behind in it.
  return ClipboardResult{ClipboardStatus::kTimeout, std::string()};
}

void ClipboardRequest::fulfill(ClipboardResult result) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (value_.has_value())
      return;
    value_ = std::move(result);
  }
  // Notified with the lock released: the waiter would only have to drop it
  // again.
  cv_.notify_all();
}

std::shared_ptr<ClipboardRequest> ClipboardRequests::create() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Reap requests whose waiter has gone. The map holds weak references, so
  // an abandoned request leaves an expired entry behind; sweeping here keeps
  // the map bounded by the number of live requests without needing a
  // back-pointer from the slot, whose destructor would otherwise have to
  // reach a registry that may already be gone.
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.expired())
      it = pending_.erase(it);
    else
      ++it;
  }

  // Not make_shared: the constructor is private to keep ids the registry's
  // to issue.
  std::shared_ptr<ClipboardRequest> slot(new ClipboardRequest(nextId_++));
  pending_[slot->id()] = slot;
  return slot;
}

bool ClipboardRequests::answer(uint64_t id, ClipboardResult result) {
  std::shared_ptr<ClipboardRequest> slot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    // No such request: a reply to one already answered, or abandoned. The
    // id is what makes this detectable -- pairing a reply with "whichever
    // request is outstanding" would hand this text to an unrelated one.
    if (it == pending_.end())
      return false;
    slot = it->second.lock();
    pending_.erase(it);
  }

  // The waiter is gone, but the reply was safe to deliver at every point:
  // the shared_ptr above is what guarantees the slot cannot be freed
  // underneath fulfill().
  if (!slot)
    return false;

  // Outside the registry lock: fulfill() takes the slot's own mutex, and
  // holding both would put an ordering constraint on every future caller for
  // no gain.
  slot->fulfill(std::move(result));
  return true;
}

void ClipboardRequests::failAll(ClipboardStatus status) {
  std::vector<std::shared_ptr<ClipboardRequest>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : pending_) {
      if (auto slot = entry.second.lock())
        live.push_back(std::move(slot));
    }
    // Forgotten under the lock, before anyone is woken: a reply racing this
    // call either found its entry first (and is the answer, since the first
    // fulfil wins) or finds nothing and is refused.
    pending_.clear();
  }
  // Fulfilled outside the registry lock, for the reason answer() gives.
  for (auto &slot : live)
    slot->fulfill(ClipboardResult{status, std::string()});
}

} // namespace node_compat
} // namespace hermes
