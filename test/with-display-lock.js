// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// utils/with-display.py's macOS display lock is MACHINE-wide: two window
// tests never have windows up at once, whichever lit invocation (or hand
// run) started them. Sokol centres every window, so two of the same size
// cover each other exactly; the covered one's swapchain goes invalid and
// its capture never matches.
//
// The lock used to live under $TMPDIR, and lit sets TMPDIR to a fresh
// lit_tmp_* directory per invocation (hermes/external/llvh/utils/lit/lit/
// main.py), so check-hermes-node and a concurrent utils/lit-run.sh took
// different locks. So the two runs here get DIFFERENT TMPDIRs and must
// still serialise. A third run whose limit is shorter than the wait must
// give up at its limit with 124: waiting for the display counts against
// the wrapper's limit, it is not added to it.
//
// The darwin branch is forced (test/fixtures/with-display/run_as_darwin.py),
// so this runs everywhere; it uses only flock and process groups. The lock
// path is pointed into %t through HERMES_NODE_UI_DISPLAY_LOCK, so a real
// Mac's lock is never touched.
//
// (Mutation: derive the lock path from TMPDIR again -- OVERLAP. Take the
// lock without a deadline -- C waits for A and runs: ran=1, status 0.)
//
// RUN: rm -rf %t.d && mkdir -p %t.d
// RUN: %python %S/fixtures/with-display/lock_check.py %source_dir/utils/with-display.py %t.d/display.lock %t.d | %FileCheck %s

// CHECK: {{^}}serialised{{$}}
// CHECK-NEXT: {{^}}A status=0{{$}}
// CHECK-NEXT: {{^}}B status=0{{$}}
// CHECK-NEXT: {{^}}C status=124 ran=0 bounded=1{{$}}
// CHECK-NEXT: C said: with-display.py: timed out after 1 s waiting for the display lock
