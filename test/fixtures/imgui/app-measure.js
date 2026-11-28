// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The application's per-frame cost, measured (the binding design's goal
// 5). Not a lit test and no threshold: run by hand, and the numbers go in
// the progress file.
//
//   HERMES_NODE_UI_TEST_HOOKS=1 python3 utils/with-display.py 300 \
//     <build>/bin/hermes-node-ui test/fixtures/imgui/app-measure.js
//
// The application runs driven (examples/imgui-app/app.js, createApp with
// `driven: true` and the fixed clock), every panel open, rooted at a
// temporary directory holding `b.js` (a copy of the fixture's
// dir1/b.js, which the viewer shows) and `big/`, BIG_ENTRIES files. Two
// states are measured, each MEASURED frames after WARMUP:
//
//  default  the root listed, big/ collapsed;
//  big      big/ opened by a click on its row, its whole listing loaded,
//           the Files panel scrolled to the top.
//
// For each frame, the JavaScript time of the whole frame is the one
// app.frame() measures itself with process.hrtime.bigint() around its
// body (app.lastFrameMs), and the binding calls are the frame's increase
// in the sum of imgui.__test.bindCalls(). In driven mode the application
// also records each drawn row's rectangle (app.rects), two binding calls
// per row that a normal run does not make; they are in these numbers. The
// time between frames (the host's tick, the GPU) is not.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const harness = require('./harness.js');
const {createApp} = require('../../../examples/imgui-app/app.js');

const WARMUP = 10;
const MEASURED = 600;
const BIG_ENTRIES = 5000;

function totalCalls(imgui) {
  const counts = imgui.__test.bindCalls();
  let sum = 0;
  for (const name of Object.keys(counts))
    sum += counts[name];
  return sum;
}

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'imgui-app-measure-'));
  const big = path.join(root, 'big');
  fs.mkdirSync(big);
  for (let i = 0; i < BIG_ENTRIES; ++i)
    fs.writeFileSync(path.join(big, 'f' + String(i).padStart(4, '0')), 'x\n');
  const viewed = path.join(root, 'b.js');
  fs.copyFileSync(path.join(__dirname, 'app-tree', 'dir1', 'b.js'), viewed);
  process.on('exit', () => fs.rmSync(root, {recursive: true, force: true}));

  const app = await createApp({root, clock: 'fixed', driven: true});
  const h = await harness.adopt(app);
  const {imgui} = app;
  let t = 0;
  const step = () => {
    app.setTime(t++ / 60);
    return h.run();
  };

  async function measure(label) {
    for (let i = 0; i < WARMUP; ++i)
      await step();
    let sumMs = 0;
    let maxMs = 0;
    let sumCalls = 0;
    for (let i = 0; i < MEASURED; ++i) {
      const before = totalCalls(imgui);
      await step();
      sumCalls += totalCalls(imgui) - before;
      const ms = app.lastFrameMs;
      sumMs += ms;
      if (ms > maxMs)
        maxMs = ms;
    }
    console.log(label + ': JS time per frame mean ' +
        (sumMs / MEASURED).toFixed(3) + ' ms, max ' + maxMs.toFixed(3) +
        ' ms; binding calls per frame mean ' +
        (sumCalls / MEASURED).toFixed(1));
  }

  await app.panels.viewer.load(viewed);
  for (let i = 0; i < WARMUP; ++i)
    await step();
  await app.idle();
  const open = Object.values(app.panels).filter((p) => p.open[0]).length;
  console.log('frames ' + MEASURED + ' per state (after ' + WARMUP +
      ' warm-up), ' + open + ' panels open, scale ' + app.scale);
  await measure('default');

  const row = app.rects.get(big);
  if (row === undefined) {
    console.log('FAILED big/ was not drawn');
    process.exit(1);
  }
  await harness.clickAt(h, null, harness.center(row));
  await app.idle();
  if (!app.rects.has(path.join(big, 'f0000'))) {
    for (let i = 0; i < 600 && !app.rects.has(path.join(big, 'f0000')); ++i)
      await step();
  }
  if (!app.rects.has(path.join(big, 'f0000'))) {
    console.log('FAILED big/ did not open');
    process.exit(1);
  }
  await measure('big (' + BIG_ENTRIES + ' entries open)');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack || e));
  process.exit(1);
});
