// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Closing the window (spec: Input and platform services, Closing; The JS
// API; Lifecycle). The window system's close request is cancelled on the
// platform thread every time and forwarded as `close`; a `close` that no
// listener cancels closes the window, which ends the process as if
// `process.exit()` had been called with no argument. `win.close()` does
// the same without emitting `close`.
//
// Every close request but REPLACED's is REAL: `x11_window_driver --close`
// sends WM_PROTOCOLS / WM_DELETE_WINDOW, which is the only thing that sets
// Sokol's own quit flag. An injected QUIT_REQUESTED enters below Sokol and
// leaves that flag alone, so a close test built on one could not see the
// quit reaching Sokol uncancelled. Every trace run forbids `sapp_run
// returned` and `cleanup_cb` anywhere in the trace: either is Sokol having
// left its loop, which closes the X display under the JS thread's context.
//
//  DEFAULT   process.exitCode = 7, one close request, a `close` listener
//            that does not cancel, and nothing else that could end the
//            script. The process ends with status 7, after its 'exit'
//            listener's eight lines arrived through a pipe. (Mutations:
//            delete the uiCloseWindow call in deliver, and the window stays
//            open until `timeout` -- 124; pass an explicit 0 to
//            process.exit, and the status is 0.)
//  CANCEL    the first close is cancelled with e.preventDefault(); a frame
//            then runs (the window is alive and drawing), the kept event's
//            preventDefault() called there -- outside any close event --
//            is ignored and says so, and the second close request, not
//            cancelled, ends the process. (Mutations: make preventDefault
//            do nothing, and the first request ends the run before
//            FRAME-AFTER-CANCEL; stop resetting closeCancelled per event,
//            and the second request is cancelled too and the run ends at
//            `timeout`.)
//  NATIVE    the same through hnui_cancel_close, from the reference
//            addon's native listener: false outside an event (OUTSIDE),
//            true inside the first close request, which it cancels; the
//            second ends the process. (Mutation: have hnui_cancel_close
//            return true without setting closeCancelled, and the first
//            request ends the run: the LOG line and the second JS-CLOSE
//            are missing.)
//  WINCLOSE  win.close() from a timer, with process.exitCode = 9: status
//            9, no `close` emitted, and win.close() does not return.
//            (Mutation: have win.close() only enter CLOSING and return.
//            RETURNED prints, and the run does not hang: CLOSING released
//            the referenced handle, so the loop drains and the
//            runtime-return hook exits with process.exitCode, 9. CODE and
//            WINCLOSE's positive line still pass; the implicit check
//            against RETURNED, and the trace's `close: ending the process`
//            line, are what fail.)
//  NOEXIT    process.exit replaced by a non-function, process.exitCode =
//            8: win.close() enters CLOSING and then throws a TypeError
//            naming process.exit, which the caller catches. The loop handle
//            is released, so the script's end ends the process with status
//            8. (Mutation: remove the napi_throw_type_error call in
//            uiCloseWindow, and win.close() returns: RETURNED prints.)
//  LATE      a close request arriving while the window is already CLOSING
//            -- sent from the 'exit' listener of the close that ended the
//            run -- is still cancelled, and the platform thread keeps
//            running frames afterwards: QUITS counts executed
//            sapp_cancel_quit() calls, and FRAMES, taken after it, is the
//            liveness check, since a platform loop that left sapp_run()
//            runs no further frame. (Mutation: cancel only the first quit
//            request, with a static flag in uiHandlePlatformEvent; Sokol
//            leaves its loop, `sapp_run returned` appears, which the
//            implicit check forbids, and FRAMES is false.)
//  REPLACED  process.exit replaced by a function that records the call and
//            returns. An injected close request (the subject here is what
//            the JS thread does after the default action returns, which is
//            after the converter) is delivered in the same dispatch pass as
//            a KEY_DOWN injected with it. The default action enters CLOSING
//            -- closing the dispatcher from inside the function it is
//            running -- and calls the replacement, which returns: the
//            window stays in CLOSING, the KEY_DOWN behind it is not
//            delivered, a frame request is refused, and a timer the
//            replacement set calls the original process.exit(6). Under
//            ASAN. (Mutation: remove the closing check in
//            uiDispatchEvents, and KEY-AFTER-CLOSE prints.)
//            Nothing enforces "the same dispatch pass": it holds because the
//            two back-to-back __injectEvent calls are taken by one
//            drainInjectedEvents() call on the platform thread (measured
//            64/64 at 16-way parallel; ui-events.js relies on the same
//            pattern). Were they split across two frames with the JS thread
//            running the first pass in between, the KEY_DOWN would arrive
//            after the dispatcher closed, and the trace would read `event
//            dispatch: post refused (dispatcher closed)` where REPLACED-TRACE
//            expects `event keydown: not delivered (closing)`.
//  RFRAME    the same replacement, with win.close() called from a frame
//            callback that has drawn: the tick does not go on to submit
//            the frame, since the table is closing and the slot is put
//            back instead. Under ASAN. (Mutation: remove that check in
//            runFrameTick, and the tick traces `1 callback(s) ran, drew`.)
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// Redirections inside `sh -c` and a `timeout` on every run, as in every
// window test (ui-present.js gives the reasons). DEFAULT's stdout goes
// through a pipe, because the flush is part of the claim: a pipe is a
// libuv stream whose writes are queued.
//
// RUN: %with-display 120 sh -c '{ HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s default %hnui_demo_addon %x11_window_driver 2> %t.default.err; echo "EXITCODE $?" > %t.default.code; } | cat > %t.default.out'
// RUN: %FileCheck --check-prefix=DEFAULT-CODE %s < %t.default.code
// RUN: %FileCheck --check-prefix=DEFAULT %s < %t.default.out
// RUN: %FileCheck --check-prefix=DEFAULT-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.default.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s cancel %hnui_demo_addon %x11_window_driver > %t.cancel.out 2> %t.cancel.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CANCEL-CODE %s
// RUN: %FileCheck --check-prefix=CANCEL %s < %t.cancel.out
// RUN: %FileCheck --check-prefix=CANCEL-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.cancel.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s native %hnui_demo_addon %x11_window_driver > %t.native.out 2> %t.native.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=NATIVE-CODE %s
// RUN: %FileCheck --check-prefix=NATIVE %s < %t.native.out
// RUN: %FileCheck --check-prefix=NATIVE-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.native.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s winclose %hnui_demo_addon %x11_window_driver > %t.winclose.out 2> %t.winclose.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=WINCLOSE-CODE %s
// RUN: %FileCheck --check-prefix=WINCLOSE --implicit-check-not=CLOSE-EVENT-EMITTED --implicit-check-not=RETURNED %s < %t.winclose.out
// RUN: %FileCheck --check-prefix=WINCLOSE-TRACE --implicit-check-not='quit requested' --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.winclose.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s noexit %hnui_demo_addon %x11_window_driver > %t.noexit.out 2> %t.noexit.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=NOEXIT-CODE %s
// RUN: %FileCheck --check-prefix=NOEXIT --implicit-check-not=RETURNED %s < %t.noexit.out
// RUN: %FileCheck --check-prefix=NOEXIT-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.noexit.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s late %hnui_demo_addon %x11_window_driver > %t.late.out 2> %t.late.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=LATE-CODE %s
// RUN: %FileCheck --check-prefix=LATE %s < %t.late.out
// RUN: %FileCheck --check-prefix=LATE-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.late.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s replaced %hnui_demo_addon %x11_window_driver > %t.replaced.out 2> %t.replaced.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=REPLACED-CODE %s
// RUN: %FileCheck --check-prefix=REPLACED --implicit-check-not=KEY-AFTER-CLOSE --implicit-check-not=FRAME-AFTER-CLOSE %s < %t.replaced.out
// RUN: %FileCheck --check-prefix=REPLACED-TRACE --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.replaced.err
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s rframe %hnui_demo_addon %x11_window_driver > %t.rframe.out 2> %t.rframe.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=RFRAME-CODE %s
// RUN: %FileCheck --check-prefix=RFRAME --implicit-check-not=CB2-RAN %s < %t.rframe.out
// RUN: %FileCheck --check-prefix=RFRAME-TRACE --implicit-check-not=', drew' --implicit-check-not='sapp_run returned' --implicit-check-not='cleanup_cb' --implicit-check-not=AddressSanitizer %s < %t.rframe.err

