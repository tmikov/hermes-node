/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_X11_CLIPBOARD_H
#define HERMES_NODE_COMPAT_UI_X11_CLIPBOARD_H

#include <hermes/node-compat/compositor/channels.h>

#include <cstddef>

namespace hermes {
namespace node_compat {

/// Read the X11 `CLIPBOARD` selection as UTF-8, with a status.
///
/// **This replaces `sapp_get_clipboard_string()` on X11** (Ruling 55).
/// Sokol's getter returns its previous buffer on a timeout, on an INCR
/// reply and on an oversize property, so a failed read there cannot be told
/// from a successful read of stale text. Here every path builds its result
/// from this call's own reply, or from nothing: **no path returns text from
/// an earlier read.**
///
/// - Our own window owns the selection: `sapp_get_clipboard_string()`,
///   which is correct in exactly this case (`sokol_app.h:13221`) -- Sokol's
///   buffer *is* the selection's contents, and it is what Sokol hands any
///   other client that asks.
/// - Nothing owns it: kUnavailable, without a round trip to an owner.
/// - Otherwise this fetches a fresh server timestamp (a zero-length append
///   to a scratch property of our window, whose PropertyNotify carries the
///   server's time), converts the selection to `UTF8_STRING` with that
///   timestamp, and waits up to \p timeoutMs in total (kTimeout) for the
///   SelectionNotify that echoes it. **That timestamp is what pairs a reply
///   with this read**: a SelectionNotify for CLIPBOARD with any other time
///   answers an earlier, abandoned conversion, and is removed from the queue
///   and discarded (traced) while the wait continues. A `None` property is
///   kUnavailable; a property of type `INCR`, or one over \p maxBytes, is
///   kTooLarge; any other type, a reply naming a property this read did not
///   ask for, or a failed property read, is kError.
///
/// **Blocks the calling thread while it waits**, as Sokol's own read does;
/// the spec accepts that, and the caller bounds \p timeoutMs. The wait
/// removes from Xlib's queue only our scratch property's PropertyNotify and
/// CLIPBOARD SelectionNotifys sent to our window (`XCheckIfEvent`); every
/// other event -- an XDnD drop's SelectionNotify above all -- stays queued,
/// in order, for Sokol's own loop.
///
/// Platform thread only: \p display is Sokol's connection, which is not
/// thread-safe. \p display and \p window are `Display *` and `Window`,
/// passed as `void *` and `unsigned long` so this header stays free of
/// `<X11/Xlib.h>` and its macros (`None`, `Bool`, `Status`), for the reason
/// `x11_readback.h` gives. Linux (X11) only, built under that file's
/// condition.
ClipboardResult x11ReadClipboard(
    void *display,
    unsigned long window,
    int timeoutMs,
    size_t maxBytes);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_X11_CLIPBOARD_H
