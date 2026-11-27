// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Style group: colours and variables pushed and popped, the two
// built-in themes, and the colour getters, read back from captured pixels,
// item rectangles and returned values.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  NOWINDOW  before any window exists, styleColorsDark() throws
//            ERR_IMGUI_NO_WINDOW, not called (it needs the context).
//  THEMES    styleColorsLight() before the first newFrame(): a window
//            made opaque with setNextWindowBgAlpha(1) shows exactly the
//            colour getColorU32(Col_WindowBg) answers inside the frame;
//            styleColorsDark(), again outside a frame: the same holds for
//            its colour, and the two colours differ. styleColorsLight()
//            inside a frame is accepted too (Frame: either), and the dark
//            theme is put back.
//  COLOR     pushStyleColor(Col_WindowBg, [0, 0, 1, 1]) and the number form
//            0xFFFF0000: each window is exactly blue; a window drawn after
//            popStyleColor() shows the previous colour (the bytes of
//            getColorU32(Col_WindowBg) read after the pop).
//  GETCOLOR  with Col_WindowBg pushed as [0, 0, 1, 1], getColorU32
//            (Col_WindowBg) is 0xFFFF0000 and getColorU32(Col_WindowBg,
//            0.5) is 0x80FF0000; Col_COUNT and a non-integer idx are
//            ERR_OUT_OF_RANGE, not called. Outside the frame,
//            colorConvertFloat4ToU32 gives 0xFFFF0000 and 0x80FF0000 for
//            the same colour at alpha 1 and 0.5, and saturates [2, -1, 0,
//            1] to 0xFF0000FF.
//  PADDING   pushStyleVar(StyleVar_WindowPadding, [30, 30]) puts a
//            window's first item 20 units further right and down than
//            pushStyleVar(StyleVar_WindowPadding, [10, 10]) does.
//  ALPHA     pushStyleVar(StyleVar_Alpha, 0.5) is accepted, and
//            getColorU32([1, 0, 0, 1]) then answers alpha 0x80 (ImGui
//            multiplies by style.Alpha, imgui.cpp:3774); after
//            popStyleVar(), 0xFF again.
//  MISMATCH  pushStyleVar(StyleVar_Alpha, [1, 1]) and pushStyleVar
//            (StyleVar_WindowPadding, 1): ImGui's own recoverable check
//            ("Calling PushStyleVar() variant with wrong type!",
//            imgui.cpp:3900, 3930), so ERR_IMGUI_USAGE after the call;
//            nothing was pushed, so a following popStyleVar() is "too many
//            times", ERR_IMGUI_USAGE too. StyleVar_COUNT is
//            ERR_OUT_OF_RANGE, not called.
//  COVERAGE  every Style function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  StyleColorsLight() removed, enterImGui() kept: THEMES fails;
//  enterImGui() removed from colorConvertFloat4ToU32: COVERAGE fails;
//  getColorU32 returning 0 (its result stands in for a write-back): THEMES
//  and GETCOLOR fail;
//  getColorU32's Col_COUNT refusal removed: GETCOLOR fails;
//  pushStyleVar's float overload passing 1 instead of the value: ALPHA
//  fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePath = process.argv[2];
const BLUE = [0, 0, 255];

function fail(line) {
  console.log(line);
  process.exit(1);
}

