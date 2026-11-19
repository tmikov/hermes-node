/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_PLATFORM_SERVICES_H
#define HERMES_NODE_COMPAT_UI_PLATFORM_SERVICES_H

#include <hermes/node-compat/compositor/channels.h>

#include <cstddef>
#include <string>

namespace hermes {
namespace node_compat {

/// What `ui_platform.cpp` needs from the window system beyond what
/// `sokol_app` gives it: the clipboard, both directions, and the read-backs
/// the trace prints after a command, so that a trace line records what the
/// window system holds rather than what was requested.
///
/// One implementation per platform, chosen at link time:
/// `platform_services_x11.cpp` on Linux and, with Task 16e,
/// `platform_services_mac.mm` on macOS. `ui_platform.cpp` has no platform
/// conditional of its own.
///
/// **Platform thread only, every function** -- the process main thread,
/// which owns `sokol_app`. On macOS that is also AppKit's rule, and a
/// wrong-thread AppKit call usually works, which is why it is stated here
/// rather than left to be discovered.

/// Read the clipboard now, waiting at most \p timeoutMs for another
/// client's reply, refusing (`kTooLarge`) text longer than \p maxBytes.
///
/// Never returns text from an earlier read: on X11 this is our own read
/// (Ruling 55, `x11_clipboard.h`) rather than `sapp_get_clipboard_string()`,
/// whose failures return its previous buffer.
ClipboardResult uiPlatformReadClipboard(int timeoutMs, size_t maxBytes);

/// Make \p utf8 the clipboard's contents.
void uiPlatformWriteClipboard(const std::string &utf8);

/// The text after "command clipboard: ", read back after a write of
/// \p bytesWritten bytes. X11: `owns_selection=<0|1> bytes=<n>`, ownership
/// asked of the server.
std::string uiPlatformDescribeClipboardWrite(size_t bytesWritten);

/// The window's title as the window system holds it.
///
/// \return false if the platform cannot read the title back.
bool uiPlatformWindowTitle(std::string &out);

/// The text after "command icon: ", read back after `sapp_set_icon()`.
/// X11: `_NET_WM_ICON <w>x<h> first_pixel=0x<rrggbbaa>`, or
/// `no _NET_WM_ICON readback`.
std::string uiPlatformDescribeIcon();

/// Before `sapp_run()`, after `CompositorBackend::selectContextRoute()`
/// succeeded. The one point at which the platform can act before Sokol
/// creates its application object (macOS: install the `NSApplication`
/// subclass). X11: nothing.
void uiPlatformBeforeRun();

/// In `init_cb`, after `CompositorBackend::createSharedContext()`
/// succeeded: the window exists and the backend is set up. X11: nothing.
void uiPlatformAfterInit();

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_PLATFORM_SERVICES_H
