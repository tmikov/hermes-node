// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The application, examples/imgui-app/app.js, in its window, driven by the
// test: createApp({root, clock: 'fixed', driven: true}) registers no frame
// callback of its own, and every frame here is one app.frame() run through
// harness.adopt(app)'s h.run. Clicks go to the rectangles the application
// records in test mode (app.rects: exactly what the last frame drew, by
// path for a Files row, by label for a menu item, `tab:<name>` for a
// Widgets tab, `more:<path>` for a directory's "... N more" row);
// asynchronous file work is waited for with app.idle() while frames keep
// running; nothing is asserted from elapsed time.
//
// The root is a copy of test/fixtures/imgui/app-tree at %t.tree, to which
// the test adds `gone.txt` (removed after it was listed), `locked/` (mode
// 000; as root, which can read it anyway, the case prints `SKIP locked`),
// `big/` (5001 files, one over the Files panel's 5000-entry cap),
// `small/` (50 files) and `odd/` (file names ImGui would not show as they
// are, and `plain.txt`).
//
// Two runs, selected by the first argument.
//
// `main`, cases in order, one line each (`<CASE> ok`; any failure prints
// its own line and exits 1):
//
//  SCALE <n>  app.scale, unchecked: 1 under Xvfb, 2 on a Retina Mac.
//  PANELS     after 10 frames state().windows holds Files, Viewer,
//             Widgets, Monitor, Canvas and Log, each active and docked.
//             (Then, frames until app.idle(): the root's listing, `APP dir
//             <tree> entries=10`.)
//  OPEN       a click on a.txt's row: `APP opened <tree>/a.txt 12
//             lines=3`, and the viewer's state is text with 3 lines.
//  DIR        a click on dir1's row: `APP dir <tree>/dir1 entries=2`, and
//             a rectangle for dir1/b.js appears in app.rects.
//  EDGES      empty.txt (`APP opened ... 0 lines=0`), binary.bin (hex, 16
//             rows), gone.txt removed after it was listed (`APP error
//             <path> ENOENT`), locked/ (`APP error <path> EACCES`): each
//             prints its line and the next frame draws. (The size and
//             encoding edges are test/imgui-app-viewer.js's.)
//  NAMES      odd/ opened: `hash##tag.txt` and `line<LF>break.txt` (each
//             `SKIP name <json>` instead if the file system refused it)
//             are drawn as one-line rows, their rectangles the height of
//             plain.txt's and the three rows evenly spaced; the "##" name
//             is shown whole (its text's width is calcTextSize() of the
//             whole name); a click on each opens it in the viewer.
//  COLLAPSE   a click on dir1's row again: in the next frame app.rects
//             has no rectangle under dir1/ (it holds what was drawn).
//  BIG        big/ opened (`APP dir ... entries=5001`), then collapsed and
//             small/ opened (`... entries=50`): the binding calls of one
//             frame (__test.bindCalls()) with each open differ by at most
//             BIG_CALL_SLACK and stay under BIG_CALL_LIMIT, so the cost
//             does not grow with the entries (the list clipper draws what
//             is visible). Then big/ again, a wheel over the panel to the
//             end of the list: the last entry shown (f4999) and the
//             `... 1 more` row are drawn, and a click on f4999 opens it.
//             Back to the top, big/ collapsed: nothing under big/ in
//             app.rects.
//  CANVAS     at setTime(0) the canvas's centre pixel is the centre
//             circle's configured colour; at setTime(1) the pixel at
//             canvas.orbitPosition(1) is the orbiting circle's colour and
//             the one at orbitPosition(0) is not. Logical positions are
//             multiplied by app.scale: a capture is in framebuffer pixels.
//  TINY       the window resized through the window driver to 1x1 (the
//             size granted is printed, unchecked) and back to 1280x800,
//             with part A's resize protocol: no `APP error` between, no
//             usage error (the trace's exclusion), PANELS holds after.
//  RESET      FLOATING (ImGui's own saved settings with every panel
//             floating, see below) loaded between frames: no panel is
//             docked; Canvas closed through its open flag: not active;
//             app.resetLayout() and two frames: all six active and docked
//             again, Canvas included (a reset reopens closed panels).
//  QUIT       app.quit(): the process ends with status 0 (CODE).
//
// `throw` (Review Focus 2): the Widgets panel's Table tab clicked (its
// rectangle in app.rects), until its child window is active in
// state().windows; then with an uncaughtException listener installed
// `panels.widgets.throwNextFrame = true` (honoured only in test mode): the
// panel throws from inside a listClipper callback, inside a table, inside
// a child window, inside its tab and window. That frame is a frame
// callback of its own, registered with ui.requestAnimationFrame (not
// harness.rawTick, which would catch the throw itself and the listener
// would never see it); the listener sees the error; after harness.resync,
// 10 frames later PANELS holds. Its stderr: the discarded frame's usage
// errors and `frame discarded`, and after the marker the test writes once
// the next frame has drawn, no usage error at all.
//
// How FLOATING was produced: a scratch run of the application loaded,
// between two frames, a window-only settings section for Log without a
// DockId (the shape test/ui-imgui-bind-docking.js's UNDOCK loads), which
// clears ImGui's dock data and so left all six panels floating; after five
// more frames imgui.saveIniSettingsToMemory() returned this text, less the
// fallback window's section and the LastUsed= lines. So what the test
// loads is ImGui's own output, as the application's DEFAULT is.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  the Files list clipper removed (every row drawn): BIG fails on the
//  call count;
//  app.rects not emptied per frame: COLLAPSE fails;
//  resetLayout() not reopening panels: RESET fails;
//  the Files panel's label sanitising removed (the name passed to
//  treeNodeEx as it is): NAMES fails;
//  the Reset Layout load done inside the frame: RESET (the load throws
//  ERR_IMGUI_USAGE);
//  the canvas drawing's orbit radius changed: CANVAS;
//  the callback scope left raised after the callback threw: `throw` (the
//  next newFrame() throws ERR_IMGUI_USAGE, uncaught, and no `THROW ok`
//  is printed).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: chmod -R u+rwx %t.tree 2>/dev/null; rm -rf %t.tree && cp -R %S/fixtures/imgui/app-tree %t.tree
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s main %t.tree %t %x11_window_driver > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck -DTREE=%t.tree %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err
//
// RUN: chmod -R u+rwx %t.tree 2>/dev/null; rm -rf %t.tree && cp -R %S/fixtures/imgui/app-tree %t.tree
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s throw %t.tree %t %x11_window_driver > %t.throw.out 2> %t.throw.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=THROW %s < %t.throw.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=THROW-TRACE %s < %t.throw.err
// RUN: awk 'f { print } /APPTEST after the discarded frame/ { f = 1 }' %t.throw.err | %FileCheck --implicit-check-not='imgui: usage error' --check-prefix=THROW-AFTER %s
//
// CODE: EXITCODE 0
//
// CHECK: SCALE {{[0-9.]+$}}
// CHECK: APP dir [[TREE]] entries=10
// CHECK: PANELS ok
// CHECK: APP opened [[TREE]]/a.txt 12 lines=3
// CHECK: OPEN ok
// CHECK: APP dir [[TREE]]/dir1 entries=2
// CHECK: DIR ok
// CHECK: APP opened [[TREE]]/empty.txt 0 lines=0
// CHECK: EDGES empty.txt ok
// CHECK: APP opened [[TREE]]/binary.bin 256 hex=16
// CHECK: EDGES binary.bin ok
// CHECK: APP error [[TREE]]/gone.txt ENOENT
// CHECK: EDGES gone.txt ok
// CHECK: {{^(APP error .*/locked EACCES|SKIP locked)$}}
// CHECK: EDGES locked ok
// CHECK: NAMES ok
// CHECK: COLLAPSE ok
// CHECK: APP dir [[TREE]]/big entries=5001
// CHECK: APP dir [[TREE]]/small entries=50
// CHECK: BIG calls per frame: big {{[0-9]+}}, small {{[0-9]+$}}
// CHECK: APP opened [[TREE]]/big/f4999 2 lines=1
// CHECK: BIG ok
// CHECK: CANVAS ok
// CHECK: TINY start
// CHECK-NOT: APP error
// CHECK: TINY size {{[0-9]+x[0-9]+$}}
// CHECK-NOT: APP error
// CHECK: TINY ok
// CHECK: RESET floating
// CHECK: RESET closed
// CHECK: RESET ok
// CHECK: QUIT calling
// CHECK: QUIT exit
//
// TRACE: imgui: context created
//
// THROW: PANELS ok
// THROW: THROW tab Table
// THROW: THROW caught thrown from the list clipper's callback
// THROW: THROW ok
//
// The discarded frame's own errors, exactly as measured: what the throw
// left open, innermost first, reported by EndFrame()'s recovery in the
// next newFrame().
// THROW-TRACE: imgui: usage error: Missing EndTable()
// THROW-TRACE-NEXT: imgui: usage error: Missing EndChild()
// THROW-TRACE-NEXT: imgui: usage error: Missing EndChild()
// THROW-TRACE-NEXT: imgui: usage error: Missing EndTabBar()
// THROW-TRACE-NEXT: imgui: usage error: Missing PopID()
// THROW-TRACE-NEXT: imgui: frame discarded
// THROW-TRACE: APPTEST after the discarded frame
// THROW-TRACE: APPTEST end
//
// THROW-AFTER: APPTEST end

