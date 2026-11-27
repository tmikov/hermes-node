// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Docking and settings group: the dock space's ID, a docked layout
// loaded from an ini string before the first frame and again between two
// frames, the saved settings, the out-of-frame rule of
// loadIniSettingsFromMemory, and the group's coverage. A run of its own
// (`nowindow`) calls the out-of-frame functions with no window.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  BEFORE    before the first newFrame(): saveIniSettingsToMemory()
//            returns ImGui's empty settings (only the [Docking][Data]
//            header), then loadIniSettingsFromMemory(DOCKED) is called.
//  FIRST     the first frame: dockSpaceOverViewport() returns
//            DOCKSPACE_ID, and 'Left Pane' and 'Right Pane', submitted
//            after it, are docked (state().windows). Waited for after it
//            (harness.until): they tile the display, both the full height,
//            Left from the left edge, Right to the right edge, the docking
//            separator (2, imgui.cpp:1646) between them. In the first
//            frame they do not yet: the split lays out only the nodes
//            visible in the previous frame (imgui.cpp:20615-20618), and
//            Right reads back the whole display (measured).
//  STABLE    dockSpaceOverViewport() returns the same ID in each of three
//            more frames, and dockSpaceOverViewport(DOCKSPACE_ID) does too.
//  SAVE      saveIniSettingsToMemory() inside a frame names both windows,
//            the dock space by its ID, and each window's dock node.
//  INFRAME   loadIniSettingsFromMemory() inside a frame: ERR_IMGUI_USAGE,
//            not called; the next frame is still docked.
//  UNDOCK    FLOATING loaded between two frames: in the next frame both
//            windows are floating, at the position and size FLOATING
//            gives them.
//  REDOCK    DOCKED loaded between two frames: in the next frame both are
//            docked again, and (waited for) tile the display as in FIRST.
//  REJECTED  a U+0000 in the ini string: ERR_INVALID_ARG_VALUE, not
//            called; a private DockNodeFlags bit (DockSpace, 1 << 10):
//            ERR_OUT_OF_RANGE, not called.
//  COVERAGE  every Docking function has reached ImGui.
//  NOWINDOW  (a run of its own, no window) every function that works
//            outside a frame and needs a context throws
//            ERR_IMGUI_NO_WINDOW, not called; dockSpaceOverViewport()
//            throws ERR_IMGUI_NO_FRAME.
//
// How DOCKED was produced: written from the format DockSettingsHandler_
// ReadLine and WindowSettingsHandler_ReadLine parse (imgui.cpp:21883-21922,
// 16807-16823), loaded before the first frame of a scratch run that drew
// this test's frames, and replaced by what saveIniSettingsToMemory()
// returned after four of them, less the fallback window's section and the
// LastUsed lines (a date). The progress file's Task 8 notes give the
// steps.
// DOCKSPACE_ID is what DockSpaceOverViewport() gives a zero argument:
// GetID("DockSpace") inside its host window "WindowOverViewport_11111111"
// (imgui.cpp:21035-21046; 0x11111111 is the main viewport's ID,
// imgui.cpp:1518), the same for every run.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  ImGui::DockSpaceOverViewport() removed, enterImGui() kept: FIRST fails;
//  enterImGui() removed from saveIniSettingsToMemory: COVERAGE fails;
//  needNoFrame() removed from loadIniSettingsFromMemory: INFRAME fails;
//  ImGui::LoadIniSettingsFromMemory() removed: FIRST fails;
//  dockSpaceOverViewport's flags check removed: REJECTED fails (ImGui
//  aborts);
//  the returned ID replaced by the argument: FIRST fails;
//  needContext() removed from saveIniSettingsToMemory: NOWINDOW fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err
// RUN: sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 %timeout 60 %hermes-node-ui %s nowindow > %t.nowindow.out'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=NOWINDOW %s < %t.nowindow.out

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const mode = process.argv[2];

const DOCKSPACE_ID = 0x08BD597D;

const DOCKED = [
  '[Window][Left Pane]',
  'Pos=0,0',
  'Size=320,480',
  'Collapsed=0',
  'DockId=0x00000001,0',
  '',
  '[Window][Right Pane]',
  'Pos=322,0',
  'Size=318,480',
  'Collapsed=0',
  'DockId=0x00000002,0',
  '',
  '[Window][WindowOverViewport_11111111]',
  'Pos=0,0',
  'Size=640,480',
  'Collapsed=0',
  '',
  '[Docking][Data]',
  'DockSpace   ID=0x08BD597D Window=0x1BBC0F80 Pos=0,0 Size=640,480 Split=X',
  '  DockNode  ID=0x00000001 Parent=0x08BD597D SizeRef=320,480 Selected=0x97F83831',
  '  DockNode  ID=0x00000002 Parent=0x08BD597D SizeRef=318,480 CentralNode=1 Selected=0xACD8EF16',
  '',
].join('\n');

