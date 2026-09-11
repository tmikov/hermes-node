// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// process.exit() calls _exit() after flushing stdio, bypassing anything that
// would run later in the normal shutdown path. A recorder that only wrote at
// "the end of the run" would lose everything the moment a program calls
// process.exit() -- this is the case that would catch that, since
// WasmRecordWriter::record() actually rewrites the file on every call rather
// than accumulating for a final flush.

// REQUIRES: wasm

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.tree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23)); process.exit(0);" > %t.tree/app.js

// RUN: %hermes-node --record-wasm=%t.rec %t.tree/app.js | %FileCheck --check-prefix=ADD %s
// RUN: %hermes-node --dump-wasm=%t.rec | %FileCheck --check-prefix=ONE %s

// ADD: add 42
// ONE: WASM (1)
// ONE: [0]
