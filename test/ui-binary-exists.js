// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Gated on `ui-enabled` (Task 5's feature, which probes the PLAIN
// hermes-node binary via --test-embedder-module and is therefore on
// whenever HERMES_NODE_ENABLE_UI is), deliberately NOT on `ui` (this
// file's own binary's feature, decided by test/lit.cfg running
// hermes-node-ui --version). A self-referential probe has a blind spot: if
// hermes-node-ui itself regresses (fails to build, --version stops
// working, prints something the probe no longer recognizes), the `ui`
// probe reports False -- correctly, from its own point of view -- and
// every test gated `REQUIRES` `ui`, this file's neighbours included, goes
// UNSUPPORTED. lit counts UNSUPPORTED as success, so `ninja
// check-hermes-node` would stay green while the regression the probe was
// supposed to be checking for goes unreported.
//
// This test's gate does not depend on that probe's verdict at all -- only
// on whether the option was compiled in -- so it runs, and asserts
// against the real binary directly, in exactly the scenario where the `ui`
// probe would otherwise be hiding the story. Confirmed to actually work
// this way (not merely assumed): both %hermes-node-ui's --version output
// and the `ui` probe's own pass/fail were checked independently while
// developing this file, and this test's RUN lines below make no reference
// to the `ui` feature or to anything test/lit.cfg's probe function
// computes.
//
// REQUIRES: ui-enabled
// RUN: %hermes-node-ui --version > %t.out; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out

// OUT: hermes-node-ui
// CODE: EXITCODE 0
