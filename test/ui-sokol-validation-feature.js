// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// **The forcing function for the `sokol-validation` lit feature.**
//
// test/lit.cfg turns that feature on when `hermes-node-ui --version` prints
// `sokol_gfx validation: on` on a line of its own, and
// test/ui-sokol-validation.js -- the only test that provokes a REAL Sokol
// validation failure on the drawing thread and requires the process to
// terminate -- is gated on it. A feature that can only ever make a test
// UNSUPPORTED can be lost with nothing going red: reword the --version line,
// stop printing it, or let lit.cfg's match drift, and that test silently
// stops running in the one CI job able to run it. The same hole `display`
// had until test/ui-display-feature.js; this is its counterpart.
//
// Two assertions, gated differently, because they ask different questions.
//
//  AGREE, everywhere the UI binary exists: the binary prints the line, with
//  exactly `on` or `off`, and lit.cfg computed the SAME answer. This is what
//  catches a reworded line or a drifted match -- lit would compute `off`
//  while the binary still says `on` -- and it needs no display and no CI.
//
//  CI, only where a GPU backend was built and the environment says it is CI:
//  the answer is `on`. .github/workflows/ci.yml enables
//  HERMES_NODE_ENABLE_UI in its Debug configuration and nowhere else, and a
//  Debug sokol_gfx validates; so in CI, a UI build that does not validate is
//  a defect rather than a choice. A developer's Release+UI build is a
//  legitimate `off`, which is why this arm is not asserted there.
//
// THE MUTATIONS THIS MUST BREAK: (1) change the text lit.cfg matches in
// `_hermes_node_ui_validates()` -- AGREE goes red (binary `on`, lit `off`);
// (2) make that function return False -- same; (3) print the line with a
// different wording from tools/hermes-node-ui/main.cpp -- AGREE goes red,
// because the BINARY check below no longer finds it.
//
// REQUIRES: ui
// RUN: %hermes-node-ui --version | %FileCheck --check-prefix=BINARY %s
// RUN: %hermes-node-ui --version | grep '^sokol_gfx validation: ' | sed 's/^sokol_gfx validation: /agree binary=/' | tr -d '\n' > %t.agree
// RUN: echo ' lit=%sokol-validation-state' >> %t.agree
// RUN: %FileCheck --check-prefix=AGREE %s < %t.agree

// BINARY: {{^}}sokol_gfx validation: {{on|off}}{{$}}

// AGREE: {{^agree binary=(on lit=on|off lit=off)$}}