'use strict';

const ui = require('hermes-node:ui');
const {spawnSync} = require('child_process');

const mode = process.argv[2];
const addonPath = process.argv[3];
const driverPath = process.argv[4];

const TITLE = 'hnui close ' + process.pid;
const SAPP_KEYCODE_A = 65;

/** One real close request: the driver waits for the window and sends
    WM_DELETE_WINDOW, and has exited (XSync'd) when this returns. */
function requestClose() {
  const r = spawnSync(driverPath, ['--close', TITLE], {stdio: 'inherit'});
  if (r.status !== 0)
    console.log('DRIVER FAILED status=' + r.status);
}

function nextFrame(cb) {
  ui.requestAnimationFrame(cb);
}

async function main() {
  const win = await ui.createWindow({width: 320, height: 240, title: TITLE});
  const addon = require(addonPath);

  if (mode === 'default') {
    process.exitCode = 7;
    process.on('exit', (code) => {
      for (let i = 0; i < 8; ++i)
        console.log('EXIT-LINE ' + i + ' code=' + code);
    });
    win.on('close', () => console.log('CLOSE-EVENT'));
    requestClose();
    return;
  }

  if (mode === 'cancel') {
    process.on('exit', (code) => console.log('EXIT code=' + code));
    let calls = 0;
    win.on('close', (e) => {
      if (++calls > 1) {
        console.log('SECOND-CLOSE');
        return;
      }
      e.preventDefault();
      console.log('CANCELLED');
      const kept = e;
      nextFrame(() => {
        console.log('FRAME-AFTER-CANCEL');
        kept.preventDefault();
        requestClose();
      });
    });
    requestClose();
    return;
  }

  if (mode === 'native') {
    process.on('exit', (code) => console.log('EXIT code=' + code));
    addon.startEventLog();
    addon.armCloseCancel(1);
    console.log('OUTSIDE ' + addon.cancelCloseNow());
    let calls = 0;
    win.on('close', () => {
      console.log('JS-CLOSE');
      if (++calls > 1)
        return;
      nextFrame(() => {
        const lines =
            addon.takeEventLog().filter((l) => l.includes('cancel_close'));
        console.log('LOG ' + lines.join(' '));
        requestClose();
      });
    });
    requestClose();
    return;
  }

  if (mode === 'winclose') {
    process.exitCode = 9;
    process.on('exit', (code) => console.log('EXIT code=' + code));
    win.on('close', () => console.log('CLOSE-EVENT-EMITTED'));
    setTimeout(() => {
      win.close();
      console.log('RETURNED');
    }, 1);
    return;
  }

  if (mode === 'noexit') {
    process.exitCode = 8;
    process.on('exit', (code) => console.log('EXIT code=' + code));
    process.exit = 42;
    setTimeout(() => {
      try {
        win.close();
        console.log('RETURNED');
      } catch (e) {
        console.log('THREW ' + e.name + ': ' + e.message);
      }
    }, 1);
    return;
  }

  if (mode === 'late') {
    // One frame first, for the reason ui-exit-paths.js's LISTENER waits
    // one: sent straight after the await, the first request can be
    // delivered by openEventDelivery() while its own posted dispatch is
    // still queued behind it in the same pass. That dispatch never runs --
    // process.exit() ends the pass -- so it leaves gDispatchPosted set, and
    // the second request's platform thread then attempts no post at all,
    // leaving nothing for the closed dispatcher to refuse. The tick is
    // posted after openEventDelivery() was, and the dispatcher runs in
    // posting order, so by then delivery is open.
    await new Promise((resolve) => nextFrame(resolve));
    process.on('exit', () => {
      requestClose();
      console.log(
          'QUITS ' + ui.__waitPlatformQuitRequests(2) + ' FRAMES ' +
          ui.__waitPlatformFrames(1));
    });
    requestClose();
    return;
  }

  if (mode === 'replaced' || mode === 'rframe') {
    const originalExit = process.exit;
    process.on('exit', (code) => console.log('EXIT code=' + code));
    process.exit = function() {
      console.log('REPLACED-EXIT args=' + arguments.length);
      setTimeout(() => {
        console.log(
            'RAF ' +
            ui.requestAnimationFrame(() => console.log('FRAME-AFTER-CLOSE')));
        originalExit.call(process, 6);
      }, 1);
    };
    if (mode === 'replaced') {
      win.on('close', () => console.log('CLOSE-EVENT'));
      win.on('keydown', () => console.log('KEY-AFTER-CLOSE'));
      // One injected batch: the platform thread pushes both in one frame,
      // so one dispatch pass delivers them, close first. Not enforced; see
      // REPLACED in the header.
      ui.__injectEvent({type: 'QUIT_REQUESTED'});
      ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
      return;
    }
    nextFrame(() => {
      addon.drawFixedPattern();
      console.log('CB1 drew');
      win.close();
      console.log('CLOSE-RETURNED');
    });
    nextFrame(() => console.log('CB2-RAN'));
    return;
  }

  console.log('unknown mode ' + mode);
  process.exit(2);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(3);
});

