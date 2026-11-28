// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Draw list group (`imgui.drawList`): each primitive drawn into a
// borderless, padding-free window covering the display, whose background
// is one flat opaque colour pushed as WindowBg, and read back from a
// captured frame at flat interiors only: every stroke is 9 units thick and
// sampled at least 3 px from both of its edges, never on a 1 px stroke or
// an anti-aliased edge. The anti-aliasing flags are style state the
// binding does not expose; interior samples do not depend on them.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  NOWINDOW  before any window exists, colorConvertFloat4ToU32([1, 0, 0,
//            1]) is 0xFF0000FF (it needs no context).
//  COLOR     inside a frame, getColorU32([1, 0, 0, 1]) is 0xFF0000FF and
//            equal to colorConvertFloat4ToU32 of the same vector.
//  SHAPES    one captured frame:
//            addRectFilled([100, 100], [200, 180], red): red at (150, 140),
//              the background at (95, 140) and (205, 140);
//            addRect([240, 40], [400, 160], thickness 9): its stroke lies
//              inside the rectangle (StrokeInside, imgui_draw.cpp:2359), so
//              the colour 4 px inside the middle of each side, the
//              background at the centre;
//            addTriangleFilled: the colour at its centroid;
//            addCircleFilled: the colour at its centre;
//            addLine, horizontal on y = 300.5, thickness 9 (centred,
//              imgui_draw.cpp:1999, so rows 296..304): the colour on row
//              300, the background 8 px above and below;
//            addCircle, radius 60, thickness 9: its stroke lies inside the
//              radius (imgui_draw.cpp:2710, 2723-2724), so the colour on
//              the four axes at radius minus 4.5, the background at the
//              centre and at radius plus 4.5;
//            addBezierCubic with four collinear control points, thickness
//              9: a straight segment, sampled like the line;
//            a red filled rectangle drawn with the negative number
//              `0xFF000000 | 0xFF` and one with 0xFF0000FF: both red.
//  POLYLINE  addPolyline of a closed square (DrawFlags_Closed), thickness
//            9, centred on the outline: the colour at the middle of each
//            side's stroke, the background at the centre; the capture from
//            a flat Array and the capture from a Float32Array are
//            identical, byte for byte.
//  TEXT      addText: some pixel inside the text's calcTextSize box is
//            exactly the given colour (found by scanning the box), and none
//            outside the box is.
//  LIMITS    addPolyline with 16384 points (an Array and a Float32Array of
//            32768), an Array of length 2^32 - 1 or a sparse one of length
//            1e9 (refused on the length, before any element is read), or an
//            odd-length sequence, addBezierCubic with
//            numSegments 513 or -1, addRect with a flag outside
//            ImDrawFlags (bit 0): ERR_OUT_OF_RANGE, not called. A detached
//            Float32Array (__test.detachArrayBuffer):
//            ERR_INVALID_ARG_VALUE, not called. Accepted,
//            called, and the frame then renders: a closed 16383-point
//            polyline (65534 vertices, the most one primitive may reserve)
//            and a Bezier curve of 512 segments.
//  LENGTHS   the lengths and coordinates ImGui converts to int (the
//            derivation is lengthLimit()'s, in bind/bind_util.cpp; this
//            window's pixel density is 1, so the length limit is 2^20 =
//            1048576 units). Refused with ERR_OUT_OF_RANGE, not called:
//            Codex's addCircleFilled radius 1e10 and addLine thickness
//            1e10 with DrawFlags_StrokeCenterBiased; a radius of 1048577
//            for addCircle and addCircleFilled with an automatic segment
//            count; a thickness of 1048577 or -1048577 for addLine,
//            addRect, addCircle, addBezierCubic and addPolyline; a rounding
//            of 1048577 or -1048577 for addRect and addRectFilled; an
//            addRect or addRectFilled corner component of 2^30 + 128 (the
//            next float above 2^30) or its negative; an addText position
//            component of 2^31 or the float below -2^31. Accepted, called,
//            and the frame then renders: each length at 1048576 and
//            -1048576 (a radius at 1048576 only, and 1e10 with 32
//            segments, which is not converted), corners at 2^30 and
//            -2^30, addText at -2^31 and the largest float below 2^31.
//  COVERAGE  every DrawList function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  AddCircle() removed, enterImGui() kept: SHAPES fails;
//  enterImGui() removed from addTriangleFilled: COVERAGE fails;
//  getColorU32 returning 0 (its result, the group's stand-in for a
//  write-back, since no draw-list function writes one): COLOR fails;
//  addPolyline's 16383-point cap removed: LIMITS fails;
//  addPolyline passing one point fewer than given: POLYLINE fails;
//  (codex) radiusOk() returning true, addLine's thickness check removed,
//  cornerOk() returning true, or addText's position check removed: LENGTHS
//  fails, each at the first refusal it reopens.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePath = process.argv[2];

