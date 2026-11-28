// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Monitor: process.memoryUsage() (rss; the heap figures only when the
// runtime reports them, which this one does not), os.loadavg(), ImGui's
// frame rate and the JavaScript time per frame, sampled four times a
// second by an unref'd setInterval timer into Float32Array rings and drawn
// with plotLines and plotHistogram. The timer only writes numbers into
// the rings; drawing reads whatever is there.

'use strict';

const os = require('os');

const SAMPLES = 240;
const PERIOD_MS = 250;
const PLOT_HEIGHT = 32;

function ring() {
  return {values: new Float32Array(SAMPLES), next: 0, last: 0, max: 0};
}

const rings = {
  rss: ring(),
  heapUsed: ring(),
  heapTotal: ring(),
  load: ring(),
  fps: ring(),
  frameMs: ring(),
};

function push(r, v) {
  r.values[r.next] = v;
  r.next = (r.next + 1) % SAMPLES;
  r.last = v;
  if (v > r.max)
    r.max = v;
}

let timer = null;
let loadAverage = [0, 0, 0];

const MiB = 1024 * 1024;

function mib(bytes) {
  return (bytes / MiB).toFixed(1) + ' MiB';
}

module.exports = {
  name: 'Monitor',
  open: [true],
  rings,

  /**
   * Start sampling. `sample()` returns the application's frame figures,
   * `{fps, frameMs}`, as of the last frame.
   */
  start(sample) {
    if (timer !== null)
      return;
    timer = setInterval(() => {
      const m = process.memoryUsage();
      push(rings.rss, m.rss / MiB);
      push(rings.heapUsed, m.heapUsed / MiB);
      push(rings.heapTotal, m.heapTotal / MiB);
      loadAverage = os.loadavg();
      push(rings.load, loadAverage[0]);
      const f = sample();
      push(rings.fps, f.fps);
      push(rings.frameMs, f.frameMs);
    }, PERIOD_MS);
    timer.unref();
  },

  draw(ctx) {
    const {imgui} = ctx;
    const plot = (label, r, overlay, histogram) => {
      const fn = histogram ? imgui.plotHistogram : imgui.plotLines;
      fn(label, r.values, r.next, overlay, 0, Math.max(r.max, 1e-3),
          [-imgui.FLT_MIN, PLOT_HEIGHT]);
    };
    imgui.separatorText('Memory');
    plot('##rss', rings.rss, 'rss ' + mib(rings.rss.last * MiB));
    // This runtime reports heapUsed and heapTotal as 0; plot them only
    // once they are real, rather than draw two flat lines.
    if (rings.heapTotal.max > 0) {
      plot('##heapUsed', rings.heapUsed,
          'heap used ' + mib(rings.heapUsed.last * MiB));
      plot('##heapTotal', rings.heapTotal,
          'heap total ' + mib(rings.heapTotal.last * MiB));
    } else {
      imgui.textDisabled('heap: not reported by this runtime');
    }
    imgui.separatorText('Load');
    imgui.text(
        'load average ' + loadAverage.map((v) => v.toFixed(2)).join(' '));
    plot('##load', rings.load, '1 min ' + rings.load.last.toFixed(2));
    imgui.separatorText('Frames');
    plot('##fps', rings.fps, rings.fps.last.toFixed(1) + ' frames/s');
    plot('##frameMs', rings.frameMs,
        'JS ' + rings.frameMs.last.toFixed(2) + ' ms per frame', true);
  },
};
