// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Maintenance on Metal (Task 16d, the plan's Design (a)). Sokol defers the
// Metal release of every destroyed resource to a later sg_commit(), so a
// program that resizes and then draws nothing would never get a retired
// generation's textures back: nothing else commits. The backend's
// maintenance, run from the worker's loop turn when nothing it submitted is
// in flight, issues the empty commits that collect them.
//
// The script draws one frame that also creates and destroys a buffer
// (churnBufferThisFrame: a release queued in a frame about to be in
// flight), then issues twelve real resizes alternating 400x300 and 320x240
// with NO requestAnimationFrame between them -- the idle case, in which
// nothing but maintenance commits -- waiting for each `resize` event and
// then for one loop turn, then draws one frame and captures it at the
// final size.
//
// **The loop turn is what makes each resize a separate idle period.** The
// in-process driver applies a resize synchronously, and event delivery
// keeps delivering while the input queue is non-empty, so without it the
// `await` chain resumes inside the delivery of the previous `resize`, the
// next resize's event lands in the same pass, and all twelve run in ONE
// dispatcher pass. Measured: every retired generation's destruction --
// posted to that same dispatcher -- then waits for the pass to end, and
// all eleven run back to back just before the final frame. A user, or the
// X11 driver, delivers each resize on a turn of its own.
//
// Generation 1 is not among the destroyed: it holds the presented frame
// until a frame of a later generation is presented, which is the final
// capture, and it is released after that, on the way out. So the twelve
// resizes retire generations 2 to 12 -- the twelfth resize retires the
// generation the eleventh started -- and generation 13 is the one drawn.
//
// What is asserted:
//  - for each of generations 2 to 12, `generation <g>: surfaces destroyed`
//    and after it -- before any other generation's -- a `maintenance:`
//    line that collected everything (`pending 0`) in one to four commits;
//    and no maintenance line anywhere leaves anything pending, so each
//    generation was collected by one pass;
//  - the id pool does not drain: the last `idpool free` value is within 16
//    of the first (awk over the trace);
//  - the final frame, in the last generation, is presented and captured.
//
// (Mutations: never run maintenance -- no `maintenance:` lines; one commit
// instead of the loop -- `pending` is not 0.)
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sokol-' --implicit-check-not='pending {{[1-9]}}' %s < %t.err
// RUN: awk '/maintenance:/ { split($0, a, "idpool free "); split(a[2], b, " "); sub(",", "", b[3]); if (first == "") first = b[1]; last = b[3] } END { d = last - first; if (d < 0) d = -d; print "POOL first=" first " last=" last " " ((first != "" && d <= 16) ? "ok" : "DRAINED") }' %t.err | %FileCheck --check-prefix=POOL %s

// CODE: EXITCODE 0

// CHECK: CHURN true
// CHECK-NEXT: RESIZES 12
// CHECK-NEXT: FINAL matched=true
// CHECK-NEXT: DONE

// POOL: POOL first={{[0-9]+}} last={{[0-9]+}} ok

// TRACE: [ui] generation 2: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 3: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 4: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 5: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 6: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 7: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 8: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 9: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 10: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 11: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] generation 12: surfaces destroyed
// TRACE-NOT: surfaces destroyed
// TRACE: [ui] maintenance: {{[1-4]}} commit(s), idpool free {{[0-9]+}} -> {{[0-9]+}}, pending 0
// TRACE: [ui] capture: written generation 13 publication 2

'use strict';

const ui = require('hermes-node:ui');
const {startDriver} = require('./fixtures/hnui-demo/window_driver.js');
const addonPath = process.argv[2];
const capturePath = process.argv[3];

const TITLE = 'hnui maintenance ' + process.pid;
const SIZES = [[400, 300], [320, 240]];

async function main() {
  const win = await ui.createWindow({width: 320, height: 240, title: TITLE});
  const addon = require(addonPath);
  const driver = await startDriver({ui, driverPath: '', title: TITLE});

  // One frame that draws and churns a buffer, presented before anything
  // else happens, so the first generation is the one on screen.
  const firstShown = ui.__captureFrame({
    generation: 1,
    publicationId: 1,
    targetWidth: 320,
    targetHeight: 240,
    framebufferWidth: 320,
    framebufferHeight: 240,
    dpiScale: win.dpiScale,
    giveUpAfterFrames: 900,
    path: capturePath + '.first',
  });
  const churned = await new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve(addon.churnBufferThisFrame());
    });
  });
  await firstShown;
  console.log('CHURN ' + churned);

  // Twelve real resizes, and no frame requested between them.
  let resizes = 0;
  for (let i = 0; i < 12; i++) {
    const [width, height] = SIZES[i % 2];
    const resized = new Promise((resolve) => win.once('resize', resolve));
    await driver.send('resize ' + width + ' ' + height);
    await resized;
    // One loop turn: see the header.
    await new Promise((resolve) => setImmediate(resolve));
    resizes++;
  }
  console.log('RESIZES ' + resizes);

  // The last resize was to 320x240; one frame there, captured.
  const [finalWidth, finalHeight] = SIZES[1];
  const target = await new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve(addon.lastTarget());
    });
  });
  const final = await ui.__captureFrame({
    generation: target.generation,
    publicationId: 2,
    targetWidth: finalWidth,
    targetHeight: finalHeight,
    framebufferWidth: finalWidth,
    framebufferHeight: finalHeight,
    dpiScale: win.dpiScale,
    giveUpAfterFrames: 900,
    path: capturePath,
  });
  console.log('FINAL matched=' + final.matched);

  driver.stop();
  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
