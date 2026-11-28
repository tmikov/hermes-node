// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Input: hnui events, fed to ImGui by the addon's one event listener, move
// the mouse, press buttons, type into a field and press keys, and what
// ImGui made of them is read back with imgui.__test.state().
//
// Every frame draws the `widgets` scene: a window "T" at (50,50), 300x200,
// holding a 100x30 button "B" and a text field. Events are injected below
// Sokol with `ui.__injectEvent` (through harness.inject, which takes
// logical units), except in REAL, which sends a real key through the
// window driver. An injected event reaches ImGui frames later, and ImGui
// trickles queued input over several frames, so every expectation is
// reached through harness.until() (a frame-count bound; TIMEOUT <label>
// and exit 1 when it is hit), never by counting frames or waiting on time.
// Each keyAPresses expectation is a delta from a baseline sampled once the
// previous case has settled, just before the case's first injection.
//
// Cases, in the order they run, one output line each:
//
//  HOVER       the mouse moved to the button's centre: once ImGui has that
//              position, it wants the mouse.
//  AWAY        moved to (600,440), outside the window: it no longer does.
//  CLICK       left down and up on the button: one click.
//  RIGHT       Sokol's right button (1) down is ImGui's button 1.
//  MIDDLE      Sokol's middle button (2) down is ImGui's button 2. Between
//              them, hnui numbers buttons as the DOM does, so these two
//              pin the renumbering.
//  WHEEL       one wheel step over the window reaches io.MouseWheel.
//  FIELD       a click on the field makes ImGui want text input.
//  TEXT        CHAR h, i; then Left and CHAR X; then Backspace.
//  ASTRAL      End, then CHAR U+1F600: the field holds `hi` and that one
//              code point, which needs a 32-bit ImWchar.
//  REPEATCHAR  a CHAR with repeat set still types (holding a key types it
//              again).
//  APRESS      with the field unfocused (Escape, then a click outside the
//              window): A down, A down again as a repeat, three frames, A
//              up: one press.
//  UNKNOWN     key codes 0 and 999, which ImGui has no key for, cause no
//              assertion, and A still presses once after them. That is
//              all it proves: it cannot tell whether the addon or ImGui
//              dropped them (mutation (f) below).
//  HELD, BLUR  A and the left button held, read back held; then the window
//              loses focus and ImGui reads both released. FOCUSED follows,
//              only after that, since ImGui applies a focus change at the
//              next NewFrame and a loss and regain in one frame would
//              cancel out.
//  LEAVE       the mouse leaves the window: ImGui's position is "none"
//              (-FLT_MAX) and it does not want the mouse.
//  CLAMP       a mouse move to (1e9, -1e9) is read back as (65536,
//              -65536), a wheel step of 1e9 as 65536, and a move to
//              (-300, -40) as itself: the platform half
//              clamps both to +-2^16 (kMaxMouseCoordinate, imgui_addon.h),
//              each clamp traced. ImGui converts a position to int as it is,
//              a drag multiplies the difference of two positions by its
//              speed, and a wheel step becomes a scroll target ImGui
//              converts to an ImS64 (Codex's review, C5).
//  REAL        a real key, `key a` through the window driver (an X client
//              on Linux, the in-process driver on macOS), enters above
//              Sokol: one press.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) buttons passed through unrenumbered: RIGHT and MIDDLE fail;
//  (b) repeated key-downs passed on: APRESS still passes, because ImGui
//      itself drops a down for a key already down; the drop is kept, as the
//      spec states it, and this test cannot tell;
//  (c) no listener registered: every case fails, REAL included;
//  (d) IMGUI_USE_WCHAR32 dropped: ASTRAL fails;
//  (e) modifier events skipped: no case here fails (the clipboard chords
//      test does);
//  (codex) the clamp removed (clampedInput() returning its argument):
//      CLAMP fails.
// And two this test cannot detect, recorded so nobody relies on it for
// them:
//  (f) the listener's `key != ImGuiKey_None` guard removed: UNKNOWN still
//      passes, because ImGui's AddKeyAnalogEvent returns at once for
//      ImGuiKey_None and filters a down for a key already down;
//  (g) AddFocusEvent(true) for HNUI_EVENT_FOCUS removed: every case still
//      passes; a regained focus only clears ImGui's AppFocusLost, which no
//      case reads, and input is applied either way.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-events.js's; the driver argument is
// ui-real-events.js's.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %x11_window_driver > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');
const {startDriver} = require('./fixtures/hnui-demo/window_driver.js');

