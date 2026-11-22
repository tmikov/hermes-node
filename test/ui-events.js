// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Input events, end to end: a synthetic PLATFORM event (a sapp_event, the
// converter's input) is injected, the platform thread converts and queues
// it exactly as it would a real one, and the JS thread delivers it -- to
// the reference addon's native listener first, then to the window's
// EventEmitter.
//
// What is asserted, each line of output naming one property:
//
//  ORDER      every event reaches the native listener before JavaScript,
//             and the sequence across events is the injection order.
//             (Mutation: emit before the native listeners.)
//  PAYLOAD    keydown's code/shiftKey, mousedown's DOM button, the summed
//             mousemove dx, paste's text and drop's paths. The number of
//             mousemoves is NOT asserted: whether three injected moves
//             coalesce depends on which thread runs first, and the sum is
//             what coalescing preserves.
//  MIRROR     inside `blur`, win.focused is false; inside `focus`, true.
//             (Mutation: update the mirror after emit.)
//  TICKS      a promise callback queued by a keydown listener runs
//             before a CHAR that the same listener queued behind it, in
//             one dispatch pass.
//             (Mutation: remove the _tickCallback call.)
//  CLOSE      an injected QUIT_REQUESTED is converted and emitted as
//             `close`, exactly once. That is ALL it shows. It does not show
//             that the quit is cancelled: Sokol acts on its own
//             `_sapp.quit_requested`, which only a real close request
//             sets, so an injected event leaves nothing to cancel and this
//             line (and the TRACE line) stays green with the
//             sapp_cancel_quit() call removed. test/ui-real-events.js,
//             whose close request is a real WM_DELETE_WINDOW, pins the
//             cancellation. The `close` listener calls e.preventDefault():
//             an uncancelled `close` closes the window, which ends the
//             process, and would end this test at that point
//             (test/ui-close.js pins both outcomes).
//  STATE      hnui_get_window_state agrees with the JS object.
//  MINIMIZED  a frame requested while minimized does not run until the
//             window is restored. (Mutation: remove setMinimized.)
//  REMOVAL    a native listener removed during an event's delivery still
//             receives THAT event and no later one; removing it twice, or
//             removing an id never issued, is a no-op; and
//             hnui_event_dispatch_quiescent is false inside a listener and
//             true in the next frame callback. (Mutations: make removal a
//             no-op; skip a removed listener within the event it was
//             removed during; return true from the quiescent query.)
//  TRACKED    after UNFOCUSED and ICONIFIED, a later unrelated key event
//             still says focused=false minimized=true, in the JS mirror and
//             in hnui_get_window_state. The converter changes only the
//             fields its event is about and copies the rest from the
//             platform thread's tracked state, so that state has to be
//             stored back after each event (Ruling 52). (Mutation: remove
//             `gTracked = converted.window` in uiHandlePlatformEvent.)
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// The RUN-line shape is ui-present.js's, for the reasons given there:
// redirections INSIDE `sh -c`, so the trace file is the binary's own stderr
// whatever the display wrapper does, and `timeout` because lit has no
// per-test timeout (dz 01a0d797-e326) and a mutation here typically waits
// forever for an event that never arrives. The test ends through
// process.exit(): its subject is not closing.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

// CODE: EXITCODE 0

// CHECK: ORDER ok
// CHECK: PAYLOAD ok
// CHECK: MIRROR ok
// CHECK: TICKS ok
// CHECK: CLOSE ok
// CHECK: STATE ok
// CHECK: MINIMIZED ok
// CHECK: REMOVAL ok
// CHECK: TRACKED ok
// CHECK: DONE

// The QUIT_REQUESTED branch ran on the platform thread. For an injected
// event, "cancelled" is the call having been made, not proof that it
// mattered -- see CLOSE above.
// TRACE: quit requested: cancelled, forwarded as close

'use strict';

const ui = require('hermes-node:ui');
const addonPath = process.argv[2];

