// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --record-wasm on its own, with nothing baked back in. See test-wasm-bake.js
// for the bake+run round trip and test-wasm-record-exit.js for the
// process.exit() case.
//
// Hits and misses are asserted from HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE
// tracing, never from timing -- see test-wasm-cache.js for why.

// REQUIRES: wasm
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.tree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.tree/app.js

// Recording a plain run (cache off, the suite default) writes a well-formed
// one-entry file, and the program still produces its own output.
// RUN: %hermes-node --record-wasm=%t.rec1 %t.tree/app.js | %FileCheck --check-prefix=ADD %s
// RUN: %hermes-node --dump-wasm=%t.rec1 | %FileCheck --check-prefix=ONE %s

// Recording against a warm disk cache must record what a LOOKUP hit hands
// back, not only what a STORE compiles -- otherwise a second recording run
// against an already-warm cache would silently produce an empty file. The
// first run below compiles and stores; the second is a disk hit; the
// second run's own record file must still show the one entry.
// RUN: rm -rf %t.cache2 && mkdir -p %t.cache2
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc --compile-cache=%t.cache2 --record-wasm=%t.rec2a %t.tree/app.js 2>&1 >/dev/null | %FileCheck --check-prefix=STORE %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc --compile-cache=%t.cache2 --record-wasm=%t.rec2b %t.tree/app.js 2>&1 >/dev/null | %FileCheck --check-prefix=DISKREC %s
// RUN: %hermes-node --dump-wasm=%t.rec2b | %FileCheck --check-prefix=ONE %s

// A program that never compiles any Wasm still gets a well-formed, empty
// record file rather than nothing at all.
// RUN: echo "console.log('no wasm here');" > %t.tree/none.js
// RUN: %hermes-node --record-wasm=%t.rec3 %t.tree/none.js | %FileCheck --check-prefix=NOWASM %s
// RUN: %hermes-node --dump-wasm=%t.rec3 | %FileCheck --check-prefix=ZERO %s

// A --record-wasm path that cannot be written fails before the program's own
// output: hermes_node_runtime.cpp writes the (empty) file once, immediately,
// before the Hermes runtime is even created, so a script that would print
// something never gets the chance to run.
// RUN: %not %hermes-node --record-wasm=%t.nosuchdir/rec %t.tree/app.js > %t.out4 2> %t.err4
// RUN: %FileCheck --check-prefix=NOPROGRAM --allow-empty %s < %t.out4
// RUN: %FileCheck --check-prefix=WRITEERR %s < %t.err4

// ADD: add 42
// ONE: WASM (1)
// ZERO: WASM (0)
// NOWASM: no wasm here
// STORE: wasm miss
// STORE: wasm store
// STORE: wasm record compile
// DISKREC: wasm hit
// DISKREC: wasm record disk
// NOPROGRAM-NOT: add
// WRITEERR: Error: --record-wasm={{.*}}/rec: cannot be written
// WRITEERR: error: cannot open {{.*}} for writing:
