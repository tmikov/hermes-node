// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The clipboard: ImGui's copy, cut and paste reach hnui_write_clipboard and
// hnui_read_clipboard_sync through the two platform_io functions, and the
// shortcut chords reach ImGui with their modifier.
//
// Every frame draws the `widgets` scene (a window "T" at (50,50) holding a
// button and a text field); the field is focused with a click and edited
// with injected events. A chord is the shortcut modifier's key down, the
// key down and up, and the modifier's key up, each event carrying
// SAPP_MODIFIER_CTRL on Linux and SAPP_MODIFIER_SUPER on macOS: ImGui's
// shortcut modifier follows ConfigMacOSXBehaviors, which is on for an
// Apple build.
//
// An edit is read back with settle(): type a marker `z`, wait for the field
// to end with it, Backspace, wait for it to be gone. ImGui applies queued
// input in order, so once the marker has come and gone every event before
// it has been applied, and a paste that inserted nothing (or the wrong
// thing) prints what the field holds rather than timing out.
//
// Cases, in the order they run, one output line each:
//
//  EMPTYPASTE  with nothing copied in this run: `q`, then the paste chord.
//              Nothing owns the clipboard -- under %with-display each Linux
//              run has its own fresh Xvfb, and on macOS a pasteboard of its
//              own, which is new -- so the read reports
//              HNUI_UNAVAILABLE (4), ImGui gets NULL and the field keeps
//              `q`. (Review Focus 2.)
//  PASTE       the field cleared (select-all, Backspace), `hello` typed,
//              select-all, copy, End, paste: `hellohello`. The paste is
//              served by this process's own selection on X11 (the host's
//              reader short-circuits when its window owns it) and by the
//              run's pasteboard on macOS.
//  CUT         select-all, cut: the field is empty...
//  CUTPASTE    ...and a paste puts the cut text back.
//
// The trace is read for what was done and read back: the addon's
// `clipboard write requested` / `clipboard get` lines (lengths only, never
// the text), and the host's per-backend read-back of each write. The
// addon traces a write before making it, so its line precedes the host's
// by program order. Stdout alone passes with no backend clipboard at all,
// since ImGui falls back to an in-process buffer of its own when no
// backend sets the functions: the TRACE run is what proves the backend.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) get returns "" on failure, untraced: TRACE fails on the status line;
//  (b) set does not call hnui_write_clipboard: PASTE fails;
//  (c) modifier events skipped in the listener: PASTE fails;
// And one it cannot: (e) HNUI_EVENT_PASTE also inserted into the field
// still passes, because the chords are injected below Sokol, which produces
// its paste event only from a real key.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-events.js's; the backend argument and the
// per-backend prefixes are ui-commands.js's.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %ui-backend > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefixes=TRACE,TRACE-%ui-backend %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

// `gl` or `metal`, from the RUN line's %ui-backend.
const backend = process.argv[2];
if (backend !== 'gl' && backend !== 'metal') {
  console.log('BACKEND argument is not gl or metal: ' + backend);
  process.exit(2);
}

// macOS writes the clipboard to the pasteboard HERMES_NODE_UI_TEST_PASTEBOARD
// names, and to the general one -- the clipboard of whoever is using the
// Mac -- without it. utils/with-display.py sets a private hnui-test-* name;
// a run without the wrapper is refused before anything is written.
if (process.platform === 'darwin' &&
    !(process.env.HERMES_NODE_UI_TEST_PASTEBOARD || '').startsWith(
        'hnui-test-')) {
  console.log(
      'REFUSED: HERMES_NODE_UI_TEST_PASTEBOARD does not name a hnui-test-* ' +
      'pasteboard, so this run would write the general pasteboard. Run it ' +
      'under utils/with-display.py.');
  process.exit(2);
}

// Sokol's key codes (GLFW's numbering), modifiers and buttons.
const SAPP_KEYCODE_A = 65;
const SAPP_KEYCODE_C = 67;
const SAPP_KEYCODE_V = 86;
const SAPP_KEYCODE_X = 88;
const SAPP_KEYCODE_BACKSPACE = 259;
const SAPP_KEYCODE_END = 269;
const SAPP_KEYCODE_LEFT_CONTROL = 341;
const SAPP_KEYCODE_LEFT_SUPER = 343;
const SAPP_MODIFIER_CTRL = 0x2;
const SAPP_MODIFIER_SUPER = 0x8;
const SAPP_MOUSEBUTTON_LEFT = 0;

const MAC = process.platform === 'darwin';
const SHORTCUT_KEY = MAC ? SAPP_KEYCODE_LEFT_SUPER : SAPP_KEYCODE_LEFT_CONTROL;
const SHORTCUT_MOD = MAC ? SAPP_MODIFIER_SUPER : SAPP_MODIFIER_CTRL;

