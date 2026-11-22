// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A program that requires `hermes-node:ui` for the first time from an
// 'exit' listener finds the window CLOSING, as one that required it earlier
// does: its createWindow() throws `the process is exiting` instead of
// opening a window into a process on its way out (spec: Lifecycle).
//
// The case exists because the process exit hooks act only for the runtime
// that instantiated the module (uiBeforeExitListeners in ui_module.cpp), and
// when the runtime reaches step 15 here no runtime has. The hook remembers
// the env instead, and initUiModule() records CLOSING when it is
// instantiated by a runtime the hook already saw exit. (Mutation: drop the
// gLifecycle store in initUiModule, and createWindow() requests a window;
// with no display, Sokol's XOpenDisplay panic ends the process before
// either line or the status 3 comes out.)
//
// No window is opened, so no display is needed; `ui-gpu` because the
// backend-less build's createWindow refuses for a different reason. The
// status is the program's own, 3.
//
// REQUIRES: ui, ui-gpu
// RUN: timeout 60 env HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out

'use strict';

process.exitCode = 3;
process.on('exit', () => {
  const ui = require('hermes-node:ui');
  try {
    ui.createWindow({width: 320, height: 240, title: 'late require'});
    console.log('CREATE returned');
  } catch (e) {
    console.log('CREATE threw: ' + e.message);
  }
});

// CHECK: CREATE threw: createWindow: the process is exiting.
// CODE: EXITCODE 3
