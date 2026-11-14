/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * A second X client that sends REAL X events to a hermes-node-ui window,
 * for test/ui-real-events.js. Every other window test injects platform
 * events below Sokol; this one is what makes Sokol itself, and our
 * `event_cb`, produce them.
 *
 *   x11_window_driver <title>
 *       Find the top-level window named exactly <title>, print
 *       `READY <w>x<h>`, then take one command per line on stdin:
 *         key <keysym>   a KeyPress and KeyRelease (XSendEvent)
 *         resize <w> <h> XResizeWindow
 *         close          a WM_PROTOCOLS / WM_DELETE_WINDOW client message,
 *                        which is what a window manager's close button
 *                        sends
 *         state          print `STATE viewable=<0|1> <w>x<h>`
 *       Each command but `state` answers `SENT <command>` once the server
 *       has processed it (XSync). Independently of any command, the moment
 *       the window is unmapped or destroyed this prints `LOST unmapped` or
 *       `LOST destroyed`, so a test waiting for something the window
 *       should still do sees its disappearance as an answer rather than as
 *       silence. Exits at EOF on stdin.
 *
 *   x11_window_driver --close <title>
 *       Wait for the window, send it one close request, and exit.
 *
 * Bare Xvfb has no window manager, which is what makes this simple:
 * XResizeWindow takes effect directly rather than being redirected, the
 * window is a direct child of the root, and the close request is a client
 * message this sends itself. A synthetic KeyPress is processed by Sokol
 * like a real one (it does not check `send_event`).
 *
 * The window is found by its title, which the test makes unique, and
 * waited for through MapNotify on the root rather than by polling: the
 * root's SubstructureNotify is selected BEFORE the tree is searched, so a
 * window mapped between the search and the wait still produces an event.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Atom gWmProtocols;
static Atom gWmDeleteWindow;

/** The viewable top-level window named exactly \p title, or 0. */
static Window findViewable(Display *d, const char *title) {
  Window root, parent, *kids = NULL;
  unsigned n = 0, i;
  Window found = 0;
  if (!XQueryTree(d, DefaultRootWindow(d), &root, &parent, &kids, &n))
    return 0;
  for (i = 0; i < n && found == 0; ++i) {
    char *name = NULL;
    XWindowAttributes a;
    if (XFetchName(d, kids[i], &name) && name != NULL) {
      if (strcmp(name, title) == 0 && XGetWindowAttributes(d, kids[i], &a) &&
          a.map_state == IsViewable) {
        found = kids[i];
      }
      XFree(name);
    }
  }
  if (kids != NULL)
    XFree(kids);
  return found;
}

static Window waitForWindow(Display *d, const char *title) {
  Window w;
  XSelectInput(d, DefaultRootWindow(d), SubstructureNotifyMask);
  XSync(d, False);
  while ((w = findViewable(d, title)) == 0) {
    XEvent ev;
    /* Blocks until some top-level window changes; a MapNotify for ours,
       or the property change that names it, is among them. */
    XNextEvent(d, &ev);
  }
  XSelectInput(d, DefaultRootWindow(d), NoEventMask);
  /* Watched from here on, for `LOST`. Several clients may each select
     StructureNotify on the same window. */
  XSelectInput(d, w, StructureNotifyMask);
  XSync(d, False);
  return w;
}

static void sendClose(Display *d, Window w) {
  XEvent e;
  memset(&e, 0, sizeof(e));
  e.xclient.type = ClientMessage;
  e.xclient.window = w;
  e.xclient.message_type = gWmProtocols;
  e.xclient.format = 32;
  e.xclient.data.l[0] = (long)gWmDeleteWindow;
  e.xclient.data.l[1] = CurrentTime;
  XSendEvent(d, w, False, NoEventMask, &e);
  XSync(d, False);
}

static int sendKey(Display *d, Window w, const char *keysym) {
  KeySym sym = XStringToKeysym(keysym);
  KeyCode code = sym == NoSymbol ? 0 : XKeysymToKeycode(d, sym);
  XEvent e;
  if (code == 0)
    return 0;
  memset(&e, 0, sizeof(e));
  e.xkey.type = KeyPress;
  e.xkey.display = d;
  e.xkey.window = w;
  e.xkey.root = DefaultRootWindow(d);
  e.xkey.subwindow = None;
  e.xkey.time = CurrentTime;
  e.xkey.x = e.xkey.y = 1;
  e.xkey.same_screen = True;
  e.xkey.keycode = code;
  e.xkey.state = 0;
  XSendEvent(d, w, True, KeyPressMask, &e);
  e.xkey.type = KeyRelease;
  XSendEvent(d, w, True, KeyReleaseMask, &e);
  XSync(d, False);
  return 1;
}

