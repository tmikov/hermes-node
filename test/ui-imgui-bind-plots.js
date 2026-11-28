// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Plots group: plotLines and plotHistogram on a known, opaque frame
// background, with their values from an Array and from a Float32Array.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  RECTS     plotHistogram and plotLines of [0, 1, 0, 1], scale 0..1, graph
//            size [200, 100]: each item rectangle is 200 x 100 plus the
//            label (ItemInnerSpacing.x, pushed as 6, and the label's
//            calcTextSize width; PlotEx's total_bb, imgui_widgets.cpp:9083).
//  HISTOGRAM in the captured frame, with FrameBg green and PlotHistogram red
//            (and their hovered colours the same): the middle of bar 1 is
//            red, the middle of bar 0's upper half is green (bar n spans
//            the n-th quarter of the frame's inner width and has value n's
//            height, imgui_widgets.cpp:9146-9175). Inside the line plot's
//            frame some pixel is not green (a 1 px line is drawn there; its
//            exact colour is not asserted).
//  TYPED     the same frame with both plots' values a Float32Array: the
//            capture is identical, byte for byte.
//  OFFSET    the histogram of [1, 0, 1, 0] with valuesOffset 1 is the same
//            picture as that of [0, 1, 0, 1] (ImGui reads value
//            (i + offset) % count, imgui_widgets.cpp:9138).
//  EMPTY     an empty Array (and an empty Float32Array): the frame is drawn
//            and its whole inner area is green.
//  REJECTED  valuesOffset 4 on 4 values, -1, 1.5, and 1 on an empty array;
//            a NaN value in an Array and in a Float32Array; an empty label
//            at the window's root (label-id): refused, not called
//            (ERR_OUT_OF_RANGE, the last ERR_IMGUI_USAGE). For both plots:
//            an Array of length 2^32 - 1, a sparse Array of length 1e9, and
//            4097 values as an Array and a Float32Array (the cap is 4096,
//            bind_plots.cpp's kMaxPlotValues): ERR_OUT_OF_RANGE, refused
//            before any element is read; a detached Float32Array (made with
//            __test.detachArrayBuffer): ERR_INVALID_ARG_VALUE. graphSize[0]
//            3e9 (PlotEx's (int) of the width): ERR_OUT_OF_RANGE.
//            valuesOffset 3 on 4 values, an overlay text, and 4096 values
//            in a frame 4096 wide (the widest the cap allows) are
//            accepted.
//  HISTSCALE a histogram's values and scales (histogramOk() in
//            bind/bind_plots.cpp: a bar's corners are converted to int, and
//            NaN reaches them from an overflowing scale): refused with
//            ERR_OUT_OF_RANGE, not called: a value of FLT_MAX, a scaleMin
//            or scaleMax of -FLT_MAX, the values [0, 1e-45] scaled from
//            themselves and the explicit scale 0 .. 1e-45 (each a range
//            whose reciprocal is infinite). Accepted, called and drawn: the
//            values [FLT_MAX/2, -FLT_MAX/2] scaled from themselves, [0, 0]
//            (equal scales), and plotLines of [0, 1e-45], which draws lines
//            and needs no bound.
//  COVERAGE  every Plots function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  PlotHistogram() removed, enterImGui() kept: RECTS and HISTOGRAM fail;
//  enterImGui() removed from plotLines: REJECTED fails first
//  (`NOTCALLED plotLines`, its accepted edge), before COVERAGE;
//  valuesOffset's upper bound removed: REJECTED fails;
//  valuesOffset passed as 0: OFFSET fails;
//  the graph size not passed (ImVec2(0, 0)): RECTS fails;
//  (codex) histogramOk() returning true: HISTSCALE fails.
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
const GREEN = [0, 255, 0];
const RED = [255, 0, 0];
const SIZE = [200, 100];
const INNER_SPACING = 6;
const FRAME_PADDING = 4;

function fail(line) {
  console.log(line);
  process.exit(1);
}

