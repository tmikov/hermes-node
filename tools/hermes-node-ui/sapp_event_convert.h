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
bool convertSappEvent(
    const sapp_event &in,
    const PlatformSource &src,
    UiEvent *out);

/// The KeyboardEvent.code name for one of our key codes, or nullptr.
const char *domCodeForKey(int keyCode);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_SAPP_EVENT_CONVERT_H
