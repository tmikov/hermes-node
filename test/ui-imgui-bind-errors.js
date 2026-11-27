// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The binding's error model: usage errors ImGui reports and recovers from
// become ERR_IMGUI_USAGE, from the call or from render(); the closers ImGui
// does not check recoverably are refused by the binding's guards; and a
// throwing render() contributes nothing to its tick.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  UNMATCHED_END  end() with only the implicit window: ERR_IMGUI_USAGE from
//                 the call; the frame continues and render() returns true.
//  LEFT_OPEN      begin('w') with no end(): render() throws ERR_IMGUI_USAGE,
//                 the frame is closed, nothing was published (h.published
//                 re-read from the host is unchanged), and the next frame
//                 draws.
//  PUSHID_LEFT    pushID('a') unpopped inside a window: that window's end()
//                 throws.
//  MIXED_TICK     one tick, two callbacks: a complete frame filling the
//                 window red, then a frame whose render() throws. The
//                 tick's picture is the first callback's, red everywhere,
//                 the second callback's window included.
//  NO_TARGET      a frame left open, rendered outside any frame callback:
//                 ERR_IMGUI_USAGE rather than the `false` return.
//  GUARD_GROUP    endGroup() with no group: refused, not called. A group
//                 ImGui closes itself inside end() (which throws), then
//                 endGroup(): refused, not an abort. A group begun in the
//                 implicit window, then endGroup() inside another window:
//                 refused as the wrong window.
//  GUARD_CHILD    endChild() in a top-level window: refused. A child ImGui
//                 clips (placed far below its scrolling parent's visible
//                 area), whose beginChild() returned false (`SETUP child
//                 clipped`, read back), then endChild(): accepted.
//  DISCARD        newFrame(); begin('w'); newFrame(): no throw, `frame
//                 discarded` traced, the frame that follows renders.
//  GUARDS         every guard condition registered has refused at least
//                 once (harness.checkGuardCoverage).
//  CLOSING        (a run of its own) from an 'exit' listener, newFrame();
//                 begin('w'); render(): ERR_IMGUI_USAGE; the exit status of
//                 process.exit(5) is preserved.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  configureContext() not called from ensureContext: ImGui asserts on the
//  first usage error (abort), or the cases fail;
//  the usage-error check moved after hnui_frame_target in render():
//  LEFT_OPEN's publication check fails;
//  canEndGroup() returning nullptr always: GUARD_GROUP aborts;
//  canEndChild() requiring a true beginChild: GUARD_CHILD's accepted half
//  fails;
//  a guard condition registered and never refused: GUARDS fails
//  (`UNREFUSED <name>`);
//  every usage error traced twice (throwUsage() tracing too): ONCE fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s main %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
// RUN: sh -c 'echo BEGIN; grep -E "imgui: (usage error|frame discarded)" %t.err; echo END' | %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=ONCE %s
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s closing > %t.closing.out 2> %t.closing.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CLOSINGCODE %s
// RUN: %FileCheck --check-prefix=CLOSING %s < %t.closing.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=CLOSINGTRACE %s < %t.closing.err
// RUN: sh -c 'echo BEGIN; grep -E "imgui: (usage error|frame discarded)" %t.closing.err; echo END' | %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=CLOSINGONCE %s

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const mode = process.argv[2];
const capturePath = process.argv[3];

function fail(line) {
  console.log(line);
  process.exit(1);
}

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

