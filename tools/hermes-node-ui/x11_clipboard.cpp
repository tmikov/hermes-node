/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "x11_clipboard.h"

#include <ui_trace.h>

#include <sokol_app.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <poll.h>

#include <chrono>
#include <cstdio>

namespace hermes {
namespace node_compat {

namespace {

using Clock = std::chrono::steady_clock;

/// What the events this read waits for look like.
struct Match {
  Window window;
  /// CLIPBOARD, for a SelectionNotify.
  Atom selection;
  /// The scratch property, for a PropertyNotify.
  Atom scratch;
};

/// XCheckIfEvent predicate: a SelectionNotify sent to our window for
/// CLIPBOARD, from ANY conversion -- this read's or an earlier one's. Which
/// one it answers is decided afterwards, by its timestamp, so that a late
/// reply is taken off the queue and discarded instead of being left there to
/// match the next read. An XDnD drop's SelectionNotify names XdndSelection
/// and is left where it is, for Sokol's own loop.
Bool isClipboardNotify(Display *, XEvent *event, XPointer arg) {
  const Match *match = reinterpret_cast<const Match *>(arg);
  return event->type == SelectionNotify &&
          event->xselection.requestor == match->window &&
          event->xselection.selection == match->selection
      ? True
      : False;
}

/// XCheckIfEvent predicate: the PropertyNotify our timestamp append causes.
/// Consuming it starves nothing: Sokol's own PropertyNotify handler acts on
/// WM_STATE alone (sokol_app.h:13839-13853), and this atom is ours.
Bool isScratchNotify(Display *, XEvent *event, XPointer arg) {
  const Match *match = reinterpret_cast<const Match *>(arg);
  return event->type == PropertyNotify &&
          event->xproperty.window == match->window &&
          event->xproperty.atom == match->scratch &&
          event->xproperty.state == PropertyNewValue
      ? True
      : False;
}

/// Wait until \p predicate matches a queued event, removing only that one,
/// or until \p deadline. Each XCheckIfEvent reads whatever the connection
/// has, so the poll() only sleeps until more arrives.
bool waitForEvent(
    Display *display,
    Bool (*predicate)(Display *, XEvent *, XPointer),
    Match *match,
    Clock::time_point deadline,
    XEvent *event) {
  for (;;) {
    if (XCheckIfEvent(
            display, event, predicate, reinterpret_cast<XPointer>(match)))
      return true;
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero())
      return false;
    // Rounded up, so a sub-millisecond remainder is a short wait rather
    // than a busy spin.
    const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            remaining + std::chrono::microseconds(999))
                            .count();
    pollfd fd = {ConnectionNumber(display), POLLIN, 0};
    poll(&fd, 1, static_cast<int>(waitMs));
  }
}

/// The server time of the previous conversion. Platform thread only.
Time gLastConversionTime = CurrentTime;

/// A fresh server timestamp, distinct from the previous conversion's, or
/// false at \p deadline.
///
/// Obtained the ICCCM way: a zero-length append to a property of our own
/// window changes nothing but produces a PropertyNotify carrying the
/// server's time. It must differ from the last conversion's because that
/// timestamp is what identifies a reply: the owner echoes it in its
/// SelectionNotify. Server time is in milliseconds, so two reads in the
/// same millisecond would otherwise share one; another append a moment
/// later gets a later time.
bool fetchServerTime(
    Display *display,
    Match *match,
    Clock::time_point deadline,
    Time *out) {
  for (;;) {
    XChangeProperty(
        display,
        match->window,
        match->scratch,
        XA_INTEGER,
        8,
        PropModeAppend,
        nullptr,
        0);
    XFlush(display);
    XEvent event;
    if (!waitForEvent(display, isScratchNotify, match, deadline, &event))
      return false;
    if (event.xproperty.time != gLastConversionTime) {
      *out = event.xproperty.time;
      return true;
    }
  }
}

/// The property a conversion is asked to land in, rotated over a few names.
///
/// This is NOT what pairs a reply with its read -- the timestamp is. It
/// narrows a different hazard: an owner answering an abandoned conversion
/// late writes the property that conversion named, and if that were the
/// property the current read is about to read, a late WRITE could land
/// between the current owner's write and our XGetWindowProperty. With
/// rotation that needs a reply exactly a multiple of 8 foreign reads late
/// and racing the current owner's write; it is narrowed, not closed.
/// Rotated rather than unique per read because X never frees an interned
/// atom. Platform thread only.
Atom nextProperty(Display *display) {
  static unsigned counter = 0;
  char name[32];
  std::snprintf(name, sizeof(name), "HNUI_CLIPBOARD_%u", counter++ % 8u);
  return XInternAtom(display, name, False);
}

ClipboardResult status(ClipboardStatus s) {
  return ClipboardResult{s, std::string()};
}

} // namespace

