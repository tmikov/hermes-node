// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// ImGui's own demo window, through the addon, in a 1280x800 window. The
// frames make the same three calls examples/imgui-demo/demo.js makes
// (newFrame(), showDemoWindow(), render()), driven through the harness; the
// window is opened without highDpi and the example file itself is not
// run. The demo window is
// anti-aliased text and widgets, not a pure-colour scene, so nothing here
// compares a pixel's colour inside it; the assertions are counts and an
// exact clear colour outside it.
//
// Cases, in the order they run, one output line each:
//
//  DEMO ok          30 frames of newFrame(); showDemoWindow(); render(),
//                   then one more captured. ShowDemoWindow() places its
//                   window at (650,20), 550x680, on first use (imgui_demo.cpp;
//                   measured here: the non-clear pixels of the capture span
//                   exactly (650,20)-(1199,699)). At least 1,000 pixels of
//                   (660,30)-(1190,690) differ from the clear colour, and
//                   every pixel of the column x = 300 and of the row
//                   y = 760, both outside that rectangle, is the clear
//                   colour.
//  DEMO vertices ok state().vertices of that frame is over 500 (measured:
//                   760).
//  DEMO scrolled    the pointer moved over the demo window, then a wheel
//                   event (MOUSE_SCROLL, scrollY -1) injected there, and
//                   frames run until ImGui's io.MouseWheel reads -1 inside
//                   a frame (the `wheel` scene's state().lastWheel; ImGui
//                   clears the field when the frame ends, so it is read
//                   inside one), then three more. Collapsed, as on first
//                   use, the demo window's content fits it (ScrollMax.y is
//                   0, measured), so a scroll offset could not show the
//                   wheel arriving; io.MouseWheel does. The point of the
//                   case is that input over a real, large window raises no
//                   Sokol line, which the stderr run's exclusions check.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) every sg_draw_ex skipped: DEMO ok fails, DEMO vertices ok passes;
//  (b) HNUI_EVENT_MOUSE_WHEEL not passed to ImGui: TIMEOUT DEMO wheel.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-events.js's.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePrefix = process.argv[2];

const WIDTH = 1280;
const HEIGHT = 800;

/** Frames drawn before the captured one. */
const SETTLE_FRAMES = 30;

/** Where the demo window's content is looked for, inside its rectangle. */
const INSIDE = {x0: 660, y0: 30, x1: 1190, y1: 690};
const MIN_INSIDE = 1000;

/** A column and a row outside the demo window. */
const CLEAR_COLUMN = 300;
const CLEAR_ROW = 760;

const MIN_VERTICES = 500;

/** A point over the demo window, for the wheel. */
const OVER = [900, 400];

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

async function main() {
  const h = await harness.open(
      {title: 'hermes-node imgui demo test', width: WIDTH, height: HEIGHT});
  const {win, imgui} = h;
  const state = () => imgui.__test.state();
  const demo = () => imgui.showDemoWindow();

  // --- DEMO ok ----------------------------------------------------------
  for (let i = 0; i < SETTLE_FRAMES; ++i)
    await harness.frame(h, demo);
  const image = await harness.capture(h, demo, capturePrefix + '.demo.ppm');
  {
    // Logical coordinates, in framebuffer pixels.
    const s = win.dpiScale;
    let inside = 0;
    for (let y = Math.floor(INSIDE.y0 * s); y < Math.floor(INSIDE.y1 * s); ++y)
      for (let x = Math.floor(INSIDE.x0 * s); x < Math.floor(INSIDE.x1 * s);
           ++x)
        if (!sameColor(harness.pixelAt(image, x, y), harness.CLEAR))
          ++inside;
    const notClear = [];
    const column = Math.floor(CLEAR_COLUMN * s);
    for (let y = 0; y < image.height; ++y)
      if (!sameColor(harness.pixelAt(image, column, y), harness.CLEAR))
        notClear.push(column + ',' + y);
    const row = Math.floor(CLEAR_ROW * s);
    for (let x = 0; x < image.width; ++x)
      if (!sameColor(harness.pixelAt(image, x, row), harness.CLEAR))
        notClear.push(x + ',' + row);
    if (inside >= MIN_INSIDE && notClear.length === 0)
      console.log('DEMO ok');
    else
      console.log(
          'DEMO WRONG inside=' + inside + ' notClear=' + notClear.length +
          (notClear.length > 0 ? ' first ' + notClear[0] : ''));
  }

  // --- DEMO vertices ok -------------------------------------------------
  {
    const vertices = state().vertices;
    console.log(
        'DEMO vertices ' +
        (vertices > MIN_VERTICES ? 'ok' : 'WRONG ' + vertices));
  }

  // --- DEMO scrolled ----------------------------------------------------
  {
    const demoAndWheel = () => {
      imgui.showDemoWindow();
      imgui.__test.scene('wheel');
    };
    const [x, y] = OVER;
    const mouseAt = (st) =>
      Math.abs(st.mouseX - x) < 0.01 && Math.abs(st.mouseY - y) < 0.01;
    harness.inject(h, {type: 'MOUSE_MOVE', mouseX: x, mouseY: y});
    await harness.until(h, demoAndWheel, mouseAt, 'DEMO move');
    harness.inject(
        h, {type: 'MOUSE_SCROLL', scrollY: -1, mouseX: x, mouseY: y});
    await harness.until(
        h, demoAndWheel, (st) => st.lastWheel === -1, 'DEMO wheel');
    for (let i = 0; i < 3; ++i)
      await harness.frame(h, demoAndWheel);
    console.log('DEMO scrolled');
  }

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CODE: EXITCODE 0

// CHECK: {{^}}DEMO ok{{$}}
// CHECK-NEXT: {{^}}DEMO vertices ok{{$}}
// CHECK-NEXT: {{^}}DEMO scrolled{{$}}

// TRACE: {{^}}[ui] imgui: context created (imgui {{.+}}, docking=1){{$}}
// TRACE: {{^}}[ui] imgui: pipeline created (format {{[0-9]+}}, samples 1){{$}}
