// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The addon follows the window: a real resize, and a window opened with
// `highDpi: true`. The addon reads the window's logical size and its
// framebuffer scale every frame (platformNewFrame), so neither needs any
// event handling; this test proves that it does, and that nothing the
// renderer made (the pipeline, the font atlas) is made again for it.
//
// Two runs of this file, selected by the first argument.
//
// `resize` (highDpi unset). Every frame before the FILL800 capture draws
// the `glyphs` scene with argument 0 (`abc`, so the font atlas exists),
// never `fill`.
//
//  BEFORE     a frame of generation 1 is captured, matched: so the frame
//             the resize follows was published and presented, and which
//             publication the next one is cannot be a race
//             (test/ui-real-events.js explains).
//  RESIZED    a real resize to 800x600 through the window driver (an X
//             client on Linux, `ui.__driveWindow` on macOS); once the
//             `resize` event has arrived and one setImmediate has passed,
//             frames run until ImGui's DisplaySize is 800 wide, and its
//             size is printed.
//  GENERATION the target the last of those frames drew into is
//             generation 2, read back (`state().targetGeneration`), not
//             counted. Then harness.resync(h): frames drawn into
//             generation 1 after its retirement got no publication id, so
//             the count is re-read from the host.
//  TEXTURES   the renderer's live texture count, and the id it handed
//             ImGui for the font atlas (state().atlasTexId, a Sokol view
//             id, which a re-created texture does not keep), are the same
//             before the resize and after the resync: the atlas was not
//             remade. The count alone cannot see a destroy-and-create.
//  FILL800    a `fill` frame captured at the new size (the capture's
//             agreement fields are the 800x600 window's) is red out to
//             (790,590), so DisplaySize and the viewport cover the new
//             framebuffer. A full-display fill covers the viewport under
//             any projection that maps the display onto at least the
//             viewport, so this cannot see a stale projection; MANY800
//             can.
//  MANY800    the `many` scene (4-pixel quads, 160 by 120 of them, tiling
//             the top-left 640x480 of the display) captured at the
//             new size: quad centres have their colours, the last quad's
//             at (638,478) included, and (700,300) and (400,550), outside
//             the tiling, are the clear colour. A projection still over
//             640x480 stretches the tiling to 800x600 and fails both.
//
// Its trace: the host's `generation 2: started at 800x600, replacing
// generation 1 at 640x480`; and exactly one `imgui: pipeline created` and
// one `imgui: texture <n> created` in the whole run. Each is two FileCheck
// runs: PIPELINE / TEXTURE check the one line's exact text, and
// PIPELINE-ONCE / TEXTURE-ONCE read the stderr with that line's first
// occurrence replaced by a marker (`awk`), under an --implicit-check-not
// of the line, which covers the whole input: a second occurrence anywhere
// fails, before the first or after it, while the marker proves the first
// was there to replace.
//
// `highdpi` (`open({highDpi: true})`).
//
//  SCALE      ImGui's DisplayFramebufferScale is the window's framebuffer
//             size over its logical size, both axes, and it is also
//             exactly win.dpiScale (as a float), both axes. The second
//             leg is independent of the addon's formula, which the first
//             repeats.
//  SCALEVALUE that scale, printed and unchecked: 1 under Xvfb, 2 on a
//             Retina Mac, so a Mac run's log shows the 2x case was run.
//  CLIP       the `clip` scene's rectangle, sampled at logical points
//             times win.dpiScale: the scissor is in framebuffer pixels.
//  CLICK      the `widgets` button clicked at logical coordinates
//             (harness.inject scales them to Sokol's framebuffer pixels):
//             one click, so ImGui's mouse position is logical.
//
// Under Xvfb the scale is 1, so the `highdpi` run repeats the `resize`
// run's geometry and only a Mac run is the 2x proof.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) DisplayFramebufferScale hard-coded to 1: cannot fail under Xvfb,
//      where the scale is 1; only a run on a 2x display (the Mac) can see
//      it, where SCALE and CLIP fail;
//  (b) DisplaySize read once, at context creation: RESIZED fails with
//      TIMEOUT RESIZED.
//  (c) the projection taken from the first frame's display size and
//      never updated: MANY800 fails (FILL800 cannot, as said above); the
//      viewport taken from the first frame's size: FILL800 fails;
//  (d) the pipeline made again, and the textures re-created, when the
//      target size changes: PIPELINE-ONCE, TEXTURE-ONCE and
//      `TEXTURES same` fail.
// And the two exclusions, by a doctored stderr file: a second
// `imgui: pipeline created` line fails PIPELINE-ONCE, a second
// `imgui: texture <n> created` line fails TEXTURE-ONCE, wherever in the
// file it is put.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-events.js's; the driver argument is
// ui-real-events.js's.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s resize %t.resize %x11_window_driver > %t.resize.out 2> %t.resize.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=RESIZE %s < %t.resize.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=GEN %s < %t.resize.err
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=PIPELINE %s < %t.resize.err
// RUN: awk '!seen && /imgui: pipeline created/ { seen = 1; print "FIRST PIPELINE LINE"; next } { print }' %t.resize.err | %FileCheck --implicit-check-not='imgui: pipeline created' --check-prefix=PIPELINE-ONCE %s
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TEXTURE %s < %t.resize.err
// RUN: awk '!seen && /imgui: texture [0-9]+ created/ { seen = 1; print "FIRST TEXTURE LINE"; next } { print }' %t.resize.err | %FileCheck --implicit-check-not='{{imgui: texture [0-9]+ created}}' --check-prefix=TEXTURE-ONCE %s
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s highdpi %t.highdpi > %t.highdpi.out 2> %t.highdpi.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=HIGHDPI %s < %t.highdpi.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=HIGHDPI-TRACE %s < %t.highdpi.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');
const {startDriver} = require('./fixtures/hnui-demo/window_driver.js');