'use strict';

const fs = require('fs');
const path = require('path');
const harness = require('./fixtures/imgui/harness.js');
const {startDriver} = require('./fixtures/hnui-demo/window_driver.js');
const {createApp} = require('../examples/imgui-app/app.js');

const mode = process.argv[2];
const tree = process.argv[3];
const capturePrefix = process.argv[4];
const driverPath = process.argv[5];

const PANEL_NAMES = ['Files', 'Viewer', 'Widgets', 'Monitor', 'Canvas', 'Log'];
const TITLE = 'hermes-node ImGui';

const BIG_FILES = 5001;
const SMALL_FILES = 50;
/** How far apart one frame's binding calls may be, big/ against small/. */
const BIG_CALL_SLACK = 60;
/** A bound on one frame's binding calls with big/ open. */
const BIG_CALL_LIMIT = 1500;
/** Sokol's MOUSE_SCROLL: far more than any list here, either way. */
const WHEEL_TO_END = -100000;
/** File names ImGui would not display as they are (NAMES). */
const ODD_NAMES = ['hash##tag.txt', 'line\nbreak.txt'];

// ImGui's own saved settings with every panel floating (header).
const FLOATING = [
  '[Window][Files]',
  'Pos=0,19',
  'Size=380,390',
  'Collapsed=0',
  '',
  '[Window][Viewer]',
  'Pos=0,19',
  'Size=380,702',
  'Collapsed=0',
  '',
  '[Window][Widgets]',
  'Pos=382,19',
  'Size=518,520',
  'Collapsed=0',
  '',
  '[Window][Log]',
  'Pos=200,200',
  'Size=300,200',
  'Collapsed=0',
  '',
  '[Window][Monitor]',
  'Pos=902,19',
  'Size=378,330',
  'Collapsed=0',
  '',
  '[Window][Canvas]',
  'Pos=902,19',
  'Size=378,702',
  'Collapsed=0',
  '',
  '[Window][WindowOverViewport_11111111]',
  'Pos=0,19',
  'Size=1280,781',
  'Collapsed=0',
  '',
  '[Docking][Data]',
  'DockSpace ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1280,781 CentralNode=1',
  '',
].join('\n');

