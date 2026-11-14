// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The reference addon's entry script, and **a smoke test a person can
// run** (spec: The reference addon, job 1):
//
//   hermes-node-ui test/fixtures/hnui-demo/demo.js <addon path> [mode]
//
// It shows a window with the addon's fixed pattern: a blue surface with a
// yellow top-left quadrant. `mode` is `js` (the default) or `native`.
//
// **Two modes, because there are two ways to drive a frame and both need
// a consumer.** `js` runs an ordinary JavaScript requestAnimationFrame
// loop that calls into the addon -- the common shape, so it is the one
// demonstrated by default. `native` hands the loop to the addon itself
// through hnui_request_animation_frame, which is the path an ImGui-style
// renderer would use and which nothing else in this tree exercises. Lit
// runs both (test/ui-present.js).
//
// This file is deliberately NOT where the capture test lives. It is the
// demo; test/ui-present.js loads the same addon and drives the capture
// hook itself, so that what a person runs by hand stays free of test
// scaffolding.

'use strict';

const ui = require('hermes-node:ui');

const addonPath = process.argv[2];
const mode = process.argv[3] || 'js';

if (!addonPath) {
  console.log(
      'usage: hermes-node-ui demo.js <path to hnui_demo_addon.node> ' +
      '[js|native]');
  process.exit(2);
}
if (mode !== 'js' && mode !== 'native') {
  console.log('unknown mode: ' + mode + ' (expected js or native)');
  process.exit(2);
}

async function main() {
  const win = await ui.createWindow({
    width: 640,
    height: 480,
    title: 'hermes-node-ui demo',
  });
  console.log(
      'WINDOW ' + win.framebufferWidth + 'x' + win.framebufferHeight);

  // Loaded AFTER the window exists, deliberately: hnui_get_window(env)
  // answers null until then, and an addon that cached that null at load
  // time would never find the window. Saying so here is cheaper than
  // every addon discovering it.
  const addon = require(addonPath);
  console.log('ADDON hasWindow=' + addon.hasWindow());

  if (mode === 'native') {
    // The addon re-registers itself each tick, so one call starts the
    // loop and JavaScript is not involved in any frame after this one.
    addon.startNativeFrames();
    console.log('MODE native');
  } else {
    const frame = () => {
      addon.drawFixedPattern();
      ui.requestAnimationFrame(frame);
    };
    ui.requestAnimationFrame(frame);
    console.log('MODE js');
  }
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.message));
  process.exit(1);
});
