// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// harness.adopt(app): a handle for a window the harness did not open,
// whose `app.frame` is already a whole frame (its own newFrame() and
// render()). The application here is the smallest there is: a window
// opened directly with ui.createWindow, and a frame drawing part A's
// `fill` scene (the whole display red).
//
// Cases, in order, one line each (`<CASE> ok`; a failure prints its own
// line and exits 1):
//
//  ADOPT     adopt() resolves for an application that has drawn nothing,
//            with h.published 0. (It used to call resync() first, which
//            never settles before the first publication: the host counts
//            a capture request's frames as it presents them.)
//  RUN       h.run() runs app.frame as one tick and resolves with what its
//            render() returned (true), and h.published is then 1.
//  CAPTURE   harness.capture(h, null, ...) matches the next publication,
//            so the count adopt() started from was right; the picture is
//            the fill's red.
//  RESYNC    harness.resync(h), once something has been published, reads
//            the same count back from the host.
//  MOVE      harness.moveTo(h, null, [100, 120]): a helper given `null`
//            for its draw function runs app.frame and ImGui reads the
//            position back.
//
// THE MUTATION THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  resync(h) called by adopt() again: ADOPT never prints and the run
//  ends at its time limit (status 124).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err
//
// CODE: EXITCODE 0
// CHECK: ADOPT ok
// CHECK-NEXT: RUN ok
// CHECK-NEXT: CAPTURE ok
// CHECK-NEXT: RESYNC ok
// CHECK-NEXT: MOVE ok
// TRACE: imgui: context created

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePrefix = process.argv[2];

function fail(line) {
  console.log(line);
  process.exit(1);
}

async function main() {
  const ui = require('hermes-node:ui');
  const win = await ui.createWindow(
      {title: 'hnui imgui adopt ' + process.pid, width: 640, height: 480});
  const imgui = require('hermes-node:imgui');
  const app = {
    ui,
    win,
    imgui,
    frame() {
      imgui.newFrame();
      imgui.__test.scene('fill');
      return imgui.render();
    },
  };

  const h = await harness.adopt(app);
  if (h.published !== 0)
    fail('ADOPT WRONG published=' + h.published);
  console.log('ADOPT ok');

  const drew = await h.run(null);
  if (drew !== true || h.published !== 1)
    fail('RUN WRONG drew=' + drew + ' published=' + h.published);
  console.log('RUN ok');

  const image = await harness.capture(h, null, capturePrefix + '.fill.ppm');
  const px = harness.pixelAt(image, 320, 240);
  if (px[0] !== 255 || px[1] !== 0 || px[2] !== 0 || h.published !== 2)
    fail('CAPTURE WRONG ' + px + ' published=' + h.published);
  console.log('CAPTURE ok');

  const counted = h.published;
  await harness.resync(h);
  if (h.published !== counted)
    fail('RESYNC WRONG ' + h.published + ' want ' + counted);
  console.log('RESYNC ok');

  await harness.moveTo(h, null, [100, 120]);
  console.log('MOVE ok');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack || e));
  process.exit(1);
});
