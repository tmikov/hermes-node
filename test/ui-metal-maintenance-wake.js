// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Deferred maintenance is retried in an idle program (Task 16d review, A).
// Maintenance runs from the worker's loop turn, and only when no producer
// frame is in flight (metal_backend.mm says why). When it is put off, the
// completion that ends the last frame in flight must wake the loop: in an
// idle program nothing else turns it, and the retired generation's
// textures would stay until something unrelated did.
//
// A frame is in flight for as long as the GPU takes to draw it, which no
// test can aim at. So the test holds it: `gpu-hold` makes the producer's
// next frame wait on the GPU for a shared event (metal_test_hooks.h).
// `gpu-release` answers at once, and the event is signalled from another
// thread only after the worker's next loop turn has finished -- so the
// frame completes while the loop has nothing to do, which is the case the
// wake is for. A completion that landed while the script was still
// running would be collected by that same turn's own check, wake or no
// wake (measured: the first version of this test released synchronously
// and stayed green with the wake removed).
//
// The script:
//  1. draws and captures one frame, so generation 1 is on screen;
//  2. holds the GPU and draws one more frame, which stays in flight;
//  3. resizes twice, yielding loop turns, so generation 2 is started and
//     retired with nothing referring to it: it is destroyed, maintenance
//     is due, and the turn finds the held frame in flight -- `deferred`;
//  4. from a timer callback, releases the GPU and queues one empty
//     immediate, so that this loop turn reaches its check phase -- the
//     turn the release waits for -- without blocking in poll first;
//  5. and in the same callback asks for a capture that cannot match
//     (generation 3 never draws), with a budget of 60 presented frames.
//     Then the script does nothing. When the budget runs out the platform
//     thread posts the answer through the owner dispatcher, and its
//     continuation writes `[test] capture gave up` to stderr --
//     synchronously, stderr being a file here -- and exits.
//
// **What is asserted is that the wake arrives, not that nothing else would
// have collected the release.** Any loop turn runs the check phase, and so
// maintenance, and three ends tried for "nothing else turned the loop"
// each let one in -- all measured with the wake's post removed. A
// one-second exit timer: libuv's poll returned a millisecond early and
// that turn's check ran first. process.exit() itself: it flushes through
// a nested uv_run, whose check phase runs maintenance. This capture's
// continuation: the worker's check ran before the microtask drain. So the
// trace asserts `worker loop: woken` -- printed by the function the wake
// posts, when the worker's loop runs it -- and the maintenance line after
// the handler's `woken by completion`, in either order, both before the
// marker. The marker bounds the run; it proves no absence.
//
// Each yield in step 3 is a setImmediate. Three after the second resize:
// the in-process driver's resize is delivered nested inside the previous
// event's delivery (ui-metal-maintenance.js says why), the destruction it
// causes is posted from there and drained on the next turn, and the
// worker's check after that drain is the one that defers. Three turns
// cover that without depending on the order of check handles in a turn.
//
// (Mutation: drop the wake's post -- `worker loop: woken` is missing, and
// the run still ends when the capture gives up.)
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sokol-' --implicit-check-not='pending {{[1-9]}}' %s < %t.err

// CODE: EXITCODE 0

// CHECK: FIRST matched=true
// CHECK-NEXT: HELD SENT gpu-hold
// CHECK-NEXT: RESIZES 2
// CHECK-NEXT: RELEASED in_flight=1
// CHECK-NEXT: NEVER matched=false
// CHECK-NEXT: DONE

// TRACE: [ui] capture: written generation 1 publication 1
// TRACE: [ui] drive: gpu-hold applied (held=1)
// TRACE: [ui] generation 2: surfaces destroyed
// TRACE: [ui] maintenance: deferred, 1 frame(s) in flight
// TRACE-NOT: maintenance:
// TRACE: [ui] drive: gpu-release applied (in_flight=1)
// TRACE-NOT: maintenance:
// TRACE: [ui] test hook: producer GPU released after a worker loop turn
// TRACE-NOT: maintenance:
// TRACE: [ui] maintenance: woken by completion
// TRACE-DAG: [ui] worker loop: woken
// TRACE-DAG: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [test] capture gave up

'use strict';

const ui = require('hermes-node:ui');
const {startDriver} = require('./fixtures/hnui-demo/window_driver.js');
const addonPath = process.argv[2];
const capturePath = process.argv[3];

const TITLE = 'hnui maintenance wake ' + process.pid;

const turn = () => new Promise((resolve) => setImmediate(resolve));

async function main() {
  const win = await ui.createWindow({width: 320, height: 240, title: TITLE});
  const addon = require(addonPath);
  const driver = await startDriver({ui, driverPath: '', title: TITLE});
  const drawOnce = () => new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve();
    });
  });

  // 1. Generation 1 on screen.
  const first = ui.__captureFrame({
    generation: 1,
    publicationId: 1,
    targetWidth: 320,
    targetHeight: 240,
    framebufferWidth: 320,
    framebufferHeight: 240,
    dpiScale: win.dpiScale,
    giveUpAfterFrames: 900,
    path: capturePath,
  });
  await drawOnce();
  console.log('FIRST matched=' + (await first).matched);

  // 2. One frame held in flight.
  console.log('HELD ' + (await driver.send('gpu-hold')));
  await drawOnce();

  // 3. Generation 2 started and retired, with nothing referring to it.
  let resizes = 0;
  for (const [width, height] of [[400, 300], [320, 240]]) {
    const resized = new Promise((resolve) => win.once('resize', resolve));
    await driver.send('resize ' + width + ' ' + height);
    await resized;
    resizes++;
    await turn();
  }
  await turn();
  await turn();
  console.log('RESIZES ' + resizes);

  // 4. The held frame completes after this turn's check phase; its
  // handler wakes the loop. Called directly rather than through
  // driver.send(), whose answer arrives by a Promise.
  setTimeout(() => {
    const released = String(ui.__driveWindow('gpu-release'));
    setImmediate(() => {});
    // 5. Idle until a request that cannot match gives up.
    ui.__captureFrame({
      generation: 3,
      publicationId: 1,
      targetWidth: 320,
      targetHeight: 240,
      framebufferWidth: 320,
      framebufferHeight: 240,
      dpiScale: win.dpiScale,
      giveUpAfterFrames: 60,
      path: capturePath + '.never',
    }).then((never) => {
      process.stderr.write('[test] capture gave up\n');
      driver.stop();
      console.log('RELEASED ' + released);
      console.log('NEVER matched=' + never.matched);
      console.log('DONE');
      process.exit(0);
    });
  }, 0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
