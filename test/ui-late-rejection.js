// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A createWindow() rejected LATE leaves no window behind: hnui_get_window()
// answers NULL afterwards, as hnui.h says it does for a runtime that
// "called it and was rejected".
//
// Late means in onSharedContextReady() (ui_module.cpp), on the JS thread,
// after Sokol's window, the shared context and generation 1's surfaces all
// exist. Three steps can fail there, and a script can provoke two of them
// by taking something away between the createWindow() call and its
// `await`:
//
//  REQUIRE  the window object is built by calling
//           `globalThis.require('events')`; with the global cleared that
//           step fails before the object exists.
//  ORIGIN   the frame origin is read by calling `process.uptime()`; with
//           the property cleared that step fails after the window object
//           exists and is held, so this is the abandon path that has a
//           reference to delete.
//
// (The third, surface allocation, is the backend's and cannot be provoked
// from a script.) The hnui_window is built in a local and published to
// gWindow only after every step succeeded, which is the line this pins.
//
// Also asserted, because they are the rest of "no window": the frame
// globals were not installed, a second createWindow() still throws (one
// window per process counts the request, not the outcome), and a script
// that handles the rejection and returns ends with status 0 (Ruling 72:
// the referenced handle is released).
//
// Mutation: in onSharedContextReady(), move `gWindow = w;` up to just
// after `new hnui_window()`. Under ASAN the worker's per-turn check then
// reads the deleted window (heap-use-after-free, status 1, nothing after
// BEFORE); without ASAN the line reads `AFTER true`.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon require > %t.require.out 2> %t.require.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefixes=CHECK,REQUIRE %s < %t.require.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='window ready' --implicit-check-not=AddressSanitizer %s < %t.require.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon origin > %t.origin.out 2> %t.origin.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefixes=CHECK,ORIGIN %s < %t.origin.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='window ready' --implicit-check-not=AddressSanitizer %s < %t.origin.err

// CHECK: BEFORE false
// REQUIRE-NEXT: REJECTED could not create the window object: globalThis.require is not a function, so EventEmitter could not be loaded
// ORIGIN-NEXT: REJECTED could not read process.uptime(), which frame timestamps are measured against
// CHECK-NEXT: AFTER false
// CHECK-NEXT: RAF undefined
// CHECK-NEXT: SECOND threw
// CODE: EXITCODE 0

// The rejection really was the late one: the platform thread had finished
// init_cb, so Sokol's window existed.
// TRACE: init_cb: shared context ready
// TRACE: lifecycle: FAILED
// TRACE: lifecycle: event-loop handle released (createWindow rejected)
// TRACE: lifecycle: runtime returned, exiting with status 0

'use strict';

const ui = require('hermes-node:ui');
const addon = require(process.argv[2]);
const mode = process.argv[3];

async function main() {
  console.log('BEFORE ' + addon.hasWindow());
  const savedRequire = globalThis.require;
  const savedUptime = process.uptime;
  const pending = ui.createWindow(
      {width: 200, height: 150, title: 'hermes-node-ui late rejection'});
  // Between the call and its await: onSharedContextReady() runs on a later
  // loop turn and finds no function here.
  if (mode === 'require')
    globalThis.require = undefined;
  else
    process.uptime = undefined;
  let outcome;
  try {
    await pending;
    outcome = 'RESOLVED';
  } catch (e) {
    outcome = 'REJECTED ' + e.message;
  }
  globalThis.require = savedRequire;
  process.uptime = savedUptime;
  console.log(outcome);
  console.log('AFTER ' + addon.hasWindow());
  console.log('RAF ' + typeof globalThis.requestAnimationFrame);
  let second = 'did-not-throw';
  try {
    await ui.createWindow({width: 100, height: 100});
  } catch (e) {
    second = 'threw';
  }
  console.log('SECOND ' + second);
  // No process.exit(): returning is the subject of the last TRACE line.
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
