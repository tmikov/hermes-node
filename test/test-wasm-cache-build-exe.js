// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm, linker-available
// RUN: rm -rf %t.exe && mkdir -p %t.exe/ship %t.exe/cache
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.exe/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.exe/app.js
// RUN: %hermes-node --build-bundle=%t.exe/app.hbb %t.exe/app.js
// RUN: %hermes-node --build-exe=%t.exe/ship/app --kit=%kit_dir %t.exe/app.hbb
//
// A produced executable inherits the suite-wide
// HERMES_NODE_DISABLE_COMPILE_CACHE=1 like any other subprocess lit spawns
// (it is not run through %hermes-node-cc, which only wraps hermes-node
// itself), so both invocations below clear it explicitly. Measured: without
// `-u` here, the run below writes no cache entry at all.
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t.exe/cache %t.exe/ship/app | %FileCheck --check-prefix=BUNDLE %s
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t.exe/cache HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t.exe/ship/app 2>&1 >/dev/null | %FileCheck --check-prefix=TRACE %s

// A produced executable uses the cache too. Gated, like the other build-exe
// tests, on a linker and a cut kit. There is no --compile-cache flag here:
// every argument to a produced executable belongs to the program, so the
// only override an executable has is the HERMES_NODE_COMPILE_CACHE
// environment variable.

// BUNDLE: add 42
// TRACE: wasm hit
