/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_EVENT_INJECTOR_H
#define HERMES_NODE_COMPAT_UI_EVENT_INJECTOR_H

#include <hermes/node-compat/compositor/channels.h>

// Declarations only, as in sapp_event_convert.h: the injector stores a
// sapp_event and calls no sapp_* function.
#include <sokol_app.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// A synthetic platform event, together with the platform state a real one
/// would have been converted against.
///
/// **The converter's input, not its output** (Ruling 48, discharged here).
/// An injected event goes through `convertSappEvent()` and everything after
/// it exactly as a real one does; only what the converter would otherwise
/// fetch from the platform is supplied by the test instead -- the clipboard
/// for a paste, the paths for a drop, and the window state, each when
/// present.
struct InjectedPlatformEvent {
  sapp_event event{};
  /// A paste reads this instead of the real clipboard.
  bool hasClipboard = false;
  std::string clipboard;
  /// A drop reports these instead of asking Sokol, whose drop buffer holds
  /// nothing for an event Sokol did not produce.
  std::vector<std::string> paths;
  /// Converted against this instead of the tracked state.
  bool hasWindow = false;
  WindowState window;
};

/// Queues injected platform events for the platform thread to take, once
/// per frame, in injection order.
///
/// Threading: inject() from any thread (the JS thread, in practice);
/// takeNext() from the platform thread.
class PlatformEventInjector {
 public:
  PlatformEventInjector() = default;

  PlatformEventInjector(const PlatformEventInjector &) = delete;
  PlatformEventInjector &operator=(const PlatformEventInjector &) = delete;

  void inject(InjectedPlatformEvent event);

  /// Pop the oldest injected event into \p out.
  /// \return false, leaving \p out alone, if nothing is queued.
  bool takeNext(InjectedPlatformEvent *out);

 private:
  std::mutex mutex_;
  std::deque<InjectedPlatformEvent> queue_;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_EVENT_INJECTOR_H