const mode = process.argv[2];
const capturePrefix = process.argv[3];
const driverPath = process.argv[4];

const RED = [255, 0, 0];
const GREEN = [0, 255, 0];
const BLUE = [0, 0, 255];

const SAPP_MOUSEBUTTON_LEFT = 0;
const IMGUI_LEFT = 0;

const RESIZED_WIDTH = 800;
const RESIZED_HEIGHT = 600;

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

async function runResize() {
  const title = 'hnui imgui resize ' + process.pid;
  const h = await harness.open({title});
  const {win, imgui} = h;
  const state = () => imgui.__test.state();
  const glyphs = () => imgui.__test.scene('glyphs', 0);
  const out = (name) => capturePrefix + '.' + name + '.ppm';

  // --- BEFORE -----------------------------------------------------------
  // harness.capture exits with `CAPTURE unmatched` if it does not match.
  await harness.capture(h, glyphs, out('before'));
  console.log('BEFORE matched');
  const texturesBefore = state().liveTextures;
  const atlasBefore = state().atlasTexId;

  // --- RESIZED ----------------------------------------------------------
  const driver = await startDriver({ui: h.ui, driverPath, title});
  const resized = new Promise((resolve) => win.once('resize', resolve));
  const answer =
      await driver.send('resize ' + RESIZED_WIDTH + ' ' + RESIZED_HEIGHT);
  if (!answer.startsWith('SENT')) {
    console.log('RESIZED driver: ' + answer);
    process.exit(1);
  }
  await resized;
  await new Promise((resolve) => setImmediate(resolve));
  await harness.until(
      h, glyphs, (s) => s.displayWidth === RESIZED_WIDTH, 'RESIZED');
  {
    const s = state();
    console.log('RESIZED ' + s.displayWidth + 'x' + s.displayHeight);
  }

  // --- GENERATION -------------------------------------------------------
  h.generation = state().targetGeneration;
  console.log('GENERATION ' + h.generation);
  await harness.resync(h);

  // --- TEXTURES ---------------------------------------------------------
  {
    const after = state().liveTextures;
    const atlasAfter = state().atlasTexId;
    console.log(
        'TEXTURES ' +
        (after === texturesBefore && atlasAfter === atlasBefore &&
                 atlasBefore !== 0 ?
             'same' :
             'changed ' + texturesBefore + ' -> ' + after + ', atlas ' +
                 atlasBefore + ' -> ' + atlasAfter));
  }

  // --- FILL800 ----------------------------------------------------------
  {
    const image = await harness.capture(
        h, () => imgui.__test.scene('fill'), out('fill800'));
    report('FILL800', image, win.dpiScale, [
      [8, 8, RED],
      [790, 8, RED],
      [8, 590, RED],
      [790, 590, RED],
      [400, 300, RED],
    ]);
  }

  // --- MANY800 ----------------------------------------------------------
  {
    const image = await harness.capture(
        h, () => imgui.__test.scene('many'), out('many800'));
    const colors = [RED, GREEN, BLUE];
    const quad = (c, r) => [4 * c + 2, 4 * r + 2, colors[(c + r) % 3]];
    report('MANY800', image, win.dpiScale, [
      quad(0, 0),
      quad(80, 60),
      quad(159, 0),
      quad(0, 119),
      quad(159, 119),
      [700, 300, harness.CLEAR],
      [400, 550, harness.CLEAR],
    ]);
  }

  driver.stop();
  process.exit(0);
}