const FLOATING = [
  '[Window][Left Pane]',
  'Pos=40,60',
  'Size=200,150',
  'Collapsed=0',
  '',
  '[Window][Right Pane]',
  'Pos=300,200',
  'Size=220,160',
  'Collapsed=0',
  '',
].join('\n');

function fail(line) {
  console.log(line);
  process.exit(1);
}

function same(a, b) {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

/** The functions that work outside a frame and need a context. */
const OUT_OF_FRAME = {
  getTime: [],
  getFrameRate: [],
  getDeltaTime: [],
  getDisplaySize: [],
  wantCaptureMouse: [],
  wantCaptureKeyboard: [],
  styleColorsDark: [],
  styleColorsLight: [],
  saveIniSettingsToMemory: [],
  loadIniSettingsFromMemory: [''],
};

function noWindow() {
  const imgui = require('hermes-node:imgui');
  const calls = (name) => imgui.__test.bindCalls()[name] || 0;
  for (const name of Object.keys(OUT_OF_FRAME)) {
    const e = harness.expectThrow(() => imgui[name](...OUT_OF_FRAME[name]),
                                  'ERR_IMGUI_NO_WINDOW');
    if (calls(name) !== 0)
      fail('CALLED ' + name + ': ' + e.message);
  }
  harness.expectThrow(() => imgui.dockSpaceOverViewport(),
                      'ERR_IMGUI_NO_FRAME');
  if (calls('dockSpaceOverViewport') !== 0)
    fail('CALLED dockSpaceOverViewport');
  console.log('NOWINDOW ok');
  console.log('NOWINDOW message ' +
              harness.expectThrow(() => imgui.loadIniSettingsFromMemory(''),
                                  'ERR_IMGUI_NO_WINDOW').message);
  process.exit(0);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);

  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ' ' + code + ': ' + e.message);
    return e;
  }

  /** What state().windows says of `name` after the last frame. */
  function windowState(name) {
    return imgui.__test.state().windows.find((w) => w.name === name);
  }

  /**
   * A frame's body: the dock space first, then the two windows, each
   * reading back its position and size into `out`, with the dock space's
   * ID and the display size; then `body`.
   */
  function dockDraw(out, body = () => {}) {
    return () => {
      out.id = imgui.dockSpaceOverViewport();
      for (const name of ['Left Pane', 'Right Pane']) {
        imgui.begin(name);
        out[name] = {pos: imgui.getWindowPos(), size: imgui.getWindowSize()};
        imgui.text(name);
        imgui.end();
      }
      out.display = imgui.getDisplaySize();
      body();
    };
  }

  /** One frame of dockDraw(); resolves with what it read back. */
  async function dockFrame(body) {
    const out = {};
    await h.run(dockDraw(out, body));
    return out;
  }

  /** Whether both windows are docked and tile the display. */
  function tiles(f) {
    const l = f['Left Pane'], r = f['Right Pane'];
    const [w, ht] = f.display;
    return windowState('Left Pane').docked && windowState('Right Pane').docked &&
        same(l.pos, [0, 0]) && l.size[1] === ht && r.pos[1] === 0 &&
        r.size[1] === ht && l.size[0] > 0 && r.size[0] > 0 &&
        r.pos[0] === l.pos[0] + l.size[0] + 2 && r.pos[0] + r.size[0] === w;
  }

  /** Frames of dockDraw() until tiles() holds (harness.until). */
  async function untilTiled(label) {
    const out = {};
    await harness.until(h, dockDraw(out), () => tiles(out), label);
  }

  const describe = (f) => JSON.stringify(f) + ' ' +
      JSON.stringify(imgui.__test.state().windows.filter(
          (w) => w.name.endsWith('Pane')));

  // --- BEFORE ----------------------------------------------------------
  {
    if (imgui.__test.state().frameOpen)
      fail('BEFORE frame open');
    const saved = imgui.saveIniSettingsToMemory();
    const before = calls('loadIniSettingsFromMemory');
    const ret = imgui.loadIniSettingsFromMemory(DOCKED);
    const ok = saved === '[Docking][Data]\n\n' && ret === undefined &&
        calls('loadIniSettingsFromMemory') === before + 1;
    console.log(ok ? 'BEFORE ok' : 'BEFORE WRONG ' + JSON.stringify(saved));
  }

  // --- FIRST -----------------------------------------------------------
  {
    const f = await dockFrame();
    const ok = f.id === DOCKSPACE_ID && windowState('Left Pane').docked &&
        windowState('Right Pane').docked;
    console.log(ok ? 'FIRST ok' : 'FIRST WRONG ' + f.id + ' ' + describe(f));
    await untilTiled('FIRST tiled');
    console.log('FIRST tiled');
  }

  // --- STABLE ----------------------------------------------------------
  {
    const ids = [];
    for (let i = 0; i < 3; ++i)
      ids.push((await dockFrame()).id);
    let explicit;
    await h.run(() => {
      explicit = imgui.dockSpaceOverViewport(DOCKSPACE_ID);
    });
    const ok = ids.every((id) => id === DOCKSPACE_ID) &&
        explicit === DOCKSPACE_ID;
    console.log(ok ? 'STABLE ok' : 'STABLE WRONG ' + ids + ' ' + explicit);
  }

  // --- SAVE ------------------------------------------------------------
  {
    let saved;
    await dockFrame(() => {
      saved = imgui.saveIniSettingsToMemory();
    });
    const want = [
      '[Window][Left Pane]', '[Window][Right Pane]', 'DockSpace',
      ' ID=0x08BD597D ', 'DockId=0x00000001', 'DockId=0x00000002',
    ];
    const missing = want.filter((w) => !saved.includes(w));
    console.log(missing.length === 0 ? 'SAVE ok' :
                                       'SAVE MISSING ' + JSON.stringify(missing) +
                    ' in ' + JSON.stringify(saved));
  }

  // --- INFRAME ---------------------------------------------------------
  {
    let message;
    await dockFrame(() => {
      message = rejects('loadIniSettingsFromMemory',
                        () => imgui.loadIniSettingsFromMemory(FLOATING),
                        'ERR_IMGUI_USAGE').message;
    });
    const f = await dockFrame();
    console.log(tiles(f) ? 'INFRAME ok' : 'INFRAME WRONG ' + describe(f));
    console.log('INFRAME message ' + message);
  }

  // --- UNDOCK ----------------------------------------------------------
  {
    imgui.loadIniSettingsFromMemory(FLOATING);
    const f = await dockFrame();
    const l = f['Left Pane'], r = f['Right Pane'];
    const ok = !windowState('Left Pane').docked &&
        !windowState('Right Pane').docked && same(l.pos, [40, 60]) &&
        same(l.size, [200, 150]) && same(r.pos, [300, 200]) &&
        same(r.size, [220, 160]);
    console.log(ok ? 'UNDOCK ok' : 'UNDOCK WRONG ' + describe(f));
  }

  // --- REDOCK ----------------------------------------------------------
  {
    imgui.loadIniSettingsFromMemory(DOCKED);
    const f = await dockFrame();
    const ok = windowState('Left Pane').docked &&
        windowState('Right Pane').docked;
    console.log(ok ? 'REDOCK ok' : 'REDOCK WRONG ' + describe(f));
    await untilTiled('REDOCK tiled');
    console.log('REDOCK tiled');
  }

  // --- REJECTED --------------------------------------------------------
  {
    let nul;
    nul = rejects('loadIniSettingsFromMemory',
                  () => imgui.loadIniSettingsFromMemory('[Window][a\0b]'),
                  'ERR_INVALID_ARG_VALUE').message;
    await h.run(() => {
      rejects('dockSpaceOverViewport',
              () => imgui.dockSpaceOverViewport(0, 1 << 10),
              'ERR_OUT_OF_RANGE');
    });
    console.log('REJECTED ok');
    console.log('REJECTED message ' + nul);
  }

  harness.checkGroupCoverage(h, 'Docking');
  process.exit(0);
}

