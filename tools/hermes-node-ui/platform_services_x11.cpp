/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The X11 implementation of `platform_services.h`. Platform thread only,
/// every function.
///
/// What used to be three `#ifdef HERMES_NODE_UI_HAVE_X11` blocks in
/// `ui_platform.cpp`, moved here unchanged in behaviour: the traces they
/// feed are byte-identical to what they were.

#include "platform_services.h"

#include "x11_clipboard.h"
#include "x11_readback.h"

#include <sokol_app.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace hermes {
namespace node_compat {

ClipboardResult uiPlatformReadClipboard(int timeoutMs, size_t maxBytes) {
  return x11ReadClipboard(
      const_cast<void *>(sapp_x11_get_display()),
      reinterpret_cast<unsigned long>(sapp_x11_get_window()),
      timeoutMs,
      maxBytes);
}

void uiPlatformWriteClipboard(const std::string &utf8) {
  // Sokol's setter takes the selection and answers other clients' requests
  // for it from its own buffer, which is all a write needs on X11. (The
  // READ is ours -- see uiPlatformReadClipboard -- because Sokol's getter
  // returns its previous buffer on failure; its setter has no such path.)
  sapp_set_clipboard_string(utf8.c_str());
}

std::string uiPlatformDescribeClipboardWrite(size_t bytesWritten) {
  // Ownership, asked of the server. sapp_get_clipboard_string() is not
  // used: when we are not the owner it runs a selection conversion that
  // stalls this thread and overwrites Sokol's buffer.
  char buffer[64];
  std::snprintf(
      buffer,
      sizeof buffer,
      "owns_selection=%d bytes=%zu",
      uiX11OwnsClipboard() ? 1 : 0,
      bytesWritten);
  return buffer;
}

bool uiPlatformWindowTitle(std::string &out) {
  return uiX11FetchWindowName(out);
}

std::string uiPlatformDescribeIcon() {
  int w = 0, h = 0;
  uint32_t first = 0;
  if (!uiX11FetchIconHead(w, h, first))
    return "no _NET_WM_ICON readback";
  char buffer[96];
  std::snprintf(
      buffer,
      sizeof buffer,
      "_NET_WM_ICON %dx%d first_pixel=0x%08" PRIx32,
      w,
      h,
      first);
  return buffer;
}

void uiPlatformBeforeRun() {
  // Nothing: on X11 sokol_app needs nothing installed before sapp_run().
}

void uiPlatformAfterInit() {
  // Nothing: on X11 the window is on screen and needs no further help.
}

} // namespace node_compat
} // namespace hermes
