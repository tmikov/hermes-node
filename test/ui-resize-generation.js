// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A change of framebuffer size starts a new surface generation, and the
// old one is retired by SurfaceTable's rules and destroyed on the owner
// thread (spec: Surfaces, "Generations and texture lifetime").
//
// The resize is INJECTED: a RESIZED platform event carrying a `window`
// override of 320x240, while the real framebuffer stays 640x480. That is
// what makes the stretched case reachable at all -- the presenter blits a
// 320x240 surface into a 640x480 swapchain -- and it is also why nothing
// here may assume that no other event arrives. Any REAL platform event
// refreshes the tracked state from sapp_* and reports 640x480 again, which
// would legitimately start yet another generation. So every assertion names
// the generation and publication it is about, and the frames it captures
// are keyed by those, never by "the latest".
//
// What is asserted, each line of output naming one property:
//
//  FIRST      generation 1 is 640x480 and its first publication presents.
//  INFLIGHT   a frame drawn into generation 1 whose completion is HELD
//             across the resize. Its publication is refused and its slot
//             reclaimed (the TRACE line); without the hold, whether its
//             GPU work finished before or after the resize would be a race.
//  RESIZE     the `resize` event reports 320x240 and win.framebufferWidth
//             is already 320 inside its listener.
//  TARGET     the next frame's hnui_frame_target is generation 2, 320x240.
//             (Mutation: skip newGeneration.)
//  STRETCHED  a capture naming generation 2, target 320x240, framebuffer
//             640x480 matches, and interior samples at least 16 px from any
//             pattern edge are exact.
//  ZERO       a RESIZED to 0x0 starts no generation: the next frame is
//             still generation 2 at 320x240.
//  CONTINUES  and that frame is published and presented in generation 2.
//  TRACE      generation 1 is destroyed on the owner thread (owner=1),
//             after generation 2 was created, and its last reference was
//             dropped on the platform thread (owner=0) -- which is what
//             makes owner=1 mean something. (Mutation: destroy inline from
//             the SurfaceTable callback instead of posting.)
//
// --- The tolerance policy for the stretched case ---------------------
//
// ui-present.js compares exactly with an 8 px margin because its blit is
// 1:1. Here it is a 2x magnification with linear filtering, so a
// framebuffer pixel blends the two or four nearest texels. Inside a solid
// region all of those texels are the same colour, and a weighted average of
// equal values is that value; with every channel 0 or 255, the result
// lands exactly on it under any rounding rule. Blending is therefore
// confined to within about one framebuffer pixel of a texel boundary where
// the colour changes. A 16 px margin -- 8 texels -- puts every sample far
// outside that, and outside the clamp-to-edge region at the image border,
// so the comparison stays exact: a tolerance would only hide a wrong
// colour, and a wrongly stretched (or not stretched) picture moves the
// quadrant boundary by 160 px, which these samples see.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// The RUN-line shape is ui-present.js's: redirections INSIDE `sh -c`
// because xvfb-run merges its child's stderr into stdout, and `timeout`
// because lit has no per-test timeout (dz 01a0d797-e326). The test ends
// through process.exit() (Ruling 59): closing the window still hangs (dz
// 01a0d88a-6518). Polling completion is forced so that producer fences are
// completed on the platform thread, the path a real run takes.
//
// RUN: timeout 300 xvfb-run -a sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI HERMES_NODE_UI_GL_COMPLETION=poll %hermes-node-ui %s %hnui_demo_addon %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=INFLIGHT %s < %t.err
// RUN: %FileCheck --check-prefix=LIFETIME %s < %t.err
// RUN: %FileCheck --check-prefix=NOZERO %s < %t.err

// CODE: EXITCODE 0

// CHECK: FIRST ok
// CHECK-NEXT: INFLIGHT drawn
// CHECK-NEXT: RESIZE ok
// CHECK-NEXT: TARGET ok
// CHECK-NEXT: STRETCHED matched=true
// CHECK-NEXT: TOPLEFT ok
// CHECK-NEXT: TOPRIGHT ok
// CHECK-NEXT: BOTTOMLEFT ok
// CHECK-NEXT: BOTTOMRIGHT ok
// CHECK-NEXT: TOPLEFTDEEP ok
// CHECK-NEXT: ZERO ok
// CHECK-NEXT: CONTINUES matched=true
// CHECK-NEXT: DONE

