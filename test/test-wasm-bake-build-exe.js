// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The bake feature through a produced executable -- the case it exists for:
// an artifact shipped to a machine with no source tree and no warm cache
// still gets to skip compiling the WebAssembly module it carries.

// REQUIRES: wasm, linker-available
// RUN: rm -rf %t.exe && mkdir -p %t.exe/ship %t.exe/cache
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.exe/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.exe/app.js
// RUN: %hermes-node --record-wasm=%t.exe/rec.bin %t.exe/app.js > /dev/null
// RUN: %hermes-node --build-bundle=%t.exe/app.hbb --bake-wasm=%t.exe/rec.bin %t.exe/app.js
// RUN: %hermes-node --build-exe=%t.exe/ship/app --kit=%kit_dir %t.exe/app.hbb

// A produced executable inherits the suite-wide
// HERMES_NODE_DISABLE_COMPILE_CACHE=1 like any other subprocess lit spawns,
// so both invocations below clear it explicitly -- exactly the precedent in
// test-wasm-cache-build-exe.js. There is no --compile-cache flag here: every
// argument to a produced executable belongs to the program, so
// HERMES_NODE_COMPILE_CACHE is the only override an executable has.
//
// %t.exe/cache is freshly created and therefore live but COLD, not
// disabled: this case pins that the baked module is still a container hit
// with nothing written to that disk cache, i.e. the container tier is
// consulted and answers before the disk tier gets a chance to compile and
// store. It does NOT exercise the disk-cache-fully-disabled install rule --
// that regression (a baked container must still hit with no disk cache at
// all) is test-wasm-bake.js's Case 3, which reaches it through
// --no-compile-cache on an ordinary --bundle= run, a lever this produced
// executable does not have.
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t.exe/cache %t.exe/ship/app | %FileCheck --check-prefix=ADD %s
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t.exe/cache HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t.exe/ship/app 2>&1 >/dev/null | %FileCheck --check-prefix=TRACE %s
// RUN: find %t.exe/cache -name 'w*' | wc -l | %FileCheck --check-prefix=NONE %s

// ADD: add 42
// TRACE: wasm container hit
// TRACE-NOT: wasm store
// NONE: 0
