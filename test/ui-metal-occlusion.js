// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The invalid swapchain on macOS (Task 16d). A miniaturized window is the
// one way a test can make Sokol report its swapchain invalid on demand:
// windowDidMiniaturize: starts Sokol's obscured timer, and while it runs
// sapp_acquire_swapchain() answers `invalid` (sokol_app.h,
// _sapp_macos_mtl_is_obscured). Covering the window does not do it -- a
// window covered from the moment it opens keeps a valid swapchain, measured
// in Task 16b -- so this drives a real miniaturize through ui.__driveWindow.
//
// What is asserted:
//  - FIRST: a frame drawn before miniaturizing is presented and captured.
//  - the miniaturize reaches the program as a `minimize` event; the
//    driver's read-back, taken once AppKit says the animation is over, is
//    `miniaturized=1`; and `state` then reports `viewable=0` -- the
//    window's occlusion state no longer has NSWindowOcclusionStateVisible;
//  - while it is, frames still arrive (from Sokol's NSTimer): the script
//    waits for ten of them (WAITED), and each submits nothing:
//    `present: swapchain invalid; nothing submitted` is printed on entering
//    the state, and no `frame tick:` until `present: swapchain valid
//    again` -- a requestAnimationFrame made while miniaturized does not run
//    until the window can be seen (the plan's Ruling 88);
//  - the deminiaturize reaches the program as `restore`, and a frame drawn
//    afterwards is presented and captured: SECOND.
//
// Both transition lines are printed by the backend only on the transition,
// after what they describe was observed: `invalid` from presentInvalid(),
// which onFrame calls only when the acquisition failed, and `valid again`
// from acquireFrameSwapchain() after one succeeded.
//
// **What the `frame tick:` check cannot pin.** Two guards each keep a
// frame callback from running while the window is miniaturized: onFrame's
// early return on an invalid swapchain, and TickMachine::shouldSendTick
// declining while `minimized_` is set. Removing either one alone leaves the
// check green, because the other still holds; it fails only if both go.
// A test that pinned onFrame's guard alone would need an occlusion that
// invalidates the swapchain without minimizing, and none could be produced
// on demand: a covered window keeps a valid swapchain.
//
// (Mutations, as measured in Task 16d. In acquireSwapchain, ignore
// `swapchain.invalid`: green, with an identical trace -- while the window
// is miniaturized Sokol hands back a nil drawable, so the nil guard
// refuses the frame anyway. Remove the nil guard too: red only at the
// missing `swapchain invalid` line. The run still exits 0 with both
// captures written and no `[sokol-` line, because Sokol's Metal
// begin_pass itself skips a nil drawable -- sokol_gfx.h, "current_drawable
// will not be valid if window is minimized".)
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err
// RUN: %FileCheck --check-prefix=DRIVE %s < %t.err

// CODE: EXITCODE 0

// CHECK: FIRST matched=true
// CHECK-NEXT: MINIMIZED ok
// CHECK-NEXT: WAITED true
// CHECK-NEXT: RESTORED ok
// CHECK-NEXT: SECOND matched=true
// CHECK-NEXT: DONE

// TRACE: [ui] capture: written generation 1 publication 1
// TRACE: [ui] drive: miniaturize applied (miniaturized=1)
// TRACE: [ui] present: swapchain invalid; nothing submitted
// TRACE-NOT: frame tick:
// TRACE: [ui] present: swapchain valid again
// TRACE: [ui] capture: written generation 1 publication 2

// The driver's own read-backs, taken once AppKit says the animation is
// over. Where `deminiaturize`'s line falls against `valid again` is not
// fixed, so it has its own prefix; it must come before the second capture,
// which the script requests only after the driver has answered.
// DRIVE: [ui] drive: miniaturize applied (miniaturized=1)
// DRIVE: [ui] drive: deminiaturize applied (miniaturized=0)
// DRIVE: [ui] capture: written generation 1 publication 2

'use strict';

const ui = require('hermes-node:ui');
const {startDriver} = require('./fixtures/hnui-demo/window_driver.js');
const addonPath = process.argv[2];
const capturePath = process.argv[3];

const TITLE = 'hnui occlusion ' + process.pid;

async function main() {
  const win = await ui.createWindow({width: 320, height: 240, title: TITLE});
  const addon = require(addonPath);
  const driver = await startDriver({ui, driverPath: '', title: TITLE});

  const capture = (publicationId, suffix) => ui.__captureFrame({
    generation: 1,
    publicationId,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
    giveUpAfterFrames: 900,
    path: capturePath + suffix,
  });
  const drawOnce = () => new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve();
    });
  });

  // --- FIRST ------------------------------------------------------------
  const first = capture(1, '.first');
  await drawOnce();
  console.log('FIRST matched=' + (await first).matched);

  // --- miniaturized -----------------------------------------------------
  const minimized = new Promise((resolve) => win.once('minimize', resolve));
  const miniAnswer = await driver.send('miniaturize');
  await minimized;
  const state = String(ui.__driveWindow('state'));
  console.log(
      'MINIMIZED ' +
      (miniAnswer === 'SENT miniaturize' && state.startsWith('viewable=0') ?
           'ok' :
           'WRONG answer=' + miniAnswer + ' state=' + state));

  // A frame requested now must not tick while the window is miniaturized.
  // The platform thread keeps running frames meanwhile; ten of them, each
  // finding the swapchain invalid.
  // The trace's TRACE-NOT is what checks it.
  ui.requestAnimationFrame(() => {});
  console.log('WAITED ' + ui.__waitPlatformFrames(10));

  // --- restored ---------------------------------------------------------
  const restored = new Promise((resolve) => win.once('restore', resolve));
  const deminiAnswer = await driver.send('deminiaturize');
  await restored;
  console.log(
      'RESTORED ' +
      (deminiAnswer === 'SENT deminiaturize' ? 'ok' :
                                                'WRONG answer=' + deminiAnswer));

  // --- SECOND -----------------------------------------------------------
  const second = capture(2, '');
  await drawOnce();
  console.log('SECOND matched=' + (await second).matched);

  driver.stop();
  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