/* Xlib's default handler exits the process on any error, and a request
   naming the window after it is destroyed (`state` after a LOST) earns a
   BadWindow. The answers above already say what happened. */
static int ignoreErrors(Display *d, XErrorEvent *e) {
  (void)d;
  (void)e;
  return 0;
}

static void say(const char *line) {
  printf("%s\n", line);
  fflush(stdout);
}

/** Report the window's disappearance, once; true if it is gone. */
static int drainEvents(Display *d, Window w, int *lost) {
  while (XPending(d) > 0) {
    XEvent ev;
    XNextEvent(d, &ev);
    if (*lost)
      continue;
    if (ev.type == DestroyNotify && ev.xdestroywindow.window == w) {
      say("LOST destroyed");
      *lost = 1;
    } else if (ev.type == UnmapNotify && ev.xunmap.window == w) {
      say("LOST unmapped");
      *lost = 1;
    }
  }
  return *lost;
}

static void command(Display *d, Window w, char *line, int lost) {
  char verb[16];
  char arg[64];
  int width = 0, height = 0;
  line[strcspn(line, "\r\n")] = '\0';
  if (sscanf(line, "%15s", verb) != 1)
    return;
  if (strcmp(verb, "state") == 0) {
    XWindowAttributes a;
    char out[96];
    if (lost || !XGetWindowAttributes(d, w, &a)) {
      say("STATE viewable=0 0x0");
      return;
    }
    snprintf(out, sizeof(out), "STATE viewable=%d %dx%d",
             a.map_state == IsViewable ? 1 : 0, a.width, a.height);
    say(out);
  } else if (strcmp(verb, "key") == 0 && sscanf(line, "%*s %63s", arg) == 1) {
    say(sendKey(d, w, arg) ? "SENT key" : "ERROR unknown keysym");
  } else if (strcmp(verb, "resize") == 0 &&
             sscanf(line, "%*s %d %d", &width, &height) == 2 && width > 0 &&
             height > 0) {
    XResizeWindow(d, w, (unsigned)width, (unsigned)height);
    XSync(d, False);
    say("SENT resize");
  } else if (strcmp(verb, "close") == 0) {
    sendClose(d, w);
    say("SENT close");
  } else {
    say("ERROR unknown command");
  }
}

int main(int argc, char **argv) {
  const int oneShotClose = argc == 3 && strcmp(argv[1], "--close") == 0;
  const char *title = oneShotClose ? argv[2] : argc == 2 ? argv[1] : NULL;
  Display *d;
  Window w;
  XWindowAttributes a;
  char ready[64];
  char line[256];
  size_t used = 0;
  int lost = 0;

  if (title == NULL) {
    fprintf(stderr, "usage: x11_window_driver <title> | --close <title>\n");
    return 2;
  }
  d = XOpenDisplay(NULL);
  if (d == NULL) {
    fprintf(stderr, "x11_window_driver: cannot open the display\n");
    return 2;
  }
  XSetErrorHandler(ignoreErrors);
  gWmProtocols = XInternAtom(d, "WM_PROTOCOLS", False);
  gWmDeleteWindow = XInternAtom(d, "WM_DELETE_WINDOW", False);
  w = waitForWindow(d, title);

  if (oneShotClose) {
    sendClose(d, w);
    XCloseDisplay(d);
    return 0;
  }

  XGetWindowAttributes(d, w, &a);
  snprintf(ready, sizeof(ready), "READY %dx%d", a.width, a.height);
  say(ready);

  for (;;) {
    struct pollfd fds[2] = {
        {ConnectionNumber(d), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    char *nl;
    ssize_t got;
    drainEvents(d, w, &lost);
    if (poll(fds, 2, -1) < 0)
      continue;
    if ((fds[1].revents & (POLLIN | POLLHUP)) == 0)
      continue;
    got = read(STDIN_FILENO, line + used, sizeof(line) - 1 - used);
    if (got <= 0)
      break;
    used += (size_t)got;
    line[used] = '\0';
    while ((nl = strchr(line, '\n')) != NULL) {
      *nl = '\0';
      /* A command is answered after anything already queued, so a LOST
         that preceded it is reported before its answer. */
      drainEvents(d, w, &lost);
      command(d, w, line, lost);
      used -= (size_t)(nl + 1 - line);
      memmove(line, nl + 1, used + 1);
    }
    if (used == sizeof(line) - 1)
      used = 0; /* an over-long line: discard it */
  }
  XCloseDisplay(d);
  return 0;
}