// Sokol's numbering, because these are platform events.
const SAPP_KEYCODE_A = 65;
const SAPP_KEYCODE_F1 = 290;
const SAPP_KEYCODE_F2 = 291;
const SAPP_KEYCODE_F4 = 293;
const SAPP_KEYCODE_F5 = 294;
const SAPP_KEYCODE_F6 = 295;
const SAPP_KEYCODE_F7 = 296;
const SAPP_MODIFIER_SHIFT = 0x1;
const SAPP_MOUSEBUTTON_RIGHT = 1;

const EVENT_NAMES = [
  'keydown', 'keyup', 'char', 'mousedown', 'mouseup', 'mousemove', 'wheel',
  'mouseenter', 'mouseleave', 'resize', 'focus', 'blur', 'minimize',
  'restore', 'close', 'paste', 'drop',
];

/** What phase one injects, as the names JavaScript sees them under. */
const EXPECTED = [
  'keydown', 'char', 'mousemove', 'mousedown', 'wheel', 'blur', 'focus',
  'paste', 'drop', 'close',
];

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

/**
 * Check the log's shape: each event is a `native:<name>` line immediately
 * followed by `js:<name>`, and nothing else; runs of mousemove collapse to
 * one.
 */
function checkOrder(log) {
  const names = [];
  let i = 0;
  while (i < log.length) {
    const line = log[i];
    const m = /^native:(\S+)/.exec(line);
    if (!m) {
      return 'expected a native line at ' + i + ', got "' + line + '"';
    }
    if (log[i + 1] !== 'js:' + m[1]) {
      return 'native:' + m[1] + ' at ' + i + ' is followed by "' +
          log[i + 1] + '", not js:' + m[1];
    }
    if (!(m[1] === 'mousemove' && names[names.length - 1] === 'mousemove')) {
      names.push(m[1]);
    }
    i += 2;
  }
  if (names.join(',') !== EXPECTED.join(',')) {
    return 'sequence ' + names.join(',');
  }
  return null;
}

