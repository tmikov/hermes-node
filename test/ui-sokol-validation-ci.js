// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The CI arm of test/ui-sokol-validation-feature.js, whose header says what
// both arms are for. A file of its own only because lit gates whole files
// and this arm has a narrower gate: where a GPU backend was built AND the
// environment says it is CI, `sokol-validation` must be on, because CI
// builds the UI in its Debug configuration only and a Debug sokol_gfx
// validates. Without it, test/ui-sokol-validation.js would report
// UNSUPPORTED there and the suite would stay green.
//
// THE MUTATION THIS MUST BREAK: build the CI UI job without Sokol's
// validation layer (or break the probe so it reports `off`). With CI in the
// environment this goes RED.
//
// REQUIRES: ui-gpu, ci
// RUN: echo 'sokol-validation-feature %sokol-validation-state' | %FileCheck %s

// CHECK: sokol-validation-feature on
