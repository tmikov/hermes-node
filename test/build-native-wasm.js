// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A natively compiled executable is not a Wasm-free one. It installs the
// Wasm cache hooks and uses the disk cache exactly as a --build-exe
// artifact does (test-wasm-cache-build-exe.js), and baked entries live in
// the same v6 container this producer already writes, so --bake-wasm works
// here too (test-wasm-bake-build-exe.js). Native code and WebAssembly in
// one process have never been exercised together before this file.
//
// Hits and misses are read from the tracing, never from timing: the suite
// runs 16-way parallel and both of its known flaky tests got that way
// through a timing dependency.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t/src/app.js

// Case 1: the disk tier, through a build-native executable. Miss then hit,
// exactly as test-wasm-cache-build-exe.js pins for --build-exe.
// RUN: mkdir -p %t/cache1
// RUN: %hermes-node build-native %t/src/app.js -o %t/app1 --kit=%kit_dir
//
// A produced executable takes no --compile-cache flag -- every argument
// belongs to the program -- so HERMES_NODE_COMPILE_CACHE in the environment
// is the only override it has. It also inherits the suite-wide
// HERMES_NODE_DISABLE_COMPILE_CACHE=1 like any other subprocess lit spawns
// (it is not run through %hermes-node-cc, which only wraps hermes-node
// itself), so both invocations below clear it explicitly.
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t/cache1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app1 2>&1 >/dev/null | %FileCheck --check-prefix=MISS %s
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t/cache1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app1 2>&1 >/dev/null | %FileCheck --check-prefix=HIT %s
// MISS: wasm miss
// HIT: wasm hit

// Case 2: a baked entry, through a build-native executable. Record on a
// plain run, bake with --bake-wasm into the build-native container, then
// run with the disk cache disabled entirely -- the container tier must
// still answer, with nothing written to the (nonexistent) disk cache.
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node build-native %t/src/app.js -o %t/app2 --bake-wasm=%t/rec.bin --kit=%kit_dir
// RUN: HERMES_NODE_DISABLE_COMPILE_CACHE=1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app2 2>&1 >/dev/null | %FileCheck --check-prefix=CHIT %s
// CHIT: wasm container hit
// CHIT-NOT: wasm store

// Both executables still produce the right answer.
// RUN: %t/app1 | %FileCheck --check-prefix=ADD %s
// RUN: %t/app2 | %FileCheck --check-prefix=ADD %s
// ADD: add 42

// The produced executables are large under ASAN, so they go when they are
// no longer needed. This is the LAST line deliberately: lit stops at the
// first failing RUN line, so a failure leaves every artifact in place for
// post-mortem and only a passing run cleans up after itself.
// RUN: rm -f %t/app1 %t/app2

// This file is a lit driver only; the RUN lines above are the test.