async function closing() {
  const h = await harness.open();
  const {ui, imgui} = h;
  await harness.frame(h, () => {});
  process.on('exit', () => {
    imgui.newFrame();
    imgui.begin('w');
    try {
      imgui.render();
      console.log('CLOSING no throw');
    } catch (e) {
      console.log('CLOSING ' + e.code);
    }
  });
  ui.requestAnimationFrame(() => process.exit(5));
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);
  const usage = (fn) => harness.expectThrow(fn, 'ERR_IMGUI_USAGE');

  /** `fn` must be refused with ERR_IMGUI_USAGE, `name` not called. */
  function refused(name, fn) {
    const before = calls(name);
    const e = usage(fn);
    if (calls(name) !== before)
      fail('CALLED ' + name + ': ' + e.message);
    return e;
  }

  // --- UNMATCHED_END ---------------------------------------------------
  {
    let message;
    const drew = await harness.frame(h, () => {
      message = usage(() => imgui.end()).message;
      imgui.text('after');
    });
    console.log(drew ? 'UNMATCHED_END ok' : 'UNMATCHED_END render=' + drew);
    console.log('UNMATCHED_END message ' + message);
  }

  // --- LEFT_OPEN -------------------------------------------------------
  {
    await harness.resync(h);
    const before = h.published;
    let code = 'none';
    await harness.rawTick(h, () => {
      imgui.newFrame();
      imgui.begin('left open');
      try {
        imgui.render();
      } catch (e) {
        code = e.code;
      }
      return false;
    });
    const frameOpen = imgui.__test.state().frameOpen;
    await harness.resync(h);
    const after = h.published;
    const next = await harness.frame(h, () => imgui.text('next'));
    const ok = code === 'ERR_IMGUI_USAGE' && !frameOpen && after === before &&
        next === true;
    console.log(
        ok ? 'LEFT_OPEN ok' :
             'LEFT_OPEN WRONG code=' + code + ' frameOpen=' + frameOpen +
                 ' published=' + before + '->' + after + ' next=' + next);
  }

  // --- PUSHID_LEFT -----------------------------------------------------
  {
    let message;
    const drew = await harness.frame(h, () => {
      imgui.begin('pushid');
      imgui.pushID('a');
      message = usage(() => imgui.end()).message;
    });
    console.log(drew ? 'PUSHID_LEFT ok' : 'PUSHID_LEFT render=' + drew);
    console.log('PUSHID_LEFT message ' + message);
  }

  // --- MIXED_TICK ------------------------------------------------------
  {
    let code = 'none';
    const image = await harness.captureTick(h, [
      () => {
        imgui.newFrame();
        imgui.__test.scene('fill');
        return imgui.render();
      },
      () => {
        imgui.newFrame();
        imgui.setNextWindowPos([100, 100]);
        imgui.setNextWindowSize([200, 150]);
        imgui.begin('mixed');
        try {
          imgui.render();
        } catch (e) {
          code = e.code;
        }
        return false;
      },
    ], capturePath);
    // Inside the second callback's window, and elsewhere.
    const points = [[150, 150], [250, 200], [20, 20], [600, 440]];
    const wrong = points.filter(
        ([x, y]) => !sameColor(harness.pixelAt(image, x, y), [255, 0, 0]));
    const ok = code === 'ERR_IMGUI_USAGE' && wrong.length === 0;
    console.log(
        ok ? 'MIXED_TICK ok' :
             'MIXED_TICK WRONG code=' + code + ' ' +
                 wrong
                     .map(([x, y]) =>
                         x + ',' + y + '=' + harness.pixelAt(image, x, y))
                     .join(' '));
  }

  // --- NO_TARGET -------------------------------------------------------
  {
    imgui.newFrame();
    imgui.begin('no target');
    usage(() => imgui.render());
    const frameOpen = imgui.__test.state().frameOpen;
    console.log(frameOpen ? 'NO_TARGET frame still open' : 'NO_TARGET ok');
  }

  // --- GUARD_GROUP -----------------------------------------------------
  {
    let recovered = 'none';
    const drew = await harness.frame(h, () => {
      refused('endGroup', () => imgui.endGroup());
      imgui.begin('group');
      imgui.beginGroup();
      // ImGui closes the group itself, inside End(), and reports it.
      recovered = usage(() => imgui.end()).message;
      refused('endGroup', () => imgui.endGroup());
      // A group of the implicit window, closed from inside another one.
      imgui.beginGroup();
      imgui.begin('group other');
      refused('endGroup', () => imgui.endGroup());
      imgui.end();
      imgui.endGroup();
    });
    console.log(drew ? 'GUARD_GROUP ok' : 'GUARD_GROUP render=' + drew);
    console.log('GUARD_GROUP recovered ' + recovered);
  }

  // --- GUARD_CHILD -----------------------------------------------------
  {
    await harness.frame(h, () => {
      imgui.begin('child top');
      refused('endChild', () => imgui.endChild());
      imgui.end();
    });
    // A parent of 300x200 whose content reaches y 2000, so it scrolls; the
    // child sits there, entirely below the visible area. A new child is
    // not clipped on its first frames (it is still being sized), so frames
    // run until beginChild() answers false, at most a few.
    let clipped = false;
    let accepted = false;
    for (let i = 0; i < 10 && !clipped; ++i) {
      await harness.frame(h, () => {
        imgui.setNextWindowPos([40, 40]);
        imgui.setNextWindowSize([300, 200]);
        imgui.begin('child parent');
        imgui.setCursorScreenPos([60, 2000]);
        const shown = imgui.beginChild('clipped', [100, 100]);
        const before = calls('endChild');
        let threw = 'none';
        try {
          imgui.endChild();
        } catch (e) {
          threw = e.code;
        }
        // Counted only in the frame whose beginChild() returned false: the
        // case is about the clipped child.
        if (shown === false && threw === 'none' &&
            calls('endChild') === before + 1)
          accepted = true;
        if (threw === 'none') {
          imgui.end();
        } else {
          // Wrongly refused, the child is still current: two end() calls
          // close it and the parent, each reporting a usage error, so that
          // the frame stays balanced and the case reports instead of
          // throwing.
          for (let k = 0; k < 2; ++k) {
            try {
              imgui.end();
            } catch (e) {
            }
          }
        }
        clipped = shown === false;
      });
    }
    if (!clipped)
      fail('SETUP child not clipped');
    console.log('SETUP child clipped');
    console.log(accepted ? 'GUARD_CHILD ok' : 'GUARD_CHILD not accepted');
  }

  // --- DISCARD ---------------------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      imgui.begin('discarded');
      imgui.newFrame();
    });
    console.log(drew ? 'DISCARD ok' : 'DISCARD render=' + drew);
  }

  harness.checkGuardCoverage(h);
  process.exit(0);
}