function same(a, b) {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

async function main() {
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

  /** `fn` must reach ImGui exactly once through `name`. */
  function accepted(name, fn) {
    const before = calls(name);
    const result = fn();
    if (calls(name) !== before + 1)
      fail('NOTCALLED ' + name);
    return result;
  }

  /**
   * One opaque window with the plot colours pushed; `body` draws in it and
   * returns the item rectangles it records.
   */
  function plotWindow(body) {
    for (const col of [imgui.Col_FrameBg, imgui.Col_FrameBgHovered,
                       imgui.Col_FrameBgActive])
      imgui.pushStyleColor(col, [0, 1, 0, 1]);
    for (const col of [imgui.Col_PlotHistogram, imgui.Col_PlotHistogramHovered,
                       imgui.Col_PlotLines, imgui.Col_PlotLinesHovered])
      imgui.pushStyleColor(col, [1, 0, 0, 1]);
    imgui.pushStyleColor(imgui.Col_WindowBg, [0, 0, 1, 1]);
    imgui.pushStyleVar(imgui.StyleVar_ItemInnerSpacing,
                       [INNER_SPACING, INNER_SPACING]);
    imgui.pushStyleVar(imgui.StyleVar_FramePadding,
                       [FRAME_PADDING, FRAME_PADDING]);
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin('plots', null,
                imgui.WindowFlags_NoTitleBar | imgui.WindowFlags_NoMove |
                    imgui.WindowFlags_NoResize |
                    imgui.WindowFlags_NoSavedSettings);
    body();
    imgui.end();
    imgui.popStyleVar(2);
    imgui.popStyleColor(8);
  }

  /** Both plots of `values`; the rectangles go into `rects`. */
  function bothPlots(values, rects, offset = 0) {
    return () => plotWindow(() => {
      imgui.plotHistogram('hist', values, offset, null, 0, 1, SIZE);
      rects.hist = harness.rect(imgui);
      imgui.plotLines('lines', values, offset, null, 0, 1, SIZE);
      rects.lines = harness.rect(imgui);
    });
  }

  /** Pixels in the inner area (frame minus FramePadding) of `r`. */
  function innerPixels(image, r) {
    const out = [];
    const x0 = Math.floor(r.min[0]), y0 = Math.floor(r.min[1]);
    for (let y = y0 + FRAME_PADDING; y < y0 + SIZE[1] - FRAME_PADDING; ++y) {
      for (let x = x0 + FRAME_PADDING; x < x0 + SIZE[0] - FRAME_PADDING; ++x)
        out.push(harness.pixelAt(image, x, y));
    }
    return out;
  }

  // --- RECTS, HISTOGRAM ------------------------------------------------
  const rects = {};
  let labelWidth = {};
  let fromArray;
  {
    const draw = bothPlots([0, 1, 0, 1], rects);
    await h.run(() => {
      labelWidth = {
        hist: imgui.calcTextSize('hist')[0],
        lines: imgui.calcTextSize('lines')[0],
      };
      draw();
    });
    fromArray = await harness.capture(h, draw, capturePath);
    const wrong = [];
    for (const name of ['hist', 'lines']) {
      const r = rects[name];
      const w = r.max[0] - r.min[0], hgt = r.max[1] - r.min[1];
      if (w !== SIZE[0] + INNER_SPACING + labelWidth[name] || hgt !== SIZE[1])
        wrong.push(name + ' ' + JSON.stringify(r) + ' label ' +
                   labelWidth[name]);
    }
    console.log(wrong.length === 0 ? 'RECTS ok' :
                                     'RECTS WRONG ' + wrong.join(' '));

    const r = rects.hist;
    const innerW = SIZE[0] - 2 * FRAME_PADDING;
    const innerH = SIZE[1] - 2 * FRAME_PADDING;
    const x0 = r.min[0] + FRAME_PADDING;
    const y0 = r.min[1] + FRAME_PADDING;
    const bar1 = [Math.floor(x0 + innerW * 1.5 / 4), Math.floor(y0 + innerH / 2)];
    const bar0Top =
        [Math.floor(x0 + innerW * 0.5 / 4), Math.floor(y0 + innerH / 4)];
    const pixels = [];
    const got1 = harness.pixelAt(fromArray, ...bar1);
    const got0 = harness.pixelAt(fromArray, ...bar0Top);
    if (!same(got1, RED))
      pixels.push('bar1@' + bar1 + '=' + got1);
    if (!same(got0, GREEN))
      pixels.push('bar0top@' + bar0Top + '=' + got0);
    const lineDrawn =
        innerPixels(fromArray, rects.lines).some((p) => !same(p, GREEN));
    if (!lineDrawn)
      pixels.push('no line drawn');
    console.log(pixels.length === 0 ? 'HISTOGRAM ok' :
                                      'HISTOGRAM WRONG ' + pixels.join(' '));
  }

  // --- TYPED -----------------------------------------------------------
  {
    const typedRects = {};
    const draw = bothPlots(new Float32Array([0, 1, 0, 1]), typedRects);
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    console.log(image.data.equals(fromArray.data) ? 'TYPED ok' :
                                                     'TYPED WRONG differs');
  }

  // --- OFFSET ----------------------------------------------------------
  {
    const offsetRects = {};
    const draw = () => plotWindow(() => {
      imgui.plotHistogram('hist', [1, 0, 1, 0], 1, null, 0, 1, SIZE);
      offsetRects.hist = harness.rect(imgui);
    });
    const plain = () => plotWindow(() => {
      imgui.plotHistogram('hist', [0, 1, 0, 1], 0, null, 0, 1, SIZE);
    });
    await h.run(plain);
    const a = await harness.capture(h, plain, capturePath);
    await h.run(draw);
    const b = await harness.capture(h, draw, capturePath);
    console.log(a.data.equals(b.data) ? 'OFFSET ok' : 'OFFSET WRONG differs');
  }

  // --- EMPTY -----------------------------------------------------------
  {
    const wrong = [];
    for (const values of [[], new Float32Array(0)]) {
      const emptyRects = {};
      const draw = () => plotWindow(() => {
        imgui.plotHistogram('hist', values, 0, null, 0, 1, SIZE);
        emptyRects.hist = harness.rect(imgui);
      });
      await h.run(draw);
      const image = await harness.capture(h, draw, capturePath);
      const r = emptyRects.hist;
      if (r.max[1] - r.min[1] !== SIZE[1])
        wrong.push('rect ' + JSON.stringify(r));
      const bad = innerPixels(image, r).filter((p) => !same(p, GREEN));
      if (bad.length !== 0)
        wrong.push(bad.length + ' pixels not green, first ' + bad[0]);
    }
    console.log(wrong.length === 0 ? 'EMPTY ok' :
                                     'EMPTY WRONG ' + wrong.join(' '));
  }

  // --- REJECTED --------------------------------------------------------
  {
    const messages = [];
    await h.run(() => plotWindow(() => {
      for (const [values, offset] of [[[0, 1, 0, 1], 4], [[0, 1, 0, 1], -1],
                                      [[0, 1, 0, 1], 1.5], [[], 1],
                                      [new Float32Array(4), 4]])
        messages.push(rejects('plotLines',
                              () => imgui.plotLines('p', values, offset),
                              'ERR_OUT_OF_RANGE').message);
      messages.push(rejects('plotHistogram',
                            () => imgui.plotHistogram('p', [0, NaN]),
                            'ERR_OUT_OF_RANGE').message);
      messages.push(
          rejects('plotHistogram',
                  () => imgui.plotHistogram('p', new Float32Array([0, NaN])),
                  'ERR_OUT_OF_RANGE').message);
      messages.push(rejects('plotLines', () => imgui.plotLines('', [0, 1]),
                            'ERR_IMGUI_USAGE').message);
      // Too many values, refused before any is read or anything is
      // allocated for them: an Array of length 2^32 - 1, a sparse one, and
      // one past the 4096-value cap in both forms.
      const sparse = [];
      sparse.length = 1e9;
      for (const name of ['plotLines', 'plotHistogram']) {
        for (const values of [new Array(2 ** 32 - 1), sparse,
                              new Array(4097).fill(0), new Float32Array(4097)])
          messages.push(rejects(name, () => imgui[name]('p', values),
                                'ERR_OUT_OF_RANGE').message);
        // A Float32Array whose buffer was detached keeps its length and has
        // no data (hermes_napi_typedarray.cpp:221-232).
        const detached = new Float32Array(4);
        imgui.__test.detachArrayBuffer(detached.buffer);
        messages.push(rejects(name, () => imgui[name]('p', detached),
                              'ERR_INVALID_ARG_VALUE').message);
      }
      messages.push(
          rejects('plotLines',
                  () => imgui.plotLines('p', [0, 1], 0, null, 0, 1, [3e9, 10]),
                  'ERR_OUT_OF_RANGE').message);
      accepted('plotHistogram',
               () => imgui.plotHistogram('cap', new Float32Array(4096), 0, null,
                                         0, 1, [4096, 20]));
      accepted('plotLines', () => imgui.plotLines('ok3', [0, 1, 0, 1], 3));
      accepted('plotHistogram',
               () => imgui.plotHistogram('ov', [0, 1], 0, 'overlay'));
    }));
    console.log('REJECTED ok');
    for (const m of messages)
      console.log('REJECTED message ' + m);
  }

  // --- HISTSCALE -------------------------------------------------------
  {
    const messages = [];
    const M = imgui.FLT_MAX;
    const drew = await h.run(() => plotWindow(() => {
      const rx = (...args) =>
          messages.push(rejects('plotHistogram',
                                () => imgui.plotHistogram('h', ...args),
                                'ERR_OUT_OF_RANGE').message);
      rx([0, M]);
      rx([0, 1], 0, null, -M, 1);
      rx([0, 1], 0, null, 0, -M);
      rx([0, 1e-45]);
      rx([0.5], 0, null, 0, 1e-45);
      accepted('plotHistogram', () => imgui.plotHistogram('a', [M / 2, -M / 2]));
      accepted('plotHistogram', () => imgui.plotHistogram('b', [0, 0]));
      accepted('plotLines', () => imgui.plotLines('c', [0, 1e-45]));
    }));
    console.log(drew === true ? 'HISTSCALE ok' : 'HISTSCALE render=' + drew);
    for (const m of messages)
      console.log('HISTSCALE message ' + m);
  }

  harness.checkGroupCoverage(h, 'Plots');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}RECTS ok{{$}}
// CHECK-NEXT: {{^}}HISTOGRAM ok{{$}}
// CHECK-NEXT: {{^}}TYPED ok{{$}}
// CHECK-NEXT: {{^}}OFFSET ok{{$}}
// CHECK-NEXT: {{^}}EMPTY ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): valuesOffset must be an integer from 0 to 3; got 4{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): valuesOffset must be an integer from 0 to 3; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): valuesOffset must be an integer{{.*}}{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): valuesOffset must be 0 for an empty values; got 1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): valuesOffset must be an integer from 0 to 3; got 4{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values[1] must be a finite number{{.*}}{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values[1] must be a finite number{{.*}}{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): label gives the item the window's own ID{{.*}}{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): values must hold at most 4096 numbers; got 4294967295{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): values must hold at most 4096 numbers; got 1000000000{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): values must hold at most 4096 numbers; got 4097{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): values must hold at most 4096 numbers; got 4097{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): values is a detached Float32Array{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values must hold at most 4096 numbers; got 4294967295{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values must hold at most 4096 numbers; got 1000000000{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values must hold at most 4096 numbers; got 4097{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values must hold at most 4096 numbers; got 4097{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotHistogram(): values is a detached Float32Array{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.plotLines(): graphSize[0] must be less than 2147483648; got 3e+09{{$}}
// CHECK-NEXT: {{^}}HISTSCALE ok{{$}}
// CHECK-NEXT: {{^}}HISTSCALE message imgui.plotHistogram(): values[1] must be within -FLT_MAX/2 .. FLT_MAX/2 in a histogram; got 3.40282e+38{{$}}
// CHECK-NEXT: {{^}}HISTSCALE message imgui.plotHistogram(): scaleMin must be FLT_MAX or within -FLT_MAX/2 .. FLT_MAX/2 in a histogram; got -3.40282e+38{{$}}
// CHECK-NEXT: {{^}}HISTSCALE message imgui.plotHistogram(): scaleMax must be FLT_MAX or within -FLT_MAX/2 .. FLT_MAX/2 in a histogram; got -3.40282e+38{{$}}
// CHECK-NEXT: {{^}}HISTSCALE message imgui.plotHistogram(): a histogram's scale range must be 0 or have a finite reciprocal; got 1.4013e-45 (scaleMax 1.4013e-45, scaleMin 0, a scale of FLT_MAX taken from the values){{$}}
// CHECK-NEXT: {{^}}HISTSCALE message imgui.plotHistogram(): a histogram's scale range must be 0 or have a finite reciprocal; got 1.4013e-45 (scaleMax 1.4013e-45, scaleMin 0, a scale of FLT_MAX taken from the values){{$}}
// CHECK-NEXT: {{^}}COVERAGE Plots ok{{$}}
// CODE: EXITCODE 0

// The label-id refusal is the binding's, traced as a usage error.
// TRACE: {{^}}[ui] imgui: context created
