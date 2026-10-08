// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A unit is compiled under the -test262 its artifact's baked --vm= list
// produces, and a run under a different setting refuses it, falls back with
// the second warning, and still gets the right semantics. The observable is
// an out-of-bounds store: dropped without test262, trapped with it, caught
// inside the program so output, warning and exit status are asserted
// together.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: cp %s %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --vm=-test262 --record-wasm=%t/rec262.bin %t/src/app.js > /dev/null

// Baked -test262, unlocked. Two recordings name the same module: one unit.
// RUN: %hermes-node build-native %t/src/app.js -o %t/baked --vm=-test262 --allow-vm-options-override --bake-wasm=%t/rec.bin --bake-wasm=%t/rec262.bin --kit=%kit_dir > %t/baked.build
// RUN: grep -c '^wasm: native ' %t/baked.build | %FileCheck --check-prefix=ONE %s
// ONE: {{^1$}}
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/baked > %t/b1.out 2> %t/b1.err
// RUN: %FileCheck --check-prefix=TRAP %s < %t/b1.out
// RUN: %FileCheck --check-prefix=HIT %s < %t/b1.err
// RUN: HERMES_NODE_VM_OPTIONS=-test262=false %t/baked > %t/b2.out 2> %t/b2.err
// RUN: %FileCheck --check-prefix=DROP %s < %t/b2.out
// RUN: %FileCheck --check-prefix=INCOMPATIBLE %s < %t/b2.err

// Nothing baked, unlocked: the inverse.
// RUN: %hermes-node build-native %t/src/app.js -o %t/plain --allow-vm-options-override --bake-wasm=%t/rec.bin --kit=%kit_dir > /dev/null
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/plain > %t/p1.out 2> %t/p1.err
// RUN: %FileCheck --check-prefix=DROP %s < %t/p1.out
// RUN: %FileCheck --check-prefix=HIT %s < %t/p1.err
// RUN: HERMES_NODE_VM_OPTIONS=-test262 %t/plain > %t/p2.out 2> %t/p2.err
// RUN: %FileCheck --check-prefix=TRAP %s < %t/p2.out
// RUN: %FileCheck --check-prefix=INCOMPATIBLE %s < %t/p2.err

// TRAP: oob store: trapped
// DROP: oob store: dropped
// HIT: wasm native hit
// HIT-NOT: warning:
// INCOMPATIBLE: warning: WebAssembly module {{[0-9a-f]+}}: its native unit is unavailable or incompatible with this run (e.g. -test262); falling back to bytecode

// RUN: rm -f %t/baked %t/plain

var m = require('./modules.js');
var i = new WebAssembly.Instance(new WebAssembly.Module(m.FIB), {env: {log: function () {}}});
try {
  i.exports.poke(70000, 7);
  console.log('oob store: dropped');
} catch (e) {
  console.log('oob store: trapped');
}
