// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// hermes-node-ui's --bundle= must pass a program's own trailing arguments
// through to process.argv, exactly as plain hermes-node's --bundle=<f> arg
// does -- CLAUDE.md treats process.argv.slice(2) meaning what it means
// under --bundle=<f> arg as load-bearing across both binaries. Two cases:
// a bare positional after --bundle=, and one after "--". Both used to be
// refused outright ("--bundle cannot be combined with a script argument"),
// because the CLI parser's positional/"--" branches set scriptArgIndex
// even when --bundle= had already supplied the program, so the very next
// token looked like a second entry point instead of the bundled program's
// first argument.
//
// REQUIRES: ui
// RUN: rm -rf %t.uiargv && mkdir -p %t.uiargv
// RUN: echo "console.log('ARGV ' + process.argv.slice(2).join(','));" > %t.uiargv/main.js
// RUN: %hermes-node --build-bundle=%t.uiargv/app.hbb %t.uiargv/main.js

// A bare positional after --bundle=.
// RUN: %hermes-node-ui --bundle=%t.uiargv/app.hbb extra1 extra2 | %FileCheck --check-prefix=POSITIONAL %s
// POSITIONAL: ARGV extra1,extra2

// A positional after "--".
// RUN: %hermes-node-ui --bundle=%t.uiargv/app.hbb -- extra3 | %FileCheck --check-prefix=DASHDASH %s
// DASHDASH: ARGV extra3

// An empty --bundle= value is still a request -- for an empty path, not
// "no --bundle at all" -- and the error must name the flag, matching the
// convention hermes-node.cpp's own checkToolOptions() uses for
// --dump-bytecode=, --out=, --build-exe=, --kit= and --cc=: "Error: --bundle
// requires a file path.", not the generic "no script provided" a plain
// empty-string bundlePath would otherwise be indistinguishable from.
// RUN: %not %hermes-node-ui --bundle= 2>&1 | %FileCheck --check-prefix=EMPTYBUNDLE %s
// EMPTYBUNDLE: Error: --bundle requires a file path.
