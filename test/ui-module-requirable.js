// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `require('hermes-node:ui')` works in EVERY build of hermes-node-ui.
//
// It did not. main.cpp registered the module under
// `#ifdef HERMES_NODE_UI_HAVE_WINDOW`, which tools/hermes-node-ui/
// CMakeLists.txt sets only `if(TARGET hermesNodeCompositorGpu)` -- absent on
// macOS until the Metal backend lands, and CI builds exactly that
// configuration. A script there got `Cannot find module 'hermes-node:ui'`
// with `code = 'MODULE_NOT_FOUND'`: the error a misspelled module name
// produces, for the one feature the binary exists to provide. The analogous
// case in UiHost::run() prints "hermes-node-ui has no window support yet",
// and the two paths must agree -- so the module is registered
// unconditionally now and its createWindow is what refuses
// (ui_module_nowindow.cpp; the refusal itself is asserted by
// ui-no-window-support.js, which is the only test that can run there).
//
// **Gated on `ui` and NOT on `ui-gpu`, which is the whole point.** This is
// the assertion that runs on the configuration where the bug lived, and it
// costs nothing on the one where it did not: a backend-less build is where
// `ui` is on and `ui-gpu` is off.
//
// THE MUTATION THIS MUST BREAK: delete the registerEmbedderModule() call in
// tools/hermes-node-ui/main.cpp -- which is what the `#ifdef` amounted to on
// a backend-less platform, reproduced here on one that has a backend. The
// require throws MODULE_NOT_FOUND, so REQUIRED prints the code instead of
// `ok` and CREATEWINDOW prints `object`/`undefined` rather than `function`.
//
// REQUIRES: ui
// RUN: timeout 60 %hermes-node-ui %s > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out

'use strict';

let mod = null;
let failure = null;
try {
  mod = require('hermes-node:ui');
} catch (e) {
  // The code, not the message: MODULE_NOT_FOUND is the exact symptom this
  // file exists to keep from coming back, and naming it here means a
  // regression reads as itself rather than as "something threw".
  failure = (e && e.code) || (e && e.message) || 'threw';
}

console.log('REQUIRED ' + (failure === null ? 'ok' : failure));
console.log('CREATEWINDOW ' + (mod === null ? 'none' : typeof mod.createWindow));

// Nothing here creates a window, so no referenced libuv handle is taken and
// the script exits on its own -- unlike ui-create-window.js and
// ui-present.js, which must call process.exit().

// OUT: REQUIRED ok
// OUT-NEXT: CREATEWINDOW function
// CODE: EXITCODE 0
