// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `require('hermes-node:ui')` works in every build of hermes-node-ui.
//
// It did not. main.cpp once registered the module under a compile
// definition that was set only where a GPU backend had been built, and
// macOS had none then while CI built it with the option on. A script there
// got `Cannot find module 'hermes-node:ui'` with
// `code = 'MODULE_NOT_FOUND'`: the error a misspelled module name produces,
// for the one feature the binary exists to provide. The backend-less build
// is gone (a platform with no backend is a configure error now), and the
// registration is unconditional.
//
// Gated on `ui` alone: nothing here needs a backend's artifacts, a window
// or a display.
//
// THE MUTATION THIS MUST BREAK: delete the registerEmbedderModule() call in
// tools/hermes-node-ui/main.cpp. The require throws MODULE_NOT_FOUND, so
// REQUIRED prints the code instead of `ok` and CREATEWINDOW prints
// `object`/`undefined` rather than `function`.
//
// REQUIRES: ui
// RUN: %hermes-node-ui %s > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out

'use strict';

// Bounded from inside the script and not by a `timeout` command on the RUN
// line: this test runs on macOS, where older releases ship none. The timer
// is unref'd, so it never keeps the run alive by itself; it fires only if
// something else holds the loop open, and then fails the run. That is all
// it bounds: it cannot fire while the JS thread is blocked, or once the
// script has finished and native code hangs. lit's missing per-test timeout
// (dz 01a0d797-e326) is the gap there.
setTimeout(() => {
  console.log('WATCHDOG: the run did not end within 60 s');
  process.exit(124);
}, 60000).unref();

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