(mode === 'closing' ? closing() : main()).catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}UNMATCHED_END ok{{$}}
// CHECK-NEXT: {{^}}UNMATCHED_END message imgui.end(): Calling End() too many times!{{$}}
// CHECK-NEXT: {{^}}LEFT_OPEN ok{{$}}
// CHECK-NEXT: {{^}}PUSHID_LEFT ok{{$}}
// CHECK-NEXT: {{^}}PUSHID_LEFT message imgui.end(): Missing PopID(){{$}}
// CHECK-NEXT: {{^}}MIXED_TICK ok{{$}}
// CHECK-NEXT: {{^}}NO_TARGET ok{{$}}
// CHECK-NEXT: {{^}}GUARD_GROUP ok{{$}}
// CHECK-NEXT: {{^}}GUARD_GROUP recovered imgui.end(): Missing EndGroup(){{$}}
// CHECK-NEXT: {{^}}SETUP child clipped{{$}}
// CHECK-NEXT: {{^}}GUARD_CHILD ok{{$}}
// CHECK-NEXT: {{^}}DISCARD ok{{$}}
// CHECK-NEXT: {{^}}GUARDS ok{{$}}
// CODE: EXITCODE 0

// Every usage error is traced exactly once, ImGui's own and the guards'
// alike, in the order the cases raise them. TRACE reads the whole stderr
// (for the Sokol exclusions); ONCE reads only the usage-error and discard
// lines, between a BEGIN and an END line, as one -NEXT chain, so a line
// traced twice, anywhere, breaks the chain.
// TRACE: {{^}}[ui] imgui: usage error: Calling End() too many times!{{$}}
// TRACE: {{^}}[ui] imgui: frame discarded{{$}}
// ONCE: {{^}}BEGIN{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Calling End() too many times!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing PopID(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndGroup() with no group open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing EndGroup(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndGroup() with no group open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndGroup() in another window than its BeginGroup(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndChild() outside a child window{{$}}
// The discarded frame's open window is reported, then the discard.
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: frame discarded{{$}}
// ONCE-NEXT: {{^}}END{{$}}

// CLOSING: {{^}}CLOSING ERR_IMGUI_USAGE{{$}}
// CLOSINGCODE: EXITCODE 5
// CLOSINGTRACE: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// CLOSINGONCE: {{^}}BEGIN{{$}}
// CLOSINGONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// CLOSINGONCE-NEXT: {{^}}END{{$}}
