/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_CHANNELS_H
#define HERMES_NODE_COMPAT_COMPOSITOR_CHANNELS_H

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

//===========================================================================
// Input: platform thread -> JS thread
//===========================================================================

enum class UiEventType {
  /// Not a real event, and deliberately the zero value: a
  /// default-constructed or half-filled UiEvent must not be a mouse move.
  /// It would coalesce into whatever move preceded it and overwrite that
  /// move's position with zeroes -- destroying a real event rather than
  /// merely adding a bogus one, and doing it silently.
  kInvalid = 0,
  kMouseMove,
  kMouseDown,
  kMouseUp,
  kMouseWheel,
  kKeyDown,
  kKeyUp,
  kTextInput,
  kResize,
  kFocus,
  kBlur,
  kCloseRequested,
  kFileDrop,
};

/// One input event, as carried between threads inside this process.
///
/// **This is not the public C ABI type.** It owns its payloads (a
/// std::string for text, a vector for dropped paths) precisely because it is
/// queued on one thread and drained on another, so nothing in it may alias
/// storage belonging to the platform callback that produced it. The flat POD
/// type that crosses the ABI boundary is a separate, later concern.
struct UiEvent {
  UiEventType type = UiEventType::kInvalid;

  /// Pointer position in logical pixels, and the movement since the previous
  /// event. Kept separately because coalescing treats them differently: a
  /// position is a sample of the latest state, a delta is a quantity that
  /// accumulates.
  double x = 0;
  double y = 0;
  double dx = 0;
  double dy = 0;

  int button = 0;
  int modifiers = 0;
  int keyCode = 0;

  /// kTextInput only.
  std::string text;
  /// kFileDrop only.
  std::vector<std::string> paths;
};

/// Events produced by the platform and consumed by JavaScript once per tick.
///
/// **Consecutive mouse moves coalesce**, because a program that reads the
/// pointer once a frame gains nothing from the intermediate samples and a
/// burst of them can outpace the frame rate. The position taken is the
/// **latest** and the deltas are **summed**: a delta is a quantity, so
/// dropping one loses real movement, where an intermediate position is
/// merely stale. A non-move event breaks the run, since coalescing across
/// one would reorder a click relative to the motion around it.
///
/// Threading: push() from the platform thread, drain() from the JS thread.
class InputQueue {
 public:
  InputQueue() = default;

  InputQueue(const InputQueue &) = delete;
  InputQueue &operator=(const InputQueue &) = delete;

  void push(const UiEvent &event);

  /// Move every queued event onto the end of \p out, leaving the queue
  /// empty. Appends rather than replaces, so a caller may reuse one buffer.
  void drain(std::vector<UiEvent> &out);

  bool empty() const;

 private:
  mutable std::mutex mutex_;
  std::vector<UiEvent> events_;
};

//===========================================================================
// Commands: JS thread -> platform thread
//===========================================================================

/// A request from JavaScript that only the platform thread may carry out.
///
/// Every payload is **owned**. The queue is drained later, on another
/// thread, by which time whatever the caller passed may be long gone -- so a
/// pointer or a string_view here would be a use-after-free that happens to
/// work whenever the drain is prompt.
class Command {
 public:
  enum class Kind {
    kSetTitle,
    kSetCursorImage,
  };

  static Command setTitle(std::string title);
  static Command setCursorImage(
      std::vector<uint8_t> rgba,
      int width,
      int height,
      int hotspotX,
      int hotspotY);

  Kind kind() const {
    return kind_;
  }

  /// kSetTitle only.
  const std::string &title() const;

  /// kSetCursorImage only. Tightly packed RGBA, width * height * 4 bytes.
  const std::vector<uint8_t> &cursorRgba() const;
  int width() const;
  int height() const;
  int hotspotX() const;
  int hotspotY() const;

 private:
  Command() = default;

  Kind kind_ = Kind::kSetTitle;
  std::string title_;
  std::vector<uint8_t> cursorRgba_;
  int width_ = 0;
  int height_ = 0;
  int hotspotX_ = 0;
  int hotspotY_ = 0;
};

/// Threading: push() from the JS thread, drain() from the platform thread.
class CommandQueue {
 public:
  CommandQueue() = default;

  CommandQueue(const CommandQueue &) = delete;
  CommandQueue &operator=(const CommandQueue &) = delete;

  void push(Command command);

  /// Move every queued command onto the end of \p out, leaving the queue
  /// empty.
  void drain(std::vector<Command> &out);

  bool empty() const;

 private:
  mutable std::mutex mutex_;
  std::vector<Command> commands_;
};

//===========================================================================
// Clipboard: a JS-thread request answered by the platform thread
//===========================================================================

class ClipboardRequests;

/// One outstanding clipboard read.
///
/// **Reference-counted and retained by the registry, deliberately.** Reading
/// the clipboard is synchronous in JavaScript but asynchronous on most
/// platforms, so the waiter has to block -- with a deadline, since the
/// thread that would answer can itself be blocked in presentation. A request
/// that times out is therefore abandoned *while a reply for it may still be
/// in flight*. Both hazards that follow are closed here: the reply carries
/// the id of the request it answers, so a late reply to A cannot satisfy a
/// later B; and the slot outlives its waiter, so a late reply writes into
/// live storage rather than a freed stack frame.
class ClipboardRequest {
 public:
  ClipboardRequest(const ClipboardRequest &) = delete;
  ClipboardRequest &operator=(const ClipboardRequest &) = delete;

  /// Identifies this request. A reply names it, so that answers and requests
  /// cannot be paired up by "whichever is outstanding".
  uint64_t id() const {
    return id_;
  }

  /// Block until answered or \p timeoutMs elapses. Empty on timeout, which
  /// is a refusal to hang the JS thread rather than an error.
  ///
  /// An answer that has already arrived is returned immediately, so a
  /// responder that runs before the waiter blocks is not lost -- the reason
  /// the cross-thread test needs no synchronization of its own.
  std::optional<std::string> wait(int timeoutMs);

 private:
  explicit ClipboardRequest(uint64_t id) : id_(id) {}

  /// The first answer wins; later ones are ignored.
  void fulfill(std::string text);

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  const uint64_t id_;
  std::optional<std::string> value_;

  friend class ClipboardRequests;
};

/// The outstanding clipboard requests, keyed by id.
///
/// Holds **weak** references: the registry tracks requests, it does not keep
/// abandoned ones alive. An entry whose waiter has gone expires by itself
/// and is reaped on the next create(), so a request that timed out and was
/// dropped costs nothing and a reply that arrives for it is a safe no-op.
class ClipboardRequests {
 public:
  ClipboardRequests() = default;

  ClipboardRequests(const ClipboardRequests &) = delete;
  ClipboardRequests &operator=(const ClipboardRequests &) = delete;

  /// Register a new request. Callable from the JS thread.
  std::shared_ptr<ClipboardRequest> create();

  /// Answer the request with id \p id, if it is still outstanding. Callable
  /// from the platform thread.
  ///
  /// \return false if no such request is outstanding -- a reply to one that
  ///   was already answered, or abandoned. That is an ordinary outcome, not
  ///   an error.
  bool answer(uint64_t id, std::string text);

 private:
  std::mutex mutex_;
  uint64_t nextId_ = 1;
  std::map<uint64_t, std::weak_ptr<ClipboardRequest>> pending_;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_CHANNELS_H