function fail(line) {
  console.log(line);
  process.exit(1);
}

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

/** ImGui's float-to-byte conversion (IM_F32_TO_INT8_SAT). */
function toByte(v) {
  return Math.trunc(Math.min(Math.max(v, 0), 1) * 255 + 0.5);
}

function fillDirectory(dir, count, prefix, digits) {
  fs.mkdirSync(dir);
  for (let i = 0; i < count; ++i) {
    fs.writeFileSync(
        path.join(dir, prefix + String(i).padStart(digits, '0')), 'x\n');
  }
}

async function main() {
  const locked = path.join(tree, 'locked');
  const gone = path.join(tree, 'gone.txt');
  fs.mkdirSync(locked);
  fs.writeFileSync(path.join(locked, 'hidden.txt'), 'hidden\n');
  fs.chmodSync(locked, 0o000);
  // Restored on every exit path, so the next run's `rm -rf` can remove it.
  process.on('exit', () => {
    try {
      fs.chmodSync(locked, 0o755);
    } catch (e) {
      void e;
    }
  });
  fs.writeFileSync(gone, 'removed after it was listed\n');
  const big = path.join(tree, 'big');
  const small = path.join(tree, 'small');
  fillDirectory(big, BIG_FILES, 'f', 4);
  fillDirectory(small, SMALL_FILES, 's', 2);
  const odd = path.join(tree, 'odd');
  fs.mkdirSync(odd);
  fs.writeFileSync(path.join(odd, 'plain.txt'), 'plain\n');
  const oddNames = [];
  for (const name of ODD_NAMES) {
    try {
      fs.writeFileSync(path.join(odd, name), 'odd\n');
      oddNames.push(name);
    } catch (e) {
      console.log('SKIP name ' + JSON.stringify(name) + ' ' + e.code);
    }
  }
  const isRoot = typeof process.getuid === 'function' && process.getuid() === 0;

  const app = await createApp({root: tree, clock: 'fixed', driven: true});
  const h = await harness.adopt(app);
  const {imgui, win} = app;
  const state = () => imgui.__test.state();

  /** Frames until app.idle() has resolved, then one that draws it. */
  async function settle(label) {
    let done = false;
    app.idle().then(() => {
      done = true;
    });
    for (let i = 0; i < 600 && !done; ++i)
      await h.run();
    if (!done)
      fail('TIMEOUT idle ' + label);
    await h.run();
  }

  /** Frames until the last frame drew `key` (app.rects). */
  async function waitRect(key) {
    for (let i = 0; i < 600 && !app.rects.has(key); ++i)
      await h.run();
    if (!app.rects.has(key))
      fail('TIMEOUT rect ' + key);
    return app.rects.get(key);
  }

  async function clickRow(key) {
    const r = await waitRect(key);
    await harness.clickAt(h, null, harness.center(r));
  }

  /** The keys of app.rects under directory `dir`. */
  function rectsUnder(dir) {
    return [...app.rects.keys()].filter((k) => k.startsWith(dir + '/'));
  }

  /** The total of __test.bindCalls() across one frame. */
  async function callsInOneFrame() {
    const total = () => Object.values(imgui.__test.bindCalls())
                            .reduce((a, b) => a + b, 0);
    const before = total();
    await h.run();
    return total() - before;
  }

  /** `null` if every panel is active and docked, else what is wrong. */
  function panelsProblem() {
    const windows = state().windows;
    const wrong = [];
    for (const name of PANEL_NAMES) {
      const w = windows.find((x) => x.name === name);
      if (w === undefined || !w.active || !w.docked)
        wrong.push(name + '=' + JSON.stringify(w));
    }
    return wrong.length === 0 ? null : wrong.join(' ');
  }

  async function checkPanels(label) {
    for (let i = 0; i < 10; ++i)
      await h.run();
    const problem = panelsProblem();
    if (problem !== null)
      fail(label + ' WRONG ' + problem);
  }

  console.log('SCALE ' + app.scale);

  // --- PANELS -----------------------------------------------------------
  await checkPanels('PANELS');
  await settle('root');
  console.log('PANELS ok');

  if (mode === 'throw')
    return runThrow(app, h, checkPanels, waitRect);

  // --- OPEN -------------------------------------------------------------
  {
    const file = path.join(tree, 'a.txt');
    await clickRow(file);
    await settle('OPEN');
    const s = app.panels.viewer.state;
    if (s.status !== 'text' || s.path !== file || s.lines.length !== 3)
      fail('OPEN WRONG ' + JSON.stringify(
          {status: s.status, path: s.path, lines: s.lines}));
    console.log('OPEN ok');
  }

  // --- DIR --------------------------------------------------------------
  const dir1 = path.join(tree, 'dir1');
  {
    await clickRow(dir1);
    await settle('DIR');
    await waitRect(path.join(dir1, 'b.js'));
    console.log('DIR ok');
  }

  // --- EDGES ------------------------------------------------------------
  {
    const viewer = app.panels.viewer;
    const edge = async (name, check) => {
      const p = path.join(tree, name);
      await clickRow(p);
      await settle('EDGES ' + name);
      const problem = check(p);
      if (problem)
        fail('EDGES ' + name + ' WRONG ' + problem);
      if (await h.run() !== true)
        fail('EDGES ' + name + ' WRONG the next frame did not draw');
      console.log('EDGES ' + name + ' ok');
    };
    const viewerIs = (p, status) => () => viewer.state.path === p &&
            viewer.state.status === status ?
        null :
        JSON.stringify({path: viewer.state.path, status: viewer.state.status});
    await edge('empty.txt', (p) => viewerIs(p, 'text')());
    await edge('binary.bin', (p) => viewerIs(p, 'hex')());
    fs.unlinkSync(gone);
    await edge('gone.txt', (p) => viewer.state.code === 'ENOENT' ?
        viewerIs(p, 'error')() :
        'code ' + viewer.state.code);
    if (isRoot)
      console.log('SKIP locked');
    await edge('locked', () => null);
  }

  // --- NAMES ------------------------------------------------------------
  {
    await clickRow(odd);
    await settle('NAMES');
    const plainPath = path.join(odd, 'plain.txt');
    const plain = await waitRect(plainPath);
    const rowHeight = plain.max[1] - plain.min[1];
    const rects = [];
    for (const name of oddNames.slice().sort()) {
      const r = await waitRect(path.join(odd, name));
      rects.push({name, min: r.min.slice(), max: r.max.slice()});
    }
    rects.push(
        {name: 'plain.txt', min: plain.min.slice(), max: plain.max.slice()});
    for (const r of rects) {
      if (r.max[1] - r.min[1] !== rowHeight)
        fail('NAMES WRONG row height ' + JSON.stringify(r) + ' want ' +
            rowHeight);
    }
    for (let i = 2; i < rects.length; ++i) {
      if (rects[i].min[1] - rects[i - 1].min[1] !==
          rects[1].min[1] - rects[0].min[1])
        fail('NAMES WRONG uneven rows ' + JSON.stringify(rects));
    }
    for (const name of oddNames.filter((n) => n.includes('##'))) {
      const key = 'label:' + path.join(odd, name);
      if (!app.rects.has(key))
        fail('NAMES WRONG no whole-name text drawn for ' +
            JSON.stringify(name));
      const r = app.rects.get(key);
      let want = 0;
      await harness.tick(h, [() => {
        imgui.newFrame();
        want = imgui.calcTextSize(name)[0];
        return imgui.render();
      }]);
      if (r.max[0] - r.min[0] !== want)
        fail('NAMES WRONG ' + JSON.stringify(name) + ' shown ' +
            (r.max[0] - r.min[0]) + ' wide, want ' + want);
    }
    for (const name of oddNames) {
      const p = path.join(odd, name);
      await clickRow(p);
      await settle('NAMES open');
      const v = app.panels.viewer.state;
      if (v.path !== p || v.status !== 'text')
        fail('NAMES WRONG open ' + JSON.stringify(
            {name, path: v.path, status: v.status}));
    }
    await clickRow(odd);
    await h.run();
    console.log('NAMES ok');
  }

  // --- COLLAPSE ---------------------------------------------------------
  {
    if (rectsUnder(dir1).length === 0)
      fail('COLLAPSE setup: nothing under dir1/ drawn');
    await clickRow(dir1);
    await h.run();
    const left = rectsUnder(dir1);
    if (left.length !== 0)
      fail('COLLAPSE WRONG still recorded: ' + left.join(' '));
    console.log('COLLAPSE ok');
  }

  // --- BIG --------------------------------------------------------------
  {
    await clickRow(big);
    await settle('BIG open');
    await waitRect(path.join(big, 'f0000'));
    const bigCalls = await callsInOneFrame();
    await clickRow(big);
    await clickRow(small);
    await settle('BIG small');
    await waitRect(path.join(small, 's00'));
    const smallCalls = await callsInOneFrame();
    if (Math.abs(bigCalls - smallCalls) > BIG_CALL_SLACK ||
        bigCalls > BIG_CALL_LIMIT)
      fail('BIG WRONG calls per frame: big ' + bigCalls + ', small ' +
          smallCalls);
    console.log('BIG calls per frame: big ' + bigCalls + ', small ' +
        smallCalls);
    await clickRow(small);
    await clickRow(big);

    // To the end of the list: the end of big/ is followed only by the
    // root's other directories and files, which all fit in the panel.
    const last = path.join(big, 'f4999');
    const at = harness.center(await waitRect(big));
    await harness.moveTo(h, null, at);
    harness.inject(h, {type: 'MOUSE_SCROLL', scrollY: WHEEL_TO_END,
                       mouseX: at[0], mouseY: at[1]});
    await waitRect(last);
    if (!app.rects.has('more:' + big))
      fail('BIG WRONG no "... 1 more" row drawn at the end');
    if (app.rects.has(path.join(big, 'f5000')))
      fail('BIG WRONG the entry past the cap was drawn');
    await clickRow(last);
    await settle('BIG last');
    if (app.panels.viewer.state.path !== last)
      fail('BIG WRONG viewer on ' + app.panels.viewer.state.path);

    const top = harness.center(await waitRect(last));
    harness.inject(h, {type: 'MOUSE_SCROLL', scrollY: -WHEEL_TO_END,
                       mouseX: top[0], mouseY: top[1]});
    await clickRow(big);
    await h.run();
    if (rectsUnder(big).length !== 0)
      fail('BIG WRONG rows under big/ after collapsing it');
    console.log('BIG ok');
  }

  // --- CANVAS -----------------------------------------------------------
  {
    const canvas = app.panels.canvas;
    const scale = app.scale;
    const out = (name) => capturePrefix + '.' + name + '.ppm';
    const at = (image, p) => harness.pixelAt(
        image, Math.floor(p[0] * scale), Math.floor(p[1] * scale));
    const wrong = [];

    app.setTime(0);
    await h.run();
    let image = await harness.capture(h, null, out('canvas0'));
    const centerWant = canvas.centerColor.slice(0, 3).map(toByte);
    const centerGot = at(image, canvas.center);
    if (!sameColor(centerGot, centerWant))
      wrong.push('centre=' + centerGot + ' want ' + centerWant);

    app.setTime(1);
    image = await harness.capture(h, null, out('canvas1'));
    const orbitWant = canvas.orbitColor.slice(0, 3).map(toByte);
    const now = at(image, canvas.orbitPosition(1));
    const before = at(image, canvas.orbitPosition(0));
    if (!sameColor(now, orbitWant))
      wrong.push('orbit(1)=' + now + ' want ' + orbitWant);
    if (sameColor(before, orbitWant))
      wrong.push('orbit(0)=' + before + ' still the orbit colour');
    if (wrong.length !== 0)
      fail('CANVAS WRONG ' + wrong.join(' '));
    console.log('CANVAS ok');
  }

  // --- TINY -------------------------------------------------------------
  {
    const driver = await startDriver({ui: h.ui, driverPath, title: TITLE});
    const resizeTo = async (w, ht) => {
      const resized = new Promise((resolve) => win.once('resize', resolve));
      const answer = await driver.send('resize ' + w + ' ' + ht);
      if (!answer.startsWith('SENT'))
        fail('TINY driver: ' + answer);
      await resized;
      await new Promise((resolve) => setImmediate(resolve));
    };
    console.log('TINY start');
    await resizeTo(1, 1);
    await harness.until(h, null,
        (s) => s.displayWidth === win.width && s.displayHeight === win.height,
        'TINY small');
    console.log('TINY size ' + win.width + 'x' + win.height);
    for (let i = 0; i < 5; ++i)
      await h.run();
    await resizeTo(1280, 800);
    await harness.until(h, null,
        (s) => s.displayWidth === 1280 && s.displayHeight === 800,
        'TINY back');
    h.generation = state().targetGeneration;
    await harness.resync(h);
    await checkPanels('TINY');
    driver.stop();
    console.log('TINY ok');
  }

  // --- RESET ------------------------------------------------------------
  {
    imgui.loadIniSettingsFromMemory(FLOATING);
    await h.run();
    const docked = state().windows.filter(
        (w) => PANEL_NAMES.includes(w.name) && w.docked);
    if (docked.length !== 0)
      fail('RESET WRONG still docked: ' + JSON.stringify(docked));
    console.log('RESET floating');
    app.panels.canvas.open[0] = false;
    await h.run();
    const canvasWindow = state().windows.find((w) => w.name === 'Canvas');
    if (canvasWindow !== undefined && canvasWindow.active)
      fail('RESET WRONG Canvas still active: ' + JSON.stringify(canvasWindow));
    console.log('RESET closed');
    app.resetLayout();
    await h.run();
    await h.run();
    const problem = panelsProblem();
    if (problem !== null)
      fail('RESET WRONG ' + problem);
    console.log('RESET ok');
  }

  // --- QUIT -------------------------------------------------------------
  process.on('exit', (code) => console.log('QUIT exit ' + code));
  console.log('QUIT calling');
  app.quit();
}

async function runThrow(app, h, checkPanels, waitRect) {
  const {imgui} = app;
  // The Table tab's child window, active in the last frame: the read-back
  // that the tab's contents are drawn.
  const tableShown = () => imgui.__test.state().windows.some(
      (w) => w.name.startsWith('Widgets/system') && w.active);
  const tab = await waitRect('tab:Table');
  await harness.clickAt(h, null, harness.center(tab));
  for (let i = 0; i < 600 && !tableShown(); ++i)
    await h.run();
  if (!tableShown())
    fail('TIMEOUT THROW tab');
  console.log('THROW tab Table');

  const caught = new Promise((resolve) => {
    process.once('uncaughtException', (e) => {
      console.log('THROW caught ' + e.message);
      setImmediate(resolve);
    });
  });
  app.panels.widgets.throwNextFrame = true;
  h.ui.requestAnimationFrame(app.frame);
  await caught;
  await harness.resync(h);
  if (await h.run() !== true)
    fail('THROW WRONG the frame after the throw did not draw');
  console.error('APPTEST after the discarded frame');
  await checkPanels('THROW');
  console.log('THROW ok');
  console.error('APPTEST end');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack || e));
  process.exit(1);
});