async function runHighDpi() {
  const h = await harness.open({highDpi: true});
  const {win, imgui} = h;
  const state = () => imgui.__test.state();
  const out = (name) => capturePrefix + '.' + name + '.ppm';

  // --- SCALE / SCALEVALUE -----------------------------------------------
  // state() is as of the last completed frame, so one is drawn first.
  // ImGui holds the scale as a float.
  await harness.frame(h, () => imgui.__test.scene('none'));
  {
    const s = state();
    const wantX = Math.fround(win.framebufferWidth / win.width);
    const wantY = Math.fround(win.framebufferHeight / win.height);
    const dpi = Math.fround(win.dpiScale);
    console.log(
        'SCALE ' +
        (s.scaleX === wantX && s.scaleY === wantY && s.scaleX === dpi &&
                 s.scaleY === dpi ?
             'ok' :
             'WRONG ' + s.scaleX + 'x' + s.scaleY + ' want ' + wantX + 'x' +
                 wantY + ' and dpiScale ' + dpi));
    console.log('SCALEVALUE ' + s.scaleX);
  }

  // --- CLIP -------------------------------------------------------------
  {
    const image = await harness.capture(
        h, () => imgui.__test.scene('clip'), out('clip'));
    report('CLIP', image, win.dpiScale, [
      [200, 150, GREEN],
      [50, 50, harness.CLEAR],
      [350, 150, harness.CLEAR],
      [200, 250, harness.CLEAR],
      [101, 101, GREEN],
      [99, 99, harness.CLEAR],
    ]);
  }

  // --- CLICK ------------------------------------------------------------
  {
    const draw = () => imgui.__test.scene('widgets');
    const until = (predicate, label) =>
      harness.until(h, draw, predicate, label);
    const mouseAt = (s, x, y) =>
      Math.abs(s.mouseX - x) < 0.01 && Math.abs(s.mouseY - y) < 0.01;

    await until((s) => s.buttonRect[2] > s.buttonRect[0], 'CLICK layout');
    const r = state().buttonRect;
    const x = Math.floor((r[0] + r[2]) / 2);
    const y = Math.floor((r[1] + r[3]) / 2);
    const base = state().clicks;
    harness.inject(h, {type: 'MOUSE_MOVE', mouseX: x, mouseY: y});
    await until((s) => mouseAt(s, x, y), 'CLICK move');
    harness.inject(h, {type: 'MOUSE_DOWN', button: SAPP_MOUSEBUTTON_LEFT,
                       mouseX: x, mouseY: y});
    await until((s) => s.mouseDown[IMGUI_LEFT], 'CLICK down');
    harness.inject(h, {type: 'MOUSE_UP', button: SAPP_MOUSEBUTTON_LEFT,
                       mouseX: x, mouseY: y});
    await until((s) => !s.mouseDown[IMGUI_LEFT], 'CLICK up');
    await until((s) => s.clicks !== base, 'CLICK');
    console.log('CLICK clicks=' + (state().clicks - base));
  }

  process.exit(0);
}

async function main() {
  if (mode === 'resize')
    await runResize();
  else if (mode === 'highdpi')
    await runHighDpi();
  else {
    console.log('MODE argument is not resize or highdpi: ' + mode);
    process.exit(2);
  }
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CODE: EXITCODE 0

// RESIZE: {{^}}BEFORE matched{{$}}
// RESIZE-NEXT: {{^}}RESIZED 800x600{{$}}
// RESIZE-NEXT: {{^}}GENERATION 2{{$}}
// RESIZE-NEXT: {{^}}TEXTURES same{{$}}
// RESIZE-NEXT: {{^}}FILL800 ok{{$}}
// RESIZE-NEXT: {{^}}MANY800 ok{{$}}

// GEN: {{^}}[ui] generation 2: started at 800x600, replacing generation 1 at 640x480{{$}}

// PIPELINE: {{^}}[ui] imgui: pipeline created (format {{[0-9]+}}, samples 1){{$}}
// PIPELINE-ONCE: {{^}}FIRST PIPELINE LINE{{$}}

// TEXTURE: {{^}}[ui] imgui: texture {{[0-9]+}} created {{[0-9]+}}x{{[0-9]+}}{{$}}
// TEXTURE-ONCE: {{^}}FIRST TEXTURE LINE{{$}}

// HIGHDPI: {{^}}SCALE ok{{$}}
// HIGHDPI-NEXT: {{^}}SCALEVALUE {{[0-9.]+}}{{$}}
// HIGHDPI-NEXT: {{^}}CLIP ok{{$}}
// HIGHDPI-NEXT: {{^}}CLICK clicks=1{{$}}

// HIGHDPI-TRACE: {{^}}[ui] imgui: pipeline created (format {{[0-9]+}}, samples 1){{$}}
