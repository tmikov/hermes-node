// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The application, examples/imgui-app/app.js, in its window, driven by the
// test: createApp({root, clock: 'fixed', driven: true}) registers no frame
// callback of its own, and every frame here is one app.frame() run through
// harness.adopt(app)'s h.run. Clicks go to the rectangles the application
// records (app.rects) for each Files row; asynchronous file work is waited
// for with app.idle() while frames keep running; nothing is asserted from
// elapsed time.
//
// The root is a copy of test/fixtures/imgui/app-tree at %t.tree, to which
// the test adds `gone.txt` (removed after it was listed) and `locked/`
// (mode 000; as root, which can read it anyway, the case prints
// `SKIP locked`).
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
//             <tree> entries=7`.)
//  OPEN       a click on a.txt's row: `APP opened <tree>/a.txt 12
//             lines=3`, and the viewer's state is text with 3 lines.
//  DIR        a click on dir1's row: `APP dir <tree>/dir1 entries=2`, and
//             a rectangle for dir1/b.js appears in app.rects.
//  EDGES      empty.txt (`APP opened ... 0 lines=0`), binary.bin (hex, 16
//             rows), gone.txt removed after it was listed (`APP error
//             <path> ENOENT`), locked/ (`APP error <path> EACCES`): each
//             prints its line and the next frame draws. (The size and
//             encoding edges are test/imgui-app-viewer.js's.)
//  CANVAS     at setTime(0) the canvas's centre pixel is the centre
//             circle's configured colour; at setTime(1) the pixel at
//             canvas.orbitPosition(1) is the orbiting circle's colour and
//             the one at orbitPosition(0) is not. Logical positions are
//             multiplied by app.scale: a capture is in framebuffer pixels.
//  TINY       the window resized through the window driver to 1x1 (the
//             size granted is printed, unchecked) and back to 1280x800,
//             with part A's resize protocol: no `APP error` between, no
//             usage error (the trace's exclusion), PANELS holds after.
//  RESET      a layout in which Log floats, loaded between frames with
//             imgui.loadIniSettingsFromMemory: Log is then not docked;
//             app.resetLayout() and two frames: all six docked again.
//  QUIT       app.quit(): the process ends with status 0 (CODE).
//
// `throw` (Review Focus 2): the Widgets panel's Table tab selected, then
// with an uncaughtException listener installed
// `panels.widgets.throwNextFrame = true`: the panel throws from inside a
// listClipper callback, inside a table, inside a child window, inside its
// tab and window. That frame is a frame callback of its own, registered
// with ui.requestAnimationFrame (not harness.rawTick, which would catch
// the throw itself and the listener would never see it); the listener
// sees the error; after harness.resync, 10 frames later PANELS holds. Its
// stderr: the discarded frame's usage errors and `frame discarded`, and
// after the marker the test writes once the next frame has drawn, no
// usage error at all.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  the Reset Layout load done inside the frame: RESET (the load throws
//  ERR_IMGUI_USAGE);
//  the canvas drawing's orbit radius changed: CANVAS;
//  CallbackScope's destructor not lowering the callback depth: `throw`
//  (the next newFrame() throws ERR_IMGUI_USAGE and PANELS fails).
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
// CHECK: APP dir [[TREE]] entries=7
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
// CHECK: CANVAS ok
// CHECK: TINY start
// CHECK-NOT: APP error
// CHECK: TINY size {{[0-9]+x[0-9]+$}}
// CHECK-NOT: APP error
// CHECK: TINY ok
// CHECK: RESET floating
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

  /** Frames until app.rects has `key`. */
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
    return runThrow(app, h, checkPanels);

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
  {
    await clickRow(path.join(tree, 'dir1'));
    await settle('DIR');
    await waitRect(path.join(tree, 'dir1', 'b.js'));
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
    // Log's window settings without a DockId: what Task 8's UNDOCK case
    // loaded between frames, for one window.
    imgui.loadIniSettingsFromMemory(
        '[Window][Log]\nPos=200,200\nSize=300,200\nCollapsed=0\n');
    await h.run();
    const log = state().windows.find((w) => w.name === 'Log');
    if (log === undefined || log.docked)
      fail('RESET WRONG not floating ' + JSON.stringify(log));
    console.log('RESET floating');
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

async function runThrow(app, h, checkPanels) {
  const widgets = app.panels.widgets;
  widgets.selectTab = 'Table';
  for (let i = 0; i < 600 && widgets.visibleTab !== 'Table'; ++i)
    await h.run();
  if (widgets.visibleTab !== 'Table')
    fail('TIMEOUT THROW tab');
  console.log('THROW tab ' + widgets.visibleTab);

  const caught = new Promise((resolve) => {
    process.once('uncaughtException', (e) => {
      console.log('THROW caught ' + e.message);
      setImmediate(resolve);
    });
  });
  widgets.throwNextFrame = true;
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
