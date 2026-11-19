// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The window enters CLOSING before the program's 'exit' listeners run, on
// the paths that end the process (spec: Lifecycle). From that point no
// frame callback, event or posted continuation reaches the program, and the
// window's commands are refused.
//
// Every case's 'exit' listener asks `ui.requestAnimationFrame()` for a
// frame; `rafRefused=true` -- the request answered 0 and registered nothing
// -- is the in-process evidence that CLOSING preceded the listener, and
// unlike a comparison of trace and stdout it does not depend on how the two
// streams interleave.
//
//  LISTENER  process.exit(0) from a `keydown` listener while a second
//            KEY_DOWN's dispatch and a frame tick are both already posted
//            to the JS thread. Neither runs: the dispatcher's close drains
//            them, and each refuses. `__waitPlatformFrames(1)` is what
//            makes that deterministic -- the frame that began after it has
//            drained the injected event (posting a dispatch) and, at its
//            end, posted the tick, so both are queued when process.exit()
//            runs. (Mutations: delete runFrameTick's closing refusal, and
//            the tick runs -- with no callback, since CLOSING released them
//            first -- so its refusal line is missing; delete the hook call
//            in processExit, and rafRefused=false, FRAME-AFTER-EXIT is
//            printed and there is no CLOSING line.) SECOND-KEY-DELIVERED
//            is checked but pins little: without CLOSING the second
//            dispatch runs from inside the first listener, where event
//            delivery does not re-enter. The refusal lines are what pin it.
//            The first KEY_DOWN waits one frame; see the comment there.
//  FRAME     process.exit(4) from the first of three frame callbacks. The
//            other two -- a JS one and a native one -- are released, not
//            run, and `hnui_frame_target` answers false in the 'exit'
//            listener although the tick is still on the stack. Run under
//            ASAN: the batch's references are released while that batch is
//            mid-iteration. (Mutations: leave both per-tick fields,
//            inFrameCallback and haveSlot, set in uiEnterClosing, and
//            target=true -- either one cleared alone is enough for
//            hnui_frame_target to refuse; skip releaseAll, and the trace
//            reads released=0.)
//  FATAL     an uncaught throw in a timer. `process._fatalException`
//            enters CLOSING through `process._preExit()` before it emits
//            'exit'. (Mutation: delete that call, and rafRefused=false.)
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// Redirections inside `sh -c` and a `timeout` on every run, as in every
// window test (ui-present.js gives the reasons).
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s listener %hnui_demo_addon > %t.listener.out 2> %t.listener.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=LISTENER-CODE %s
// RUN: %FileCheck --check-prefix=LISTENER %s < %t.listener.out
// RUN: %FileCheck --check-prefix=LISTENER-TRACE --implicit-check-not=AddressSanitizer %s < %t.listener.err
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s frame %hnui_demo_addon > %t.frame.out 2> %t.frame.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FRAME-CODE %s
// RUN: %FileCheck --check-prefix=FRAME %s < %t.frame.out
// RUN: %FileCheck --check-prefix=FRAME-TRACE --implicit-check-not=AddressSanitizer %s < %t.frame.err
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s fatal %hnui_demo_addon > %t.fatal.out 2> %t.fatal.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FATAL-CODE %s
// RUN: %FileCheck --check-prefix=FATAL %s < %t.fatal.out
// RUN: %FileCheck --check-prefix=FATAL-TRACE --implicit-check-not=AddressSanitizer %s < %t.fatal.err

'use strict';

const ui = require('hermes-node:ui');

const mode = process.argv[2];
const addonPath = process.argv[3];

const SAPP_KEYCODE_A = 65;

function installExitListener() {
  process.on('exit', (code) => console.log(
      'EXIT code=' + code +
      ' rafRefused=' + (ui.requestAnimationFrame(() => {}) === 0)));
}

async function main() {
  const win = await ui.createWindow({
    width: 320,
    height: 240,
    title: 'exit paths',
  });
  const addon = require(addonPath);

  if (mode === 'listener') {
    // One frame first, so that the first KEY_DOWN is delivered by an
    // ordinary posted dispatch with event delivery already open. Injected
    // straight after the await, it can instead be delivered by
    // openEventDelivery() while its own dispatch is still queued behind it
    // in the same pass -- and then the platform thread, finding a dispatch
    // outstanding, posts none for the second KEY_DOWN, and there is nothing
    // for CLOSING to refuse. The tick is posted after openEventDelivery()
    // was, and the dispatcher runs in posting order.
    await new Promise((resolve) => ui.requestAnimationFrame(resolve));
    installExitListener();
    let calls = 0;
    win.on('keydown', () => {
      if (++calls > 1) {
        console.log('SECOND-KEY-DELIVERED');
        return;
      }
      ui.requestAnimationFrame(() => console.log('FRAME-AFTER-EXIT'));
      ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
      ui.__waitPlatformFrames(1);
      process.exit(0);
    });
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
    return;
  }

  if (mode === 'frame') {
    process.on('exit', (code) => console.log(
        'EXIT code=' + code + ' target=' + addon.frameTargetAvailable()));
    ui.requestAnimationFrame(() => {
      console.log('CB1');
      process.exit(4);
    });
    ui.requestAnimationFrame(() => console.log('CB2-RAN'));
    addon.requestNativeFrame('n1');
    return;
  }

  if (mode === 'fatal') {
    installExitListener();
    setTimeout(() => {
      throw new Error('boom in timer');
    }, 1);
    return;
  }

  console.log('unknown mode ' + mode);
  process.exit(2);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(3);
});

// LISTENER: EXIT code=0 rafRefused=true
// LISTENER-NOT: FRAME-AFTER-EXIT
// LISTENER-NOT: SECOND-KEY-DELIVERED
// LISTENER-CODE: EXITCODE 0
// LISTENER-TRACE: lifecycle: CLOSING (reason=exit)
// LISTENER-TRACE-DAG: event dispatch: refused (closing)
// LISTENER-TRACE-DAG: frame tick: refused (closing)
// LISTENER-TRACE: lifecycle: dispatcher closed
// LISTENER-TRACE: lifecycle: event-loop handle released
// LISTENER-TRACE: requestAnimationFrame: refused (closing)
// LISTENER-TRACE-NOT: callback(s) ran

// FRAME: CB1
// FRAME-NEXT: EXIT code=4 target=false
// FRAME-NOT: CB2-RAN
// FRAME-CODE: EXITCODE 4
// FRAME-TRACE: lifecycle: CLOSING (reason=exit)
// FRAME-TRACE: lifecycle: frame callbacks released=2

// FATAL: EXIT code=1 rafRefused=true
// FATAL-CODE: EXITCODE 1
// FATAL-TRACE: lifecycle: CLOSING (reason=fatal)
// FATAL-TRACE: Error: boom in timer
