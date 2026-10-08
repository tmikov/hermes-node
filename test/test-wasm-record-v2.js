// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Record format v2 changes nothing for the bytecode producers: --build-bundle
// --bake-wasm reads only its bytecode table, and --build-exe consumes the
// container, never a recording.
//
// REQUIRES: wasm, linker-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: echo "var m = require('./modules.js'); console.log('add', new WebAssembly.Instance(new WebAssembly.Module(m.ADD)).exports.add(19, 23));" > %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --dump-wasm=%t/rec.bin | %FileCheck --check-prefix=DUMP %s
// DUMP: format v2
// DUMP: WASM (1)
// DUMP: SOURCES (1)
// RUN: %hermes-node --build-bundle=%t/app.hbb --bake-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --build-exe=%t/app --kit=%kit_dir %t/app.hbb > /dev/null
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app 2> %t/err | %FileCheck --check-prefix=ADD %s
// RUN: %FileCheck --check-prefix=HIT %s < %t/err
// ADD: add 42
// HIT: wasm container hit
// HIT-NOT: warning:
// RUN: rm -f %t/app
