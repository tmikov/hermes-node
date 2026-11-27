// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The application's per-frame cost, measured (the binding design's goal
// 5). Not a lit test and no threshold: run by hand, and the numbers go in
// the progress file.
//
//   HERMES_NODE_UI_TEST_HOOKS=1 python3 utils/with-display.py 120 \
//     <build>/bin/hermes-node-ui test/fixtures/imgui/app-measure.js \
//     test/fixtures/imgui/app-tree
//
// The application runs driven (examples/imgui-app/app.js, createApp with
// `driven: true` and the fixed clock), every panel open, the viewer
// showing dir1/b.js of the root given. After WARMUP frames (the first
// builds the font atlas and the dock nodes), MEASURED frames are run one
// tick each; for each, the JavaScript time of the whole frame is the one
// app.frame() measures itself with process.hrtime.bigint() around its
// body (app.lastFrameMs), and the binding calls are the frame's increase
// in the sum of imgui.__test.bindCalls(). The time between frames (the
// host's tick, the GPU) is not in it.

'use strict';

const path = require('path');
const harness = require('./harness.js');
const {createApp} = require('../../../examples/imgui-app/app.js');

const WARMUP = 10;
const MEASURED = 600;

function totalCalls(imgui) {
  const counts = imgui.__test.bindCalls();
  let sum = 0;
  for (const name of Object.keys(counts))
    sum += counts[name];
  return sum;
}

async function main() {
  const root = path.resolve(process.argv[2] || '.');
  const app = await createApp({root, clock: 'fixed', driven: true});
  const h = await harness.adopt(app);
  const {imgui} = app;
  await app.panels.viewer.load(path.join(root, 'dir1', 'b.js'));
  for (let i = 0; i < WARMUP; ++i) {
    app.setTime(i / 60);
    await h.run();
  }
  await app.idle();

  let sumMs = 0;
  let maxMs = 0;
  let sumCalls = 0;
  for (let i = 0; i < MEASURED; ++i) {
    app.setTime((WARMUP + i) / 60);
    const before = totalCalls(imgui);
    await h.run();
    sumCalls += totalCalls(imgui) - before;
    const ms = app.lastFrameMs;
    sumMs += ms;
    if (ms > maxMs)
      maxMs = ms;
  }
  const open = Object.values(app.panels).filter((p) => p.open[0]).length;
  console.log('frames ' + MEASURED + ' (after ' + WARMUP + ' warm-up), ' +
      open + ' panels open, scale ' + app.scale);
  console.log('JS time per frame: mean ' + (sumMs / MEASURED).toFixed(3) +
      ' ms, max ' + maxMs.toFixed(3) + ' ms');
  console.log('binding calls per frame: mean ' +
      (sumCalls / MEASURED).toFixed(1));
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack || e));
  process.exit(1);
});
