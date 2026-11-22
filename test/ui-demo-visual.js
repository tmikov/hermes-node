// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The reference addon's visual mode -- a triangle drawn with sokol_gl -- in
// both of demo.js's frame-driving modes, `js` and `native`.
//
// This file starts the demo through demo.js's own `startDemo()`, so what is
// checked is the wiring a person runs by hand, not a copy of it; then it
// adds what a test needs and the demo must not carry: a frozen rotation, a
// capture, and an injected key.
//
// What is asserted, per mode:
//
//  FIRST      a capture of the next frame drawn after the rotation is
//             frozen: the four corners are the first clear colour (white)
//             EXACTLY, and the centre is not -- the triangle drew there,
//             which is the evidence that sokol_gl's default pipeline, left
//             to take its colour format and sample count from the host's
//             sg_setup environment, was accepted by the pass the host's
//             surface defines. (Its depth format cannot be left to the
//             environment, which demo_visual.c's header explains; stop
//             stating it there and validation rejects the pipeline, the
//             triangle is not drawn, and both this line and the implicit
//             check on '[sokol-' in the RUN lines go red.)
//  KEY        after an injected KEY_DOWN has reached the addon's
//             hnui_add_event_listener listener, the next captured frame's
//             corners are the second clear colour (blue), exactly.
//             (Mutation: do not register the event listener -> the corners
//             stay white and KEY reads WRONG.)
//  and, from tracing, that both captures were of real publications.
//
// --- The comparison policy ---------------------------------------------
//
// Corners are compared EXACTLY, the centre only for INEQUALITY. The corners
// are sampled 8 px in from the edges, and the triangle is kept inside the
// target's inscribed circle with a margin (demo_visual.c), so at EVERY
// angle nothing but the clear reaches them: a flat colour whose channels
// are all 0 or 255, through a 1:1 blit (the agreement fields name equal
// target and framebuffer sizes) with no blending and no sRGB conversion --
// the reasoning test/ui-present.js gives at length. The bound is what keeps
// the corners clear, not the rotation freeze; the freeze only makes the
// captured frame one fixed picture rather than whichever angle the clock
// had reached. The
// centre is a blend of the three vertex colours, a value that depends on
// interpolation precision, so it is not compared for a value; only for
// being something other than the clear.
//
// --- Naming the publication to capture --------------------------------
//
// The demo's loop runs freely, so the publication to capture is computed:
// with nothing else drawing, each frame the demo draws is exactly one
// publication, in order, so the next frame drawn is publication
// `demoFramesDrawn() + 1`. The request is installed synchronously on the JS
// thread, which is also the thread that draws, so that frame cannot have
// been drawn -- let alone presented -- before the request exists.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-present.js: redirections inside `sh -c`, so the
// trace file is the binary's own stderr whatever the display wrapper does,
// and `timeout` because lit has no per-test timeout (dz 01a0d797-e326). Ends
// through process.exit(): its subject is not closing.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon js %t.js.ppm > %t.js.out 2> %t.js.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.js.out
// RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.js.err
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon native %t.native.ppm > %t.native.out 2> %t.native.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.native.out
// RUN: %FileCheck --check-prefixes=TRACE,NATIVETRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.native.err

// CODE: EXITCODE 0

// CHECK: STARTED
// CHECK-NEXT: FIRST matched=true
// CHECK-NEXT: FIRST corners ok
// CHECK-NEXT: FIRST centre ok
// CHECK-NEXT: KEY matched=true
// CHECK-NEXT: KEY corners ok
// CHECK-NEXT: DONE

// The native mode's frames are requested by the addon itself.
// NATIVETRACE: hnui_request_animation_frame: id

// Both captures were of real publications, and neither sokol_gfx nor
// sokol_gl reported anything (the implicit check on '[sokol-' above).
// TRACE: capture: written generation 1 publication {{[0-9]+}}
// TRACE: capture: written generation 1 publication {{[0-9]+}}

'use strict';

const fs = require('fs');
const path = require('path');
const ui = require('hermes-node:ui');
const demo = require(path.join(__dirname, 'fixtures', 'hnui-demo', 'demo.js'));

const addonPath = process.argv[2];
const mode = process.argv[3];
const capturePath = process.argv[4];

const WHITE = [255, 255, 255];
const BLUE = [0, 0, 255];
// Any key does; Sokol's numbering, because this is a platform event.
const SAPP_KEYCODE_SPACE = 32;

/** Parse the binary PPM (P6) the capture hook wrote (as ui-present.js). */
function readPpm(file) {
  const buf = fs.readFileSync(file);
  let offset = 0;
  function token() {
    while (offset < buf.length && /\s/.test(String.fromCharCode(buf[offset])))
      offset++;
    const start = offset;
    while (offset < buf.length && !/\s/.test(String.fromCharCode(buf[offset])))
      offset++;
    return buf.toString('ascii', start, offset);
  }
  const magic = token();
  const width = parseInt(token(), 10);
  const height = parseInt(token(), 10);
  const maxValue = parseInt(token(), 10);
  offset++;
  if (magic !== 'P6' || maxValue !== 255)
    throw new Error('not a P6/255 PPM: ' + magic + ' ' + maxValue);
  return {width, height, data: buf.slice(offset)};
}

function pixelAt(image, x, y) {
  const i = (y * image.width + x) * 3;
  return [image.data[i], image.data[i + 1], image.data[i + 2]];
}

function same(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

function corners(image) {
  const m = 8;
  return [
    pixelAt(image, m, m),
    pixelAt(image, image.width - 1 - m, m),
    pixelAt(image, m, image.height - 1 - m),
    pixelAt(image, image.width - 1 - m, image.height - 1 - m),
  ];
}

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

async function main() {
  const {win, addon} = await demo.startDemo(addonPath, mode);
  console.log('STARTED');
  addon.freezeRotation(0);

  const agreement = {
    generation: 1,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
  };
  // See "Naming the publication to capture" above.
  const captureNext = (file) => ui.__captureFrame(Object.assign(
      {path: file, publicationId: addon.demoFramesDrawn() + 1,
       giveUpAfterFrames: 900},
      agreement));

  const first = await captureNext(capturePath);
  console.log('FIRST matched=' + first.matched);
  let image = readPpm(capturePath);
  let got = corners(image);
  report(
      'FIRST corners', got.every((p) => same(p, WHITE)), JSON.stringify(got));
  const centre = pixelAt(image, image.width >> 1, image.height >> 1);
  report('FIRST centre', !same(centre, WHITE), JSON.stringify(centre));

  // The addon's native listener runs before any JavaScript listener for
  // the same event, so once this one has fired, the clear colour has been
  // toggled -- or never will be.
  await new Promise((resolve) => {
    const onKey = (e) => {
      if (e.keyCode !== SAPP_KEYCODE_SPACE)
        return;
      win.off('keydown', onKey);
      resolve();
    };
    win.on('keydown', onKey);
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_SPACE});
  });

  const second = await captureNext(capturePath + '.key');
  console.log('KEY matched=' + second.matched);
  image = readPpm(capturePath + '.key');
  got = corners(image);
  report('KEY corners', got.every((p) => same(p, BLUE)), JSON.stringify(got));

  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
