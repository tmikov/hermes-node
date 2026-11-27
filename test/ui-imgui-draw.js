// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The renderer's draw path: ImGui's command lists drawn with sokol_gfx --
// shader, pipeline, appended buffers, the texture protocol, scissor and
// user callbacks -- read back from the presented frame and compared
// exactly. Every scene is an opaque pure-colour fill on the background
// draw list with anti-aliased fill off, so a sample is 0 or 255 per
// channel, or the clear colour.
//
// Cases, in the order they run, one output line each:
//
//  FILL          A full-window red fill: five samples red.
//  CLIP          A full-window green fill under the clip rectangle
//                (100,100)-(300,200): green inside, the clear colour
//                outside, including one pixel past each corner. This
//                decides the scissor's y orientation: flipped, the
//                rectangle lands at y 280..380. (The fill covers the window
//                either way up, so the projection's is MANY's to decide.)
//  MANY          19,200 4x4 quads (76,800 vertices) in one list: past
//                16-bit indices, so ImGui splits the list and the later
//                commands carry a vertex offset; past the initial buffer
//                sizes, so both buffers grow. Quad centres have the colour
//                (c + r) % 3 gives -- quad (159,119), past the split, only
//                with the right base vertex -- and state() reads the totals
//                back: more commands than FILL's, so the list was split.
//                The pattern is asymmetric, so this also decides the
//                projection's y orientation.
//  TWOPAIRS      Two newFrame()/render() pairs in one callback, red then
//                blue: the frame presented is blue.
//  TWOCALLBACKS  Two callbacks in one tick, blue then red: red.
//  MANYTWICE     `many` in two pairs in one callback, after growth: two
//                appends of the whole scene into one Sokol frame (the
//                second outgrows the buffer the first used).
//  CALLBACK      A user callback that shrinks the viewport to a quarter,
//                then ImGui's reset callback, then a full-window green
//                fill: green everywhere, so the reset re-applied the
//                viewport.
//  USERCALLBACK  The same without the reset: the fill lands in the
//                top-left quarter only, so the user callback was called.
//
// Each capture names the publication after the previous one, so each also
// proves the case before it advanced `h.published` by exactly one.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) sg_update_buffer instead of sg_append_buffer: the two-pair cases
//      produce a `[sokol-` line;
//  (b) 0 as the base vertex: MANY and MANYTWICE fail;
//  (c) no scissor: CLIP fails;
//  (d) the projection's y flipped: MANY and MANYTWICE fail (CLIP does not);
//  (d') the scissor's origin_top_left false: CLIP fails;
//  (e) buffers starting at 1,000,000 vertices: the growth trace is missing;
//  (f) the reset callback re-applying only the pipeline: Sokol validation
//      reports the missing uniforms, a `[sokol-` line;
//  (f') the reset callback re-applying pipeline and uniforms but not the
//      viewport: CALLBACK fails at (600,440);
//  (g) a user callback not called: USERCALLBACK fails at (600,440).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-present.js's, for the reasons given there.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=GROWN %s < %t.err
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TEXTURE %s < %t.err
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=PIPELINE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePrefix = process.argv[2];

const RED = [255, 0, 0];
const GREEN = [0, 255, 0];
const BLUE = [0, 0, 255];

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

/**
 * `<label> ok`, or `<label> WRONG` and every sample that missed. Points are
 * in logical units and scaled by the window's dpiScale here.
 */
function report(label, image, dpiScale, expectations) {
  const wrong = [];
  for (const [x, y, want] of expectations) {
    const px = Math.floor(x * dpiScale);
    const py = Math.floor(y * dpiScale);
    const got = harness.pixelAt(image, px, py);
    if (!sameColor(got, want))
      wrong.push(x + ',' + y + '=' + got + ' want ' + want);
  }
  console.log(
      label + ' ' + (wrong.length === 0 ? 'ok' : 'WRONG ' + wrong.join(' ')));
}

/** One newFrame()/scene/render() pair; returns what render() returned. */
function pair(imgui, name, arg) {
  imgui.newFrame();
  imgui.__test.scene(name, arg);
  return imgui.render();
}

/** The colour the `many` scene gives quad (c, r), and its centre. */
function manyQuad(c, r) {
  return [4 * c + 2, 4 * r + 2, [RED, GREEN, BLUE][(c + r) % 3]];
}

const MANY_QUADS = [[0, 0], [80, 60], [159, 119], [1, 0], [2, 0]];