async function main() {
  const win = await ui.createWindow({
    width: 320,
    height: 240,
    title: 'hermes-node-ui events test',
  });
  const addon = require(addonPath);
  addon.startEventLog();

  const received = {};
  let focusedInBlur = null;
  let focusedInFocus = null;
  let closeSeen = null;
  const closed = new Promise((resolve) => {
    closeSeen = resolve;
  });

  for (const name of EVENT_NAMES) {
    win.on(name, (e) => {
      addon.logLine('js:' + name);
      (received[name] = received[name] || []).push(e);
    });
  }
  win.on('blur', () => {
    focusedInBlur = win.focused;
  });
  win.on('focus', () => {
    focusedInFocus = win.focused;
  });
  win.on('close', (e) => {
    // Cancelled, or the default action would end the process here.
    e.preventDefault();
    closeSeen();
  });

  // --- Phase one: one of everything, in one burst. ---------------------
  ui.__injectEvent(
      {type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A, modifiers: SAPP_MODIFIER_SHIFT});
  ui.__injectEvent({type: 'CHAR', charCode: 0x61});
  for (const dx of [1, 2, 3]) {
    ui.__injectEvent({type: 'MOUSE_MOVE', mouseX: 10, mouseY: 10, mouseDx: dx});
  }
  ui.__injectEvent(
      {type: 'MOUSE_DOWN', button: SAPP_MOUSEBUTTON_RIGHT, mouseX: 5, mouseY: 6});
  ui.__injectEvent({type: 'MOUSE_SCROLL', scrollY: 1});
  ui.__injectEvent({type: 'UNFOCUSED'});
  ui.__injectEvent({type: 'FOCUSED'});
  ui.__injectEvent({type: 'CLIPBOARD_PASTED', clipboard: 'clip'});
  ui.__injectEvent({type: 'FILES_DROPPED', paths: ['/tmp/a', '/tmp/b']});
  ui.__injectEvent({type: 'QUIT_REQUESTED'});

  await closed;
  // A turn of the loop after `close`, so that anything else the dispatch
  // delivered has been delivered. (Not evidence that Sokol's quit was
  // cancelled: an injected QUIT_REQUESTED never asks Sokol to quit. That
  // is test/ui-real-events.js's and test/ui-close.js's.)
  await new Promise((resolve) => setImmediate(resolve));

  const log = addon.takeEventLog();
  const orderError = checkOrder(log);
  report('ORDER', orderError === null, orderError + ' log=' + log.join('|'));

  const keydown = received.keydown && received.keydown[0];
  const moves = received.mousemove || [];
  const dxSum = moves.reduce((sum, e) => sum + e.dx, 0);
  const mousedown = received.mousedown && received.mousedown[0];
  const paste = received.paste && received.paste[0];
  const drop = received.drop && received.drop[0];
  const payloadOk = keydown && keydown.code === 'KeyA' &&
      keydown.shiftKey === true && mousedown && mousedown.button === 2 &&
      dxSum === 6 && paste && paste.text === 'clip' && drop &&
      drop.paths.length === 2 && log.includes('native:keydown key=65 mods=1') &&
      log.includes('native:paste text=clip') &&
      log.includes('native:drop paths=2');
  report(
      'PAYLOAD', payloadOk,
      JSON.stringify({keydown, mousedown, dxSum, paste, drop}));

  // Both halves of the mirror: the JS object, and what the native listener
  // read back through hnui_get_window_state.
  const mirrorOk = focusedInBlur === false && focusedInFocus === true &&
      log.includes('native:blur focused=0') &&
      log.includes('native:focus focused=1');
  report(
      'MIRROR', mirrorOk,
      'blur=' + focusedInBlur + ' focus=' + focusedInFocus);

  // TICKS, in a burst of its own. The CHAR is injected from inside the
  // keydown listener, and the barrier holds that listener until the
  // platform thread has converted and queued it -- so the CHAR is behind
  // the KEY_DOWN in the SAME dispatch pass, whatever the frame timing, and
  // only the checkpoint between two events of one pass can put the promise
  // callback ahead of it. (Two events injected back to back are usually
  // taken by one drain, but nothing makes them: split across two passes,
  // the promise callback runs between the passes whether or not deliver()
  // runs a checkpoint, and this line would pass with the call removed.)
  let ticksBarrier = null;
  await new Promise((resolve) => {
    win.on('keydown', (e) => {
      if (e.keyCode !== SAPP_KEYCODE_F7)
        return;
      Promise.resolve().then(() => addon.logLine('promise:keydown'));
      ui.__injectEvent({type: 'CHAR', charCode: 0x62});
      ticksBarrier = ui.__waitPlatformFrames(1);
    });
    win.once('char', () => resolve());
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F7});
  });
  const ticksLog = addon.takeEventLog();
  const promiseAt = ticksLog.indexOf('promise:keydown');
  const charAt = ticksLog.findIndex((line) => line.startsWith('native:char'));
  report(
      'TICKS',
      ticksBarrier === true && promiseAt !== -1 && charAt !== -1 &&
          promiseAt < charAt,
      'barrier ' + ticksBarrier + ', promise at ' + promiseAt + ', char at ' +
          charAt + ' log=' + ticksLog.join('|'));

  report('CLOSE', (received.close || []).length === 1);

  const state = addon.getWindowState();
  report(
      'STATE',
      state !== null && state.width === win.width &&
          state.framebufferWidth === win.framebufferWidth &&
          state.width > 0 && state.focused === win.focused,
      JSON.stringify(state) + ' vs ' + win.width + '/' +
          win.framebufferWidth);

  // --- Phase two: minimized means no ticks. ---------------------------
  //
  // A frame is requested from the `minimize` listener, which runs after
  // the platform thread has already told the tick machine. Two marker
  // round trips then guarantee that a platform frame went past with the
  // request pending: the first marker is converted in some frame N, whose
  // tick check comes after its injected events, and the second is
  // converted in a frame after N.
  //
  // **The second is injected from setImmediate, not from the first one's
  // listener**, and that is what makes the mutation detectable rather
  // than usually detectable. A tick sent in frame N is a function posted
  // behind the dispatch that is running -- and that dispatch keeps
  // draining for as long as events keep arriving, which under Xvfb (no
  // vsync, so frames come as fast as the platform thread can run them)
  // can include the second marker and the restore. The callback would
  // then run after both and look withheld when it was not. Injected from
  // setImmediate, the second marker can only be queued once that dispatch
  // has returned, so it arrives in a dispatch of its own, posted after
  // the tick, and a tick that was sent has run by then.
  const minimizedResult = await new Promise((resolve) => {
    let rafRan = false;
    let restored = false;
    let ranBeforeSecondMarker = null;
    win.once('minimize', () => {
      requestAnimationFrame(() => {
        rafRan = true;
        resolve({restored, ranBeforeSecondMarker, minimized: win.minimized});
      });
      ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F1});
    });
    win.on('keydown', (e) => {
      if (e.keyCode === SAPP_KEYCODE_F1) {
        setImmediate(
            () => ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F2}));
      } else if (e.keyCode === SAPP_KEYCODE_F2) {
        ranBeforeSecondMarker = rafRan;
        ui.__injectEvent({type: 'RESTORED'});
      }
    });
    win.once('restore', () => {
      restored = true;
    });
    ui.__injectEvent({type: 'ICONIFIED'});
  });
  report(
      'MINIMIZED',
      minimizedResult.ranBeforeSecondMarker === false &&
          minimizedResult.restored === true &&
          minimizedResult.minimized === false,
      JSON.stringify(minimizedResult));

  // --- Phase three: deferred removal of a native listener. -------------
  //
  // Listener A (registered first) removes listener B while event 1 is
  // being delivered. B is later in the same event's list, and must still
  // see event 1; it must not see event 2. The probe's lines are `A:` and
  // `B:` plus the key, and `frame:` for the quiescence read in a frame
  // callback; the first phase's listener keeps logging too, so lines are
  // filtered by prefix.
  addon.takeEventLog();
  addon.startRemovalProbe();
  await new Promise((resolve) => {
    win.on('keydown', (e) => {
      if (e.keyCode === SAPP_KEYCODE_F5) resolve();
    });
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F4});
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F5});
  });
  addon.requestQuiescenceRead();
  await new Promise((resolve) => requestAnimationFrame(() => resolve()));
  const removalLog = addon.takeEventLog().filter(
      (line) => /^(A|B|frame):/.test(line));
  const removalExpected = [
    'A:293 quiescent=0',
    'B:293',
    'A:294 quiescent=0',
    'frame: quiescent=1',
  ];
  report(
      'REMOVAL', JSON.stringify(removalLog) === JSON.stringify(removalExpected),
      removalLog.join('|'));

  // --- Phase four: tracked state survives unrelated events. -----------
  //
  // Last, because it leaves the window minimized, and a minimized window
  // is sent no frame ticks.
  const tracked = await new Promise((resolve) => {
    win.on('keydown', (e) => {
      if (e.keyCode !== SAPP_KEYCODE_F6)
        return;
      const native = addon.getWindowState();
      resolve({
        focused: win.focused,
        minimized: win.minimized,
        nativeFocused: native && native.focused,
        nativeMinimized: native && native.minimized,
      });
    });
    ui.__injectEvent({type: 'UNFOCUSED'});
    ui.__injectEvent({type: 'ICONIFIED'});
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F6});
  });
  report(
      'TRACKED',
      tracked.focused === false && tracked.minimized === true &&
          tracked.nativeFocused === false && tracked.nativeMinimized === true,
      JSON.stringify(tracked));

  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
