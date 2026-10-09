// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A --record-wasm file whose module bytes no longer hash to the digest they
// are filed under is refused by build-native's bake step, naming the file
// and the digest and saying to re-record. Without the check the native
// unit would be compiled from the altered bytes but registered under the
// recorded digest, so the artifact would serve it for a module it was
// never compiled from. The refusal comes before anything is compiled.
//
// --build-bundle reads only the bytecode entries, so the same file still
// bakes there: the check belongs to the native path alone.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// The digest the error must name, read from the file before it is damaged
// (the tamper leaves the recorded digest alone). One FileCheck over both
// outputs, since a pattern variable does not outlive its invocation.
// RUN: %hermes-node --dump-wasm=%t/rec.bin > %t/check.txt
// RUN: python3 %source_dir/test/fixtures/wasm/tamper-record-source.py %t/rec.bin
// RUN: %not %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir >> %t/check.txt 2>&1
// RUN: %FileCheck --check-prefix=TAMPERED %s < %t/check.txt
// TAMPERED: SOURCES (1)
// TAMPERED-NEXT: [0] {{[0-9]+}} bytes  sha256:[[DIGEST:[0-9a-f]+]]
// TAMPERED: error: {{.*}}rec.bin: the WebAssembly module bytes recorded under [[DIGEST]] do not hash to that digest
// TAMPERED-NEXT: Re-record it with --record-wasm.
// TAMPERED-NOT: wasm: native
// RUN: test ! -e %t/app

// RUN: %hermes-node --build-bundle=%t/app.hbb --bake-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --bundle=%t/app.hbb | %FileCheck --check-prefix=ADD %s
// ADD: add 42
