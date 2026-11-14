/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_HNUI_EVENT_CONVERT_H
#define HERMES_NODE_COMPAT_COMPOSITOR_HNUI_EVENT_CONVERT_H

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/ui/hnui_abi.h>

#include <vector>

namespace hermes {
namespace node_compat {

/// Converts one internal `UiEvent` into the public, flat `hnui_event` an
/// `hnui_event_cb` receives.
///
/// `UiEvent` owns its variable-length payloads (a `std::string` for text, a
/// `std::vector<std::string>` for dropped paths); `hnui_event` cannot, since
/// it crosses the C ABI boundary. So `hnui_event::text` and `::paths`
/// **borrow** directly from the source `UiEvent`'s own storage rather than
/// copying it -- for `text` that is simply `source.text.c_str()`, but
/// `paths` needs an array of `const char *`, which does not exist inside a
/// `vector<std::string>` and has to be built; this object is where that
/// array lives.
///
/// **Lifetime.** The event returned by `event()` borrows from two places at
/// once: its scalar-adjacent string data from the source `UiEvent`, and its
/// `paths` array itself from this converter object. Both must outlive every
/// use of `event()` -- in practice: construct one of these immediately
/// before invoking a native `hnui_event_cb`, keep both the source `UiEvent`
/// and this converter alive for exactly that one call, and let both go out
/// of scope right after. Neither may be retained past the callback
/// returning.
///
/// **"Alive" is not sufficient -- the source `UiEvent` needs ADDRESS
/// STABILITY for the whole borrow, not merely liveness.** `event_.text`
/// aliases `source.text`'s own buffer, and for a short string that buffer
/// is almost certainly inside the `UiEvent` object itself (small-string
/// optimization), not on the heap -- so the borrow is invalidated the
/// moment the `UiEvent` object is *moved*, even if some `UiEvent` with the
/// same contents is technically still "alive" afterwards. This is not a
/// hypothetical: `InputQueue::drain()` (`channels.h`) appends into a
/// caller-reused `std::vector<UiEvent>` and documents exactly that
/// contract. A dispatch loop iterating that vector where invoking one
/// listener triggers a second `drain()` call on the SAME buffer
/// reallocates it, moving every already-queued `UiEvent` -- and a
/// converter built over element *i* before that reallocation is left
/// holding a `text` pointer into freed small-string bytes. Nothing in the
/// type system objects; a release-build addon gets silently wrong bytes,
/// not a crash. The caller of this converter must therefore ensure: the
/// source `UiEvent` is not moved, erased, or reallocated-by-container for
/// the duration of the borrow, and the dispatch loop that owns the
/// `UiEvent` vector does not append to (or otherwise reallocate) that same
/// vector while a callback built from one of its elements is still
/// running.
class HnuiEventConverter {
 public:
  explicit HnuiEventConverter(const UiEvent &source);

  HnuiEventConverter(const HnuiEventConverter &) = delete;
  HnuiEventConverter &operator=(const HnuiEventConverter &) = delete;

  const hnui_event &event() const {
    return event_;
  }

 private:
  hnui_event event_{};
  /// Backing storage for `event_.paths`. Empty (and `event_.paths` left
  /// NULL) for every event kind but `kFileDrop`.
  std::vector<const char *> pathPointers_;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_HNUI_EVENT_CONVERT_H
