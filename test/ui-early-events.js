// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// An event that arrives between the window's creation and the script's
// `await createWindow()` continuation is delivered to the listeners that
// continuation attaches (spec: JS API). The platform thread can queue one
// before the window object exists -- a real window gets FOCUSED, RESIZED
// and the like as it maps -- and the JS thread may be busy for any length
// of time before it resolves the Promise.
//
// The case is made likely rather than lucky: an UNFOCUSED is injected
// right after createWindow() is called, and the JS thread then stays busy
// for 600 ms, so by the time it wakes for the window, the platform thread
// has long since converted the event and posted a dispatch for it. Before
// the fix that dispatch ran in the same OwnerDispatch drain pass as the
// window's creation -- ahead of the continuation -- and `blur` was emitted
// into an object with no listeners (measured 3/3: `listeners saw: []`).
// The busy loop is what makes the MUTATION fail reliably; the passing case
// does not depend on it, since with the fix delivery cannot open before
// the continuation however the threads are scheduled.
//
//  EARLY   the early `blur` reaches a listener attached after the `await`,
//          with win.focused already false inside it, and before a marker
//          injected afterwards.
//          (Mutation: in onSharedContextReady, call openEventDelivery()
//          directly instead of posting it.)
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js: redirections inside `sh -c`, so the
// trace file is the binary's own stderr whatever the display wrapper does,
// and `timeout` because lit has no per-test timeout (dz 01a0d797-e326).
// Ends through process.exit(): its subject is not closing.
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err

// CODE: EXITCODE 0
// CHECK: EARLY ok
// CHECK: DONE
// ERR: window ready

'use strict';

const ui = require('hermes-node:ui');

const SAPP_KEYCODE_F1 = 290;

async function main() {
  const pending = ui.createWindow(
      {width: 200, height: 150, title: 'hermes-node-ui early events test'});
  ui.__injectEvent({type: 'UNFOCUSED'});
  // Stands in for a script that require()s something heavy before its
  // first `await`.
  const until = Date.now() + 600;
  while (Date.now() < until) {
  }
  const win = await pending;

  const seen = [];
  let focusedInBlur = null;
  win.on('blur', () => {
    seen.push('blur');
    focusedInBlur = win.focused;
  });
  await new Promise((resolve) => {
    win.on('keydown', (e) => {
      if (e.keyCode !== SAPP_KEYCODE_F1)
        return;
      seen.push('marker');
      resolve();
    });
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F1});
  });
  const ok = seen.join(',') === 'blur,marker' && focusedInBlur === false &&
      win.focused === false;
  console.log(
      'EARLY ' +
      (ok ? 'ok' :
            'WRONG seen=' + JSON.stringify(seen) +
               ' focusedInBlur=' + focusedInBlur + ' focused=' + win.focused));
  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
