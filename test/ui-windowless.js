// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// hermes-node-ui's Task 6 gate: a script that never asks for a window runs
// to completion on the JS thread while the main thread's condition-variable
// wait is unblocked by the OTHER half of its predicate -- "the runtime
// finished" -- and the process exits with the script's own exit code.
//
// If UiHost's wait predicate only checked for a requested window (dropping
// the "OR the runtime finished" half described in the Task 6 brief), this
// script would never reach either CHECK below: the main thread would block
// in uv_cond_wait forever, since nothing in this file, or anywhere else in
// this build, can ever call requestWindow(). That hang is exactly what a
// timing-based lit assertion cannot safely pin (the suite runs 16-way
// parallel, and lit has no configured per-test timeout here), so this test
// instead pins the two observable ends of a run that completed: stdout,
// and the process's own exit status. A predicate that hangs forever fails
// this test not by printing something wrong, but by making the RUN line
// itself never finish -- see the task report for how that was verified by
// hand (a mutated build run directly under `timeout`, outside the lit
// suite, since a hang inside the suite itself has nothing to bound it).

// REQUIRES: ui
// RUN: %hermes-node-ui %s > %t.out; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
//
// The same run with tracing on, both streams kept: a script that never asks
// for a window must still end by returning through UiHost::run(), not
// through the pre-cleanup hook's _exit(), and must print no lifecycle line
// at all. (Mutation: let uiBeforeRuntimeCleanup proceed with no window
// requested, and `lifecycle: runtime returned` is printed.)
//
// RUN: env HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.dbg 2>&1; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT --implicit-check-not='lifecycle:' %s < %t.dbg
//
// And a script that LOADS the module but never calls createWindow: requiring
// `hermes-node:ui` must not by itself make the process a windowed one. Same
// two checks, so the same ending: its own status, through the ordinary
// cleanup, and no lifecycle line. Portable on purpose -- it runs on macOS
// too -- so no `timeout` and nothing but POSIX `env` and `echo`. (Mutation: in uiEnterClosing, move
// the CLOSING trace above the `!uiWindowRequested()` return, and
// `lifecycle: CLOSING (reason=return)` is printed -- by the run above too:
// nothing in the code is specific to a loaded module today, and this case
// is here so that something that becomes so is covered.)
//
// RUN: env HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s load > %t.load.dbg 2>&1; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=LOAD --implicit-check-not='lifecycle:' %s < %t.load.dbg

if (process.argv[2] === 'load') {
  const ui = require('hermes-node:ui');
  console.log('LOADED ' + typeof ui.createWindow);
}
console.log('PASS');
process.exitCode = 7;

// OUT: PASS
// CODE: EXITCODE 7
// LOAD: LOADED function
// LOAD-NEXT: PASS
