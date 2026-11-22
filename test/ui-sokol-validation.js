// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The VALIDATION case of test/ui-sokol-panic.js (whose header says what is
// asserted, and the mutation): a pass an addon begins with the target's
// colour and resolve views swapped, on the drawing thread, terminates the
// process with Sokol's validation messages on stderr. A file of its own
// only because lit gates whole files, and this case needs a sokol_gfx
// compiled with its validation layer -- `sokol-validation`, which lit.cfg
// decides by running the binary. Off in a Release build, where nothing
// validates the pass and its behaviour is the driver's.
//
// REQUIRES: ui, ui-gpu, sokol-validation
// REQUIRES: display
//
// RUN: %with-display 120 sh -c '%hermes-node-ui %source_dir/test/ui-sokol-panic.js %hnui_demo_addon validation > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
// RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-present' %s < %t.err
//
// This run is one of the two exceptions to the suite-wide stderr rule
// (test/ui-stderr-rule.js). It provokes the PRODUCING instance into a real
// validation failure, so that instance's error and panic lines are asserted;
// a sokol_app error and anything from the presenting instance are still
// refused.

// CODE: EXITCODE 1

// OUT: REQUESTING
// OUT-NOT: PASS BEGAN
// OUT-NOT: FRAME AFTER
// OUT-NOT: EXIT listener

// ERR: [sokol-produce:error] item {{[0-9]+}}: VALIDATE_BEGINPASS_COLORATTACHMENTVIEW_TYPE: sg_begin_pass: color attachment view has wrong type
// ERR: [sokol-produce:panic] item {{[0-9]+}}: VALIDATION_FAILED: validation layer checks failed
// ERR-NEXT: hermes-node-ui: the producing sokol_gfx instance -- the one addons draw with -- panicked.
