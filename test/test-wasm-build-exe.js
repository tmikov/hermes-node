// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm, linker-available
// RUN: rm -rf %t.tree && mkdir -p %t.tree/ship
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.tree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('SUM', i.exports.add(19, 23));" > %t.tree/app.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/app.js
// RUN: %hermes-node --build-exe=%t.tree/ship/app --kit=%kit_dir %t.tree/app.hbb
// Run the executable from a directory holding nothing but itself.
// RUN: cd %t.tree/ship && ./app | %FileCheck %s

// The same property as test-wasm-bundle.js, one step further along: a
// standalone executable compiles Wasm at run time too.
//
// It is a separate test rather than another RUN line there because it needs
// a linker and a cut kit, which test-wasm-bundle.js does not -- gating that
// test on linker-available would stop it running in a kitless checkout for a
// reason that has nothing to do with what it checks.
//
// An executable links bundle_main.cpp instead of the CLI entry point, and is
// dead-stripped; both are places where the Wasm frontend could plausibly be
// dropped as unreachable, and it is reached only through a NativeFunction
// pointer that --gc-sections has no way to follow back to a name.

// CHECK: SUM 42
