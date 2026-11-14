// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Input events from REAL X events. Every other window test injects a
// sapp_event below Sokol (`__injectEvent`), which onFrame hands straight to
// uiHandlePlatformEvent -- so `desc.event_cb`, onEvent's refresh of the
// tracked window state from sapp_*, Sokol's own event production and its
// `_sapp.quit_requested` flag are executed by nothing else. Here a second
// X client (test/fixtures/hnui-demo/x11_window_driver.c) sends the events,
// and Sokol turns them into sapp_events the way it does for a person.
//
// What is asserted, each line of output naming one property:
//
//  KEY      a real KeyPress arrives as `keydown` with code 'KeyA'.
//           (Mutation: remove `desc.event_cb = onEvent`. Nothing then
//           arrives at all; the test waits into its `timeout`, and the
//           EXITCODE line goes red with 124.)
//  RESIZE   a real XResizeWindow produces a `resize` event reporting the
//           new size, with the mirror already at it inside the listener.
//           (Mutation: remove onEvent's refreshTrackedState() -- the
//           event then reports the size the window had before.)
//  GEN      the next frame's hnui_frame_target is a NEW generation of
//           exactly that size, and a frame drawn into it is published and
//           presented: a capture naming that generation and publication,
//           with target and framebuffer both at the new size, matches.
//           This is the resize of the REAL framebuffer --
//           test/ui-resize-generation.js injects its resize, so there the
//           framebuffer never changes size and only the target does.
//  CLOSE    a real close request (WM_DELETE_WINDOW) emits `close`, and the
//           window is still there afterwards: a frame requested from the
//           `close` listener runs, and the driver then reports the window
//           still viewable. This is what pins Ruling 54's
//           sapp_cancel_quit(): only a real close request sets Sokol's
//           quit flag, so the injected QUIT_REQUESTED of ui-events.js has
//           nothing to cancel. (Mutation: remove the sapp_cancel_quit()
//           call. Sokol then leaves its loop and destroys the window, no
//           frame is ever ticked again, and the process does NOT exit --
//           closing still hangs, dz 01a0d88a-6518. So waiting for the
//           frame alone would only ever end at the `timeout`. The driver
//           watches the window and reports its destruction the moment it
//           happens, and the test races that report against the frame:
//           the mutant prints CLOSE WRONG and exits 1 promptly.)
//
// The DEMO run is the reference demo's close button (F8 of the Slice C
// fix wave): a real close request ends test/fixtures/hnui-demo/demo.js
// with status 0, through its `close` listener. The driver's `--close`
// waits for the demo's window to be mapped (no sleeping) and may therefore
// send the request before the demo's script has attached that listener;
// that it still arrives is the early-delivery guarantee (spec: JS API --
// no event is delivered before the window's Promise continuation has
// run). (Mutation: remove the demo's close listener; the demo stays up
// and the run ends at its `timeout` with 124.)
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js: redirections inside `sh -c`, so the
// trace file is the binary's own stderr whatever the display wrapper does,
// and `timeout` because lit has no per-test timeout (dz 01a0d797-e326).
// Ends through process.exit() (Ruling 59).
//
// RUN: timeout 120 %with-xvfb sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon %x11_window_driver %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err
//
// RUN: timeout 120 %with-xvfb sh -c '%hermes-node-ui %source_dir/test/fixtures/hnui-demo/demo.js %hnui_demo_addon js > %t.demo.out 2> %t.demo.err & %x11_window_driver --close "hermes-node-ui demo"; wait $!'; echo "DEMOEXIT $?" | %FileCheck --check-prefix=DEMO %s
// RUN: %FileCheck --check-prefix=DEMOOUT %s < %t.demo.out

// CODE: EXITCODE 0

// CHECK: KEY ok
// CHECK: RESIZE ok
// CHECK: GEN ok
// CHECK: CLOSE ok
// CHECK: DONE

// The close came through the real path and was cancelled where it is
// APPLIED, on the platform thread; and Sokol never left its loop.
// TRACE: quit requested: cancelled, forwarded as close
// TRACE-NOT: sapp_run returned

// DEMO: DEMOEXIT 0
// DEMOOUT: MODE js

'use strict';

const ui = require('hermes-node:ui');
const {spawn} = require('child_process');
const addonPath = process.argv[2];
const driverPath = process.argv[3];

// Unique within this run's display; each lit run has its own server, so
// the pid is belt and braces rather than the thing that makes it unique.
const TITLE = 'hnui real events ' + process.pid;
const RESIZED_WIDTH = 400;
const RESIZED_HEIGHT = 300;

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + detail));
}

/**
 * Start the driver and wait for READY. The handle's send() writes one
 * command and resolves with its answer line; onLost() registers a callback
 * for the driver's asynchronous `LOST` report.
 */
