/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "x11_readback.h"

#include <sokol_app.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace hermes {
namespace node_compat {

namespace {

bool haveWindow(Display *&display, Window &window) {
  display = static_cast<Display *>(const_cast<void *>(sapp_x11_get_display()));
  window = reinterpret_cast<Window>(sapp_x11_get_window());
  return display != nullptr && window != 0;
}

} // namespace

bool uiX11FetchWindowName(std::string &out) {
  Display *display = nullptr;
  Window window = 0;
  if (!haveWindow(display, window))
    return false;
  char *name = nullptr;
  if (!XFetchName(display, window, &name) || name == nullptr)
    return false;
  out = name;
  XFree(name);
  return true;
}

bool uiX11OwnsClipboard() {
  Display *display = nullptr;
  Window window = 0;
  if (!haveWindow(display, window))
    return false;
  const Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
  return XGetSelectionOwner(display, clipboard) == window;
}

bool uiX11FetchIconHead(int &width, int &height, uint32_t &firstPixel) {
  Display *display = nullptr;
  Window window = 0;
  if (!haveWindow(display, window))
    return false;
  const Atom property = XInternAtom(display, "_NET_WM_ICON", True);
  if (property == None)
    return false;
  Atom type = None;
  int format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *data = nullptr;
  // Three longs: width, height, first pixel. Format 32 is delivered as
  // `long` whatever its width on the wire.
  if (XGetWindowProperty(
          display,
          window,
          property,
          0,
          3,
          False,
          XA_CARDINAL,
          &type,
          &format,
          &count,
          &remaining,
          &data) != Success ||
      data == nullptr) {
    return false;
  }
  bool ok = false;
  if (type == XA_CARDINAL && format == 32 && count >= 3) {
    const long *longs = reinterpret_cast<const long *>(data);
    width = static_cast<int>(longs[0]);
    height = static_cast<int>(longs[1]);
    firstPixel = static_cast<uint32_t>(longs[2]);
    ok = true;
  }
  XFree(data);
  return ok;
}

} // namespace node_compat
} // namespace hermes