const driverPath = process.argv[2];

// Sokol's key codes (GLFW's numbering), modifiers and buttons.
const SAPP_KEYCODE_A = 65;
const SAPP_KEYCODE_ESCAPE = 256;
const SAPP_KEYCODE_BACKSPACE = 259;
const SAPP_KEYCODE_LEFT = 263;
const SAPP_KEYCODE_END = 269;
const SAPP_MOUSEBUTTON_LEFT = 0;
const SAPP_MOUSEBUTTON_RIGHT = 1;
const SAPP_MOUSEBUTTON_MIDDLE = 2;

// ImGui's button numbering, for state().mouseDown.
const IMGUI_LEFT = 0;

const TITLE = 'hnui imgui input ' + process.pid;
const OUTSIDE = [600, 440];

async function main() {
  const h = await harness.open({title: TITLE});
  const {win, imgui} = h;
  const draw = () => imgui.__test.scene('widgets');
  const state = () => imgui.__test.state();
  const until = (predicate, label) =>
    harness.until(h, draw, predicate, label);
  const inject = (desc) => harness.inject(h, desc);

  // Whether ImGui's mouse position is (x, y). The position goes through
  // float and a dpiScale division, so it is compared to within 0.01; and
  // ImGui floors it, so every target is a whole number.
  const mouseAt = (s, x, y) =>
    Math.abs(s.mouseX - x) < 0.01 && Math.abs(s.mouseY - y) < 0.01;
  const centre = (r) =>
    [Math.floor((r[0] + r[2]) / 2), Math.floor((r[1] + r[3]) / 2)];

  async function moveTo(x, y, label) {
    inject({type: 'MOUSE_MOVE', mouseX: x, mouseY: y});
    await until((s) => mouseAt(s, x, y), label + ' move');
  }

  // A left click at (x, y), each half read back before the next is sent.
  async function click(x, y, label) {
    await moveTo(x, y, label);
    inject({type: 'MOUSE_DOWN', button: SAPP_MOUSEBUTTON_LEFT,
            mouseX: x, mouseY: y});
    await until((s) => s.mouseDown[IMGUI_LEFT], label + ' down');
    inject({type: 'MOUSE_UP', button: SAPP_MOUSEBUTTON_LEFT,
            mouseX: x, mouseY: y});
    await until((s) => !s.mouseDown[IMGUI_LEFT], label + ' up');
  }

  // A key down then up; ImGui trickles them into separate frames.
  function key(keyCode) {
    inject({type: 'KEY_DOWN', keyCode});
    inject({type: 'KEY_UP', keyCode});
  }

  // A JS keydown listener runs after the native one, so seeing the repeat
  // here means the addon's listener has been handed it.
  let repeatsSeen = 0;
  win.on('keydown', (e) => {
    if (e.repeat)
      ++repeatsSeen;
  });

  // The layout exists once the scene has been drawn.
  await until((s) => s.buttonRect[2] > s.buttonRect[0], 'layout');
  const button = centre(state().buttonRect);
  const field = centre(state().fieldRect);

  // --- HOVER / AWAY -----------------------------------------------------
  await moveTo(button[0], button[1], 'HOVER');
  console.log('HOVER wantCaptureMouse=' + state().wantCaptureMouse);
  await moveTo(OUTSIDE[0], OUTSIDE[1], 'AWAY');
  console.log('AWAY wantCaptureMouse=' + state().wantCaptureMouse);

  // --- CLICK ------------------------------------------------------------
  {
    const base = state().clicks;
    await click(button[0], button[1], 'CLICK');
    await until((s) => s.clicks !== base, 'CLICK');
    console.log('CLICK clicks=' + (state().clicks - base));
  }

  // --- RIGHT / MIDDLE ---------------------------------------------------
  for (const [label, sokolButton] of
       [['RIGHT', SAPP_MOUSEBUTTON_RIGHT], ['MIDDLE', SAPP_MOUSEBUTTON_MIDDLE]]) {
    const at = {mouseX: OUTSIDE[0], mouseY: OUTSIDE[1]};
    inject(Object.assign({type: 'MOUSE_DOWN', button: sokolButton}, at));
    await until((s) => s.mouseDown.some((d) => d), label);
    console.log(label + ' mouseDown=' + state().mouseDown.join(','));
    inject(Object.assign({type: 'MOUSE_UP', button: sokolButton}, at));
    await until((s) => !s.mouseDown.some((d) => d), label + ' up');
  }

  // --- WHEEL ------------------------------------------------------------
  {
    const over = [200, 150];
    await moveTo(over[0], over[1], 'WHEEL');
    const base = state().wheel;
    inject({type: 'MOUSE_SCROLL', scrollY: 1, mouseX: over[0],
            mouseY: over[1]});
    await until((s) => s.wheel !== base, 'WHEEL');
    console.log('WHEEL ' + (state().wheel - base));
  }

  // --- FIELD ------------------------------------------------------------
  await click(field[0], field[1], 'FIELD');
  await until((s) => s.wantTextInput, 'FIELD');
  console.log('FIELD wantTextInput=' + state().wantTextInput);

  // --- TEXT -------------------------------------------------------------
  // Each step waits for the field's length, not its content, so a wrong
  // edit prints instead of timing out.
  const length = (s) => Array.from(s.text).length;
  inject({type: 'CHAR', charCode: 0x68}); // h
  inject({type: 'CHAR', charCode: 0x69}); // i
  await until((s) => length(s) === 2, 'TEXT hi');
  console.log('TEXT ' + state().text);
  key(SAPP_KEYCODE_LEFT);
  inject({type: 'CHAR', charCode: 0x58}); // X
  await until((s) => length(s) === 3, 'TEXT hXi');
  console.log('TEXT ' + state().text);
  key(SAPP_KEYCODE_BACKSPACE);
  await until((s) => length(s) === 2, 'TEXT backspace');
  console.log('TEXT ' + state().text);

  // --- ASTRAL -----------------------------------------------------------
  {
    key(SAPP_KEYCODE_END);
    inject({type: 'CHAR', charCode: 0x1F600});
    await until((s) => length(s) === 3, 'ASTRAL');
    const chars = Array.from(state().text);
    const ok = chars[0] === 'h' && chars[1] === 'i' &&
        chars[2].codePointAt(0) === 0x1F600;
    console.log('ASTRAL ' + (ok ? 'ok' : 'WRONG ' + JSON.stringify(chars)));
  }

  // --- REPEATCHAR -------------------------------------------------------
  {
    const before = state().text;
    inject({type: 'CHAR', charCode: 0x72, repeat: true}); // r
    await until((s) => s.text !== before, 'REPEATCHAR');
    const text = state().text;
    console.log('REPEATCHAR ' +
        (text === before + 'r' ? 'ok' : 'WRONG ' + JSON.stringify(text)));
  }

  // --- APRESS -----------------------------------------------------------
  // Unfocus the field: Escape, read back; then a click outside the window.
  key(SAPP_KEYCODE_ESCAPE);
  await until((s) => !s.wantTextInput, 'APRESS escape');
  await click(OUTSIDE[0], OUTSIDE[1], 'APRESS');
  {
    const base = state().keyAPresses;
    const repeatsBefore = repeatsSeen;
    inject({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
    inject({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A, repeat: true});
    await until((s) => s.keyADown && repeatsSeen > repeatsBefore,
                'APRESS down');
    for (let i = 0; i < 3; ++i)
      await harness.frame(h, draw);
    inject({type: 'KEY_UP', keyCode: SAPP_KEYCODE_A});
    await until((s) => !s.keyADown, 'APRESS up');
    console.log('APRESS ' + (state().keyAPresses - base));
  }

  // --- UNKNOWN ----------------------------------------------------------
  {
    const base = state().keyAPresses;
    inject({type: 'KEY_DOWN', keyCode: 0});
    inject({type: 'KEY_DOWN', keyCode: 999});
    inject({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
    await until((s) => s.keyADown, 'UNKNOWN down');
    inject({type: 'KEY_UP', keyCode: SAPP_KEYCODE_A});
    await until((s) => !s.keyADown, 'UNKNOWN up');
    const delta = state().keyAPresses - base;
    console.log('UNKNOWN ' + (delta === 1 ? 'ok' : 'WRONG') +
        ' apress=' + delta);
  }

  // --- HELD / BLUR ------------------------------------------------------
  {
    inject({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
    inject({type: 'MOUSE_DOWN', button: SAPP_MOUSEBUTTON_LEFT,
            mouseX: OUTSIDE[0], mouseY: OUTSIDE[1]});
    await until((s) => s.keyADown && s.mouseDown[IMGUI_LEFT], 'HELD');
    let s = state();
    console.log('HELD keyADown=' + s.keyADown +
        ' mouseDown=' + s.mouseDown.join(','));
    inject({type: 'UNFOCUSED'});
    await until((st) => !st.keyADown && !st.mouseDown.some((d) => d),
                'BLUR');
    s = state();
    console.log('BLUR keyADown=' + s.keyADown +
        ' mouseDown=' + s.mouseDown.join(','));
    inject({type: 'FOCUSED'});
  }

  // --- LEAVE ------------------------------------------------------------
  {
    await moveTo(button[0], button[1], 'LEAVE');
    await until((s) => s.wantCaptureMouse, 'LEAVE hover');
    inject({type: 'MOUSE_LEAVE'});
    await until((s) => s.mouseX < -1e30, 'LEAVE');
    const s = state();
    console.log('LEAVE wantCaptureMouse=' + s.wantCaptureMouse +
        ' mouseY<-1e30=' + (s.mouseY < -1e30));
  }

  // --- CLAMP ------------------------------------------------------------
  {
    inject({type: 'MOUSE_MOVE', mouseX: 1e9, mouseY: -1e9});
    await until((s) => s.mouseX !== -Infinity && s.mouseX > -1e30, 'CLAMP move');
    let s = state();
    const pos = s.mouseX + ',' + s.mouseY;
    const base = s.wheel;
    inject({type: 'MOUSE_SCROLL', scrollY: 1e9, mouseX: 1e9, mouseY: -1e9});
    await until((t) => t.wheel !== base, 'CLAMP wheel');
    s = state();
    // A negative coordinate inside the bound (a drag captured outside the
    // window) reaches ImGui unchanged.
    inject({type: 'MOUSE_MOVE', mouseX: -300, mouseY: -40});
    await until((t) => mouseAt(t, -300, -40), 'CLAMP inside');
    console.log('CLAMP mouse=' + pos + ' wheel=' + (s.wheel - base) +
        ' inside=' + state().mouseX + ',' + state().mouseY);
  }

  // --- REAL -------------------------------------------------------------
  {
    const driver = await startDriver({ui: h.ui, driverPath, title: TITLE});
    const base = state().keyAPresses;
    const answer = await driver.send('key a');
    if (!answer.startsWith('SENT')) {
      console.log('REAL driver: ' + answer);
      process.exit(1);
    }
    await until((s) => s.keyAPresses !== base && !s.keyADown, 'REAL');
    console.log('REAL apress=' + (state().keyAPresses - base));
    driver.stop();
  }

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}HOVER wantCaptureMouse=true{{$}}
// CHECK-NEXT: {{^}}AWAY wantCaptureMouse=false{{$}}
// CHECK-NEXT: {{^}}CLICK clicks=1{{$}}
// CHECK-NEXT: {{^}}RIGHT mouseDown=false,true,false{{$}}
// CHECK-NEXT: {{^}}MIDDLE mouseDown=false,false,true{{$}}
// CHECK-NEXT: {{^}}WHEEL 1{{$}}
// CHECK-NEXT: {{^}}FIELD wantTextInput=true{{$}}
// CHECK-NEXT: {{^}}TEXT hi{{$}}
// CHECK-NEXT: {{^}}TEXT hXi{{$}}
// CHECK-NEXT: {{^}}TEXT hi{{$}}
// CHECK-NEXT: {{^}}ASTRAL ok{{$}}
// CHECK-NEXT: {{^}}REPEATCHAR ok{{$}}
// CHECK-NEXT: {{^}}APRESS 1{{$}}
// CHECK-NEXT: {{^}}UNKNOWN ok apress=1{{$}}
// CHECK-NEXT: {{^}}HELD keyADown=true mouseDown=true,false,false{{$}}
// CHECK-NEXT: {{^}}BLUR keyADown=false mouseDown=false,false,false{{$}}
// CHECK-NEXT: {{^}}LEAVE wantCaptureMouse=false mouseY<-1e30=true{{$}}
// CHECK-NEXT: {{^}}CLAMP mouse=65536,-65536 wheel=65536 inside=-300,-40{{$}}
// CHECK-NEXT: {{^}}REAL apress=1{{$}}
// CODE: EXITCODE 0

// TRACE: {{^}}[ui] imgui: event listener registered (id {{[1-9][0-9]*}}){{$}}
// TRACE: {{^}}[ui] imgui: input: mouse x 1e+09 clamped to 65536{{$}}
// TRACE-NEXT: {{^}}[ui] imgui: input: mouse y -1e+09 clamped to -65536{{$}}
// TRACE: {{^}}[ui] imgui: input: wheel y 1e+09 clamped to 65536{{$}}
