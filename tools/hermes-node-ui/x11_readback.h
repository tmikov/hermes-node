/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_X11_READBACK_H
#define HERMES_NODE_COMPAT_UI_X11_READBACK_H

#include <cstdint>
#include <string>

namespace hermes {
namespace node_compat {

/// Reads window properties back from the X server, so that the trace of a
/// platform command can print what the server holds rather than what was
/// asked for. **Called only when tracing is enabled** (`uiTracingEnabled()`)
/// -- each call is a synchronous round trip -- and nothing here affects
/// behaviour or is a place to fix a problem.
///
/// Linux (X11) only. Built under the same condition as the GL backend that
/// provides the X11 libraries (`UNIX AND NOT APPLE`), and a separate
/// translation unit so that `<X11/Xlib.h>`'s macros (`None`, `Bool`,
/// `Status`...) never meet `sokol_app.h` or NAPI.
///
/// Platform thread only: it uses Sokol's own display connection, which is
/// not thread-safe, and it round-trips to the server, so the answer is
/// ordered after every request Sokol already sent on that connection.

/// The window's `WM_NAME`. False if there is no X window or no name.
bool uiX11FetchWindowName(std::string &out);

/// Whether this window currently owns the CLIPBOARD selection. Asked of the
/// server directly (`XGetSelectionOwner`) instead of through
/// `sapp_get_clipboard_string()`, which when the window is NOT the owner
/// starts a selection conversion that blocks the thread for up to 100 ms
/// and overwrites Sokol's clipboard buffer with another client's text.
bool uiX11OwnsClipboard();

/// The first image of `_NET_WM_ICON`: its size and first pixel (ARGB, as X
/// stores it). False if the property is absent or empty.
bool uiX11FetchIconHead(int &width, int &height, uint32_t &firstPixel);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_X11_READBACK_H
