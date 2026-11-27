// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shared helpers for the ImGui window tests (test/ui-imgui-*.js).
//
// A test opens one window with `open()` and passes the handle it returns to
// every other helper. The handle carries two numbers the capture agreement
// needs, and both are kept honest rather than guessed:
//
// - `h.published` is the id of the last publication this harness caused.
//   The host submits once per tick however many `render()` calls ran in it,
//   so the count goes up once per tick in which any callback drew. A
//   retired generation's work gets no publication id, so after anything
//   that can retire one (a resize) the count is re-read from the host with
//   `resync()`.
// - `h.generation` starts at 1 and is read back, never counted: after a
//   resize a test draws one uncaptured frame, sets
//   `h.generation = imgui.__test.state().targetGeneration`, then calls
//   `resync(h)`.
//
// The window tests run with HERMES_NODE_UI_TEST_HOOKS set, so `__test` and
// `ui.__captureFrame` exist.

'use strict';

const fs = require('fs');
const path = require('path');

/** The renderer's clear colour, as bytes: {26, 26, 31} / 255. */
const CLEAR = [26, 26, 31];

/** How many presented frames a capture expected to match may take. */
const CAPTURE_FRAME_BUDGET = 900;

/** The bound on `until()`, in frames (never in time). */
const UNTIL_FRAME_BOUND = 600;

/**
 * Open the test window and load the addon after it exists
 * (`hnui_get_window(env)` answers NULL until then).
 */
async function open(options) {
  const ui = require('hermes-node:ui');
  const win = await ui.createWindow(Object.assign(
      {title: 'hermes-node imgui test', width: 640, height: 480}, options));
  const imgui = require('hermes-node:imgui');
  return {ui, win, imgui, published: 0, generation: 1};
}

/**
 * Run every function in `fns` as a frame callback of ONE tick, and resolve
 * with their return values once the last has run.
 *
 * Each function returns whether it drew (what its `render()` returned);
 * that is how `h.published` is kept. All are registered before the tick
 * starts, so the host runs them in one batch and submits once.
 *
 * Resolved from a setImmediate, after the tick has ended. Resolved from the
 * last callback itself, the caller's continuation would run at the host's
 * checkpoint after that callback, still inside the tick, where
 * hnui_frame_target answers and a render() the test meant to be outside
 * any frame callback would draw.
 */
function tick(h, fns) {
  // With no callbacks nothing would ever resolve the promise.
  if (fns.length === 0)
    throw new Error('harness.tick(): fns must name at least one callback');
  return new Promise((resolve) => {
    const results = new Array(fns.length);
    let remaining = fns.length;
    fns.forEach((fn, i) => {
      h.ui.requestAnimationFrame(() => {
        results[i] = fn();
        if (--remaining === 0) {
          if (results.some((r) => r === true))
            ++h.published;
          setImmediate(() => resolve(results));
        }
      });
    });
  });
}

/** One ImGui frame in one tick; resolves with what `render()` returned. */
async function frame(h, drawFn) {
  const [drew] = await tick(h, [() => {
    h.imgui.newFrame();
    drawFn();
    return h.imgui.render();
  }]);
  return drew;
}

/**
 * Re-read `h.published` from the host, for use after anything that can
 * retire a generation.
 *
 * The callback-only tick is the completion barrier: the host sends no tick
 * while a drawing tick's publication is outstanding (TickMachine: a tick
 * is sent only from IDLE, and a drawing tick leaves IDLE until its
 * publication is taken or dropped). Once it has run, nothing is drawing,
 * so the publication presenting is the last there will be until the test
 * draws again, and a capture request that cannot match (generation 0 is
 * never issued) reports it when it gives up.
 */
async function resync(h) {
  await tick(h, [() => false]);
  const result = await h.ui.__captureFrame(
      {publicationId: 0, generation: 0, giveUpAfterFrames: 2, path: ''});
  h.published = result.publicationId;
}

