// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The native tier sits under Hermes's module construction, so every entry
// point reaches it with no change to the program: new WebAssembly.Module,
// WebAssembly.compile and WebAssembly.instantiate over bytes. And a
// recording made with every module served from a warm disk cache -- no
// compile at all during the run -- still carries the module bytes, and
// builds natively.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: cp %s %t/src/app.js
// RUN: %hermes-node-cc --compile-cache=%t/cc %t/src/app.js > /dev/null
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc --compile-cache=%t/cc --record-wasm=%t/rec.bin %t/src/app.js 2> %t/rec.err > /dev/null
// RUN: %FileCheck --check-prefix=WARMREC %s < %t/rec.err
// WARMREC: wasm hit
// WARMREC-NOT: wasm store
// RUN: %hermes-node --dump-wasm=%t/rec.bin | %FileCheck --check-prefix=REC %s
// REC: SOURCES (1)

// RUN: %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir > /dev/null
// RUN: mv %t/src %t/src.away
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app > %t/out 2> %t/err
// RUN: %FileCheck --check-prefix=OUT %s < %t/out
// RUN: grep -c 'wasm native hit' %t/err | %FileCheck --check-prefix=THREE %s
// RUN: %FileCheck --check-prefix=QUIET %s < %t/err
// OUT: module 42
// OUT-DAG: compile 42
// OUT-DAG: instantiate 42
// THREE: {{^3$}}
// QUIET-NOT: warning:
// QUIET-NOT: wasm store

// RUN: rm -f %t/app

var m = require('./modules.js');
var mod = new WebAssembly.Module(m.ADD);
console.log('module', new WebAssembly.Instance(mod).exports.add(19, 23));
WebAssembly.compile(m.ADD).then(function (mod2) {
  console.log('compile', new WebAssembly.Instance(mod2).exports.add(19, 23));
});
WebAssembly.instantiate(m.ADD).then(function (r) {
  console.log('instantiate', r.instance.exports.add(19, 23));
});
