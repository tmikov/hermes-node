// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The size-prefix rule, and the two refusals of an out-of-range argument.
//
// SIZES    Each writer -- hnui_frame_target (inside a frame callback) and
//          hnui_get_window_state -- is handed a struct embedded in a larger
//          buffer poisoned with 0xA5, with `size` set to exactly the
//          struct's HNUI_*_MIN_SIZE (computed from the header's constants,
//          not sizeof). Every byte from `size` onward must still be 0xA5
//          afterwards: those bytes belong to whatever an older addon put
//          after its smaller struct. Then `size` one below the minimum:
//          the call must refuse and NO byte may change.
//          (Mutation: `memcpy(out, &filled, sizeof filled)` in
//          hnui_get_window_state. hnui_target has no padding, so its
//          MIN_SIZE equals sizeof and that mutation cannot show there; the
//          state struct's trailing padding is what makes it visible.)
// CURSOR   hnui_set_cursor with a value outside DEFAULT..NOT_ALLOWED is
//          refused with a line on stderr naming the function and the value,
//          and pushes nothing -- the platform thread's trace never shows a
//          `command cursor 999`. A valid value afterwards is applied, so
//          the refusal is not just "everything is refused".
//          (Mutation: delete the range check.)
// CLIP     hnui_write_clipboard refuses a NULL string and one of
//          HNUI_CLIPBOARD_MAX_BYTES bytes, and accepts one byte fewer.
//          (Mutation: `>` for `>=` in the bound.)
// SHOW/LOCK  hnui_show_cursor and hnui_lock_pointer reach the platform.
//
// The trace is what the platform thread APPLIED; the stdout lines are what
// the addon saw. A marker key event, injected after every command was
// pushed, bounds the wait without timing (ui-commands.js does the same).
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js (redirections inside `sh -c`, `timeout`,
// ends through process.exit()).
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=ERR %s < %t.err
// RUN: %FileCheck --check-prefixes=APPLIED,APPLIED-%ui-backend %s < %t.err
// RUN: grep -c 'command cursor' %t.err | %FileCheck --check-prefix=NCURSOR %s
// RUN: grep -c 'command clipboard' %t.err | %FileCheck --check-prefix=NCLIP %s
// RUN: %FileCheck --check-prefix=SHORT %s < %t.err

// CODE: EXITCODE 0

// CHECK: SIZES ok
// CHECK: CURSOR ok
// CHECK: CLIP ok
// CHECK: DONE

// The refusals name the function and the value.
// ERR: hnui_set_cursor: refusing cursor value 999
// ERR: hnui_set_cursor: refusing cursor value -1
// ERR: hnui_write_clipboard: refusing a NULL string
// ERR: hnui_write_clipboard: refusing {{[0-9]+}} bytes
// What was applied, in its own prefix: the refusals are printed by the JS
// thread as they happen and the commands are traced by the platform thread
// later, so the two streams' relative order in the file is not fixed.
// APPLIED: command cursor 4: sapp_mouse_cursor=4
// APPLIED: command show cursor: sapp_mouse_shown=0
// APPLIED: command show cursor: sapp_mouse_shown=1
// APPLIED: command lock pointer: sapp_mouse_locked=1
// APPLIED: command lock pointer: sapp_mouse_locked=0
// The clipboard line's read-back is X11's (selection ownership), so it is
// GL-only; every backend's `-<backend>` line must still show the full
// 1048575 bytes arriving.
// APPLIED-gl: command clipboard: owns_selection=1 bytes=1048575

// "Pushes nothing" is pinned by COUNTING what the platform thread applied,
// not by CHECK-NOT: a NOT only searches the text before the next positive
// match, so a wrongly pushed `command clipboard: ... bytes=1048576` line
// would sit after that match's start and never be seen. Exactly one cursor
// command (the valid one) and one clipboard command (the boundary write)
// must have been applied.
// Anchored: grep -c prints a bare number, and an unanchored `1` would also
// accept 10, 11 or 21.
// NCURSOR: {{^1$}}
// NCLIP: {{^1$}}

// The two short-size refusals say so under tracing, as a null window does,
// naming the size given and the minimum.
// SHORT-DAG: hnui_frame_target: size {{[0-9]+}} is below the minimum {{[0-9]+}}; refused.
// SHORT-DAG: hnui_get_window_state: size {{[0-9]+}} is below the minimum {{[0-9]+}}; refused.

'use strict';

const ui = require('hermes-node:ui');
const addonPath = process.argv[2];
const SAPP_KEYCODE_F6 = 295;

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

async function main() {
  const win = await ui.createWindow({width: 320, height: 240, title: 'abi'});
  const addon = require(addonPath);

  // hnui_frame_target only answers inside a frame callback, so the probe
  // registers one; the JS callback registered after it runs later in the
  // same tick.
  addon.probeSizes();
  await new Promise((resolve) => requestAnimationFrame(() => resolve()));
  const sizes = addon.takeSizeProbe();
  const expected = [
    'target:exact ok', 'target:short ok', 'target:full ok', 'target:larger ok',
    'state:exact ok', 'state:short ok', 'state:full ok', 'state:larger ok',
  ];
  report(
      'SIZES', JSON.stringify(sizes) === JSON.stringify(expected),
      JSON.stringify(sizes));

  const result = addon.probeCommands();
  report(
      'CURSOR', result.cursor === 'ok', JSON.stringify(result));
  report('CLIP', result.clipboard === 'ok', JSON.stringify(result));

  await new Promise((resolve) => {
    win.on('keydown', (e) => {
      if (e.keyCode === SAPP_KEYCODE_F6) resolve();
    });
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F6});
  });
  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
