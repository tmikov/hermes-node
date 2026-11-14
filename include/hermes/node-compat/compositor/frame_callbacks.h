/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_FRAME_CALLBACKS_H
#define HERMES_NODE_COMPAT_COMPOSITOR_FRAME_CALLBACKS_H

#include <functional>
#include <vector>

namespace hermes {
namespace node_compat {

/// One `requestAnimationFrame` registration, native or JavaScript.
///
/// `fn` and `user` are **opaque** here: this library knows nothing about
/// Node-API or the `hnui` types. For a native callback `fn` is an
/// `hnui_frame_cb` and `user` its argument; for a JavaScript one `fn` is a
/// `napi_ref` holding the function. The caller owns what they mean -- and,
/// for a `napi_ref`, owns releasing it whenever an entry leaves the list:
/// taken by next(), removed by cancel(), or handed back by releaseAll().
struct FrameCallback {
  enum class Kind { kNative, kJs };
  int id = 0;
  Kind kind = Kind::kNative;
  void *fn = nullptr;
  void *user = nullptr;
};

/// The frame callbacks of one window, native and JavaScript in **one**
/// registration-ordered list with one id space (Ruling 57). Two lists with
/// two counters cannot express "run in registration order among them"
/// (spec: The native C API), which is why this replaced them.
///
/// A tick is a **batch**: beginBatch() takes everything registered so far,
/// and next() hands it out in order. Anything registered after
/// beginBatch() -- from inside a callback, or from a microtask or
/// `nextTick` run at a checkpoint between callbacks -- belongs to the next
/// batch, which is the browser rule (spec: Frames).
///
/// Threading: owner thread only. No lock, because only the thread that
/// owns the window ever registers, cancels or runs a frame callback.
class FrameCallbackList {
 public:
  /// Register a callback for the next batch. Ids start at 1 and are never
  /// reused, across both kinds, so 0 stays free to mean "no id".
  int add(FrameCallback::Kind kind, void *fn, void *user);

  /// True if the callback had not run yet -- waiting for the next batch, or
  /// later in the batch now running. \p removed lets the caller release it.
  ///
  /// False, with \p removed untouched, for an id that already ran, was
  /// already cancelled, or was never issued -- and for the callback that is
  /// executing right now, which next() has already taken from the batch.
  bool cancel(int id, FrameCallback *removed);

  /// Start a batch: everything registered so far. A batch not drained to
  /// the end -- which only the fatal-exception path leaves, and that path
  /// does not come back -- keeps its unrun entries in front of the new
  /// ones, so nothing is lost and order is kept.
  void beginBatch();

  /// The next uncancelled callback of the batch, removed from the list.
  /// False when the batch is exhausted.
  bool next(FrameCallback *out);

  /// Anything registered for a later batch.
  bool pending() const;

  /// Remove every entry, pending or still unrun in the current batch,
  /// handing each to \p release in order. For whoever destroys the window:
  /// it must run while the `napi_env` is alive, so that the JavaScript
  /// entries' references can be deleted.
  void releaseAll(const std::function<void(const FrameCallback &)> &release);

 private:
  struct Entry {
    FrameCallback callback;
    /// Cancelled while in the running batch. Marked rather than erased, so
    /// that next()'s position in the batch never moves under it.
    bool cancelled = false;
  };

  /// Registered for the next batch.
  std::vector<Entry> pending_;
  /// The batch now running; `batch_[batchPos_]` is the next to hand out.
  std::vector<Entry> batch_;
  size_t batchPos_ = 0;
  int nextId_ = 1;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_FRAME_CALLBACKS_H