function same(a, b) {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

/** The colour bytes `[r, g, b]` of an ImU32. */
function rgbOf(u32) {
  return [u32 & 0xFF, (u32 >>> 8) & 0xFF, (u32 >>> 16) & 0xFF];
}

async function main() {
  // --- NOWINDOW --------------------------------------------------------
  {
    const imgui = require('hermes-node:imgui');
    harness.expectThrow(() => imgui.styleColorsDark(), 'ERR_IMGUI_NO_WINDOW');
    const n = (imgui.__test.bindCalls().styleColorsDark || 0);
    console.log(n === 0 ? 'NOWINDOW ok' : 'NOWINDOW CALLED ' + n);
  }

  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);

  /** `fn` must throw `code` and leave `name`'s call count unchanged. */
  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ': ' + e.message);
    return e;
  }

  /** `fn` must reach ImGui and then throw ERR_IMGUI_USAGE. */
  function usage(name, fn) {
    const before = calls(name);
    const e = harness.expectThrow(fn, 'ERR_IMGUI_USAGE');
    if (calls(name) !== before + 1)
      fail('NOTCALLED ' + name + ': ' + e.message);
    return e;
  }

  /** An opaque, untitled window at `pos` of `size`; `body` draws in it. */
  function windowAt(name, pos, size, body = () => {}) {
    imgui.setNextWindowPos(pos);
    imgui.setNextWindowSize(size);
    imgui.setNextWindowBgAlpha(1);
    imgui.begin(name, null,
                imgui.WindowFlags_NoTitleBar | imgui.WindowFlags_NoMove |
                    imgui.WindowFlags_NoResize |
                    imgui.WindowFlags_NoSavedSettings);
    body();
    imgui.end();
  }

  // --- THEMES ----------------------------------------------------------
  {
    /** The theme's window background, from getColorU32 and from pixels. */
    async function themeBackground() {
      let u32;
      const draw = () => {
        u32 = imgui.getColorU32(imgui.Col_WindowBg);
        windowAt('theme', [100, 100], [200, 150]);
      };
      await h.run(draw);
      const image = await harness.capture(h, draw, capturePath);
      return {u32, pixel: harness.pixelAt(image, 200, 175)};
    }
    // Before the first newFrame() of this test: the context exists, no
    // frame is open.
    const beforeFrame = imgui.__test.state().frameOpen;
    imgui.styleColorsLight();
    const light = await themeBackground();
    imgui.styleColorsDark();
    const dark = await themeBackground();
    let inFrame = 0;
    await h.run(() => {
      const before = calls('styleColorsLight');
      imgui.styleColorsLight();
      imgui.styleColorsDark();
      inFrame = calls('styleColorsLight') - before;
    });
    const ok = beforeFrame === false && same(light.pixel, rgbOf(light.u32)) &&
        same(dark.pixel, rgbOf(dark.u32)) && !same(light.pixel, dark.pixel) &&
        inFrame === 1;
    console.log(ok ? 'THEMES ok' :
                     'THEMES WRONG ' +
                         JSON.stringify({beforeFrame, light, dark, inFrame}));
  }

  // --- COLOR -----------------------------------------------------------
  {
    let previous;
    const draw = () => {
      imgui.pushStyleColor(imgui.Col_WindowBg, [0, 0, 1, 1]);
      windowAt('vec4', [40, 40], [160, 120]);
      imgui.popStyleColor();
      imgui.pushStyleColor(imgui.Col_WindowBg, 0xFFFF0000);
      windowAt('u32', [240, 40], [160, 120]);
      imgui.popStyleColor();
      previous = imgui.getColorU32(imgui.Col_WindowBg);
      windowAt('after', [440, 40], [160, 120]);
    };
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const wrong = [];
    for (const [x, y, want] of [[120, 100, BLUE], [320, 100, BLUE],
                                [520, 100, rgbOf(previous)]]) {
      const got = harness.pixelAt(image, x, y);
      if (!same(got, want))
        wrong.push(x + ',' + y + '=' + got + ' want ' + want);
    }
    if (same(rgbOf(previous), BLUE))
      wrong.push('previous is blue');
    console.log(wrong.length === 0 ? 'COLOR ok' :
                                     'COLOR WRONG ' + wrong.join(' '));
  }

  // --- GETCOLOR --------------------------------------------------------
  {
    let full, half;
    const messages = [];
    // A pure function: no frame needed, each component saturated
    // (imgui.cpp:3003-3011).
    const converted = [
      imgui.colorConvertFloat4ToU32([0, 0, 1, 1]),
      imgui.colorConvertFloat4ToU32([0, 0, 1, 0.5]),
      imgui.colorConvertFloat4ToU32([2, -1, 0, 1]),
    ];
    await h.run(() => {
      imgui.pushStyleColor(imgui.Col_WindowBg, [0, 0, 1, 1]);
      full = imgui.getColorU32(imgui.Col_WindowBg);
      half = imgui.getColorU32(imgui.Col_WindowBg, 0.5);
      imgui.popStyleColor();
      messages.push(rejects('getColorU32',
                            () => imgui.getColorU32(imgui.Col_COUNT),
                            'ERR_OUT_OF_RANGE').message);
      rejects('getColorU32', () => imgui.getColorU32(1.5), 'ERR_OUT_OF_RANGE');
    });
    const ok = full === 0xFFFF0000 && half === 0x80FF0000 &&
        converted.join() === [0xFFFF0000, 0x80FF0000, 0xFF0000FF].join();
    console.log(ok ? 'GETCOLOR ok' :
                     'GETCOLOR WRONG ' + full + ' ' + half + ' ' + converted);
    for (const m of messages)
      console.log('GETCOLOR message ' + m);
  }

  // --- PADDING ---------------------------------------------------------
  {
    const first = {};
    await h.run(() => {
      for (const p of [10, 30]) {
        imgui.pushStyleVar(imgui.StyleVar_WindowPadding, [p, p]);
        windowAt('padding ' + p, [100 + 200 * (p / 30 | 0), 300], [150, 100],
                 () => {
                   imgui.text('x');
                   first[p] = imgui.getItemRectMin();
                 });
        imgui.popStyleVar();
      }
    });
    // The two windows are 200 units apart in x, the same in y.
    const dx = first[30][0] - first[10][0] - 200;
    const dy = first[30][1] - first[10][1];
    console.log(dx === 20 && dy === 20 ? 'PADDING ok' :
                                         'PADDING WRONG ' +
                                             JSON.stringify(first));
  }

  // --- ALPHA -----------------------------------------------------------
  {
    let pushed, popped;
    await h.run(() => {
      imgui.pushStyleVar(imgui.StyleVar_Alpha, 0.5);
      pushed = imgui.getColorU32([1, 0, 0, 1]);
      imgui.popStyleVar();
      popped = imgui.getColorU32([1, 0, 0, 1]);
    });
    console.log(pushed === 0x800000FF && popped === 0xFF0000FF ?
                    'ALPHA ok' :
                    'ALPHA WRONG ' + pushed + ' ' + popped);
  }

  // --- MISMATCH --------------------------------------------------------
  {
    const messages = [];
    const drew = await h.run(() => {
      messages.push(
          usage('pushStyleVar',
                () => imgui.pushStyleVar(imgui.StyleVar_Alpha, [1, 1]))
              .message);
      usage('pushStyleVar',
            () => imgui.pushStyleVar(imgui.StyleVar_WindowPadding, 1));
      messages.push(usage('popStyleVar', () => imgui.popStyleVar()).message);
      rejects('pushStyleVar',
              () => imgui.pushStyleVar(imgui.StyleVar_COUNT, 1),
              'ERR_OUT_OF_RANGE');
    });
    console.log(drew === true ? 'MISMATCH ok' : 'MISMATCH render=' + drew);
    for (const m of messages)
      console.log('MISMATCH message ' + m);
  }

  harness.checkGroupCoverage(h, 'Style');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}NOWINDOW ok{{$}}
