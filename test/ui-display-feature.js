// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// **The forcing function for the `display` lit feature**, which was the one
// feature in test/lit.cfg that had none.
//
// `ui` and `ui-enabled` are decided by RUNNING a binary (RULING 37),
// `ui-gpu` is a platform predicate deliberately not an os.path.exists, and
// test/ui-binary-exists.js exists solely so that a regression in the `ui`
// probe cannot quietly turn its neighbours UNSUPPORTED. `display` had
// nothing at all: drop `xvfb` from the apt line in .github/workflows/ci.yml
// and lit.cfg's probe returns False, ui-create-window.js and ui-present.js
// go UNSUPPORTED, and `check-hermes-node` exits 0 -- while those two are the
// ONLY tests in the suite that execute the GL backend, the surface protocol,
// presenting, the capture hook or the publication id. The suite would still
// look like it exercises the UI, because ui-route-refusal.js,
// sokol-rename.js and sokol-isolation.js all keep running.
//
// **Why the gate is what it is.** A plain canary -- "display must be on" --
// would fail on every developer machine without Xvfb installed, which is the
// cost that made `display` a probe rather than a requirement in the first
// place, and punishing a developer for a package they never agreed to
// install is not a trade worth making. So this asserts the feature only
// where its absence is a defect rather than a local choice: a machine with a
// GPU backend (`ui-gpu`, so the window tests are meaningful at all) that
// also PROMISED a display (`display-expected`: Linux CI, whose workflow
// installs xvfb, or any machine whose environment sets
// HERMES_NODE_UI_EXPECT_DISPLAY=1 -- every Mac command of the Task 16 plan
// does). Elsewhere this file reports UNSUPPORTED, which is the correct
// answer there and the reason a developer is not punished. macOS CI is
// deliberately not `display-expected` yet: nobody knows whether its
// runners have a Metal device and a window-server session.
//
// **It reads lit.cfg's own verdict, not a second opinion.** `%display-state`
// expands to the feature this configuration actually computed, so a probe
// that goes wrong for ANY reason -- a missing package, an Xvfb or wrapper
// that changed its behaviour, a 90-second timeout under load, a locked Mac
// -- is what fails here. `%display-reason` is the probe's own account of
// why (on macOS, the console-session check's answer when that stage said
// no), so the red line says what to fix.
//
// THE MUTATION THIS MUST BREAK: make lit.cfg's `_can_start_a_display()`
// return False (equivalently: remove `xvfb` from the CI apt line). With CI
// in the environment this file goes RED; the window tests go UNSUPPORTED,
// which is precisely the silent hole it exists to convert into a failure.
// On macOS, both stages -- the console session and the window probe --
// end in the same `display` verdict, so either one failing is red here.
//
// REQUIRES: ui-gpu, display-expected
// RUN: echo 'display-feature %display-state (%display-reason)' | %FileCheck %s

// CHECK: display-feature on
