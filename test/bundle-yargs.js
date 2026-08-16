// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// End-to-end proof on a real npm dependency tree: examples/yargs-cli is a
// CLI built on yargs, 16 packages deep. Everything above this line runs on
// synthetic fixtures; this is the one test that bundles a package nobody
// wrote for us.
//
// It needs an installed node_modules, which the default suite does not have
// (examples are installed by hand and exercised by check-hermes-node-examples
// for exactly that reason), so it opts in through a feature rather than
// failing on a fresh checkout.
// REQUIRES: examples-installed

// The example is copied out of the source tree first: the bundle has to sit
// at the build root the producer printed, and node_modules is then deleted
// from the copy. Neither belongs in the checkout.
// RUN: rm -rf %t.yargs && cp -R %source_dir/examples/yargs-cli %t.yargs

// The JavaScript yargs reaches must land in the container. yargs requires
// 'yargs/yargs' (a file with no extension) and its packages ship their real
// code as build/index.cjs, so a producer that packages only *.js and *.ts
// skips both, warns, and leaves a bundle that only works while node_modules
// is still there.
//
// The two BUILD-NOTs name those two shapes rather than banning the word
// "warning" outright: skipping an .mjs or a .node addon is the producer
// doing its job, and a blanket ban would fail this test the day yargs ships
// one. A CHECK-NOT before the first positive match scans from the start of
// the input, which is where the warnings would be -- the root line is
// printed after the whole walk.
// RUN: %hermes-node --build-bundle=%t.yargs/app.bundle %t.yargs/greet.js 2>&1 | %FileCheck --check-prefix=BUILD %s
// BUILD-NOT: (.cjs is not packageable)
// BUILD-NOT: ( is not packageable)
// BUILD: bundle root: {{.*}}.yargs

// With the dependency tree still present, for the baseline.
// RUN: %hermes-node --bundle=%t.yargs/app.bundle -- --help | %FileCheck --check-prefix=HELP %s

// Now delete both the dependency tree and the entry script, leaving the
// bundle as the only thing on disk, and run the same command. Same output.
// RUN: rm -rf %t.yargs/node_modules %t.yargs/greet.js
// RUN: %hermes-node --bundle=%t.yargs/app.bundle -- --help | %FileCheck --check-prefix=HELP %s

// HELP: greet <command> [options]
// HELP: greet hello  Say hello to someone
// HELP: greet count  Count numbers

// The subcommands work too, not just the usage banner yargs prints before
// dispatching anything. Both handlers live in greet.js, which no longer
// exists on disk.
// RUN: %hermes-node --bundle=%t.yargs/app.bundle -- hello --name World --excited -r 2 | %FileCheck --check-prefix=HELLO %s
// HELLO: Hello, World!!!
// HELLO: Hello, World!!!

// RUN: %hermes-node --bundle=%t.yargs/app.bundle -- count --from 1 --to 5 | %FileCheck --check-prefix=COUNT %s
// COUNT: 1, 2, 3, 4, 5

// This file is a lit driver only; the RUN lines above are the test.