// CHECK-NEXT: {{^}}THEMES ok{{$}}
// CHECK-NEXT: {{^}}COLOR ok{{$}}
// CHECK-NEXT: {{^}}GETCOLOR ok{{$}}
// CHECK-NEXT: {{^}}GETCOLOR message imgui.getColorU32(): idx must not be Col_COUNT{{$}}
// CHECK-NEXT: {{^}}PADDING ok{{$}}
// CHECK-NEXT: {{^}}ALPHA ok{{$}}
// CHECK-NEXT: {{^}}MISMATCH ok{{$}}
// CHECK-NEXT: {{^}}MISMATCH message imgui.pushStyleVar(): Calling PushStyleVar() variant with wrong type!{{$}}
// CHECK-NEXT: {{^}}MISMATCH message imgui.popStyleVar(): Calling PopStyleVar() too many times!{{$}}
// CHECK-NEXT: {{^}}COVERAGE Style ok{{$}}
// CODE: EXITCODE 0

// The three MISMATCH errors are ImGui's, traced as they are reported.
// TRACE: {{^}}[ui] imgui: context created
// TRACE: {{^}}[ui] imgui: usage error: Calling PushStyleVar() variant with wrong type!{{$}}
// TRACE: {{^}}[ui] imgui: usage error: Calling PushStyleVar() variant with wrong type!{{$}}
// TRACE: {{^}}[ui] imgui: usage error: Calling PopStyleVar() too many times!{{$}}
