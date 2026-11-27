// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The mouse cursor: the shape ImGui asks for after each frame reaches
// hnui_set_cursor, and ImGui drawing the cursor itself (io.MouseDrawCursor)
// hides the system's through hnui_show_cursor. The host's trace prints what
// Sokol reads back after each command (`sapp_mouse_cursor=`,
// `sapp_mouse_shown=`), so a request the host dropped fails here.
//
// Every frame draws the `widgets` scene (a window "T" at (50,50) holding a
// button and a text field); the pointer is moved with injected events.
// Phases, in order, one output line each:
//
//  CURSOR 1     the pointer over the text field: ImGui asks for TextInput
//               (1), which is hnui's IBEAM (2), Sokol's IBEAM (2).
//  CURSOR 0     the pointer at (600,440), outside the window: Arrow (0), hnui
//               ARROW (1), Sokol's ARROW (1).
//  SOFTCURSOR   scene('widgets', 'softcursor') sets MouseDrawCursor: the
//               system cursor is hidden.
//  HARDCURSOR   scene('widgets', 'hardcursor') clears it: shown again.
//  HIDDENMOVE   the software cursor on again (hidden), and the pointer moved
//               to the field: ImGui now asks for TextInput, but nothing is
//               set while the cursor is hidden, so state().cursor stays 0.
//  SHOWN        the software cursor off: the cursor is shown, and then the
//               I-beam it missed while hidden is set.
//
// The pointer stays at (600,440) through SOFTCURSOR and HARDCURSOR, so the
// shape never changes there: a single cache of the shape would see nothing
// to do and never hide the cursor. state().cursor is the shape last set.
// The first frame also sets ARROW once, since the host starts at its
// DEFAULT; the ordered TRACE lines below match the later ones.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (d) TextInput mapped to ARROW: both of CURSOR 1's TRACE lines fail;
//  the shape cache updated while hidden: HIDDENMOVE and SHOWN's TRACE fail.
//
// Ordering: the addon traces each decision before the hnui call, and the
// host traces applying it on the platform thread, so every addon line
// precedes its own host line. A later addon line comes from a later JS
// frame, which the host's command drain precedes (a command pushed in one
// frame is drained before the tick for the next is sent), or follows a
// __waitPlatformFrames. The one pair in a single frame with no order
// between them -- SHOWN's host `sapp_mouse_shown=1` and its addon shape
// line -- is matched with TRACE-DAG.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-events.js's.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const OUTSIDE = [600, 440];

async function main() {
  const h = await harness.open({title: 'hnui imgui cursor ' + process.pid});
  const {ui, imgui} = h;
  const draw = () => imgui.__test.scene('widgets');
  const state = () => imgui.__test.state();
  const until = (predicate, label) =>
    harness.until(h, draw, predicate, label);
  const inject = (desc) => harness.inject(h, desc);

  async function moveTo(x, y, label) {
    inject({type: 'MOUSE_MOVE', mouseX: x, mouseY: y});
    await until((s) => Math.abs(s.mouseX - x) < 0.01 &&
                    Math.abs(s.mouseY - y) < 0.01, label + ' move');
  }

  await until((s) => s.fieldRect[2] > s.fieldRect[0], 'layout');
  const r = state().fieldRect;
  const field = [Math.floor((r[0] + r[2]) / 2), Math.floor((r[1] + r[3]) / 2)];

  // --- CURSOR 1 / CURSOR 0 ----------------------------------------------
  await moveTo(field[0], field[1], 'FIELD');
  await until((s) => s.cursor === 1, 'CURSOR 1');
  console.log('CURSOR ' + state().cursor);
  await moveTo(OUTSIDE[0], OUTSIDE[1], 'OUTSIDE');
  await until((s) => s.cursor === 0, 'CURSOR 0');
  console.log('CURSOR ' + state().cursor);

  // --- SOFTCURSOR / HARDCURSOR ------------------------------------------
  // The cursor is acted on in the render() of the frame that set the flag;
  // the platform-frame wait lets the host apply and trace the command
  // before the next phase, and before the exit.
  await harness.frame(h, () => imgui.__test.scene('widgets', 'softcursor'));
  console.log('SOFTCURSOR cursor=' + state().cursor + ' platform=' +
      ui.__waitPlatformFrames(2));
  await harness.frame(h, () => imgui.__test.scene('widgets', 'hardcursor'));
  console.log('HARDCURSOR cursor=' + state().cursor + ' platform=' +
      ui.__waitPlatformFrames(2));

  // --- HIDDENMOVE / SHOWN -----------------------------------------------
  await harness.frame(h, () => imgui.__test.scene('widgets', 'softcursor'));
  ui.__waitPlatformFrames(2);
  // The frame that reads the new position is the one whose hover asks for
  // TextInput, so moveTo's last frame has already had its chance to set it.
  await moveTo(field[0], field[1], 'HIDDENMOVE');
  await harness.frame(h, draw);
  console.log('HIDDENMOVE cursor=' + state().cursor);
  await harness.frame(h, () => imgui.__test.scene('widgets', 'hardcursor'));
  console.log('SHOWN cursor=' + state().cursor + ' platform=' +
      ui.__waitPlatformFrames(2));

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}CURSOR 1{{$}}
// CHECK-NEXT: {{^}}CURSOR 0{{$}}
// CHECK-NEXT: {{^}}SOFTCURSOR cursor=0 platform=true{{$}}
// CHECK-NEXT: {{^}}HARDCURSOR cursor=0 platform=true{{$}}
// CHECK-NEXT: {{^}}HIDDENMOVE cursor=0{{$}}
// CHECK-NEXT: {{^}}SHOWN cursor=1 platform=true{{$}}
// CODE: EXITCODE 0

// TRACE: {{^}}[ui] imgui: cursor 1 -> hnui 2{{$}}
// TRACE: {{^}}[ui] command cursor 2: sapp_mouse_cursor=2{{$}}
// TRACE: {{^}}[ui] imgui: cursor 0 -> hnui 1{{$}}
// TRACE: {{^}}[ui] command cursor 1: sapp_mouse_cursor=1{{$}}
// TRACE: {{^}}[ui] imgui: cursor hidden{{$}}
// TRACE: {{^}}[ui] command show cursor: sapp_mouse_shown=0{{$}}
// TRACE: {{^}}[ui] imgui: cursor shown{{$}}
// TRACE: {{^}}[ui] command show cursor: sapp_mouse_shown=1{{$}}
// HIDDENMOVE: hidden again (that nothing is set while hidden is stdout's cursor=0).
// TRACE: {{^}}[ui] imgui: cursor hidden{{$}}
// TRACE: {{^}}[ui] command show cursor: sapp_mouse_shown=0{{$}}
// SHOWN: shown, then the I-beam.
// TRACE: {{^}}[ui] imgui: cursor shown{{$}}
// TRACE-DAG: {{^}}[ui] command show cursor: sapp_mouse_shown=1{{$}}
// TRACE-DAG: {{^}}[ui] imgui: cursor 1 -> hnui 2{{$}}
// TRACE: {{^}}[ui] command cursor 2: sapp_mouse_cursor=2{{$}}