// DEFAULT: CLOSE-EVENT
// DEFAULT-NEXT: EXIT-LINE 0 code=7
// DEFAULT: EXIT-LINE 7 code=7
// DEFAULT-CODE: EXITCODE 7
// DEFAULT-TRACE: quit requested: cancelled, forwarded as close
// DEFAULT-TRACE: close: not cancelled
// DEFAULT-TRACE: lifecycle: CLOSING (reason=close)
// DEFAULT-TRACE: close: ending the process through process.exit()

// CANCEL: CANCELLED
// CANCEL-NEXT: FRAME-AFTER-CANCEL
// CANCEL-NEXT: SECOND-CLOSE
// CANCEL-NEXT: EXIT code=0
// CANCEL-CODE: EXITCODE 0
// CANCEL-TRACE: close: cancelled
// CANCEL-TRACE: close: preventDefault outside a close event; ignored
// CANCEL-TRACE: close: not cancelled
// CANCEL-TRACE: lifecycle: CLOSING (reason=close)

// NATIVE: OUTSIDE false
// NATIVE-NEXT: JS-CLOSE
// NATIVE-NEXT: LOG native:cancel_close=1
// NATIVE-NEXT: JS-CLOSE
// NATIVE-NEXT: EXIT code=0
// NATIVE-CODE: EXITCODE 0
// NATIVE-TRACE: close: cancelled
// NATIVE-TRACE: close: not cancelled

