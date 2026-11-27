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
  const h = {ui, win, imgui, published: 0, generation: 1};
  h.run = (drawFn) => frame(h, drawFn);
  return h;
}

/**
 * A handle for an application's own window, `{ui, win, imgui, frame}`
 * from `app`, where `app.frame` is already a whole frame (its own
 * newFrame() and render()) returning what render() returned. Every helper
 * then runs `app.frame` as its frame and ignores its `drawFn` argument
 * (pass `null`).
 *
 * Call it before the application has drawn a frame (it was created with
 * `driven: true` and nothing has run `app.frame` yet): `h.published`
 * starts at 0, as `open()`'s does, and is not read from the host. It
 * cannot be: `resync()` learns the count from a capture request that gives
 * up, and the host counts a request's frames only as it presents them, so
 * before the first publication there is nothing to count and the request
 * never settles.
 */
async function adopt(app) {
  const h = {
    ui: app.ui,
    win: app.win,
    imgui: app.imgui,
    published: 0,
    generation: 1,
  };
  h.run = () => tick(h, [app.frame]).then((r) => r[0]);
  return h;
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
 *
 * Only once something has been published: the request's frames are
 * counted as they are presented, and before the first publication none
 * is, so it would never give up (see `adopt`).
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
    await h.run(drawFn);
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

/** Capture one `h.run(drawFn)`; returns the parsed image. */
function capture(h, drawFn, outPath) {
  return captureWith(h, outPath, () => h.run(drawFn));
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

/**
 * One frame callback running `fn` in its own tick, without the
 * bookkeeping `tick()` does for `h.published`: for a frame whose render()
 * throws. Resolves with what `fn` returned, or rejects with what it threw,
 * from a setImmediate after the tick, as `tick()` does. Follow it with
 * `resync(h)` before the next ordinary frame.
 */
function rawTick(h, fn) {
  return new Promise((resolve, reject) => {
    h.ui.requestAnimationFrame(() => {
      let result;
      try {
        result = fn();
      } catch (e) {
        setImmediate(() => reject(e));
        return;
      }
      setImmediate(() => resolve(result));
    });
  });
}

// --- Helpers of the binding's tests (part B) ------------------------------

/**
 * `{min, max}` of the last item, read with getItemRectMin/Max; for use
 * inside a draw function, after the item.
 */
function rect(imgui) {
  return {min: imgui.getItemRectMin(), max: imgui.getItemRectMax()};
}

/** The centre of a `rect()`, in whole units (ImGui floors the mouse). */
function center(r) {
  return [
    Math.floor((r.min[0] + r.max[0]) / 2),
    Math.floor((r.min[1] + r.max[1]) / 2),
  ];
}

/**
 * Whether ImGui's mouse position is `[x, y]`. It goes through float and a
 * dpiScale division, so it is compared to within 0.01.
 */
function mouseAt(s, x, y) {
  return Math.abs(s.mouseX - x) < 0.01 && Math.abs(s.mouseY - y) < 0.01;
}

/**
 * Inject a mouse move to `[x, y]` and run frames of `drawFn` until ImGui
 * has that position. An injected event arrives frames later (see
 * `inject`), so this waits on the read-back, never on a frame count.
 */
async function moveTo(h, drawFn, [x, y]) {
  inject(h, {type: 'MOUSE_MOVE', mouseX: x, mouseY: y});
  await until(h, drawFn, (s) => mouseAt(s, x, y), 'moveTo ' + x + ',' + y);
}

/**
 * Inject a press of `button` (ImGui's numbering, 0..2, which is also
 * Sokol's) at `[x, y]` and wait until ImGui has it down.
 */
async function buttonDown(h, drawFn, [x, y], button) {
  inject(h, {type: 'MOUSE_DOWN', button, mouseX: x, mouseY: y});
  await until(h, drawFn, (s) => s.mouseDown[button], 'down ' + button);
}

/** The release matching `buttonDown`. */
async function buttonUp(h, drawFn, [x, y], button) {
  inject(h, {type: 'MOUSE_UP', button, mouseX: x, mouseY: y});
  await until(h, drawFn, (s) => !s.mouseDown[button], 'up ' + button);
}

/**
 * A click at `[x, y]`: move there, press, release, each read back before
 * the next is sent, then one more frame. A widget's return value is true
 * for one frame somewhere inside these waits, so `drawFn` should
 * accumulate it (`clicked = imgui.button('b') || clicked`).
 */
async function clickAt(h, drawFn, at, button = 0) {
  await moveTo(h, drawFn, at);
  await buttonDown(h, drawFn, at, button);
  await buttonUp(h, drawFn, at, button);
  await h.run(drawFn);
}

/**
 * A left-button drag from `from` to `to`, through their midpoint, each
 * step read back; then one more frame.
 */
async function dragFromTo(h, drawFn, from, to) {
  const mid = [
    Math.floor((from[0] + to[0]) / 2),
    Math.floor((from[1] + to[1]) / 2),
  ];
  await moveTo(h, drawFn, from);
  await buttonDown(h, drawFn, from, 0);
  await moveTo(h, drawFn, mid);
  await moveTo(h, drawFn, to);
  await buttonUp(h, drawFn, to, 0);
  await h.run(drawFn);
}

/**
 * Inject `text` as CHAR events, one per code point, then run frames until
 * `done(state)`, the test's own predicate on what typing must have
 * changed.
 */
async function typeText(h, drawFn, text, done) {
  for (const ch of text)
    inject(h, {type: 'CHAR', charCode: ch.codePointAt(0)});
  await until(h, drawFn, done, 'typeText ' + JSON.stringify(text));
}

/**
 * Inject a key down and up (Sokol's key code and modifier bits), then run
 * frames until `done(state)`.
 */
async function pressKey(h, drawFn, sappKeyCode, modifiers, done) {
  inject(h, {type: 'KEY_DOWN', keyCode: sappKeyCode, modifiers});
  inject(h, {type: 'KEY_UP', keyCode: sappKeyCode, modifiers});
  await until(h, drawFn, done, 'pressKey ' + sappKeyCode);
}

/** The class an error code belongs to (Node's split, and the design's). */
function errorClassOf(code) {
  if (code === 'ERR_INVALID_ARG_TYPE' || code === 'ERR_INVALID_ARG_VALUE')
    return TypeError;
  if (code === 'ERR_OUT_OF_RANGE')
    return RangeError;
  return Error;
}

/**
 * Call `fn`, which must throw an error with `code` whose class is the one
 * that code belongs to (exactly: a TypeError does not pass for Error).
 * Returns the error. Otherwise prints `NOTHROW <code>`, `WRONGCODE <got>`
 * or `WRONGCLASS <got>` and exits 1.
 */
function expectThrow(fn, code) {
  let error;
  try {
    fn();
  } catch (e) {
    error = e;
  }
  if (error === undefined) {
    console.log('NOTHROW ' + code);
    process.exit(1);
  }
  if (error === null || typeof error !== 'object' || error.code !== code) {
    console.log(
        'WRONGCODE ' + (error && error.code) + ' (want ' + code + '): ' +
        (error && error.message));
    process.exit(1);
  }
  const cls = errorClassOf(code);
  if (Object.getPrototypeOf(error) !== cls.prototype) {
    console.log(
        'WRONGCLASS ' + (error.constructor && error.constructor.name) +
        ' (want ' + cls.name + ' for ' + code + ')');
    process.exit(1);
  }
  return error;
}

/** How many times binding function `name` has called ImGui. */
function calls(h, name) {
  return h.imgui.__test.bindCalls()[name] || 0;
}

/**
 * Prints `COVERAGE <group> ok` when every function of `group` in
 * bind-groups.js has reached ImGui at least once; otherwise
 * `UNCOVERED <group> <names...>` and exits 1. A name that is not a
 * function on the module (`version`) has no call to count and is skipped.
 */
function checkGroupCoverage(h, group) {
  const bindGroups = require('./bind-groups.js');
  const names = bindGroups.groups[group];
  if (names === undefined) {
    console.log('UNCOVERED ' + group + ' (no such group)');
    process.exit(1);
  }
  const counts = h.imgui.__test.bindCalls();
  const uncovered = names.filter((name) => {
    const target = name.startsWith('drawList.') ?
        h.imgui.drawList[name.slice('drawList.'.length)] :
        h.imgui[name];
    return typeof target === 'function' && !(counts[name] > 0);
  });
  if (uncovered.length !== 0) {
    console.log('UNCOVERED ' + group + ' ' + uncovered.join(' '));
    process.exit(1);
  }
  console.log('COVERAGE ' + group + ' ok');
}

/**
 * Prints `GUARDS ok` when every registered guard condition has refused at
 * least once; otherwise `UNREFUSED <names...>` and exits 1.
 */
function checkGuardCoverage(h) {
  const guards = h.imgui.__test.guards();
  const unrefused =
      Object.keys(guards).filter((name) => !(guards[name] > 0)).sort();
  if (unrefused.length !== 0) {
    console.log('UNREFUSED ' + unrefused.join(' '));
    process.exit(1);
  }
  console.log('GUARDS ok');
}

module.exports = {
  CLEAR,
  open,
  adopt,
  tick,
  frame,
  rawTick,
  resync,
  until,
  inject,
  capture,
  captureTick,
  readPpm,
  pixelAt,
  rect,
  center,
  moveTo,
  clickAt,
  dragFromTo,
  typeText,
  pressKey,
  expectThrow,
  calls,
  checkGroupCoverage,
  checkGuardCoverage,
};
