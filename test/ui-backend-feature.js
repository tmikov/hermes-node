// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// **The forcing function for the backend features**: `ui-backend-gl`,
// `ui-backend-metal` and the `%ui-backend` substitution test/lit.cfg
// computes from the third line of `hermes-node-ui --version`
// (`gpu backend: gl|metal`).
//
// Those decide which per-backend check lines a window test selects
// (`--check-prefixes=X,X-%ui-backend`) and which tests run at all
// (a `ui-backend-gl` requirement). A feature that can only turn a test
// UNSUPPORTED, or a prefix that can only select nothing, can be lost with
// nothing going red: reword the --version line and lit computes `none`, the
// `-gl` lines are no longer selected, and every test that carried its GL
// facts there passes having checked fewer of them.
//
// So three answers are compared, and all must be the same word:
//
//  - the binary's own line, which is what the build actually compiled;
//  - `%ui-backend`, which is what lit.cfg read from that line;
//  - `%ui-backend-expected`, a platform predicate (`metal` on darwin, `gl`
//    on any other POSIX system), which is what this platform is supposed
//    to build.
//
// The third is what catches a build that compiled the wrong backend while
// lit faithfully agrees with it. (`none` is what lit computes for a line
// it could not read; no build prints it.)
//
// THE MUTATIONS THIS MUST BREAK: (1) delete the `gpu backend:` line from
// tools/hermes-node-ui/main.cpp -- grep finds nothing, lit computes `none`,
// red; (2) make lit.cfg read a different value -- the `lit=` field
// disagrees, red.
//
// REQUIRES: ui, ui-gpu
// RUN: %hermes-node-ui --version | grep '^gpu backend: ' | tr -d '\n' > %t.agree
// RUN: echo ' lit=%ui-backend expected=%ui-backend-expected' >> %t.agree
// RUN: %FileCheck %s < %t.agree

// CHECK: {{^}}gpu backend: [[B:[a-z]+]] lit=[[B]] expected=[[B]]{{$}}
