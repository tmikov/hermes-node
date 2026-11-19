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
//            reads released=0.) Nothing checks that CB2 did not run: the
//            'exit' listener's own line follows CB1 directly (FRAME-NEXT),
//            and process.exit() never returns into the batch, so no
//            mutation of the closing code could make it run.
//  FATAL     an uncaught throw in a timer. `process._fatalException`
//            enters CLOSING through `process._preExit()` before it emits
//            'exit'. (Mutation: delete that call, and rafRefused=false.)
//  READS     a `readClipboard()` Promise still pending when process.exit(0)
//            runs -- pending by construction, since its reply can only be
//            delivered by a dispatch and the JS thread has not gone back to
//            the loop -- is rejected with ERR_CLIPBOARD_FAILED, and its
//            rejection handler runs before the 'exit' listeners (ORDER). A
//            synchronous read from the 'exit' listener is refused at once
//            with HNUI_ERROR rather than reaching the platform thread.
//            (Mutations: drop the uiRunTickCallback call, and ORDER is just
//            `exit`; drop the uiFailPendingClipboardReads call, and the
//            summary reads promise=0 and ORDER is `exit`; let uiPushCommand
//            accept while closing, and the sync read is answered by the
//            platform thread: SYNC status=HNUI_UNAVAILABLE.) The
//            `ClipboardRequests::failAll` call has no observable of its own
//            here beyond the `sync=0` it returns: a synchronous read blocks
//            the only thread that can enter CLOSING, so none can be pending
//            at that moment. Its behaviour is pinned by the GTest
//            ClipboardRequestTest.FailAllCompletesEveryWaiterAndALaterAnswerIsRefused,
//            not by this file.
//  COMMANDS  the window's commands are unavailable to 'exit' listeners: a
//            title, a cursor and a clipboard write issued there are refused
//            and traced, an event injected there is pushed but its dispatch
//            post is refused, and an outstanding `__captureFrame` request is
//            abandoned. `__waitPlatformFrames(1)` at the end of the listener
//            is what makes the refusals mean something: the frame that
//            began after it drained the command queue -- so an accepted
//            'late title' would have been read back -- and the injector, so
//            the event's post was attempted. (Mutations: let uiPushCommand
//            accept while closing, and readback='late title' appears (and
//            the refusal lines go); keep the refusal trace but queue the
//            command anyway, and only the 'late title' check fails. That
//            check is an --implicit-check-not, for the reason LISTENER's
//            is: the read-back is printed by the barrier frame BEFORE that
//            frame attempts the event's post, so a CHECK-NOT after the last
//            positive line could not see it;
//            remove gDispatch->close() from uiEnterClosing, and the
//            `post refused` line is gone; remove the
//            uiTestHooksAbandonCapture() call, and its line is gone.)
//            A `readClipboard()` there returns a Promise that is already
//            rejected; nothing drains microtasks before _exit(), so no
//            handler can report it and the `clipboard read promise:
//            refused (closing)` trace, printed once the rejection was
//            performed, is the check. (Mutation: delete the rejectRead()
//            call in uiReadClipboard's refusal branch, leaving the Promise
//            pending, and that line is gone.)
//  THROW     createWindow() and then a synchronous throw from the entry
//            script, in one turn. runHermesNode reaches its step 15 with a
//            window requested: CLOSING (reason=return) before the 'exit'
//            listeners, then the pre-cleanup hook ends the process with
//            the runtime's status, 1, after step 15 has flushed stdio --
//            the eight listener lines and the barrier's must all arrive
//            through a pipe. The window's readiness, posted by init_cb, is
//            either refused at the post or drained and refused by
//            close(); it is never delivered (no `window ready`). The
//            barrier makes that alternation exact: it returns only after a
//            platform frame, and init_cb precedes the first one.
//            (Mutations: delete the runBeforeRuntimeCleanupHook call, and
//            the run hangs until `timeout` (124); delete the
//            kRuntimeReturn hook call, and there is no reason=return line;
//            move the cleanup hook above step 15's flushPendingWrites, and
//            the piped lines are lost.) The alternation (THROW-ARM) is
//            checked in a run of its own, because its two arms fall on
//            opposite sides of `dispatcher closed`: a post refused by the
//            closed dispatcher is traced after it, and one accepted before
//            it is run and refused by close()'s own drain, before its
//            trace. THROW-TRACE's NOT pins that second arm's position, and
//            with it the close() call: without close() the readiness is not
//            lost -- it is still refused, by the state check on entry, but
//            from the loop step 15 pumps after the listeners, after
//            `dispatcher closed`. (Mutation: remove gDispatch->close(), and
//            the NOT fails; THROW-ARM alone stays green.)
//
// Not pinned here: the `gTable->beginClosing()` call inside uiEnterClosing.
// Without it the platform thread could still send a tick after CLOSING, but
// that tick would be refused by the closed dispatcher a few instructions
// later, and the window between the gate and the post is not something a
// test can hit deterministically.
// TickMachineTest.NoTickIsSentOnceTheTableIsClosing pins the table's gate,
// not the call that closes it.
//
// LISTENER's `callback(s) ran` check is an --implicit-check-not in a FileCheck
// run of its own (LISTENER-TICKS), whose one positive line is the one
// legitimate tick -- the frame LISTENER waits for before its first KEY_DOWN.
// So it covers the whole trace. It cannot share LISTENER-TRACE's run: the
// stray tick lands between the two -DAG lines (the dispatch and the tick are
// drained in whichever order they were posted, which varies), and FileCheck
// checks no NOT inside a DAG group's span. It is the only line that catches a
// closing drain that runs the tick while still tracing its refusal (mutation:
// keep runFrameTick's `refused (closing)` trace but drop the return after
// it; the tick then prints `0 callback(s) ran` and every other line passes).
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
// RUN: %FileCheck --check-prefix=LISTENER-TICKS --implicit-check-not='callback(s) ran' %s < %t.listener.err
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s frame %hnui_demo_addon > %t.frame.out 2> %t.frame.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FRAME-CODE %s
// RUN: %FileCheck --check-prefix=FRAME %s < %t.frame.out
// RUN: %FileCheck --check-prefix=FRAME-TRACE --implicit-check-not=AddressSanitizer %s < %t.frame.err
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s fatal %hnui_demo_addon > %t.fatal.out 2> %t.fatal.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FATAL-CODE %s
// RUN: %FileCheck --check-prefix=FATAL %s < %t.fatal.out
// RUN: %FileCheck --check-prefix=FATAL-TRACE --implicit-check-not=AddressSanitizer %s < %t.fatal.err
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s reads %hnui_demo_addon > %t.reads.out 2> %t.reads.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=READS-CODE %s
// RUN: %FileCheck --check-prefix=READS %s < %t.reads.out
// RUN: %FileCheck --check-prefix=READS-TRACE --implicit-check-not=AddressSanitizer %s < %t.reads.err
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s commands %hnui_demo_addon > %t.commands.out 2> %t.commands.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=COMMANDS-CODE %s
// RUN: %FileCheck --check-prefix=COMMANDS %s < %t.commands.out
// RUN: %FileCheck --check-prefix=COMMANDS-TRACE --implicit-check-not=AddressSanitizer --implicit-check-not='late title' %s < %t.commands.err
//
// THROW's stdout goes through a pipe, because the flush is part of the
// claim: a pipe is a libuv stream whose writes are queued, so an _exit()
// that ran before step 15's flush would lose the listener's lines.
//
// RUN: timeout 120 %with-xvfb sh -c '{ HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s throw %hnui_demo_addon 2> %t.throw.err; echo "EXITCODE $?" > %t.throw.code; } | cat > %t.throw.out'
// RUN: %FileCheck --check-prefix=THROW-CODE %s < %t.throw.code
// RUN: %FileCheck --check-prefix=THROW %s < %t.throw.out
// RUN: %FileCheck --check-prefix=THROW-TRACE --implicit-check-not='window ready' --implicit-check-not=AddressSanitizer %s < %t.throw.err
// RUN: %FileCheck --check-prefix=THROW-ARM %s < %t.throw.err

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

  if (mode === 'reads') {
    const order = [];
    process.on('exit', () => {
      order.push('exit');
      console.log('ORDER ' + order.join(','));
      console.log('SYNC status=' + addon.readClipboardSync(5000).status);
    });
    setTimeout(() => {
      ui.readClipboard().then(
          () => order.push('resolved'),
          (e) => order.push('rejected:' + e.code));
      process.exit(0);
    }, 1);
    return;
  }

  if (mode === 'commands') {
    win.title = 'early title';
    // The frame that begins after this call drains the command queue, so
    // the early title has been applied, and read back, when it returns.
    ui.__waitPlatformFrames(1);
    // A request that can never match and never gives up: outstanding when
    // CLOSING begins.
    ui.__captureFrame({publicationId: 999, giveUpAfterFrames: 0})
        .catch(() => {});
    process.on('exit', () => {
      win.title = 'late title';
      win.setCursor('ibeam');
      ui.writeClipboard('late');
      // Nothing drains microtasks between an 'exit' listener and _exit(),
      // so a handler here never runs: the trace is what says the Promise
      // was rejected rather than left pending.
      ui.readClipboard().catch(() => {});
      ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
      console.log('BARRIER ' + ui.__waitPlatformFrames(1));
    });
    setTimeout(() => process.exit(0), 1);
    return;
  }

  console.log('unknown mode ' + mode);
  process.exit(2);
}