// The held frame: drawn into generation 1, refused after the switch.
// INFLIGHT: [ui] generation 1: 3 surfaces at 640x480
// INFLIGHT: [ui] test hook: producer completions held
// INFLIGHT: [ui] generation 2: started at 320x240, replacing generation 1 at 640x480
// INFLIGHT: [ui] test hook: producer completions released
// INFLIGHT: [ui] generation 1 slot {{[0-9]}}: publication refused, slot reclaimed

// Generation 1's end. The last reference goes on the platform thread --
// its wrappers are the last owner, and only that thread holds them -- and
// the destruction runs on the owner thread all the same.
// LIFETIME: [ui] generation 2: 3 surfaces at 320x240
// LIFETIME: [ui] generation 2: started at 320x240, replacing generation 1 at 640x480
// LIFETIME: [ui] generation 1: last reference dropped (owner=0)
// LIFETIME: [ui] generation 1: surfaces destroyed
// LIFETIME-NEXT: [ui] generation 1: destroyed (owner=1)

// NOZERO-NOT: at 0x0

'use strict';

const fs = require('fs');
const ui = require('hermes-node:ui');

const addonPath = process.argv[2];
const capturePath = process.argv[3];

const BLUE = [0, 0, 255];
const YELLOW = [255, 255, 0];

/** Parse the binary PPM (P6) the capture hook wrote. See ui-present.js. */
function readPpm(path) {
  const buf = fs.readFileSync(path);
  let offset = 0;
  function token() {
    while (offset < buf.length && /\s/.test(String.fromCharCode(buf[offset]))) {
      offset++;
    }
    const start = offset;
    while (
        offset < buf.length && !/\s/.test(String.fromCharCode(buf[offset]))) {
      offset++;
    }
    return buf.toString('ascii', start, offset);
  }
  const magic = token();
  const width = parseInt(token(), 10);
  const height = parseInt(token(), 10);
  const maxValue = parseInt(token(), 10);
  offset++;
  if (magic !== 'P6' || maxValue !== 255) {
    throw new Error('not a P6/255 PPM: ' + magic + ' ' + maxValue);
  }
  return {width, height, data: buf.slice(offset)};
}

function pixelAt(image, x, y) {
  const i = (y * image.width + x) * 3;
  return [image.data[i], image.data[i + 1], image.data[i + 2]];
}

function describe(p) {
  return '(' + p[0] + ',' + p[1] + ',' + p[2] + ')';
}

/**
 * The four quadrant interiors of the STRETCHED picture, 16 framebuffer
 * pixels from every pattern edge and image edge. The quadrant boundary is
 * at half the framebuffer, because the whole surface is stretched over it.
 */
function checkStretchedPattern(image) {
  const margin = 16;
  const midX = image.width >> 1;
  const midY = image.height >> 1;
  const samples = [
    {name: 'TOPLEFT', x: midX - margin, y: margin, want: YELLOW},
    {name: 'TOPRIGHT', x: midX + margin, y: margin, want: BLUE},
    {name: 'BOTTOMLEFT', x: margin, y: image.height - margin, want: BLUE},
    {
      name: 'BOTTOMRIGHT',
      x: image.width - margin,
      y: image.height - margin,
      want: BLUE,
    },
    {name: 'TOPLEFTDEEP', x: margin, y: midY - margin, want: YELLOW},
  ];
  for (const s of samples) {
    const got = pixelAt(image, s.x, s.y);
    const ok = got[0] === s.want[0] && got[1] === s.want[1] &&
        got[2] === s.want[2];
    const detail = 'WRONG got=' + describe(got) + ' want=' + describe(s.want);
    console.log(s.name + ' ' + (ok ? 'ok' : detail));
  }
}

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + detail));
}

