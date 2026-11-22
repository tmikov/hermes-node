// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// An hnui_* entry point called from a thread that does not own the window
// aborts, naming the function (spec: Errors; hnui.h, "Threading"). Every
// checked entry point shares requireOwnerThread() in hnui_api.cpp; this
// calls one of them, hnui_get_window_state, from a pthread the reference
// addon spawns.
//
// If the check is missing the call returns, the script prints SURVIVED and
// exits 0, and `%not` turns that into a failure. (Mutation: remove
// requireOwnerThread from hnui_get_window_state.)
//
// `%not` sees an ordinary non-zero exit, not a crash: the abort happens
// below the shell and the display wrapper, which report it as 128+SIGABRT.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN: %not %with-display 120 sh -c '%hermes-node-ui %s %hnui_demo_addon > %t.out 2> %t.err'
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
// RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err

// OUT: CALLING
// OUT-NOT: SURVIVED
// ERR: hermes-node-ui: hnui_get_window_state was called from a thread that does not own the window.

'use strict';

const ui = require('hermes-node:ui');

async function main() {
  await ui.createWindow(
      {width: 200, height: 150, title: 'hermes-node-ui wrong thread test'});
  const addon = require(process.argv[2]);
  console.log('CALLING');
  addon.callStateOffThread();
  console.log('SURVIVED');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
