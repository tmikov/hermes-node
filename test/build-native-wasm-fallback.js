// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// What a native-Wasm artifact does with a module the native tier cannot
// serve, and what --bytecode-wasm changes.
//
// The program compiles ADD (recorded, so native), GARBAGE inside a
// try/catch -- a feature-detection probe whose bytes never compile, so the
// recording never committed it -- and, only when COMPILE_FIB is set at run
// time, FIB, which the recording therefore never saw. Each twice.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: cp %s %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --dump-wasm=%t/rec.bin | %FileCheck --check-prefix=REC %s
// REC: SOURCES (1)

// The probe's failure was never committed, so the build succeeds with one
// unit.
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir --keep-temp > %t/build.out 2> %t/build.err
// RUN: grep -c '^wasm: native ' %t/build.out | %FileCheck --check-prefix=ONEUNIT %s
// ONEUNIT: {{^1$}}

// The intermediate container carries no bytecode for the native module.
// RUN: sed -n 's/^build-native: temp directory: //p' %t/build.err > %t/dir.txt
// RUN: %hermes-node --bundle="$(cat %t/dir.txt)/container.hbb" --dump | %FileCheck --check-prefix=NOWASM %s
// NOWASM: EDGES (
// NOWASM-NOT: WASM (
// NOWASM: SECTIONS
// RUN: rm -rf "$(cat %t/dir.txt)"

// Cold: FIB falls back with the first warning, once; the probe warns once
// and still throws its CompileError; ADD is native.
// RUN: COMPILE_FIB=1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app > %t/cold.out 2> %t/cold.err
// RUN: %FileCheck --check-prefix=OUT %s < %t/cold.out
// RUN: grep -c 'was not compiled natively (it is not in the recording); falling back to bytecode' %t/cold.err | %FileCheck --check-prefix=TWO %s
// RUN: grep 'was not compiled natively' %t/cold.err | sort -u | grep -c . | %FileCheck --check-prefix=TWO %s
// RUN: grep -c 'wasm native hit' %t/cold.err | %FileCheck --check-prefix=TWO %s
// OUT: add 42
// OUT-NEXT: add 42
// OUT-NEXT: probe: CompileError
// OUT-NEXT: probe: CompileError
// OUT-NEXT: fib 55
// OUT-NEXT: fib 55
// TWO: {{^2$}}
// Two warning lines that are also two distinct lines name two distinct
// modules, so each of FIB and the probe warned exactly once -- the bare
// count alone would also accept FIB warning twice and the probe never.

// Warm: the same with a live disk cache, run twice so the second is a disk
// hit for FIB. The warning does not depend on where the bytecode came from.
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t/cc COMPILE_FIB=1 %t/app > /dev/null 2> /dev/null
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t/cc COMPILE_FIB=1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app > /dev/null 2> %t/warm.err
// RUN: %FileCheck --check-prefix=WARM %s < %t/warm.err
// RUN: grep -c 'was not compiled natively' %t/warm.err | %FileCheck --check-prefix=TWO %s
// RUN: grep 'was not compiled natively' %t/warm.err | sort -u | grep -c . | %FileCheck --check-prefix=TWO %s
// WARM: wasm hit

// --bytecode-wasm: the recorded bytecode is baked and served from the
// container, nothing warns, and the container carries the entry.
// RUN: %hermes-node build-native %t/src/app.js -o %t/appbc --bake-wasm=%t/rec.bin --bytecode-wasm --kit=%kit_dir --keep-temp > /dev/null 2> %t/buildbc.err
// RUN: sed -n 's/^build-native: temp directory: //p' %t/buildbc.err > %t/dirbc.txt
// RUN: %hermes-node --bundle="$(cat %t/dirbc.txt)/container.hbb" --dump | %FileCheck --check-prefix=BCWASM %s
// BCWASM: WASM (1)
// RUN: rm -rf "$(cat %t/dirbc.txt)"
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/appbc > /dev/null 2> %t/bc.err
// RUN: %FileCheck --check-prefix=BC %s < %t/bc.err
// BC: wasm container hit
// BC-NOT: warning:
// BC-NOT: wasm native

// RUN: rm -f %t/app %t/appbc

var m = require('./modules.js');
function add() {
  var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD));
  console.log('add', i.exports.add(19, 23));
}
function probe() {
  try {
    new WebAssembly.Module(m.GARBAGE);
    console.log('probe: compiled');
  } catch (e) {
    console.log('probe: ' + (e instanceof WebAssembly.CompileError ? 'CompileError' : e));
  }
}
function fib() {
  var i = new WebAssembly.Instance(new WebAssembly.Module(m.FIB), {env: {log: function () {}}});
  console.log('fib', i.exports.fib(10));
}
add();
add();
probe();
probe();
if (process.env.COMPILE_FIB) {
  fib();
  fib();
}
