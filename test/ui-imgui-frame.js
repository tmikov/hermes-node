// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The ImGui frame protocol: `newFrame()` and `render()`, and a target the
// renderer only clears (drawing arrives with the texture protocol).
//
// Cases, in the order they run, one output line each:
//
//  NOWINDOW   Before createWindow: newFrame() returns false, and render()
//             throws the no-frame error, nothing crashing (Review Focus 1).
//  MISUSE     render() and showDemoWindow() with no frame open, inside a
//             frame callback, throw ERR_IMGUI_NO_FRAME naming the call.
//  OUTSIDE    newFrame() then render() outside any frame callback: there is
//             no target, so render() ends the frame and returns false.
//  CLEARED    One empty frame, captured: five samples equal the clear
//             colour exactly.
//  TWICE      newFrame(); newFrame(); render() in one callback: the second
//             newFrame() discards the first frame (traced), no throw.
//  CAUGHT /   A throw between newFrame() and render(), with an
//  RECOVERED  uncaughtException listener installed and the next frame
//             already registered: the throwing tick took no target, so the
//             host traces it drew nothing, and the next frame discards the
//             open one and draws.
//  DISPLAY    state() after a frame agrees with the window: display size,
//             framebuffer scale, and the size and generation of the target
//             render() drew into.
//  EXIT       process.exit(7) from inside a frame callback, between
//             newFrame() and render(); an 'exit' listener's paired
//             newFrame()/render() does not throw and render() returns false
//             (CLOSING has no target), and the status is preserved.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  (a) take the target in newFrame() instead of render(): the throwing
//      tick draws, and the TRACE run's `nothing drawn` line is missing;
//  (b) clear to black: CLEARED fails;
//  (c) no EndFrame() on a second newFrame(): ImGui asserts, or TWICE fails;
//  (d) render() throws when there is no target: OUTSIDE and EXIT fail.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// The RUN-line shape is ui-present.js's, for the reasons given there.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=ONCE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePath = process.argv[2];

/** The error `fn` throws, or `{code: 'none', message: 'none'}`. */
function errorOf(fn) {
  try {
    fn();
  } catch (e) {
    return e;
  }
  return {code: 'none', message: 'none'};
}

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

