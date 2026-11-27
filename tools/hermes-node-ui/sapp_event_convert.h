/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_SAPP_EVENT_CONVERT_H
#define HERMES_NODE_COMPAT_UI_SAPP_EVENT_CONVERT_H

#include <hermes/node-compat/compositor/channels.h>

// Declarations only. Nothing here defines SOKOL_APP_IMPL, and the converter
// calls no sapp_* function, so a unit test can link it with no window and no
// platform.
#include <sokol_app.h>

#include <functional>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Everything the converter needs that depends on the platform, passed in
/// rather than fetched, so that the conversion is a pure function of its
/// arguments.
struct PlatformSource {
  /// The caller's tracked state as of this event. The converter copies it
  /// into the result and applies what the event itself changes.
  WindowState window;
  /// False means the read failed; the paste is then not forwarded.
  std::function<bool(std::string &)> readClipboard;
  /// Called for FILES_DROPPED only.
  std::function<std::vector<std::string>()> droppedPaths;
};

/// Convert one Sokol event into a UiEvent.
///
/// Positions in \p in are framebuffer pixels; the result's are logical, so
/// they are divided by `src.window.dpiScale`. Scroll distances are not
/// positions and are carried as they are.
///
/// \return false for an event type this layer does not forward, and for a
///   paste whose clipboard read failed. \p out is unspecified then.
///
/// Stateless, so a UTF-16 surrogate in a CHAR event becomes U+FFFD here;
/// SappEventConverter below pairs them, and is what the host calls.
bool convertSappEvent(
    const sapp_event &in,
    const PlatformSource &src,
    UiEvent *out);

/// convertSappEvent() with the one piece of state it cannot do without:
/// the pairing of UTF-16 surrogates into one character.
///
/// Sokol's macOS backend sends each UTF-16 code unit of a key's characters
/// as a CHAR event of its own (`keyDown:` in sokol_app.h), so a character
/// outside the Basic Multilingual Plane arrives as a high surrogate followed
/// by a low one. X11 delivers whole code points and never takes the paths
/// below; an injected event can.
///
/// - A CHAR carrying a high surrogate (U+D800-U+DBFF) produces nothing and
///   is held.
/// - A CHAR carrying a low surrogate (U+DC00-U+DFFF) that follows a held
///   high one produces ONE text event with the combined code point. Its
///   modifiers and repeat flag are the low-surrogate event's.
/// - A held high surrogate followed by anything else is an orphan: it is
///   delivered as a text event of its own carrying U+FFFD, ahead of the
///   event that exposed it, which is then converted as usual. Delivered
///   rather than dropped, so that a keystroke the platform sent is never
///   silently lost, and as an event of its own because a text event
///   carries one character. A held surrogate waits for the next event,
///   however late; nothing flushes it on a timer.
/// - A lone low surrogate yields U+FFFD, as any non-scalar value does.
///
/// No text event ever carries an unpaired surrogate.
///
/// Not synchronized. The host's instance lives in ui_platform.cpp and is
/// touched only on the platform thread, which converts every event, real
/// and injected.
class SappEventConverter {
 public:
  /// Convert \p in, appending what it produces to \p out: nothing (a held
  /// high surrogate, or an event convertSappEvent() refuses), one event, or
  /// two (an orphan's U+FFFD, then \p in's own).
  void convert(
      const sapp_event &in,
      const PlatformSource &src,
      std::vector<UiEvent> &out);

 private:
  bool havePendingHigh_ = false;
  /// The held high surrogate's event, and the window state it arrived
  /// with, so an orphan is converted as it would have been at the time.
  sapp_event pendingHigh_{};
  WindowState pendingWindow_{};
};

/// The KeyboardEvent.code name for one of our key codes, or nullptr.
const char *domCodeForKey(int keyCode);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_SAPP_EVENT_CONVERT_H