if (mode === 'throw') {
  // Everything in the entry script's own turn: no await, so the throw is
  // the entry script's and step 13 of runHermesNode records it. The barrier
  // at the end of the listener returns only after a platform frame, and
  // init_cb precedes the first frame, so by then the window's readiness has
  // been offered to the JS thread and refused one way or the other.
  process.on('exit', (code) => {
    for (let i = 0; i < 8; ++i)
      console.log('EXIT-LINE ' + i + ' code=' + code);
    console.log('BARRIER ' + ui.__waitPlatformFrames(1));
  });
  ui.createWindow({width: 320, height: 240, title: 'exit paths'});
  throw new Error('boom after createWindow');
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(3);
});

// LISTENER: EXIT code=0 rafRefused=true
// LISTENER-NOT: FRAME-AFTER-EXIT
// LISTENER-NOT: SECOND-KEY-DELIVERED
// LISTENER-CODE: EXITCODE 0
// LISTENER-TICKS: frame tick: 1 callback(s) ran
// LISTENER-TRACE: lifecycle: CLOSING (reason=exit)
// LISTENER-TRACE-DAG: event dispatch: refused (closing)
// LISTENER-TRACE-DAG: frame tick: refused (closing)
// LISTENER-TRACE: lifecycle: dispatcher closed
// LISTENER-TRACE: lifecycle: event-loop handle released
// LISTENER-TRACE: requestAnimationFrame: refused (closing)

