// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The default: a module the recording run saw is compiled natively by
// build-native and served by the native tier at run time -- no Wasm compile,
// no bytecode, no warning -- with the source tree gone and the disk cache
// off (the suite disables it), so nothing else could have answered.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir > %t/build.out
// RUN: %FileCheck --check-prefix=BUILD %s < %t/build.out
// BUILD: wasm: native {{[0-9a-f]+}} ({{[0-9]+}} bytes)
// BUILD: modules: {{.*}}, 1 WebAssembly module

// RUN: mv %t/src %t/src.away
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app 2> %t/run.err | %FileCheck --check-prefix=ADD %s
// RUN: %FileCheck --check-prefix=NATIVE %s < %t/run.err
// ADD: add 42
// NATIVE: wasm native hit
// NATIVE-NOT: wasm store
// NATIVE-NOT: wasm container
// NATIVE-NOT: warning:

// Last on purpose: lit stops at the first failing line, so only a passing run
// deletes the (large, under ASAN) artifact.
// RUN: rm -f %t/app
