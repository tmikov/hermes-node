// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Dear ImGui's own demo window, drawn by the hermes-node:imgui addon in a
// hermes-node-ui window. Run it with hermes-node-ui (see README.md). It
// runs until the window is closed: an uncancelled close ends the process.

'use strict';

const ui = require('hermes-node:ui');
const imgui = require('hermes-node:imgui');

async function main() {
  await ui.createWindow(
      {title: 'ImGui', width: 1280, height: 800, highDpi: true});
  function frame() {
    imgui.newFrame();
    imgui.showDemoWindow();
    imgui.render();
    ui.requestAnimationFrame(frame);
  }
  ui.requestAnimationFrame(frame);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