async function main() {
  const h = await harness.open({title: 'hnui imgui clipboard ' + process.pid});
  const {imgui} = h;
  const draw = () => imgui.__test.scene('widgets');
  const state = () => imgui.__test.state();
  const until = (predicate, label) =>
    harness.until(h, draw, predicate, label);
  const inject = (desc) => harness.inject(h, desc);

  function key(keyCode) {
    inject({type: 'KEY_DOWN', keyCode});
    inject({type: 'KEY_UP', keyCode});
  }
  function chord(keyCode) {
    inject({type: 'KEY_DOWN', keyCode: SHORTCUT_KEY, modifiers: SHORTCUT_MOD});
    inject({type: 'KEY_DOWN', keyCode, modifiers: SHORTCUT_MOD});
    inject({type: 'KEY_UP', keyCode, modifiers: SHORTCUT_MOD});
    inject({type: 'KEY_UP', keyCode: SHORTCUT_KEY, modifiers: 0});
  }
  function type(text) {
    for (const ch of text)
      inject({type: 'CHAR', charCode: ch.codePointAt(0)});
  }
  async function settle(label) {
    type('z');
    await until((s) => s.text.endsWith('z'), label + ' marker');
    key(SAPP_KEYCODE_BACKSPACE);
    await until((s) => !s.text.endsWith('z'), label + ' marker gone');
    return state().text;
  }

  // Focus the field, once the layout exists.
  await until((s) => s.fieldRect[2] > s.fieldRect[0], 'layout');
  const r = state().fieldRect;
  const field = [Math.floor((r[0] + r[2]) / 2), Math.floor((r[1] + r[3]) / 2)];
  inject({type: 'MOUSE_MOVE', mouseX: field[0], mouseY: field[1]});
  inject({type: 'MOUSE_DOWN', button: SAPP_MOUSEBUTTON_LEFT,
          mouseX: field[0], mouseY: field[1]});
  inject({type: 'MOUSE_UP', button: SAPP_MOUSEBUTTON_LEFT,
          mouseX: field[0], mouseY: field[1]});
  await until((s) => s.wantTextInput, 'focus');

  // --- EMPTYPASTE -------------------------------------------------------
  type('q');
  await until((s) => s.text === 'q', 'EMPTYPASTE q');
  chord(SAPP_KEYCODE_V);
  console.log('EMPTYPASTE text=' + await settle('EMPTYPASTE'));

  // --- PASTE ------------------------------------------------------------
  chord(SAPP_KEYCODE_A);
  key(SAPP_KEYCODE_BACKSPACE);
  await until((s) => s.text === '', 'PASTE clear');
  type('hello');
  await until((s) => s.text === 'hello', 'PASTE hello');
  chord(SAPP_KEYCODE_A);
  chord(SAPP_KEYCODE_C);
  key(SAPP_KEYCODE_END);
  chord(SAPP_KEYCODE_V);
  console.log('PASTE text=' + await settle('PASTE'));

  // --- CUT / CUTPASTE ---------------------------------------------------
  chord(SAPP_KEYCODE_A);
  chord(SAPP_KEYCODE_X);
  console.log('CUT text=' + await settle('CUT'));
  chord(SAPP_KEYCODE_V);
  console.log('CUTPASTE text=' + await settle('CUTPASTE'));

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}EMPTYPASTE text=q{{$}}
// CHECK-NEXT: {{^}}PASTE text=hellohello{{$}}
// CHECK-NEXT: {{^}}CUT text={{$}}
// CHECK-NEXT: {{^}}CUTPASTE text=hellohello{{$}}
// CODE: EXITCODE 0

// TRACE-gl: {{^}}[ui] clipboard read: no owner{{$}}
// TRACE: {{^}}[ui] imgui: clipboard get: status 4{{$}}
// TRACE: {{^}}[ui] imgui: clipboard write requested, 5 bytes{{$}}
// TRACE-gl: {{^}}[ui] command clipboard: owns_selection=1 bytes=5{{$}}
// TRACE-metal: {{^}}[ui] command clipboard: pasteboard_bytes=5 bytes=5{{$}}
// TRACE-gl: {{^}}[ui] clipboard read: own selection{{$}}
// TRACE: {{^}}[ui] imgui: clipboard get: ok, 5 bytes{{$}}
// TRACE: {{^}}[ui] imgui: clipboard write requested, 10 bytes{{$}}
// TRACE-gl: {{^}}[ui] command clipboard: owns_selection=1 bytes=10{{$}}
// TRACE-metal: {{^}}[ui] command clipboard: pasteboard_bytes=10 bytes=10{{$}}
// TRACE: {{^}}[ui] imgui: clipboard get: ok, 10 bytes{{$}}