async function main() {
  // --- NOWINDOW --------------------------------------------------------
  //
  // The addon loads before any window exists; hnui_get_window(env) is NULL.
  {
    const imgui = require('hermes-node:imgui');
    const nf = imgui.newFrame();
    console.log('NOWINDOW newFrame=' + nf);
    console.log('NOWINDOW render=' + errorOf(() => imgui.render()).code);
  }

  const h = await harness.open();
  const {ui, win, imgui} = h;

  // --- MISUSE ----------------------------------------------------------
  await harness.tick(h, [() => {
    console.log(
        'MISUSE render=' + errorOf(() => imgui.render()).code +
        ' showDemoWindow=' + errorOf(() => imgui.showDemoWindow()).code);
    console.log(
        'MISUSE message=' + errorOf(() => imgui.showDemoWindow()).message);
    return false;
  }]);

  // --- OUTSIDE ---------------------------------------------------------
  imgui.newFrame();
  console.log('OUTSIDE render=' + imgui.render());

  // --- CLEARED ---------------------------------------------------------
  {
    const image = await harness.capture(h, () => {}, capturePath);
    const m = 8;
    const points = [
      [m, m],
      [image.width - 1 - m, m],
      [m, image.height - 1 - m],
      [image.width - 1 - m, image.height - 1 - m],
      [image.width >> 1, image.height >> 1],
    ];
    const wrong = points.filter(
        ([x, y]) => !sameColor(harness.pixelAt(image, x, y), harness.CLEAR));
    console.log(
        'CLEARED ' +
        (wrong.length === 0 ?
             'ok' :
             'WRONG ' +
                 wrong
                     .map(([x, y]) =>
                         x + ',' + y + '=' + harness.pixelAt(image, x, y))
                     .join(' ')));
  }

  // --- TWICE -----------------------------------------------------------
  {
    const [drew] = await harness.tick(h, [() => {
      imgui.newFrame();
      imgui.newFrame();
      return imgui.render();
    }]);
    console.log('TWICE render=' + drew);
  }

  // --- CAUGHT / RECOVERED ----------------------------------------------
  //
  // Not through harness.tick: the throwing callback never returns, and the
  // recovering one has to be registered from inside it, before the throw,
  // so that it is the next tick's.
  await new Promise((resolve) => {
    process.once('uncaughtException', (e) => {
      console.log('CAUGHT ' + e.message);
    });
    ui.requestAnimationFrame(() => {
      imgui.newFrame();
      ui.requestAnimationFrame(() => {
        imgui.newFrame();
        const drew = imgui.render();
        if (drew)
          ++h.published; // a tick that drew, as harness.tick counts them
        console.log('RECOVERED render=' + drew);
        setImmediate(resolve); // after the tick, as harness.tick resolves
      });
      throw new Error('thrown between newFrame and render');
    });
  });

  // --- DISPLAY ---------------------------------------------------------
  {
    await harness.frame(h, () => {});
    const s = imgui.__test.state();
    const ok = s.frameOpen === false && s.displayWidth === 640 &&
        s.displayWidth === win.width && s.displayHeight === win.height &&
        s.scaleX === win.framebufferWidth / win.width &&
        s.scaleY === win.framebufferHeight / win.height &&
        s.targetWidth === win.framebufferWidth &&
        s.targetHeight === win.framebufferHeight &&
        s.targetGeneration === h.generation;
    console.log(
        'DISPLAY ' +
        (ok ? 'ok' :
              'WRONG state=' + JSON.stringify(s) + ' win=' + win.width + 'x' +
                  win.height + ' fb=' + win.framebufferWidth + 'x' +
                  win.framebufferHeight));
  }

  // --- EXIT ------------------------------------------------------------
  process.on('exit', () => {
    let nf;
    let drew;
    try {
      nf = imgui.newFrame();
      drew = imgui.render();
    } catch (e) {
      console.log('EXIT threw ' + e.message);
      return;
    }
    console.log('EXIT newFrame=' + nf + ' render=' + drew);
  });
  ui.requestAnimationFrame(() => {
    imgui.newFrame();
    process.exit(7);
  });
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}NOWINDOW newFrame=false{{$}}
// CHECK-NEXT: {{^}}NOWINDOW render=ERR_IMGUI_NO_FRAME{{$}}
// CHECK-NEXT: {{^}}MISUSE render=ERR_IMGUI_NO_FRAME showDemoWindow=ERR_IMGUI_NO_FRAME{{$}}
// CHECK-NEXT: {{^}}MISUSE message=imgui.showDemoWindow(): no frame is open; call imgui.newFrame() first{{$}}
// CHECK-NEXT: {{^}}OUTSIDE render=false{{$}}
// CHECK-NEXT: {{^}}CLEARED ok{{$}}
// CHECK-NEXT: {{^}}TWICE render=true{{$}}
// CHECK-NEXT: {{^}}CAUGHT thrown between newFrame and render{{$}}
// CHECK-NEXT: {{^}}RECOVERED render=true{{$}}
// CHECK-NEXT: {{^}}DISPLAY ok{{$}}
// CHECK-NEXT: {{^}}EXIT newFrame=true render=false{{$}}
// CODE: EXITCODE 7

// The first discard is TWICE's (nothing before it opens two frames), and its
// tick drew. The throwing tick comes next: one callback ran and it took no
// target, so nothing was drawn. Then the recovering frame discards the frame
// the throw left open, and draws.
// TRACE: {{^}}[ui] imgui: frame discarded{{$}}
// TRACE-NEXT: {{^}}[ui] frame tick: 1 callback(s) ran, drew{{$}}
// TRACE: {{^}}[ui] frame tick: 1 callback(s) ran, nothing drawn{{$}}
// TRACE-NEXT: {{^}}[ui] imgui: frame discarded{{$}}
// TRACE-NEXT: {{^}}[ui] frame tick: 1 callback(s) ran, drew{{$}}

// ONCE: {{^}}[ui] imgui: context created (imgui 1.93.0, docking=1){{$}}
// ONCE-NOT: imgui: context created