async function main() {
  const win = await ui.createWindow({
    width: 640,
    height: 480,
    title: 'hermes-node-ui resize-generation test',
  });
  const addon = require(addonPath);

  const realWidth = win.framebufferWidth;
  const realHeight = win.framebufferHeight;
  const dpiScale = win.dpiScale;

  /** Draw exactly one frame; see ui-present.js for why one at a time. */
  const drawOnce = () => new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve(addon.lastTarget());
    });
  });

  /** Resolve with the next `resize` event's payload. */
  const nextResize = () => new Promise((resolve) => {
    win.once('resize', (e) => resolve({
      event: e,
      mirrorWidth: win.framebufferWidth,
      mirrorHeight: win.framebufferHeight,
    }));
  });

  /** A RESIZED carrying a window snapshot of the given framebuffer size. */
  const injectResize = (w, h) => ui.__injectEvent({
    type: 'RESIZED',
    window: {
      width: w,
      height: h,
      framebufferWidth: w,
      framebufferHeight: h,
      dpiScale,
      focused: win.focused,
    },
  });

  const capture = (fields) => ui.__captureFrame(Object.assign(
      {dpiScale, inputSerial: 0, framebufferWidth: realWidth,
       framebufferHeight: realHeight},
      fields));

  // --- Generation 1. -----------------------------------------------
  const firstPending = capture({
    generation: 1,
    publicationId: 1,
    targetWidth: realWidth,
    targetHeight: realHeight,
    path: capturePath + '.first',
    giveUpAfterFrames: 900,
  });
  const firstTarget = await drawOnce();
  const first = await firstPending;
  report(
      'FIRST',
      first.matched && firstTarget.generation === 1 &&
          firstTarget.width === 640 && firstTarget.height === 480,
      JSON.stringify({first, firstTarget}));

  // --- A frame of generation 1 still in flight across the resize. ----
  //
  // Held BEFORE it is drawn, so its completion cannot publish it before
  // the resize below has retired its generation. Its publication is
  // therefore refused whichever way the release and the fence's own
  // completion interleave: queued and run by the release, or not yet
  // polled and run by the platform thread afterwards -- both arrive after
  // the switch.
  ui.__holdProducerCompletions();
  const heldTarget = await drawOnce();
  console.log(
      heldTarget.generation === 1 ? 'INFLIGHT drawn' :
                                    'INFLIGHT WRONG ' +
              JSON.stringify(heldTarget));

  // --- The resize. -------------------------------------------------
  const resized = nextResize();
  injectResize(320, 240);
  const r = await resized;
  report(
      'RESIZE',
      r.event.framebufferWidth === 320 && r.event.framebufferHeight === 240 &&
          r.mirrorWidth === 320 && r.mirrorHeight === 240,
      JSON.stringify(r));
  ui.__releaseProducerCompletions();

  // --- Generation 2, stretched over the real framebuffer. ------------
  //
  // The capture is installed AFTER the frame is drawn, keyed by the
  // generation the addon was actually given: nothing else is drawn
  // meanwhile, so the presenter keeps re-blitting this publication and
  // one of those blits matches. Publication 2, because the held frame's
  // publication was refused and so was never numbered.
  const secondTarget = await drawOnce();
  report(
      'TARGET',
      secondTarget.generation === 2 && secondTarget.width === 320 &&
          secondTarget.height === 240,
      JSON.stringify(secondTarget));
  const stretched = await capture({
    generation: secondTarget.generation,
    publicationId: 2,
    targetWidth: 320,
    targetHeight: 240,
    path: capturePath,
    giveUpAfterFrames: 900,
  });
  console.log('STRETCHED matched=' + stretched.matched);
  if (stretched.matched) {
    checkStretchedPattern(readPpm(capturePath));
  }

  // --- A zero-sized resize starts nothing. ---------------------------
  const zeroed = nextResize();
  injectResize(0, 0);
  await zeroed;
  const thirdTarget = await drawOnce();
  report(
      'ZERO',
      thirdTarget.generation === 2 && thirdTarget.width === 320 &&
          thirdTarget.height === 240,
      JSON.stringify(thirdTarget));

  // --- And rendering carries on in generation 2. ---------------------
  const continued = await capture({
    generation: 2,
    publicationId: 3,
    targetWidth: 320,
    targetHeight: 240,
    path: capturePath + '.continued',
    giveUpAfterFrames: 900,
  });
  console.log('CONTINUES matched=' + continued.matched);

  // Let a fixed number of presented frames go past before exiting, so that
  // generation 1's destruction -- posted to this thread once its last
  // wrapper is released -- has run before process.exit(). A budget in
  // FRAMES, not milliseconds; the request names a publication that never
  // exists, so it can only give up.
  await capture({
    generation: 2,
    publicationId: 1000000,
    targetWidth: 320,
    targetHeight: 240,
    path: capturePath + '.settle',
    giveUpAfterFrames: 60,
  });

  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.message));
  process.exit(1);
});
