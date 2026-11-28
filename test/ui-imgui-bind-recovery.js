// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// ImGui's three error outputs stay off for every binding call, whatever
// turned them on in between. ShowDemoWindow() does exactly that: with its
// Configuration > Configuration node open it sets
// io.ConfigErrorRecoveryEnableAssert, ...EnableDebugLog and
// ...EnableTooltip whenever all three are off (imgui_demo.cpp:626-627),
// every frame, and the rest of the frame then runs with them on. With the
// assert on, IM_ASSERT_USER_ERROR asserts (imgui_internal.h:2374), so a
// recoverable usage error aborts a Debug build; with the log on ImGui
// printf()s it to stdout.
//
// The demo's node cannot be opened from the binding (setNextItemOpen does
// not reach inside showDemoWindow()), so the test does what the demo does
// with __test.setErrorOutputs(true), which sets all three and returns the
// values they had: what the last binding call left in force.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  CALL      in a frame, the outputs set on, then end() with only the
//            implicit window: ERR_IMGUI_USAGE (no abort), and the outputs
//            read back off after the call.
//  RENDER    begin('left open'), the outputs set on, render():
//            ERR_IMGUI_USAGE from the frame-end recovery, no abort; read
//            back off; the next frame draws.
//  DISCARD   newFrame(); begin('w'); the outputs set on; newFrame(): no
//            throw, the discarded frame's "Missing End()" recovered without
//            an abort; read back off; that frame then renders.
//  CLIPPER   the outputs set on from inside a listClipper callback: read
//            back off once listClipper() has returned (the clipper's own
//            Step() and End() after the callback run with them off too).
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  the re-assert removed from Call::enterImGui(): CALL aborts (Debug) or
//  fails on the read-back;
//  the re-assert removed from the discard path of newFrame(): DISCARD
//  aborts (Debug only: the read-back there is after NewFrame(), which
//  re-asserts anyway);
//  the re-assert removed from CallbackScope's destructor: CLIPPER fails on
//  the read-back.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
//
// CODE: EXITCODE 0
//
// CHECK: CALL ok
// CHECK-NEXT: RENDER ok
// CHECK-NEXT: DISCARD ok
// CHECK-NEXT: CLIPPER ok
// CHECK-NEXT: PASS
//
// TRACE: [ui] imgui: context created
// TRACE: [ui] imgui: usage error: Calling End() too many times!
// TRACE: [ui] imgui: usage error: Missing End()
// TRACE: [ui] imgui: usage error: Missing End()
// TRACE-NEXT: [ui] imgui: frame discarded

'use strict';

const harness = require('./fixtures/imgui/harness.js');

function fail(line) {
  console.log(line);
  process.exit(1);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const usage = (fn) => harness.expectThrow(fn, 'ERR_IMGUI_USAGE');

  /** Sets the outputs on; returns what was in force, as 'a,l,t'. */
  const turnOn = () => imgui.__test.setErrorOutputs(true).join(',');
  /** What the last binding call left, read back (and left off). */
  const leftInForce = () => imgui.__test.setErrorOutputs(false).join(',');
  const OFF = 'false,false,false';

  // --- CALL ---------------------------------------------------------------
  {
    let after = '';
    const drew = await harness.frame(h, () => {
      turnOn();
      usage(() => imgui.end());
      after = leftInForce();
    });
    if (after !== OFF || drew !== true)
      fail('CALL WRONG after=' + after + ' drew=' + drew);
    console.log('CALL ok');
  }

  // --- RENDER -------------------------------------------------------------
  {
    await harness.resync(h);
    let code = 'none';
    let after = '';
    await harness.rawTick(h, () => {
      imgui.newFrame();
      imgui.begin('left open');
      turnOn();
      try {
        imgui.render();
      } catch (e) {
        code = e.code;
      }
      after = leftInForce();
      return false;
    });
    await harness.resync(h);
    const next = await harness.frame(h, () => imgui.text('next'));
    if (code !== 'ERR_IMGUI_USAGE' || after !== OFF || next !== true)
      fail('RENDER WRONG code=' + code + ' after=' + after + ' next=' + next);
    console.log('RENDER ok');
  }

  // --- DISCARD ------------------------------------------------------------
  {
    let after = '';
    const [drew] = await harness.tick(h, [() => {
      imgui.newFrame();
      imgui.begin('w');
      turnOn();
      imgui.newFrame();
      after = leftInForce();
      imgui.text('after the discard');
      return imgui.render();
    }]);
    if (after !== OFF || drew !== true)
      fail('DISCARD WRONG after=' + after + ' drew=' + drew);
    console.log('DISCARD ok');
  }

  // --- CLIPPER ------------------------------------------------------------
  {
    let inside = '';
    let after = '';
    const drew = await harness.frame(h, () => {
      imgui.begin('clipped');
      imgui.listClipper(1, 20, () => {
        inside = turnOn();
      });
      after = leftInForce();
      imgui.end();
    });
    // `inside`: what the clipper's own Begin() and Step() ran with.
    if (inside !== OFF || after !== OFF || drew !== true)
      fail('CLIPPER WRONG inside=' + inside + ' after=' + after + ' drew=' +
          drew);
    console.log('CLIPPER ok');
  }

  console.log('PASS');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack || e));
  process.exit(1);
});