function startDriver() {
  return new Promise((resolve, reject) => {
    const child = spawn(driverPath, [TITLE], {
      env: process.env,
      stdio: ['pipe', 'pipe', 'inherit'],
    });
    const waiting = [];
    const lostCallbacks = [];
    let lost = null;
    let ready = false;
    let buffered = '';
    child.stdout.on('data', (chunk) => {
      buffered += chunk;
      let nl;
      while ((nl = buffered.indexOf('\n')) !== -1) {
        const line = buffered.slice(0, nl);
        buffered = buffered.slice(nl + 1);
        if (!ready && line.startsWith('READY')) {
          ready = true;
          resolve({
            send(command) {
              return new Promise((res) => {
                waiting.push(res);
                child.stdin.write(command + '\n');
              });
            },
            onLost(callback) {
              if (lost !== null)
                callback(lost);
              else
                lostCallbacks.push(callback);
            },
            stop() {
              child.stdin.end();
            },
          });
        } else if (line.startsWith('LOST')) {
          lost = line;
          for (const callback of lostCallbacks.splice(0))
            callback(line);
        } else if (waiting.length > 0) {
          waiting.shift()(line);
        }
      }
    });
    child.on('error', reject);
    child.on('exit', (code) => {
      if (!ready)
        reject(new Error('driver exited with ' + code + ' before READY'));
    });
  });
}

async function main() {
  const win = await ui.createWindow({width: 320, height: 240, title: TITLE});
  const addon = require(addonPath);
  const driver = await startDriver();

  // --- KEY ------------------------------------------------------------
  const keyed = new Promise((resolve) => win.once('keydown', resolve));
  await driver.send('key a');
  const key = await keyed;
  report('KEY', key.code === 'KeyA', JSON.stringify(key));

  // --- RESIZE ---------------------------------------------------------
  // The generation in use before the resize, from a frame drawn into it.
  // **Captured, not just drawn**: a frame whose completion lands after the
  // resize has retired its generation is refused at publication and never
  // numbered, so without waiting for it to present, whether the frame
  // after the resize is publication 1 or 2 would be a race.
  const drawOnce = () => new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve(addon.lastTarget());
    });
  });
  const firstPresented = ui.__captureFrame({
    generation: 1,
    publicationId: 1,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
    giveUpAfterFrames: 900,
    path: process.argv[4] + '.first',
  });
  const before = await drawOnce();
  const beforePresented = await firstPresented;

  const resized = new Promise((resolve) => {
    win.once('resize', (e) => resolve({
      event: e,
      mirrorWidth: win.framebufferWidth,
      mirrorHeight: win.framebufferHeight,
    }));
  });
  await driver.send('resize ' + RESIZED_WIDTH + ' ' + RESIZED_HEIGHT);
  const r = await resized;
  report(
      'RESIZE',
      r.event.framebufferWidth === RESIZED_WIDTH &&
          r.event.framebufferHeight === RESIZED_HEIGHT &&
          r.mirrorWidth === RESIZED_WIDTH &&
          r.mirrorHeight === RESIZED_HEIGHT,
      JSON.stringify(r));

  // --- GEN ------------------------------------------------------------
  // Installed after the frame is drawn and keyed by the generation the
  // addon was given: nothing else draws meanwhile, so the presenter keeps
  // re-blitting this publication and one of those blits matches.
  const after = await drawOnce();
  const presented = await ui.__captureFrame({
    generation: after.generation,
    publicationId: 2,
    targetWidth: RESIZED_WIDTH,
    targetHeight: RESIZED_HEIGHT,
    framebufferWidth: RESIZED_WIDTH,
    framebufferHeight: RESIZED_HEIGHT,
    dpiScale: win.dpiScale,
    giveUpAfterFrames: 900,
    path: process.argv[4],
  });
  report(
      'GEN',
      beforePresented.matched === true &&
          after.generation > before.generation &&
          after.width === RESIZED_WIDTH &&
          after.height === RESIZED_HEIGHT && presented.matched === true &&
          presented.publicationId === 2,
      JSON.stringify({before, beforePresented, after, presented}));

  // --- CLOSE ----------------------------------------------------------
  // Two answers race: a frame ticked after the close request (the window
  // survived it) or the driver's report that the window is gone.
  const outcome = new Promise((resolve) => {
    win.once('close', () => {
      ui.requestAnimationFrame(() => resolve('frame'));
    });
    driver.onLost((line) => resolve(line));
  });
  await driver.send('close');
  const first = await outcome;
  const state = first === 'frame' ? await driver.send('state') : '(none)';
  report(
      'CLOSE',
      first === 'frame' &&
          state === 'STATE viewable=1 ' + RESIZED_WIDTH + 'x' + RESIZED_HEIGHT,
      'first=' + first + ' state=' + state);

  driver.stop();
  console.log('DONE');
  process.exit(first === 'frame' ? 0 : 1);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