// Colours as ImU32 (0xAABBGGRR) and as the bytes they draw.
const BG_U32 = 0xFF004000;
const BG = [0, 64, 0];
const RED_U32 = 0xFF0000FF;
const RED = [255, 0, 0];
const GREEN_U32 = 0xFF00FF00;
const GREEN = [0, 255, 0];
const BLUE_U32 = 0xFFFF0000;
const BLUE = [0, 0, 255];
const YELLOW_U32 = 0xFF00FFFF;
const YELLOW = [255, 255, 0];
const MAGENTA_U32 = 0xFFFF00FF;
const MAGENTA = [255, 0, 255];
const CYAN_U32 = 0xFFFFFF00;
const CYAN = [0, 255, 255];
const WHITE_U32 = 0xFFFFFFFF;
const WHITE = [255, 255, 255];
const ORANGE_U32 = 0xFF0080FF;
const ORANGE = [255, 128, 0];

function fail(line) {
  console.log(line);
  process.exit(1);
}

function same(a, b) {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

async function main() {
  // --- NOWINDOW --------------------------------------------------------
  {
    const imgui = require('hermes-node:imgui');
    const v = imgui.colorConvertFloat4ToU32([1, 0, 0, 1]);
    console.log(v === 0xFF0000FF ? 'NOWINDOW ok' : 'NOWINDOW WRONG ' + v);
  }

  const h = await harness.open();
  const {imgui} = h;
  const dl = imgui.drawList;
  const calls = (name) => harness.calls(h, name);

  /** `fn` must throw `code` and leave `name`'s call count unchanged. */
  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ': ' + e.message);
    return e;
  }

  /** `fn` must reach ImGui exactly once through `name`. */
  function accepted(name, fn) {
    const before = calls(name);
    const result = fn();
    if (calls(name) !== before + 1)
      fail('NOTCALLED ' + name);
    return result;
  }

  /**
   * A window covering the display, with no decoration, padding, border or
   * rounding, and the flat background BG; `body` draws into it.
   */
  function canvas(body) {
    imgui.setNextWindowPos([0, 0]);
    imgui.setNextWindowSize([h.win.width, h.win.height]);
    imgui.pushStyleVar(imgui.StyleVar_WindowPadding, [0, 0]);
    imgui.pushStyleVar(imgui.StyleVar_WindowBorderSize, 0);
    imgui.pushStyleVar(imgui.StyleVar_WindowRounding, 0);
    imgui.pushStyleColor(imgui.Col_WindowBg, BG_U32);
    imgui.begin('canvas', null,
                imgui.WindowFlags_NoDecoration | imgui.WindowFlags_NoMove |
                    imgui.WindowFlags_NoResize |
                    imgui.WindowFlags_NoSavedSettings);
    body();
    imgui.end();
    imgui.popStyleColor();
    imgui.popStyleVar(3);
  }

  /** Each `[x, y, colour, label]` that is not `colour` in `image`. */
  function check(image, samples) {
    const wrong = [];
    for (const [x, y, color, label] of samples) {
      const got = harness.pixelAt(image, x, y);
      if (!same(got, color))
        wrong.push(label + '@' + x + ',' + y + '=' + got);
    }
    return wrong;
  }

  // --- COLOR -----------------------------------------------------------
  {
    let viaStyle, viaConvert;
    await h.run(() => canvas(() => {
      viaStyle = imgui.getColorU32([1, 0, 0, 1]);
      viaConvert = imgui.colorConvertFloat4ToU32([1, 0, 0, 1]);
    }));
    console.log(viaStyle === 0xFF0000FF && viaConvert === viaStyle ?
                    'COLOR ok' :
                    'COLOR WRONG ' + viaStyle + ' ' + viaConvert);
  }

  // --- SHAPES ----------------------------------------------------------
  {
    const draw = () => canvas(() => {
      dl.addRectFilled([100, 100], [200, 180], RED_U32);
      dl.addRect([240, 40], [400, 160], GREEN_U32, 0, 9);
      dl.addTriangleFilled([440, 40], [600, 40], [520, 160], BLUE_U32);
      dl.addCircleFilled([520, 260], 40, YELLOW_U32);
      dl.addLine([40, 300.5], [200, 300.5], MAGENTA_U32, 9);
      dl.addCircle([320, 300], 60, CYAN_U32, 0, 9);
      dl.addBezierCubic([40, 420.5], [100, 420.5], [160, 420.5], [220, 420.5],
                        WHITE_U32, 9);
      dl.addRectFilled([440, 360], [520, 440], 0xFF000000 | 0xFF);
      dl.addRectFilled([540, 360], [620, 440], 0xFF0000FF);
    });
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const wrong = check(image, [
      [150, 140, RED, 'rectFilled'],
      [95, 140, BG, 'rectFilled-left'],
      [205, 140, BG, 'rectFilled-right'],
      // addRect: stroke rows/columns 240..248, 391..399, 40..48, 151..159.
      [244, 100, GREEN, 'rect-left'],
      [395, 100, GREEN, 'rect-right'],
      [320, 44, GREEN, 'rect-top'],
      [320, 155, GREEN, 'rect-bottom'],
      [320, 100, BG, 'rect-centre'],
      [520, 80, BLUE, 'triangle'],
      [520, 260, YELLOW, 'circleFilled'],
      [120, 300, MAGENTA, 'line'],
      [120, 292, BG, 'line-above'],
      [120, 308, BG, 'line-below'],
      // addCircle: pixel centres at distance 55.5 and 64.5 from (320, 300).
      [375, 300, CYAN, 'circle-east'],
      [264, 300, CYAN, 'circle-west'],
      [320, 355, CYAN, 'circle-south'],
      [320, 244, CYAN, 'circle-north'],
      [320, 300, BG, 'circle-centre'],
      [384, 300, BG, 'circle-east-out'],
      [255, 300, BG, 'circle-west-out'],
      [320, 364, BG, 'circle-south-out'],
      [320, 235, BG, 'circle-north-out'],
      [130, 420, WHITE, 'bezier'],
      [130, 412, BG, 'bezier-above'],
      [130, 428, BG, 'bezier-below'],
      [480, 400, RED, 'negative-colour'],
      [580, 400, RED, 'unsigned-colour'],
    ]);
    console.log(wrong.length === 0 ? 'SHAPES ok' :
                                     'SHAPES WRONG ' + wrong.join(' '));
  }

  // --- POLYLINE --------------------------------------------------------
  {
    const square = [100.5, 100.5, 300.5, 100.5, 300.5, 300.5, 100.5, 300.5];
    const drawWith = (points) => () => canvas(() => {
      dl.addPolyline(points, ORANGE_U32, 9, imgui.DrawFlags_Closed);
    });
    await h.run(drawWith(square));
    const fromArray = await harness.capture(h, drawWith(square), capturePath);
    const typed = new Float32Array(square);
    await h.run(drawWith(typed));
    const fromTyped = await harness.capture(h, drawWith(typed), capturePath);
    // Stroke columns/rows 96..104 and 296..304.
    const wrong = check(fromArray, [
      [100, 200, ORANGE, 'left'],
      [300, 200, ORANGE, 'right'],
      [200, 100, ORANGE, 'top'],
      [200, 300, ORANGE, 'bottom'],
      [200, 200, BG, 'centre'],
    ]);
    if (!fromArray.data.equals(fromTyped.data))
      wrong.push('Float32Array capture differs');
    console.log(wrong.length === 0 ? 'POLYLINE ok' :
                                     'POLYLINE WRONG ' + wrong.join(' '));
  }

  // --- TEXT ------------------------------------------------------------
  {
    const pos = [50, 50];
    const label = 'Hello, ImGui';
    let size;
    const draw = () => canvas(() => {
      size = imgui.calcTextSize(label);
      dl.addText(pos, YELLOW_U32, label);
    });
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const box = {
      x0: pos[0],
      y0: pos[1],
      x1: Math.ceil(pos[0] + size[0]),
      y1: Math.ceil(pos[1] + size[1]),
    };
    let inside = 0, outside = 0;
    for (let y = 0; y < image.height; ++y) {
      for (let x = 0; x < image.width; ++x) {
        if (!same(harness.pixelAt(image, x, y), YELLOW))
          continue;
        if (x >= box.x0 && x < box.x1 && y >= box.y0 && y < box.y1)
          ++inside;
        else
          ++outside;
      }
    }
    console.log(inside > 0 && outside === 0 ?
                    'TEXT ok' :
                    'TEXT WRONG inside=' + inside + ' outside=' + outside +
                        ' box=' + JSON.stringify(box));
  }

  // --- LIMITS ----------------------------------------------------------
  {
    const messages = [];
    const zigzag = (points) => {
      const a = new Array(points * 2);
      for (let i = 0; i < points; ++i) {
        a[2 * i] = 20 + (i % 600);
        a[2 * i + 1] = 20 + (i % 2) * 40;
      }
      return a;
    };
    const drew = await h.run(() => canvas(() => {
      messages.push(rejects('drawList.addPolyline',
                            () => dl.addPolyline(zigzag(16384), RED_U32, 1),
                            'ERR_OUT_OF_RANGE').message);
      rejects('drawList.addPolyline',
              () => dl.addPolyline(new Float32Array(32768), RED_U32, 1),
              'ERR_OUT_OF_RANGE');
      // Refused on the length alone, before an element is read or anything
      // allocated for them.
      const sparse = [];
      sparse.length = 1e9;
      for (const points of [new Array(2 ** 32 - 1), sparse])
        messages.push(rejects('drawList.addPolyline',
                              () => dl.addPolyline(points, RED_U32, 1),
                              'ERR_OUT_OF_RANGE').message);
      const detached = new Float32Array(4);
      imgui.__test.detachArrayBuffer(detached.buffer);
      messages.push(rejects('drawList.addPolyline',
                            () => dl.addPolyline(detached, RED_U32, 1),
                            'ERR_INVALID_ARG_VALUE').message);
      messages.push(rejects('drawList.addPolyline',
                            () => dl.addPolyline([0, 0, 1], RED_U32, 1),
                            'ERR_OUT_OF_RANGE').message);
      rejects('drawList.addPolyline',
              () => dl.addPolyline(new Float32Array(3), RED_U32, 1),
              'ERR_OUT_OF_RANGE');
      for (const n of [513, -1])
        messages.push(
            rejects('drawList.addBezierCubic',
                    () => dl.addBezierCubic([0, 0], [1, 0], [2, 0], [3, 0],
                                            RED_U32, 1, n),
                    'ERR_OUT_OF_RANGE').message);
      rejects('drawList.addRect',
              () => dl.addRect([0, 0], [10, 10], RED_U32, 0, 1, 1),
              'ERR_OUT_OF_RANGE');
      accepted('drawList.addPolyline',
               () => dl.addPolyline(zigzag(16383), RED_U32, 1,
                                    imgui.DrawFlags_Closed));
      accepted('drawList.addBezierCubic',
               () => dl.addBezierCubic([20, 200], [200, 100], [400, 300],
                                       [600, 200], RED_U32, 1, 512));
    }));
    console.log(drew === true ? 'LIMITS ok' : 'LIMITS render=' + drew);
    for (const m of messages)
      console.log('LIMITS message ' + m);
  }

  // --- LENGTHS ---------------------------------------------------------
  {
    const L = 1048576;
    const messages = [];
    const drew = await h.run(() => canvas(() => {
      const rx = (name, fn) =>
          messages.push(rejects('drawList.' + name, fn, 'ERR_OUT_OF_RANGE')
                            .message);
      // Codex's two sequences.
      rx('addCircleFilled', () => dl.addCircleFilled([100, 100], 1e10, WHITE_U32));
      rx('addLine', () => dl.addLine([10, 10], [100, 100], WHITE_U32, 1e10,
                                     imgui.DrawFlags_StrokeCenterBiased));
      rx('addCircle', () => dl.addCircle([100, 100], L + 1, WHITE_U32));
      rx('addCircleFilled', () => dl.addCircleFilled([100, 100], L + 1, WHITE_U32, 0));
      for (const t of [L + 1, -(L + 1)]) {
        rx('addLine', () => dl.addLine([10, 10], [100, 100], WHITE_U32, t));
        rx('addRect', () => dl.addRect([10, 10], [100, 100], WHITE_U32, 0, t));
        rx('addCircle', () => dl.addCircle([100, 100], 10, WHITE_U32, 0, t));
        rx('addBezierCubic', () => dl.addBezierCubic([0, 0], [1, 0], [2, 0],
                                                     [3, 0], WHITE_U32, t));
        rx('addPolyline', () => dl.addPolyline([0, 0, 10, 10], WHITE_U32, t));
        rx('addRect', () => dl.addRect([10, 10], [100, 100], WHITE_U32, t));
        rx('addRectFilled', () => dl.addRectFilled([10, 10], [100, 100], WHITE_U32, t));
      }
      const far = 2 ** 30 + 128;
      for (const corner of [[far, 0], [0, far], [-far, 0], [0, -far]]) {
        rx('addRect', () => dl.addRect(corner, [10, 10], WHITE_U32));
        rx('addRect', () => dl.addRect([10, 10], corner, WHITE_U32));
        rx('addRectFilled', () => dl.addRectFilled(corner, [10, 10], WHITE_U32));
        rx('addRectFilled', () => dl.addRectFilled([10, 10], corner, WHITE_U32));
      }
      for (const pos of [[2 ** 31, 0], [0, 2 ** 31], [-(2 ** 31) - 256, 0],
                         [0, -(2 ** 31) - 256]])
        rx('addText', () => dl.addText(pos, WHITE_U32, 't'));

      const ok = (name, fn) => accepted('drawList.' + name, fn);
      ok('addCircle', () => dl.addCircle([100, 100], L, WHITE_U32));
      ok('addCircleFilled', () => dl.addCircleFilled([100, 100], L, WHITE_U32));
      ok('addCircleFilled', () => dl.addCircleFilled([100, 100], 1e10, WHITE_U32, 32));
      ok('addCircle', () => dl.addCircle([100, 100], 1e10, WHITE_U32, 32));
      for (const t of [L, -L]) {
        ok('addLine', () => dl.addLine([10, 10], [100, 100], WHITE_U32, t,
                                       imgui.DrawFlags_StrokeCenterBiased));
        ok('addRect', () => dl.addRect([10, 10], [100, 100], WHITE_U32, 0, t,
                                       imgui.DrawFlags_StrokeCenterBiased));
        ok('addCircle', () => dl.addCircle([100, 100], 10, WHITE_U32, 0, t,
                                           imgui.DrawFlags_StrokeOutside));
        ok('addBezierCubic', () => dl.addBezierCubic([0, 0], [1, 0], [2, 0],
                                                     [3, 0], WHITE_U32, t));
        ok('addPolyline', () => dl.addPolyline([0, 0, 10, 10], WHITE_U32, t));
        ok('addRect', () => dl.addRect([10, 10], [100, 100], WHITE_U32, t, 9));
        ok('addRectFilled', () => dl.addRectFilled([10, 10], [100, 100], WHITE_U32, t));
      }
      ok('addRect', () => dl.addRect([-(2 ** 30), -(2 ** 30)], [2 ** 30, 2 ** 30],
                                     WHITE_U32, L, L));
      ok('addRectFilled', () => dl.addRectFilled([-(2 ** 30), -(2 ** 30)],
                                                 [2 ** 30, 2 ** 30], WHITE_U32, L));
      ok('addText', () => dl.addText([-(2 ** 31), -(2 ** 31)], WHITE_U32, 't'));
      ok('addText', () => dl.addText([2147483520, 2147483520], WHITE_U32, 't'));
    }));
    console.log(drew === true ? 'LENGTHS ok' : 'LENGTHS render=' + drew);
    for (const m of messages.slice(0, 4))
      console.log('LENGTHS message ' + m);
    console.log('LENGTHS refused ' + messages.length);
  }

  harness.checkGroupCoverage(h, 'DrawList');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}NOWINDOW ok{{$}}
