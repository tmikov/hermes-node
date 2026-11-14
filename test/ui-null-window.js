// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A NULL window handle is an ordinary input to the checked hnui_* entry
// points, and answering "no" is the whole of the response.
//
// hnui.h documents NULL as `hnui_get_window()`'s ordinary answer for a
// runtime that never called createWindow -- "an addon that can render
// headless may carry on" -- so an addon that carries on is entitled to hand
// that NULL back in. It used to abort the process: requireOwnerThread()
// folded `w == nullptr` into abortWrongThread(), which prints "was called
// from a thread that does not own the window", resets the tty and aborts.
// The documented headless path killed the program, and the diagnostic
// blamed the caller's thread for a window that was simply absent.
//
// Nothing reached it because demo_addon.c guards every call site with
// `w != NULL`. So the reference addon now has exactly one call that does
// not -- probeNullWindow -- and this is the test that runs it.
//
// **No display, and that is the point of running it here.** The window is
// never created, so this adds real coverage of the hnui surface to the set
// of tests that survive a machine with no Xvfb (see ui-display-feature.js
// for why that set matters). `ui-gpu` is still required: without a GPU
// backend there is no hnui_api.cpp compiled into the binary and no
// reference addon built to call it.
//
// THE MUTATION THIS MUST BREAK: restore `w == nullptr ||` to
// requireOwnerThread() in tools/hermes-node-ui/hnui_api.cpp. The process
// aborts inside probeNullWindow, so EXITCODE goes nonzero and neither
// NULLPROBE nor ALIVE is printed -- three checks red at once.
//
// `timeout` for the reason every test in this family has one: lit
// configures no per-test timeout (dz 01a0d797-e326), and a mutation whose
// failure mode is a hang would stall a worker slot rather than fail.
//
// REQUIRES: ui, ui-gpu
// RUN: timeout 60 %hermes-node-ui %s %hnui_demo_addon > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out

'use strict';

// process.argv is [binary, script, ...rest] -- the same shape as every
// other test in this family, so the addon path is argv[2].
const addon = require(process.argv[2]);

// The premise: this runtime owns no window, so hnui_get_window() answers
// NULL. Asserted rather than assumed, because every line below is about
// what happens to that NULL, and a build where the addon somehow DID find
// a window would make the rest of this file pass without testing anything.
console.log('HASWINDOW ' + addon.hasWindow());

// hnui_frame_target(NULL, ...) answers false; hnui_request_animation_frame
// (NULL, ...) answers 0, the id these entry points already use for "no".
console.log('NULLPROBE ' + addon.probeNullWindow());

// Reached only if neither call aborted.
console.log('ALIVE');

// OUT: HASWINDOW false
// OUT-NEXT: NULLPROBE target=false raf=0
// OUT-NEXT: ALIVE
// CODE: EXITCODE 0