/**
 * Run frames of `drawFn` until `predicate(imgui.__test.state())` holds,
 * at most UNTIL_FRAME_BOUND of them. On the bound, prints
 * `TIMEOUT <label>` and exits 1.
 */
async function until(h, drawFn, predicate, label) {
  for (let i = 0; i < UNTIL_FRAME_BOUND; ++i) {
    await frame(h, drawFn);
    if (predicate(h.imgui.__test.state()))
      return;
  }
  console.log('TIMEOUT ' + label);
  process.exit(1);
}

/**
 * Inject one platform event (`ui.__injectEvent`, Sokol's terms). `mouseX`
 * and `mouseY` are given in logical units here and scaled by the window's
 * dpiScale, since Sokol's are framebuffer pixels. Returns at once; the
 * event reaches the JS thread, and so ImGui, frames later.
 */
function inject(h, desc) {
  const scaled = Object.assign({}, desc);
  for (const k of ['mouseX', 'mouseY']) {
    if (k in scaled)
      scaled[k] = scaled[k] * h.win.dpiScale;
  }
  h.ui.__injectEvent(scaled);
}

/** The agreement fields of a capture of the next publication. */
function captureRequest(h, outPath) {
  return {
    path: outPath,
    publicationId: h.published + 1,
    generation: h.generation,
    targetWidth: h.win.framebufferWidth,
    targetHeight: h.win.framebufferHeight,
    framebufferWidth: h.win.framebufferWidth,
    framebufferHeight: h.win.framebufferHeight,
    dpiScale: h.win.dpiScale,
    giveUpAfterFrames: CAPTURE_FRAME_BUDGET,
  };
}

/**
 * Capture the presented picture of the tick `run` draws. The request is
 * installed before anything draws, as test/ui-present.js explains.
 */
async function captureWith(h, outPath, run) {
  const pending = h.ui.__captureFrame(captureRequest(h, outPath));
  await run();
  const result = await pending;
  if (!result.matched) {
    console.log('CAPTURE unmatched ' + path.basename(outPath));
    process.exit(1);
  }
  return readPpm(outPath);
}

/** Capture one `frame(h, drawFn)`; returns the parsed image. */
function capture(h, drawFn, outPath) {
  return captureWith(h, outPath, () => frame(h, drawFn));
}

/** Capture one `tick(h, fns)`; returns the parsed image. */
function captureTick(h, fns, outPath) {
  return captureWith(h, outPath, () => tick(h, fns));
}

/**
 * Parse the binary PPM (P6) the capture hook writes: `{width, height,
 * data}`, rows top first, RGB. The header shape is fixed by the host's
 * writeCapturePpm(), so a hand-rolled parse is honest here.
 */
function readPpm(ppmPath) {
  const buf = fs.readFileSync(ppmPath);
  let offset = 0;
  function token() {
    while (offset < buf.length && /\s/.test(String.fromCharCode(buf[offset])))
      offset++;
    const start = offset;
    while (offset < buf.length &&
           !/\s/.test(String.fromCharCode(buf[offset])))
      offset++;
    return buf.toString('ascii', start, offset);
  }
  const magic = token();
  const width = parseInt(token(), 10);
  const height = parseInt(token(), 10);
  const maxValue = parseInt(token(), 10);
  offset++; // the single whitespace byte after the header
  if (magic !== 'P6' || maxValue !== 255)
    throw new Error('not a P6/255 PPM: ' + magic + ' ' + maxValue);
  return {width, height, data: buf.slice(offset)};
}

/** `[r, g, b]` at (x, y), y counted from the top. */
function pixelAt(image, x, y) {
  const i = (y * image.width + x) * 3;
  return [image.data[i], image.data[i + 1], image.data[i + 2]];
}

module.exports = {
  CLEAR,
  open,
  tick,
  frame,
  resync,
  until,
  inject,
  capture,
  captureTick,
  readPpm,
  pixelAt,
};