ClipboardResult x11ReadClipboard(
    void *displayPtr,
    unsigned long windowId,
    int timeoutMs,
    size_t maxBytes) {
  Display *display = static_cast<Display *>(displayPtr);
  const Window window = static_cast<Window>(windowId);
  if (display == nullptr || window == 0) {
    uiTrace("clipboard read: no X window");
    return status(ClipboardStatus::kError);
  }

  const Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
  const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
  const Atom incr = XInternAtom(display, "INCR", False);

  const Window owner = XGetSelectionOwner(display, clipboard);
  if (owner == window) {
    // Sokol's buffer is the selection's contents in exactly this case: it is
    // what Sokol hands any other client that asks (sokol_app.h:13221).
    const char *text = sapp_get_clipboard_string();
    uiTrace("clipboard read: own selection");
    if (text == nullptr)
      return status(ClipboardStatus::kError);
    return ClipboardResult{ClipboardStatus::kOk, text};
  }
  if (owner == None) {
    uiTrace("clipboard read: no owner");
    return status(ClipboardStatus::kUnavailable);
  }

  const Clock::time_point deadline =
      Clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
  Match match{window, clipboard, XInternAtom(display, "HNUI_TIMESTAMP", False)};

  // The conversion's timestamp, which the owner echoes in its reply (ICCCM
  // 2.2) and which is how its reply is told apart from a late one to an
  // earlier read. Never CurrentTime: every reply would then echo the same
  // zero and none could be told apart.
  Time stamp = CurrentTime;
  if (!fetchServerTime(display, &match, deadline, &stamp)) {
    uiTrace(
        "clipboard read: no server timestamp within %d ms; timed out",
        timeoutMs);
    return status(ClipboardStatus::kTimeout);
  }
  gLastConversionTime = stamp;

  const Atom property = nextProperty(display);
  // Whatever an earlier, abandoned conversion may have left in it goes
  // first, so nothing below can read it.
  XDeleteProperty(display, window, property);
  XConvertSelection(display, clipboard, utf8, property, window, stamp);
  XFlush(display);

  XEvent event;
  for (;;) {
    if (!waitForEvent(display, isClipboardNotify, &match, deadline, &event)) {
      uiTrace(
          "clipboard read: no SelectionNotify within %d ms; timed out",
          timeoutMs);
      return status(ClipboardStatus::kTimeout);
    }
    if (event.xselection.time == stamp)
      break;
    // A reply to an earlier conversion, which timed out: taken off the
    // queue so it cannot answer this read or the next one.
    uiTrace(
        "clipboard read: discarded a late SelectionNotify (time %lu, this "
        "read's %lu)",
        static_cast<unsigned long>(event.xselection.time),
        static_cast<unsigned long>(stamp));
  }

  if (event.xselection.property != None &&
      event.xselection.property != property) {
    uiTrace("clipboard read: reply names a property this read did not ask for");
    return status(ClipboardStatus::kError);
  }
  if (event.xselection.property == None) {
    uiTrace("clipboard read: owner refused the conversion");
    return status(ClipboardStatus::kUnavailable);
  }

  // One 32-bit unit more than the limit allows, so an oversize property is
  // seen as oversize (bytes_after > 0, or more items than maxBytes) without
  // ever transferring the whole of it.
  const long lengthUnits = static_cast<long>(maxBytes / 4 + 1);
  Atom type = None;
  int format = 0;
  unsigned long count = 0;
  unsigned long bytesAfter = 0;
  unsigned char *data = nullptr;
  const int got = XGetWindowProperty(
      display,
      window,
      property,
      0,
      lengthUnits,
      False,
      AnyPropertyType,
      &type,
      &format,
      &count,
      &bytesAfter,
      &data);
  if (got != Success) {
    if (data != nullptr)
      XFree(data);
    uiTrace("clipboard read: XGetWindowProperty failed");
    return status(ClipboardStatus::kError);
  }

  ClipboardResult result;
  if (type == incr) {
    // The owner wants an incremental transfer. Not followed: the property is
    // left in place rather than deleted, because deleting it is what tells
    // the owner to start sending chunks nobody would read.
    uiTrace("clipboard read: INCR reply; too large");
    result = status(ClipboardStatus::kTooLarge);
  } else {
    if (type != utf8 || format != 8) {
      uiTrace("clipboard read: reply is not UTF8_STRING");
      result = status(ClipboardStatus::kError);
    } else if (bytesAfter > 0 || count > maxBytes) {
      uiTrace("clipboard read: reply over %zu bytes; too large", maxBytes);
      result = status(ClipboardStatus::kTooLarge);
    } else {
      uiTrace("clipboard read: %lu bytes from the owner", count);
      result.status = ClipboardStatus::kOk;
      result.text.assign(reinterpret_cast<const char *>(data), count);
    }
    // ICCCM: the requestor deletes the property once it has read it.
    XDeleteProperty(display, window, property);
  }
  if (data != nullptr)
    XFree(data);
  return result;
}

} // namespace node_compat
} // namespace hermes
