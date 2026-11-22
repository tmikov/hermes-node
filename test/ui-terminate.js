// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A termination request on macOS is a cancelable close (Task 16e, the
// plan's Design (d)). AppKit's own `-[NSApplication terminate:]` asks the
// delegate and then ends the process from inside the call, with status 0
// and `cleanup_cb` -- so a Dock Quit or Cmd-Q would end a program no
// matter what its `close` listeners said, and its exit code would be lost
// (Task 15's review). HnuiApplication's `terminate:` (mac_app.mm) never
// calls super: it turns the request into the same close request a click on
// the close button makes, which the shared code cancels and forwards to
// the program's `close` listeners.
//
// Three cases, one per RUN group:
//
//  DEFAULT  No listener prevents it. The close proceeds as in ui-close.js's
//           default case -- `'exit'` listeners run and the status is
//           `process.exitCode` (7), not AppKit's 0. The request went
//           through the window -- `performClose:`, Sokol's delegate -- and
//           the trace says so: `(window=1)`, never `no window yet`.
//  CANCEL   A `close` listener calls preventDefault(). The window stays:
//           `state` reports it viewable afterwards, as it did before the
//           request, and the program ends on its own terms, through
//           process.exit(0). "Before" is waited for, a platform frame at a
//           time: AppKit's occlusion state lags a new window (measured:
//           `viewable=0` straight after createWindow, `viewable=1` ten
//           frames later), so a check made too soon would read 0 with the
//           window plainly there.
//  EARLY    The request is made between calling createWindow() and
//           awaiting it, before the program can have listeners. Either
//           trace line is accepted: the request may land before Sokol has
//           a window that delivers events (handed straight to the shared
//           code) or after (performClose:). Either way the `close` is
//           delivered once the window is ready, and nothing prevents it.
//
// Every case's trace check matches `quit requested: cancelled` -- printed
// inside performClose: or the direct hand-off -- before `app: terminate
// requested`, printed after either returned; and none may contain
// `cleanup_cb` or `sapp_run returned`, which is what AppKit's own terminate:
// leaves behind.
//
// **Every request here enters at -[NSApplication terminate:], in this
// process** (ui.__driveWindow('terminate') calls `[NSApp terminate:nil]` on
// the main thread). Probe P4 measured that a second process cannot reach
// it: NSRunningApplication's quit Apple event arrives and reaches no
// handler at all -- not terminate:, not applicationShouldTerminate:, not a
// kAEQuitApplication handler. So what this file leaves unproved is that a
// real Dock Quit, a Cmd-Q, or a quit Apple event reaches the override. The
// first two need a person at the Mac.
//
// The driver's answer is read back after `[NSApp terminate:nil]` returned
// -- which it does only because terminate: is ours: `visible=1` once the
// close was cancelled, `window=0` when there was no window yet.
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 120 sh -c '{ HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s default 2> %t.default.err; echo "EXITCODE $?" > %t.default.code; } | cat > %t.default.out'
// RUN: %FileCheck --check-prefix=DEFAULT-CODE %s < %t.default.code
// RUN: %FileCheck --check-prefix=DEFAULT %s < %t.default.out
// RUN: %FileCheck --check-prefix=DEFAULT-TRACE --implicit-check-not='no window yet' --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.default.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s cancel > %t.cancel.out 2> %t.cancel.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CANCEL-CODE %s
// RUN: %FileCheck --check-prefix=CANCEL %s < %t.cancel.out
// RUN: %FileCheck --check-prefix=CANCEL-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.cancel.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s early > %t.early.out 2> %t.early.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=EARLY-CODE %s
// RUN: %FileCheck --check-prefix=EARLY %s < %t.early.out
// RUN: %FileCheck --check-prefix=EARLY-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.early.err

