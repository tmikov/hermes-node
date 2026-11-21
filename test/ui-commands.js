// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Platform commands, end to end: the JS thread pushes a command, the
// platform thread applies it with the matching sapp_* call, and the trace
// prints what it READ BACK afterwards -- sapp_is_fullscreen(),
// sapp_get_mouse_cursor(), sapp_mouse_shown(), sapp_mouse_locked(), the
// clipboard buffer, and for the title and the icon the X server's own copy
// of the property (XFetchName / _NET_WM_ICON on the window). A trace that
// printed the request would pass with the sapp_* call deleted.
//
// The commands are issued first, then a marker KEY_DOWN is injected and the
// program exits when it arrives. The platform thread drains commands before
// it drains the injector, and every command was pushed before the marker
// was injected, so the marker cannot overtake them: no sleeping and no
// timing.
//
// What Xvfb does and does not do (it has no window manager):
//
//  FULLSCREEN   sapp_is_fullscreen() flips with each toggle, because Sokol
//               records the new state itself before it asks the window
//               manager for anything. There is no window manager here to
//               refuse or honour the request, so this proves the command
//               arrived and that setFullscreen(false) after
//               setFullscreen(true) toggles back (it must compare, not
//               blindly toggle). It does not prove the window changed size.
//  POINTER LOCK sapp_mouse_locked() reports the flag Sokol keeps; Sokol
//               ignores XGrabPointer's result, so a refused grab would read
//               the same. What this asserts is what reads back.
//
// Mutations (each must turn a named line below red):
//   delete the sapp_set_window_title call        -> TITLE
//   map `pointer` to SAPP_MOUSECURSOR_IBEAM      -> CURSOR pointer
//   remove the no-window check in writeClipboard -> NO-WINDOW
//
// NOT verified: that the custom cursor image was actually bound.
// sapp_bind_mouse_cursor_image returns its argument whether or not binding
// worked, and the server's cursor is not read back, so the `custom cursor`
// line proves only that CUSTOM_0 was selected; deleting the bind call
// leaves this test green. The clipboard line reports that the server says
// this window owns the selection, not the text (reading the text would
// make Sokol run a selection conversion when we do not own it).
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js: redirections inside `sh -c`, so the
// trace file is the binary's own stderr whatever the display wrapper does,
// and `timeout` because lit has no per-test timeout (dz 01a0d797-e326). Ends
// with win.close(): closing is one of the window's commands, and this is the
// one test outside test/ui-close.js that ends that way. A win.close() that
// did nothing leaves the run at its `timeout`; one that ended the process
// with the wrong status fails CODE; one that entered CLOSING and returned
// fails TRACE (see the ending's lines below).
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %ui-backend > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='runtime returned' --check-prefixes=TRACE,TRACE-%ui-backend %s < %t.err

// CODE: EXITCODE 0

// CHECK: NO-WINDOW ok
// CHECK: GETTER ok
// CHECK: UNKNOWN-CURSOR ok
// CHECK: CLIPBOARD-TOO-BIG ok
// CHECK: MARKER ok
// CHECK: DONE

// The backend (`%ui-backend`) is the script's first argument and selects
// the `TRACE-<backend>` lines. What names an X11 or GL fact -- the
// fullscreen pair, `_NET_WM_ICON`, `owns_selection` -- is under TRACE-gl,
// and its macOS counterpart -- the dock tile's image, the pasteboard the
// test wrapper named -- under TRACE-metal;
// what every backend prints the same way (the title read-back, Sokol's own
// cursor, visibility and lock read-backs) stays under TRACE. On `gl`
// nothing is skipped. On `metal` the fullscreen pair is not issued: AppKit's
// toggleFullScreen: is an animated transition into a new Space, and what it
// does with a second toggle during the first is not something this test
// should depend on. Fullscreen is therefore untested on macOS: tracker
// issue 01a1fbb5-84af.
//
// Each line is the value READ BACK after the call. `command cursor N` is the
// hnui_cursor value pushed (default, arrow, ibeam, crosshair, pointer,
// ew-resize, ns-resize, nwse-resize, nesw-resize, move, not-allowed, in the
// order the test issues them); the number after `=` is Sokol's reading. Sokol's numbering:
// DEFAULT 0, ARROW 1, IBEAM 2, CROSSHAIR 3, POINTING_HAND 4, RESIZE_EW 5,
// RESIZE_NS 6, RESIZE_NWSE 7, RESIZE_NESW 8, RESIZE_ALL 9, NOT_ALLOWED 10,
// CUSTOM_0 11.
// TRACE: command title: readback='commands test title'
// TRACE-gl: command fullscreen: is_fullscreen=1
// TRACE-gl: command fullscreen: is_fullscreen=0
// TRACE-gl: command icon: _NET_WM_ICON 16x16 first_pixel=0xff0a141e
// TRACE-metal: command icon: dock tile 16x16
// TRACE: command cursor 0: sapp_mouse_cursor=0
// TRACE: command cursor 1: sapp_mouse_cursor=1
// TRACE: command cursor 2: sapp_mouse_cursor=2
// TRACE: command cursor 3: sapp_mouse_cursor=3
// TRACE: command cursor 4: sapp_mouse_cursor=4
// TRACE: command cursor 5: sapp_mouse_cursor=5
// TRACE: command cursor 6: sapp_mouse_cursor=6
// TRACE: command cursor 7: sapp_mouse_cursor=7
// TRACE: command cursor 8: sapp_mouse_cursor=8
// TRACE: command cursor 9: sapp_mouse_cursor=9
// TRACE: command cursor 10: sapp_mouse_cursor=10
// TRACE: command custom cursor: sapp_mouse_cursor=11
// TRACE: command show cursor: sapp_mouse_shown=0
// TRACE: command show cursor: sapp_mouse_shown=1
// TRACE: command lock pointer: sapp_mouse_locked=1
// TRACE: command lock pointer: sapp_mouse_locked=0
// TRACE-gl: command clipboard: owns_selection=1 bytes=14
// TRACE-metal: clipboard: pasteboard 'hnui-test-
// TRACE-metal: command clipboard: pasteboard_bytes=14 bytes=14
// The ending: win.close() enters CLOSING and then calls process.exit(). The
// implicit NOT on `runtime returned` is what catches a win.close() that
// enters CLOSING and returns: the released handle lets the loop drain, the
// runtime-return hook exits with status 0, and every line above still
// matches.
// TRACE: lifecycle: CLOSING (reason=close)
// TRACE: close: ending the process through process.exit()

