// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// An exception that escapes an EVENT listener goes where an exception
// from any other async callback goes (CLAUDE.md, "Uncaught Exceptions From
// Async Callbacks"): to `process._fatalException`, which either hands it to
// an `uncaughtException` listener and carries on, or reports it and exits
// 1. test/ui-raf.js pins the same split for frame callbacks; nothing pinned
// it for events, and a delivery path that cleared the exception and carried
// on passed every window test while a script that threw in a listener
// exited 0 -- the failure that section exists to prevent.
//
// Two routes, because a listener's exception can escape at two points of
// deliver() (ui_events.cpp): from `window.emit` itself, and from the
// `process._tickCallback()` that runs the listener's nextTick callbacks
// before the next event.
//
//  HANDLED  with an `uncaughtException` listener, a throw from a keydown
//           listener and one from a nextTick callback queued by a keydown
//           listener both reach it, and the events after each are still
//           delivered.
//  FATAL    (two RUN lines, one per route) with no listener the process
//           exits 1, the error is on stderr, `'exit'` listeners ran, and
//           nothing after the throw ran.
//
// Mutations: in ui_events.cpp, replace emitToJs()'s handleCallbackException
// with clearing the pending exception and carrying on -- HANDLED (no
// listener line) and FATAL-LISTENER (exits 0 through AFTER) go red. The
// same in uiRunTickCallback() -- HANDLED and FATAL-TICK go red.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js: redirections inside `sh -c`, so the
// trace file is the binary's own stderr whatever the display wrapper does,
// and `timeout` because lit has no per-test timeout (dz 01a0d797-e326).
// Ends through process.exit(): its subject is not closing.
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s handled > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 %hermes-node-ui %s fatal-listener > %t.fl.out 2> %t.fl.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FATALCODE %s
// RUN: %FileCheck --check-prefix=FATAL %s < %t.fl.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=FATALERR %s < %t.fl.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 %hermes-node-ui %s fatal-tick > %t.ft.out 2> %t.ft.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FATALCODE %s
// RUN: %FileCheck --check-prefix=FATAL %s < %t.ft.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=FATALERR %s < %t.ft.err

// CODE: EXITCODE 0
// CHECK: HANDLED ok
// CHECK: DONE
// ERR: window ready

// FATALCODE: EXITCODE 1
// FATAL: THREW
// FATAL-NOT: AFTER
// FATAL: EXIT listener code=1
// FATAL-NOT: AFTER
// FATALERR: fatal-boom

'use strict';

const ui = require('hermes-node:ui');
const mode = process.argv[2];

const SAPP_KEYCODE_F1 = 290;
const SAPP_KEYCODE_F2 = 291;
const SAPP_KEYCODE_F3 = 292;
const SAPP_KEYCODE_F4 = 293;

const key = (keyCode) => ui.__injectEvent({type: 'KEY_DOWN', keyCode});

async function main() {
  const win = await ui.createWindow(
      {width: 200, height: 150, title: 'hermes-node-ui event exceptions'});

  if (mode === 'fatal-listener' || mode === 'fatal-tick') {
    // No `uncaughtException` listener: the first throw is fatal. The
    // second key must never be delivered; if it is, it exits 0 itself,
    // which is exactly what clearing the exception and carrying on does.
    process.on('exit', (code) => console.log('EXIT listener code=' + code));
    win.on('keydown', (e) => {
      if (e.keyCode === SAPP_KEYCODE_F1) {
        console.log('THREW');
        if (mode === 'fatal-listener')
          throw new Error('fatal-boom');
        process.nextTick(() => {
          throw new Error('fatal-boom');
        });
      } else if (e.keyCode === SAPP_KEYCODE_F2) {
        console.log('AFTER');
        process.exit(0);
      }
    });
    // One batch, so both are queued before the first is delivered.
    key(SAPP_KEYCODE_F1);
    key(SAPP_KEYCODE_F2);
    return;
  }

  const log = [];
  process.on('uncaughtException', (e) => log.push('listener:' + e.message));
  await new Promise((resolve) => {
    win.on('keydown', (e) => {
      switch (e.keyCode) {
        case SAPP_KEYCODE_F1:
          log.push('k1');
          throw new Error('boom1');
        case SAPP_KEYCODE_F2:
          log.push('k2');
          break;
        case SAPP_KEYCODE_F3:
          log.push('k3');
          process.nextTick(() => {
            throw new Error('boom2');
          });
          break;
        case SAPP_KEYCODE_F4:
          log.push('k4');
          resolve();
          break;
      }
    });
    key(SAPP_KEYCODE_F1);
    key(SAPP_KEYCODE_F2);
    key(SAPP_KEYCODE_F3);
    key(SAPP_KEYCODE_F4);
  });
  const got = log.join(',');
  const expected = 'k1,listener:boom1,k2,k3,listener:boom2,k4';
  console.log('HANDLED ' + (got === expected ? 'ok' : 'WRONG ' + got));
  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