// CHECK-NEXT: {{^}}COLOR ok{{$}}
// CHECK-NEXT: {{^}}SHAPES ok{{$}}
// CHECK-NEXT: {{^}}POLYLINE ok{{$}}
// CHECK-NEXT: {{^}}TEXT ok{{$}}
// CHECK-NEXT: {{^}}LIMITS ok{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addPolyline(): points must hold at most 32766 numbers; got 32768{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addPolyline(): points must hold at most 32766 numbers; got 4294967295{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addPolyline(): points must hold at most 32766 numbers; got 1000000000{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addPolyline(): points is a detached Float32Array{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addPolyline(): points must hold an even count of numbers (x, y pairs); got 3{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addBezierCubic(): numSegments must be an integer from 0 to 512; got 513{{$}}
// CHECK-NEXT: {{^}}LIMITS message imgui.drawList.addBezierCubic(): numSegments must be an integer from 0 to 512; got -1{{$}}
// CHECK-NEXT: {{^}}LENGTHS ok{{$}}
// CHECK-NEXT: {{^}}LENGTHS message imgui.drawList.addCircleFilled(): radius must be at most 1048576 (2^20 pixels at pixel density 1) with an automatic segment count; got 1e+10{{$}}
// CHECK-NEXT: {{^}}LENGTHS message imgui.drawList.addLine(): thickness must be from -1048576 to 1048576 (2^20 pixels at pixel density 1); got 1e+10{{$}}
// CHECK-NEXT: {{^}}LENGTHS message imgui.drawList.addCircle(): radius must be at most 1048576 (2^20 pixels at pixel density 1) with an automatic segment count; got 1048577{{$}}
// CHECK-NEXT: {{^}}LENGTHS message imgui.drawList.addCircleFilled(): radius must be at most 1048576 (2^20 pixels at pixel density 1) with an automatic segment count; got 1048577{{$}}
// CHECK-NEXT: {{^}}LENGTHS refused 38{{$}}
// CHECK-NEXT: {{^}}COVERAGE DrawList ok{{$}}
// CODE: EXITCODE 0

// No case here is a usage error, so none is traced (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
