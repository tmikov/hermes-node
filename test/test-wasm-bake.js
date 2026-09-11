// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The bake + run round trip: --record-wasm produces a file, --bake-wasm
// copies its entries into a container's Wasm table, and a bundled run
// consults that table before it consults the disk cache or compiles.
//
// Hits, misses and stores are asserted from
// HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE tracing, never from timing -- see
// test-wasm-cache.js for why. Never asserted from a program's own output
// alone, since a correct answer says nothing about which tier produced it.

// REQUIRES: wasm

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.tree/modules.js
// RUN: echo "var m = require('./modules.js'); var a = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', a.exports.add(19, 23)); var f = new WebAssembly.Instance(new WebAssembly.Module(m.FIB), { env: { log: function () {} } }); console.log('fib', f.exports.fib(10));" > %t.tree/app.js
// RUN: echo "var m = require('./modules.js'); var a = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', a.exports.add(19, 23));" > %t.tree/addonly.js

// One record file with both digests, one with only ADD's -- the second is
// how the "one module unbaked" case below leaves FIB out of the container
// on purpose.
// RUN: %hermes-node --record-wasm=%t.recAll %t.tree/app.js | %FileCheck --check-prefix=BOTH %s
// RUN: %hermes-node --record-wasm=%t.recAdd %t.tree/addonly.js | %FileCheck --check-prefix=ADD %s

// RUN: %hermes-node --build-bundle=%t.full.hbb --bake-wasm=%t.recAll %t.tree/app.js
// RUN: %hermes-node --build-bundle=%t.partial.hbb --bake-wasm=%t.recAdd %t.tree/app.js

// --- Case 1: both modules baked. Every WebAssembly.Module call is a
// container hit; nothing compiles and nothing is written to the disk cache,
// even though one is live and warm from a previous run of something else.
// RUN: rm -rf %t.cache1 && mkdir -p %t.cache1
// RUN: %hermes-node-cc --compile-cache=%t.cache1 --bundle=%t.full.hbb | %FileCheck --check-prefix=BOTH %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc --compile-cache=%t.cache1 --bundle=%t.full.hbb 2>&1 >/dev/null | %FileCheck --check-prefix=FULLTRACE %s
// RUN: find %t.cache1 -name 'w*' | wc -l | %FileCheck --check-prefix=NONE %s

// --- Case 2: one module baked, one left out. The baked one still hits; the
// other falls through to an ordinary miss-then-compile, exactly as it would
// with no container at all.
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node --no-compile-cache --bundle=%t.partial.hbb 2>&1 >/dev/null | %FileCheck --check-prefix=PARTIAL %s
// RUN: %hermes-node --no-compile-cache --bundle=%t.partial.hbb | %FileCheck --check-prefix=BOTH %s

// --- Case 3: both modules baked, and this run's disk cache is not merely
// cold but fully DISABLED (--no-compile-cache, no --compile-cache= at all).
// The install rule installs the Wasm cache hooks whenever the container
// itself might answer, independently of whether there is a disk cache to
// ask -- so the container must still be consulted and still hit.
// This is the regression test for the design review's CRITICAL finding:
// without that rule, a baked container is invisible on exactly the machine
// its artifact was shipped to, because that machine has no cache directory
// at all.
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node --no-compile-cache --bundle=%t.full.hbb 2>&1 >/dev/null | %FileCheck --check-prefix=INSTALLRULE %s

// --- Case 4: two --bake-wasm files that share a digest. %t.recAdd names
// ADD's digest; %t.recAll names both ADD's and FIB's. Naming recAdd FIRST
// means ADD comes from recAdd and FIB's entry from recAll is new, while
// recAll's own ADD entry is a duplicate of one already baked -- the first
// file to name a digest wins, and --verbose must say so. This is also the
// case that would catch either of the two bugs the dedupe set exists to
// prevent: winning becomes "last file" if the insert test is inverted, and
// two records sharing one digest land in a table a lookup binary-searches
// by that same digest if the `continue` after a duplicate is dropped --
// which is a silent format-invariant break, not a visible one, so the run
// below (not just the --verbose narration) is what actually catches it.
// RUN: %hermes-node --build-bundle=%t.dup.hbb --bake-wasm=%t.recAdd --bake-wasm=%t.recAll --verbose %t.tree/app.js 2>&1 >/dev/null | %FileCheck --check-prefix=DUP %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node --no-compile-cache --bundle=%t.dup.hbb 2>&1 >/dev/null | %FileCheck --check-prefix=DUP2 %s

// BOTH: add 42
// BOTH: fib 55
// ADD: add 42
// FULLTRACE: wasm container hit
// FULLTRACE: wasm container hit
// FULLTRACE-NOT: wasm container miss
// FULLTRACE-NOT: wasm store
// NONE: 0
// PARTIAL: wasm container hit
// PARTIAL: wasm container miss
// PARTIAL: wasm store
// INSTALLRULE: wasm container hit
// INSTALLRULE: wasm container hit
// INSTALLRULE-NOT: wasm container miss
// INSTALLRULE-NOT: wasm store
// DUP-DAG: wasm: {{[0-9a-f]+}} {{[0-9]+}} bytes (from {{.*}}recAdd)
// DUP-DAG: wasm: {{[0-9a-f]+}} already baked; skipping (from {{.*}}recAll)
// DUP-DAG: wasm: {{[0-9a-f]+}} {{[0-9]+}} bytes (from {{.*}}recAll)
// DUP: wasm: 2 modules baked, {{[0-9]+}} bytes
// DUP2: wasm container hit
// DUP2: wasm container hit
// DUP2-NOT: wasm container miss
// DUP2-NOT: wasm store
