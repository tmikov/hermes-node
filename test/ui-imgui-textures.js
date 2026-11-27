// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Textures: an image the addon makes itself, bound and sampled, and ImGui's
// font atlas, uploaded and then replaced when new glyphs are rasterized.
//
// Cases, in the order they run, one output line each:
//
//  QUAD     A 2x2 image (red, green / blue, white) drawn with AddImage over
//           (100,100)-(300,300). The sampler is linear with clamp-to-edge,
//           so a point within the outer quarter of the quad's width
//           samples one texel's colour unmixed: the four points, 5 units
//           inside each corner, are red, green, blue and white exactly.
//           This is what proves a command binds its own texture: bound to
//           the white fallback, all four read white.
//  GLYPHS0  `abc` at (20,20) at the default size: the box (20,20)-(60,40)
//           holds a pixel that is not the clear colour, and the box
//           (20,200)-(200,260), where GLYPHS1 draws, is all clear.
//  GLYPHS1  The same and `XYZ` at (20,200) at 40 units, for three frames
//           and then captured: both boxes hold a non-clear pixel. The text
//           is anti-aliased, so neither glyph case compares exactly.
//  LIVE     state().liveTextures is at least 1.
//
// The texture trace is read with a marker the test writes to stderr just
// before GLYPHS1 (process.stderr is synchronous on a file): before it the
// font atlas is created exactly once; after it, the new 40-unit glyphs make
// ImGui replace that texture's contents, or create a new texture if the
// atlas had to grow. Either is a correct answer to the protocol, so either
// is accepted.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) WantUpdates ignored: GLYPHS1 fails (and ImGui asserts);
//  (b) every command bound to the white fallback view: QUAD fails;
//  (c) the previous image not destroyed on replace: no test fails (checked
//      by reading; Sokol's pool would run out only after thousands).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-present.js's, for the reasons given there.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TEXTURE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePrefix = process.argv[2];

const RED = [255, 0, 0];
const GREEN = [0, 255, 0];
const BLUE = [0, 0, 255];
const WHITE = [255, 255, 255];

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
  return wrong.length === 0 ? 'ok' : 'WRONG ' + wrong.join(' ');
}

/**
 * How many pixels of the box (x0,y0)-(x1,y1), in logical units, are not
 * the clear colour.
 */
function notClear(image, dpiScale, x0, y0, x1, y1) {
  let count = 0;
  const px1 = Math.floor(x1 * dpiScale);
  const py1 = Math.floor(y1 * dpiScale);
  for (let y = Math.floor(y0 * dpiScale); y < py1; ++y) {
    for (let x = Math.floor(x0 * dpiScale); x < px1; ++x) {
      if (!sameColor(harness.pixelAt(image, x, y), harness.CLEAR))
        ++count;
    }
  }
  return count;
}

const ABC_BOX = [20, 20, 60, 40];
const XYZ_BOX = [20, 200, 200, 260];

async function main() {
  const h = await harness.open();
  const {win, imgui} = h;
  const s = win.dpiScale;
  const out = (name) => capturePrefix + '.' + name + '.ppm';

  // --- QUAD ------------------------------------------------------------
  {
    const image =
        await harness.capture(h, () => imgui.__test.scene('quad'), out('quad'));
    console.log('QUAD ' + report('QUAD', image, s, [
      [105, 105, RED],
      [295, 105, GREEN],
      [105, 295, BLUE],
      [295, 295, WHITE],
    ]));
  }

  // --- GLYPHS0 ---------------------------------------------------------
  {
    const image = await harness.capture(
        h, () => imgui.__test.scene('glyphs', 0), out('glyphs0'));
    const abc = notClear(image, s, ...ABC_BOX);
    const xyz = notClear(image, s, ...XYZ_BOX);
    console.log(
        'GLYPHS0 ' +
        (abc > 0 && xyz === 0 ? 'ok' : 'WRONG abc=' + abc + ' xyz=' + xyz));
  }

  // --- GLYPHS1 ---------------------------------------------------------
  {
    process.stderr.write('MARK glyphs1\n');
    for (let i = 0; i < 3; ++i)
      await harness.frame(h, () => imgui.__test.scene('glyphs', 1));
    const image = await harness.capture(
        h, () => imgui.__test.scene('glyphs', 1), out('glyphs1'));
    const abc = notClear(image, s, ...ABC_BOX);
    const xyz = notClear(image, s, ...XYZ_BOX);
    console.log(
        'GLYPHS1 ' +
        (abc > 0 && xyz > 0 ? 'ok' : 'WRONG abc=' + abc + ' xyz=' + xyz));
  }

  // --- LIVE ------------------------------------------------------------
  {
    const live = imgui.__test.state().liveTextures;
    console.log('LIVE ' + (live >= 1 ? 'ok' : 'WRONG ' + live));
  }

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}QUAD ok{{$}}
// CHECK-NEXT: {{^}}GLYPHS0 ok{{$}}
// CHECK-NEXT: {{^}}GLYPHS1 ok{{$}}
// CHECK-NEXT: {{^}}LIVE ok{{$}}
// CODE: EXITCODE 0

// TEXTURE: {{^}}[ui] imgui: texture {{[0-9]+}} created {{[0-9]+}}x{{[0-9]+}}{{$}}
// TEXTURE-NOT: imgui: texture {{[0-9]+}} created
// TEXTURE: {{^}}MARK glyphs1{{$}}
// TEXTURE: {{^}}[ui] imgui: texture {{[0-9]+}} {{replaced|created}} {{[0-9]+}}x{{[0-9]+}}{{$}}
