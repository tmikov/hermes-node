// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `requestAnimationFrame` with browser semantics, native and JavaScript
// callbacks together (spec: Frames; Ruling 57: one list, one id space).
//
// Ordering is read from the reference addon's shared log, never from
// stdout: the native callbacks write through C and JavaScript through
// `addon.logLine`, and only the log orders the two -- `printf` and
// `console.log` reach the pipe by different routes.
//
//  ORDER       js1, native1, js2, native2 registered in that order run in
//              that order. (Mutation: run every native callback first.)
//  CHECKPOINT  a `nextTick` and a promise callback queued by cb1 run
//              before cb2; a callback registered from that promise
//              callback runs on a LATER tick. (Mutation: remove the
//              `_tickCallback` call between callbacks.)
//  TIME        one timestamp per tick, on `process.uptime()`'s clock
//              (Ruling 56), and a later tick's is larger. (Mutation: raw
//              `uv_hrtime()` with no origin.)
//  CANCEL      cancelling a later callback of the running batch, from a
//              callback and from a promise callback; cancelling one waiting
//              for the next tick, across kinds; unknown ids and non-numbers;
//              and a tick whose only callback was cancelled still returns
//              to idle, so the next request gets a tick.
//  HANDLED     with an `uncaughtException` listener, a throw from a
//              callback, and one from a `nextTick` callback at the
//              checkpoint after it, reach the listener; the rest of the
//              batch runs; the frame an earlier callback drew is PUBLISHED;
//              and the next request still gets a tick.
//  FATAL       its own RUN line: with no listener the process exits 1,
//              the error is on stderr, `'exit'` listeners ran, and nothing
//              after the throw ran. (Mutation: restore the old
//              print-and-continue block.)
//  GLOBALS     both globals exist once the window does, and not before.
//
// **Why HANDLED's second case throws from `nextTick` and not from a promise
// callback.** A throw in a `.then()` callback is a rejection, and this
// runtime reports an unhandled rejection from a timer, 100 ms or more
// later (lib/bindings/node_task_queue.cpp, "Promise rejection tracking"),
// not at the checkpoint -- so it does not reach the listener between frame
// callbacks at all. A `nextTick` throw is the exception that genuinely
// escapes `process._tickCallback()` at the checkpoint, which is the path
// this task wires.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// The RUN-line shape is ui-present.js's, for the reasons given there:
// redirections INSIDE `sh -c`, so the trace file is the binary's own stderr
// whatever the display wrapper does, and `timeout` because lit has no
// per-test timeout (dz 01a0d797-e326) and several mutations here wait forever
// for a tick that never comes. The main run ends through process.exit()
// (Ruling 59): closing the window still hangs (dz 01a0d88a-6518).
//
// RUN: timeout 300 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon main %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
//
// RUN: timeout 300 %with-xvfb sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon fatal > %t.fatal.out 2> %t.fatal.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=FATALCODE %s
// RUN: %FileCheck --check-prefix=FATAL %s < %t.fatal.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=FATALERR %s < %t.fatal.err

'use strict';

const ui = require('hermes-node:ui');

const addonPath = process.argv[2];
const mode = process.argv[3];
const capturePath = process.argv[4];

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

/** Resolves with the timestamp of the next tick. */
function nextFrame() {
  return new Promise((resolve) => requestAnimationFrame(resolve));
}

