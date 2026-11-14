/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * A foreign owner of the X11 CLIPBOARD selection, for test/ui-clipboard.js.
 *
 *   x11_selection_owner text <s>   answers UTF8_STRING requests with <s>
 *   x11_selection_owner mute       takes the selection and never answers
 *   x11_selection_owner incr       answers with a property of type INCR
 *   x11_selection_owner late       answers each request only when the NEXT
 *                                  one arrives, with the text reply-<n>,
 *                                  n counting requests from 1 -- so the
 *                                  reply to read n reaches the requestor
 *                                  while read n+1 is in flight
 *   x11_selection_owner late-refuse  the same, but each late answer is a
 *                                  refusal (property None), which names
 *                                  no property a reader could tell apart
 *
 * Takes ownership through an unmapped 1x1 window, prints READY on stdout,
 * and serves requests until stdin reaches EOF. It then releases the
 * selection and waits for the server to confirm (XSync) before exiting, so
 * once the test has seen this process exit, the selection has no owner.
 * `incr` stops at the type: the reader under test never asks for more.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Answer \p req with \p text as UTF8_STRING, echoing its time; a NULL
   \p text refuses the conversion. */
static void answer(Display *d, const XSelectionRequestEvent *req, Atom utf8,
                   const char *text) {
  XSelectionEvent reply;
  memset(&reply, 0, sizeof(reply));
  reply.type = SelectionNotify;
  reply.requestor = req->requestor;
  reply.selection = req->selection;
  reply.target = req->target;
  reply.property = req->property;
  reply.time = req->time;
  if (text != NULL && req->target == utf8) {
    XChangeProperty(d, req->requestor, req->property, utf8, 8,
                    PropModeReplace, (const unsigned char *)text,
                    (int)strlen(text));
  } else {
    reply.property = None;
  }
  XSendEvent(d, req->requestor, False, 0, (XEvent *)&reply);
  XFlush(d);
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "";
  const char *text = argc > 2 ? argv[2] : "";
  Display *d;
  Window w;
  Atom clipboard, utf8, incr;
  XSelectionRequestEvent held;
  int haveHeld = 0;
  int requests = 0;

  if (strcmp(mode, "text") != 0 && strcmp(mode, "mute") != 0 &&
      strcmp(mode, "incr") != 0 && strcmp(mode, "late") != 0 &&
      strcmp(mode, "late-refuse") != 0) {
    fprintf(stderr, "usage: x11_selection_owner text <s> | mute | incr | "
                    "late | late-refuse\n");
    return 2;
  }
  d = XOpenDisplay(NULL);
  if (d == NULL) {
    fprintf(stderr, "x11_selection_owner: cannot open the display\n");
    return 2;
  }
  w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
  clipboard = XInternAtom(d, "CLIPBOARD", False);
  utf8 = XInternAtom(d, "UTF8_STRING", False);
  incr = XInternAtom(d, "INCR", False);
  XSetSelectionOwner(d, clipboard, w, CurrentTime);
  if (XGetSelectionOwner(d, clipboard) != w) {
    fprintf(stderr, "x11_selection_owner: could not take CLIPBOARD\n");
    return 2;
  }
  printf("READY\n");
  fflush(stdout);

  for (;;) {
    struct pollfd fds[2] = {
        {ConnectionNumber(d), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    char buf[64];
    while (XPending(d) > 0) {
      XEvent ev;
      XSelectionRequestEvent *req;
      XSelectionEvent reply;
      XNextEvent(d, &ev);
      if (ev.type != SelectionRequest || strcmp(mode, "mute") == 0)
        continue;
      req = &ev.xselectionrequest;
      ++requests;
      if (strncmp(mode, "late", 4) == 0) {
        if (haveHeld) {
          char late[32];
          snprintf(late, sizeof(late), "reply-%d", requests - 1);
          answer(d, &held, utf8, strcmp(mode, "late") == 0 ? late : NULL);
        }
        held = *req;
        haveHeld = 1;
        continue;
      }
      if (strcmp(mode, "text") == 0) {
        answer(d, req, utf8, text);
        continue;
      }
      memset(&reply, 0, sizeof(reply));
      reply.type = SelectionNotify;
      reply.requestor = req->requestor;
      reply.selection = req->selection;
      reply.target = req->target;
      reply.property = req->property;
      reply.time = req->time;
      {
        /* incr: the INCR protocol's opening move. The property's type is
           INCR and its value a lower bound on the size. */
        long lowerBound = 1L << 24;
        XChangeProperty(d, req->requestor, req->property, incr, 32,
                        PropModeReplace, (unsigned char *)&lowerBound, 1);
      }
      XSendEvent(d, req->requestor, False, 0, (XEvent *)&reply);
      XFlush(d);
    }
    if (poll(fds, 2, -1) < 0)
      continue;
    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0 &&
        read(STDIN_FILENO, buf, sizeof(buf)) <= 0)
      break;
  }

  XSetSelectionOwner(d, clipboard, None, CurrentTime);
  XSync(d, False);
  XCloseDisplay(d);
  return 0;
}