// WINCLOSE: EXIT code=9
// WINCLOSE-CODE: EXITCODE 9
// WINCLOSE-TRACE: lifecycle: CLOSING (reason=close)
// WINCLOSE-TRACE: close: ending the process through process.exit()

// NOEXIT: THREW TypeError: closing the window: process.exit is not a function, so the process could not be ended.
// NOEXIT-NEXT: EXIT code=8
// NOEXIT-CODE: EXITCODE 8
// NOEXIT-TRACE: lifecycle: CLOSING (reason=close)
// NOEXIT-TRACE: lifecycle: runtime returned, exiting with status 8

// LATE: QUITS true FRAMES true
// LATE-CODE: EXITCODE 0
// LATE-TRACE: quit requested: cancelled, forwarded as close
// LATE-TRACE: lifecycle: CLOSING (reason=close)
// LATE-TRACE: quit requested: cancelled, forwarded as close
// LATE-TRACE: event dispatch: post refused (dispatcher closed)

// REPLACED: CLOSE-EVENT
// REPLACED-NEXT: REPLACED-EXIT args=0
// REPLACED-NEXT: RAF 0
// REPLACED-NEXT: EXIT code=6
// REPLACED-CODE: EXITCODE 6
// REPLACED-TRACE: close: not cancelled
// REPLACED-TRACE: lifecycle: CLOSING (reason=close)
// REPLACED-TRACE: lifecycle: dispatcher closed
// REPLACED-TRACE: close: ending the process through process.exit()
// REPLACED-TRACE: close: process.exit returned; the window stays in CLOSING
// REPLACED-TRACE: event keydown: not delivered (closing)
// REPLACED-TRACE: requestAnimationFrame: refused (closing)

// RFRAME: CB1 drew
// RFRAME-NEXT: REPLACED-EXIT args=0
// RFRAME-NEXT: CLOSE-RETURNED
// RFRAME-NEXT: RAF 0
// RFRAME-NEXT: EXIT code=6
// RFRAME-CODE: EXITCODE 6
// RFRAME-TRACE: lifecycle: CLOSING (reason=close)
// RFRAME-TRACE: lifecycle: frame callbacks released=1
// RFRAME-TRACE: close: process.exit returned; the window stays in CLOSING
// RFRAME-TRACE: frame tick: ended by CLOSING; slot reclaimed, nothing submitted