'use strict';

const ui = require('hermes-node:ui');

// `gl` or `metal`, from the RUN line's %ui-backend.
const backend = process.argv[2];
if (backend !== 'gl' && backend !== 'metal') {
  console.log('BACKEND argument is not gl or metal: ' + backend);
  process.exit(2);
}

const SAPP_KEYCODE_F3 = 292;
const CURSORS = [
  'default', 'arrow', 'ibeam', 'crosshair', 'pointer', 'ew-resize',
  'ns-resize', 'nwse-resize', 'nesw-resize', 'move', 'not-allowed',
];

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

function pixels(width, height, r, g, b, a) {
  const p = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; ++i) {
    p[i * 4] = r;
    p[i * 4 + 1] = g;
    p[i * 4 + 2] = b;
    p[i * 4 + 3] = a;
  }
  return p;
}

async function main() {
  // Before any window has been requested nothing would drain the command
  // queue, so the call must refuse rather than do nothing.
  let early = null;
  try {
    ui.writeClipboard('too early');
  } catch (e) {
    early = e;
  }
  report('NO-WINDOW', early instanceof Error && /createWindow/.test(early.message), String(early));

  const win = await ui.createWindow({
    width: 320,
    height: 240,
    title: 'initial title',
  });

  win.title = 'commands test title';
  report('GETTER', win.title === 'commands test title', win.title);

  if (backend !== 'metal') {
    win.setFullscreen(true);
    win.setFullscreen(false);
  }

  // RGBA (10, 20, 30, 255): the X property holds ARGB, so the first pixel
  // reads back as 0xff0a141e.
  win.setIcon({width: 16, height: 16, pixels: pixels(16, 16, 10, 20, 30, 255)});

  for (const name of CURSORS)
    win.setCursor(name);
  win.setCursorImage({
    width: 8,
    height: 8,
    pixels: pixels(8, 8, 255, 0, 0, 255),
    hotspotX: 1,
    hotspotY: 1,
  });

  win.showCursor(false);
  win.showCursor(true);
  win.lockPointer(true);
  win.lockPointer(false);
  ui.writeClipboard('clipboard text');

  let unknown = null;
  try {
    win.setCursor('no-such-cursor');
  } catch (e) {
    unknown = e;
  }
  report(
      'UNKNOWN-CURSOR',
      unknown instanceof TypeError && /pointer/.test(unknown.message) &&
          /not-allowed/.test(unknown.message) &&
          /no-such-cursor/.test(unknown.message),
      String(unknown));

  let tooBig = null;
  try {
    // One byte over what Sokol's buffer can hold (1 MiB including the
    // terminator): it must be refused here, not handed to Sokol.
    ui.writeClipboard('x'.repeat(1 << 20));
  } catch (e) {
    tooBig = e;
  }
  report('CLIPBOARD-TOO-BIG', tooBig instanceof RangeError, String(tooBig));

  await new Promise((resolve) => {
    win.on('keydown', (e) => {
      if (e.keyCode === SAPP_KEYCODE_F3)
        resolve();
    });
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F3});
  });
  report('MARKER', true);

  console.log('DONE');
  win.close();
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
