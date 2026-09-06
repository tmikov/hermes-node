// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.tree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('SUM', i.exports.add(19, 23));" > %t.tree/app.js
// RUN: %hermes-node %t.tree/app.js | %FileCheck %s
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/app.js
// Delete the sources, so what runs can only be the container.
// RUN: rm %t.tree/app.js %t.tree/modules.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck %s

// A bundled program can still compile Wasm at run time.
//
// Worth pinning because the run path was built to need no compiler:
// hermesNodeBundleRun is deliberately free of the parser and code generator,
// every JS module in a container arrives as bytecode, and --bundle is refused
// with --inspect precisely because that bytecode carries no debug info. Wasm
// is the exception to the pattern -- WebAssembly.Module compiles bytes handed
// to it at run time, through the Wasm frontend inside hermesvm_a, which the
// binary links whether or not it is running a container.
//
// So this test fails if someone ever "tidies" the bundle build by dropping
// the compiler from a container-running binary, and it fails long before
// anyone tries to ship a bundled program that uses Wasm.
//
// The .wasm bytes travel inside modules.js, a packaged JS module, which is
// also the honest shape for a bundle: the producer does not package .wasm
// data files, so a program that reads one with fs still has to ship it beside
// the container.

// CHECK: SUM 42