(mode === 'nowindow' ? Promise.resolve().then(noWindow) : main())
    .catch((e) => {
      console.log('FAILED ' + (e && e.stack));
      process.exit(1);
    });

// CHECK: {{^}}BEFORE ok{{$}}
// CHECK-NEXT: {{^}}FIRST ok{{$}}
// CHECK-NEXT: {{^}}FIRST tiled{{$}}
// CHECK-NEXT: {{^}}STABLE ok{{$}}
// CHECK-NEXT: {{^}}SAVE ok{{$}}
// CHECK-NEXT: {{^}}INFRAME ok{{$}}
// CHECK-NEXT: {{^}}INFRAME message imgui.loadIniSettingsFromMemory(): must be called outside a frame (before newFrame()){{$}}
// CHECK-NEXT: {{^}}UNDOCK ok{{$}}
// CHECK-NEXT: {{^}}REDOCK ok{{$}}
// CHECK-NEXT: {{^}}REDOCK tiled{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.loadIniSettingsFromMemory(): iniData must not contain U+0000{{$}}
// CHECK-NEXT: {{^}}COVERAGE Docking ok{{$}}
// CODE: EXITCODE 0

// INFRAME's refusal is the only usage error, traced once (the
// --implicit-check-not on the TRACE run excludes any other).
// TRACE: {{^}}[ui] imgui: context created
// TRACE: {{^}}[ui] imgui: usage error: must be called outside a frame (before newFrame()){{$}}

// NOWINDOW: {{^}}NOWINDOW ok{{$}}
// NOWINDOW-NEXT: {{^}}NOWINDOW message imgui.loadIniSettingsFromMemory(): there is no window; create one with ui.createWindow() first{{$}}