async function main() {
  const globalsBefore =
      typeof globalThis.requestAnimationFrame + ',' +
      typeof globalThis.cancelAnimationFrame;

  const win = await ui.createWindow({
    width: 320,
    height: 240,
    title: 'hermes-node-ui rAF test',
  });
  const addon = require(addonPath);
  const log = (line) => addon.logLine(line);
  const take = () => addon.takeEventLog().join(',');

  if (mode === 'fatal') {
    runFatal();
    return;
  }

  // --- GLOBALS --------------------------------------------------------
  const globalsAfter =
      typeof requestAnimationFrame + ',' + typeof cancelAnimationFrame;
  report(
      'GLOBALS',
      globalsBefore === 'undefined,undefined' &&
          globalsAfter === 'function,function' &&
          typeof ui.cancelAnimationFrame === 'function',
      'before=' + globalsBefore + ' after=' + globalsAfter +
          ' exported=' + typeof ui.cancelAnimationFrame);

  // --- ORDER ----------------------------------------------------------
  take();
  await new Promise((resolve) => {
    requestAnimationFrame(() => log('js1'));
    addon.requestNativeFrame('native1');
    requestAnimationFrame(() => log('js2'));
    addon.requestNativeFrame('native2');
    requestAnimationFrame(resolve);
  });
  const order = take();
  report('ORDER', order === 'js1,native:native1,js2,native:native2', order);

  // --- CHECKPOINT -----------------------------------------------------
  let cb1Stamp = 0;
  let laterStamp = 0;
  await new Promise((resolve) => {
    requestAnimationFrame((ts) => {
      cb1Stamp = ts;
      log('cb1');
      process.nextTick(() => log('tick'));
      Promise.resolve().then(() => {
        log('micro');
        // Registered at a checkpoint INSIDE the batch: belongs to the next.
        requestAnimationFrame((later) => {
          laterStamp = later;
          log('fromMicro');
          resolve();
        });
      });
    });
    requestAnimationFrame(() => log('cb2'));
  });
  const checkpoint = take();
  report(
      'CHECKPOINT',
      checkpoint === 'cb1,tick,micro,cb2,fromMicro' && laterStamp > cb1Stamp,
      checkpoint + ' cb1Stamp=' + cb1Stamp + ' laterStamp=' + laterStamp);

  // --- TIME -----------------------------------------------------------
  const before = process.uptime() * 1000;
  const stamps = [];
  let innerStamp = 0;
  await new Promise((resolve) => {
    const record = (ts) => stamps.push({ts, after: process.uptime() * 1000});
    requestAnimationFrame(record);
    requestAnimationFrame(record);
    requestAnimationFrame((ts) => {
      record(ts);
      requestAnimationFrame((inner) => {
        innerStamp = inner;
        resolve();
      });
    });
  });
  const sameTick = stamps.length === 3 &&
      stamps.every((s) => s.ts === stamps[0].ts);
  const onUptimeClock =
      stamps.every((s) => before - 1 <= s.ts && s.ts <= s.after + 1);
  report(
      'TIME',
      sameTick && onUptimeClock && innerStamp > stamps[0].ts,
      'before=' + before + ' stamps=' + JSON.stringify(stamps) +
          ' inner=' + innerStamp);

  // --- CANCEL ---------------------------------------------------------
  //
  // (a) Inside a batch. cb3 is a NATIVE callback cancelled from
  // JavaScript, so the one id space is exercised too.
  take();
  await new Promise((resolve) => {
    let cb2 = 0;
    let cb3 = 0;
    let self = 0;
    requestAnimationFrame(() => {
      log('cb1');
      cancelAnimationFrame(cb3);
      Promise.resolve().then(() => {
        cancelAnimationFrame(cb2);
        log('micro-cancel');
      });
    });
    cb2 = requestAnimationFrame(() => log('cb2'));
    cb3 = addon.requestNativeFrame('cb3');
    // A callback cancelling itself: it is already running, so nothing.
    self = requestAnimationFrame(() => {
      cancelAnimationFrame(self);
      log('self');
    });
    requestAnimationFrame(resolve);
  });
  const inBatch = take();

  // (b) Waiting for the next tick, cancelled before it, both kinds both
  // ways round; plus ids that name nothing.
  const x = requestAnimationFrame(() => log('x'));
  cancelAnimationFrame(x);
  const y = requestAnimationFrame(() => log('y'));
  addon.cancelNativeFrame(y);
  const n = addon.requestNativeFrame('n');
  cancelAnimationFrame(n);
  let ranId = 0;
  ranId = requestAnimationFrame(() => log('ran'));
  await nextFrame();
  let threw = null;
  try {
    cancelAnimationFrame(ranId);
    cancelAnimationFrame(999999);
    cancelAnimationFrame(0);
    cancelAnimationFrame(-1);
    cancelAnimationFrame();
    cancelAnimationFrame('abc');
    cancelAnimationFrame(String(ranId));
    cancelAnimationFrame({});
    addon.cancelNativeFrame(999999);
    addon.cancelNativeFrame(0);
  } catch (e) {
    threw = e;
  }
  await nextFrame();
  const nextTickCancel = take();

  // (c) A tick whose only callback was cancelled. The request stands --
  // cancelling does not withdraw it -- so a tick runs and runs nothing.
  // Presented frames are then let go past (a capture that can never
  // match, counted in frames rather than time) so that empty tick has run
  // before the next request is made; the platform thread posts ticks and
  // capture results through the same FIFO. If the empty tick did not
  // return the machine to idle, the request below would never be served
  // and this would hang into the RUN line's timeout.
  //
  // A capture counts only frames that present something, so one frame is
  // drawn first: publication 1.
  await new Promise((resolve) => requestAnimationFrame(() => {
    addon.drawFixedPattern();
    resolve();
  }));
  const only = requestAnimationFrame(() => log('only'));
  cancelAnimationFrame(only);
  const quiet = await ui.__captureFrame({
    path: capturePath + '.never',
    publicationId: 1000000,
    giveUpAfterFrames: 30,
    generation: 1,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
  });
  requestAnimationFrame(() => log('afterEmpty'));
  await nextFrame();
  const afterEmpty = take();

  report(
      'CANCEL',
      inBatch === 'cb1,micro-cancel,self' && threw === null &&
          nextTickCancel === 'ran' && quiet.matched === false &&
          afterEmpty === 'afterEmpty',
      'inBatch=' + inBatch + ' threw=' + threw + ' nextTick=' +
          nextTickCancel + ' quiet=' + JSON.stringify(quiet) +
          ' afterEmpty=' + afterEmpty);

  // --- HANDLED (last: it installs a listener) --------------------------
  process.on('uncaughtException', (e) => log('listener:' + e.message));

  // CANCEL (c) drew publication 1, so cb0's frame is publication 2. The
  // capture is installed BEFORE the frame is requested, as ui-present.js
  // explains.
  // It is what proves the handled exception did not skip the submission:
  // a tick that bailed out after cb1's throw would never publish, the
  // machine would never leave CALLBACKS_RUNNING, and this would report
  // matched=false after its frame budget -- with the `later` tick below
  // never running at all.
  const published = ui.__captureFrame({
    path: capturePath,
    publicationId: 2,
    giveUpAfterFrames: 900,
    generation: 1,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
  });
  requestAnimationFrame(() => {
    addon.drawFixedPattern();
    log('cb0');
  });
  requestAnimationFrame(() => {
    log('cb1');
    throw new Error('boom1');
  });
  requestAnimationFrame(() => log('cb2'));
  const capture = await published;
  await nextFrame();
  log('later');

  requestAnimationFrame(() => {
    log('cb3');
    process.nextTick(() => {
      throw new Error('boom2');
    });
  });
  requestAnimationFrame(() => log('cb4'));
  await nextFrame();
  log('later2');

  const handled = take();
  report(
      'HANDLED',
      handled ===
              'cb0,cb1,listener:boom1,cb2,later,cb3,listener:boom2,cb4,later2' &&
          capture.matched === true && capture.publicationId === 2,
      handled + ' capture=' + JSON.stringify(capture));

  console.log('DONE');
  process.exit(0);
}

/**
 * No `uncaughtException` listener: the first callback's throw is fatal.
 * The second callback must never run; if it does, it exits 0 itself, which
 * is exactly what the old print-and-continue behaviour produced.
 */
function runFatal() {
  process.on('exit', (code) => console.log('EXIT listener code=' + code));
  requestAnimationFrame(() => {
    console.log('cb1');
    throw new Error('fatal-boom');
  });
  requestAnimationFrame(() => {
    console.log('AFTER');
    process.exit(0);
  });
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CODE: EXITCODE 0
// CHECK: GLOBALS ok
// CHECK-NEXT: ORDER ok
// CHECK-NEXT: CHECKPOINT ok
// CHECK-NEXT: TIME ok
// CHECK-NEXT: CANCEL ok
// CHECK-NEXT: HANDLED ok
// CHECK-NEXT: DONE

// The empty tick of CANCEL (c), traced where the batch ran.
// TRACE: frame tick: 0 callback(s) ran, nothing drawn

// FATALCODE: EXITCODE 1
// FATAL: cb1
// FATAL-NOT: AFTER
// FATAL: EXIT listener code=1
// FATAL-NOT: AFTER
// FATALERR: fatal-boom