// DEFAULT: ANSWER visible=1
// DEFAULT-NEXT: CLOSE-EVENT
// DEFAULT-NEXT: EXIT-LINE 0 code=7
// DEFAULT: EXIT-LINE 7 code=7
// DEFAULT-CODE: EXITCODE 7
// DEFAULT-TRACE: quit requested: cancelled, forwarded as close
// DEFAULT-TRACE-NEXT: app: terminate requested; forwarded as a close request (window=1)
// DEFAULT-TRACE: close: not cancelled
// DEFAULT-TRACE: lifecycle: CLOSING (reason=close)
// DEFAULT-TRACE: close: ending the process through process.exit()

// CANCEL: VIEWABLE-BEFORE true
// CANCEL-NEXT: ANSWER visible=1
// CANCEL-NEXT: CANCELLED
// CANCEL-NEXT: STATE viewable=1 320x240
// CANCEL-NEXT: EXIT code=0
// CANCEL-CODE: EXITCODE 0
// CANCEL-TRACE: quit requested: cancelled, forwarded as close
// CANCEL-TRACE-NEXT: app: terminate requested; forwarded as a close request (window=1)
// CANCEL-TRACE: close: cancelled

// EARLY: ANSWERED
// EARLY-NEXT: CLOSE-EVENT
// EARLY-NEXT: EXIT code=5
// EARLY-CODE: EXITCODE 5
// EARLY-TRACE: quit requested: cancelled, forwarded as close
// EARLY-TRACE-NEXT: app: terminate requested; {{(forwarded as a close request \(window=1\)|no window yet, forwarded directly)}}
// EARLY-TRACE: close: not cancelled
// EARLY-TRACE: lifecycle: CLOSING (reason=close)

'use strict';

const ui = require('hermes-node:ui');

const mode = process.argv[2];
const TITLE = 'hnui terminate ' + process.pid;

/** One termination request, in process: `[NSApp terminate:nil]`. */
function terminate() {
  return String(ui.__driveWindow('terminate'));
}

const turn = () => new Promise((resolve) => setImmediate(resolve));

/** Whether `state` reports the window viewable within 300 platform frames. */
function waitViewable() {
  for (let i = 0; i < 300; i++) {
    if (String(ui.__driveWindow('state')).startsWith('viewable=1'))
      return true;
    ui.__waitPlatformFrames(1);
  }
  return false;
}

async function main() {
  if (mode === 'early') {
    process.exitCode = 5;
    process.on('exit', (code) => console.log('EXIT code=' + code));
    const pending = ui.createWindow({width: 320, height: 240, title: TITLE});
    // One loop turn first, as every driver command issued from inside a
    // delivery or before a window is ready does here: the in-process
    // driver acts synchronously, and test/ui-metal-maintenance.js says
    // what that does inside one dispatcher pass.
    await turn();
    // The answer depends on whether the window existed yet, so it is not
    // printed; the trace says which path was taken.
    terminate();
    console.log('ANSWERED');
    const win = await pending;
    win.on('close', () => console.log('CLOSE-EVENT'));
    return;
  }

  const win = await ui.createWindow({width: 320, height: 240, title: TITLE});

  if (mode === 'default') {
    process.exitCode = 7;
    process.on('exit', (code) => {
      for (let i = 0; i < 8; ++i)
        console.log('EXIT-LINE ' + i + ' code=' + code);
    });
    win.on('close', () => console.log('CLOSE-EVENT'));
    console.log('ANSWER ' + terminate());
    return;
  }

  if (mode === 'cancel') {
    process.on('exit', (code) => console.log('EXIT code=' + code));
    console.log('VIEWABLE-BEFORE ' + waitViewable());
    win.on('close', (e) => {
      e.preventDefault();
      console.log('CANCELLED');
      // A driver command from inside a listener: a loop turn first (see
      // EARLY).
      turn().then(() => {
        console.log('STATE ' + ui.__driveWindow('state'));
        process.exit(0);
      });
    });
    console.log('ANSWER ' + terminate());
    return;
  }

  console.log('unknown mode ' + mode);
  process.exit(2);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(3);
});