// FRAME: CB1
// FRAME-NEXT: EXIT code=4 target=false
// FRAME-CODE: EXITCODE 4
// FRAME-TRACE: lifecycle: CLOSING (reason=exit)
// FRAME-TRACE: lifecycle: frame callbacks released=2

// FATAL: EXIT code=1 rafRefused=true
// FATAL-CODE: EXITCODE 1
// FATAL-TRACE: lifecycle: CLOSING (reason=fatal)
// FATAL-TRACE: Error: boom in timer

// READS: ORDER rejected:ERR_CLIPBOARD_FAILED,exit
// READS-NEXT: SYNC status=HNUI_ERROR
// READS-CODE: EXITCODE 0
// READS-TRACE: lifecycle: CLOSING (reason=exit)
// READS-TRACE: clipboard read promise #1: failed by uiFailPendingClipboardReads
// READS-TRACE: lifecycle: clipboard reads failed sync=0 promise=1
// READS-TRACE: command refused (closing): read-clipboard

// COMMANDS: BARRIER true
// COMMANDS-CODE: EXITCODE 0
// COMMANDS-TRACE: command title: readback='early title'
// COMMANDS-TRACE: lifecycle: CLOSING (reason=exit)
// COMMANDS-TRACE: capture: abandoned (closing)
// COMMANDS-TRACE: command refused (closing): title
// COMMANDS-TRACE: command refused (closing): cursor
// COMMANDS-TRACE: command refused (closing): write-clipboard
// COMMANDS-TRACE: command refused (closing): read-clipboard
// COMMANDS-TRACE: clipboard read promise: refused (closing)
// COMMANDS-TRACE: event dispatch: post refused (dispatcher closed)

// THROW: EXIT-LINE 0 code=1
// THROW: EXIT-LINE 7 code=1
// THROW-NEXT: BARRIER true
// THROW-CODE: EXITCODE 1
// THROW-TRACE: Error: boom after createWindow
// THROW-TRACE: lifecycle: CLOSING (reason=return)
// THROW-TRACE: lifecycle: dispatcher closed
// THROW-TRACE-NOT: window readiness: refused (closing)
// THROW-TRACE: lifecycle: runtime returned, exiting with status 1
// THROW-ARM: lifecycle: CLOSING (reason=return)
// THROW-ARM: {{(init_cb: post refused \(dispatcher closed\)|window readiness: refused \(closing\))}}
// THROW-ARM: lifecycle: runtime returned, exiting with status 1