async function main() {
  const h = await harness.open();
  const {win, imgui} = h;
  const s = win.dpiScale;
  const out = (name) => capturePrefix + '.' + name + '.ppm';

  // --- FILL ------------------------------------------------------------
  let fillCommands;
  {
    const image =
        await harness.capture(h, () => imgui.__test.scene('fill'), out('fill'));
    report('FILL', image, s, [
      [8, 8, RED],
      [631, 8, RED],
      [8, 471, RED],
      [631, 471, RED],
      [320, 240, RED],
    ]);
    fillCommands = imgui.__test.state().commands;
  }

  // --- CLIP ------------------------------------------------------------
  {
    const image =
        await harness.capture(h, () => imgui.__test.scene('clip'), out('clip'));
    report('CLIP', image, s, [
      [200, 150, GREEN],
      [50, 50, harness.CLEAR],
      [350, 150, harness.CLEAR],
      [200, 250, harness.CLEAR],
      [101, 101, GREEN],
      [99, 99, harness.CLEAR],
    ]);
  }

  // --- MANY ------------------------------------------------------------
  {
    const image =
        await harness.capture(h, () => imgui.__test.scene('many'), out('many'));
    report('MANY', image, s, MANY_QUADS.map(([c, r]) => manyQuad(c, r)));
    const st = imgui.__test.state();
    console.log(
        'MANY vertices=' + st.vertices +
        ' split=' + (st.commands > fillCommands));
  }

  // --- TWOPAIRS --------------------------------------------------------
  {
    const image = await harness.captureTick(h, [() => {
      pair(imgui, 'two', 0);
      return pair(imgui, 'two', 1);
    }], out('twopairs'));
    report('TWOPAIRS', image, s, [[320, 240, BLUE], [8, 8, BLUE]]);
  }

  // --- TWOCALLBACKS ----------------------------------------------------
  {
    const image = await harness.captureTick(h, [
      () => pair(imgui, 'two', 1),
      () => pair(imgui, 'two', 0),
    ], out('twocallbacks'));
    report('TWOCALLBACKS', image, s, [[320, 240, RED], [8, 8, RED]]);
  }

  // --- MANYTWICE -------------------------------------------------------
  {
    const image = await harness.captureTick(h, [() => {
      pair(imgui, 'many');
      return pair(imgui, 'many');
    }], out('manytwice'));
    report('MANYTWICE', image, s, MANY_QUADS.map(([c, r]) => manyQuad(c, r)));
  }

  // --- CALLBACK --------------------------------------------------------
  {
    const image = await harness.capture(
        h, () => imgui.__test.scene('callback'), out('callback'));
    report('CALLBACK', image, s, [
      [600, 440, GREEN],
      [320, 240, GREEN],
      [20, 20, GREEN],
    ]);
  }

  // --- USERCALLBACK ----------------------------------------------------
  {
    const image = await harness.capture(
        h, () => imgui.__test.scene('callback', 1), out('usercallback'));
    report('USERCALLBACK', image, s, [
      [20, 20, GREEN],
      [600, 440, harness.CLEAR],
    ]);
  }

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}FILL ok{{$}}
// CHECK-NEXT: {{^}}CLIP ok{{$}}
// CHECK-NEXT: {{^}}MANY ok{{$}}
// CHECK-NEXT: {{^}}MANY vertices=76800 split=true{{$}}
// CHECK-NEXT: {{^}}TWOPAIRS ok{{$}}
// CHECK-NEXT: {{^}}TWOCALLBACKS ok{{$}}
// CHECK-NEXT: {{^}}MANYTWICE ok{{$}}
// CHECK-NEXT: {{^}}CALLBACK ok{{$}}
// CHECK-NEXT: {{^}}USERCALLBACK ok{{$}}
// CODE: EXITCODE 0

// GROWN-DAG: {{^}}[ui] imgui: vertex buffer grown to {{[0-9]+}} bytes{{$}}
// GROWN-DAG: {{^}}[ui] imgui: index buffer grown to {{[0-9]+}} bytes{{$}}

// The font atlas, uploaded once; nothing here asks for a second texture.
// TEXTURE: {{^}}[ui] imgui: texture {{[0-9]+}} created {{[0-9]+}}x{{[0-9]+}}{{$}}
// TEXTURE-NOT: imgui: texture {{[0-9]+}} created

// One target format and sample count throughout, so one pipeline.
// PIPELINE: {{^}}[ui] imgui: pipeline created (format {{[0-9]+}}, samples 1){{$}}
// PIPELINE-NOT: imgui: pipeline created
