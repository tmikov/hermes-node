// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm
// RUN: rm -rf %t.cache && mkdir -p %t.cache
// RUN: %hermes-node-cc --compile-cache=%t.cache %s | %FileCheck %s
// RUN: %hermes-node-cc --compile-cache=%t.cache %s | %FileCheck %s
// An entry exists, and its name marks it as a Wasm entry.
// RUN: find %t.cache -name 'w*' | head -1 | %FileCheck --check-prefix=ENTRY %s
// The configuration file is written where generation pruning cannot reach it.
// RUN: cat %t.cache/config | %FileCheck --check-prefix=CONFIG %s
// The second run must report a hit rather than merely being fast.
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc \
// RUN:   --compile-cache=%t.cache %s 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=TRACE %s
// With the cache off, nothing is written.
// RUN: rm -rf %t.nocache && mkdir -p %t.nocache
// RUN: %hermes-node --no-compile-cache --compile-cache=%t.nocache %s \
// RUN:   | %FileCheck %s
// RUN: find %t.nocache -name 'w*' | wc -l | %FileCheck --check-prefix=NONE %s

// A bundled program uses the cache too: a container carries no compiled Wasm,
// so it compiles at every launch without one.
// RUN: rm -rf %t.btree %t.bcache && mkdir -p %t.btree %t.bcache
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.btree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.btree/app.js
// RUN: %hermes-node --build-bundle=%t.btree/app.hbb %t.btree/app.js
// RUN: %hermes-node-cc --compile-cache=%t.bcache --bundle=%t.btree/app.hbb | %FileCheck --check-prefix=BUNDLE %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc \
// RUN:   --compile-cache=%t.bcache --bundle=%t.btree/app.hbb 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=TRACE %s

// A corrupt entry recovers rather than surfacing as a broken program, and is
// rewritten -- content keys make that automatic, since the fresh store lands
// on the same file name.
// RUN: rm -rf %t.corrupt && mkdir -p %t.corrupt
// RUN: %hermes-node-cc --compile-cache=%t.corrupt %s | %FileCheck %s
// Count what was corrupted and assert it: a loop that silently found
// nothing would leave run 2 an ordinary hit and run 3 tracing a hit, so
// the case would pass without ever having corrupted an entry.
// RUN: for f in $(find %t.corrupt -name 'w*'); do printf 'junk' > $f; done
// RUN: find %t.corrupt -name 'w*' -size 4c | wc -l | %FileCheck --check-prefix=TWO %s
// RUN: %hermes-node-cc --compile-cache=%t.corrupt %s | %FileCheck %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc \
// RUN:   --compile-cache=%t.corrupt %s 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=TRACE %s

// Two distinct modules get two entries rather than colliding. This file
// compiles ADD and FIB, so exactly two must appear.
// RUN: rm -rf %t.two && mkdir -p %t.two
// RUN: %hermes-node-cc --compile-cache=%t.two %s | %FileCheck %s
// RUN: find %t.two -name 'w*' | wc -l | %FileCheck --check-prefix=TWO %s

// The codegen configuration is part of the key. This is the one failure in
// the feature that would be SILENT rather than loud: the same module
// compiled under different rules must not share an entry, or a run gets
// bytecode built for a configuration it is not running under. -test262 turns
// on Wasm memory bounds checks, so it genuinely changes generated code.
// This file compiles two modules, so two entries per configuration.
// RUN: rm -rf %t.cfg && mkdir -p %t.cfg
// RUN: %hermes-node-cc --compile-cache=%t.cfg %s | %FileCheck %s
// RUN: find %t.cfg -name 'w*' | wc -l | %FileCheck --check-prefix=TWO %s
// RUN: %hermes-node-cc --compile-cache=%t.cfg --vm=-test262 %s | %FileCheck %s
// RUN: find %t.cfg -name 'w*' | wc -l | %FileCheck --check-prefix=FOUR %s

// A malformed config file warns, on every run, without the debug flag and
// without stopping the program. The file exists to be hand-edited, so a typo
// that silently reverts a knob to its default is the failure worth refusing.
// RUN: rm -rf %t.badcfg && mkdir -p %t.badcfg
// RUN: printf 'recency: banana\nmax_wasm_bytes: 500MB\nnonsense\n' > %t.badcfg/config
// RUN: %hermes-node-cc --compile-cache=%t.badcfg %s 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=BADCFG %s
// And the run itself still succeeds, on the defaults.
// RUN: %hermes-node-cc --compile-cache=%t.badcfg %s | %FileCheck %s

// A well-formed config says nothing at all -- a warning on every correct run
// would train the reader to ignore the ones that matter.
// RUN: rm -rf %t.okcfg && mkdir -p %t.okcfg
// RUN: printf 'recency: mtime\nmax_wasm_bytes: 268435456\n' > %t.okcfg/config
// RUN: %hermes-node-cc --compile-cache=%t.okcfg %s 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=NOWARN --allow-empty %s

// --inspect disables the cache, because entries are compiled at THROWING and
// the debugger needs ALL. Nothing must be written.
//
// This uses --inspect rather than --inspect-brk: createCompileCache disables
// the cache on `config.inspect || config.inspectBrk`, so --inspect alone
// already exercises the property, and --inspect-brk pauses before running
// user code and never resumes without a debugger client attached. Measured:
// `hermes-node --inspect-brk -e ...` under a 5s timeout never completed
// (exit 124), while `--inspect=0` runs the script to completion and exits 0.
// RUN: rm -rf %t.insp && mkdir -p %t.insp
// RUN: %hermes-node-cc --compile-cache=%t.insp --inspect=0 %s > /dev/null 2>&1
// RUN: find %t.insp -name 'w*' | wc -l | %FileCheck --check-prefix=NONE %s

// Caching a compiled WebAssembly module.
//
// Hit and miss are asserted from tracing, never from timing: these fixtures
// are tiny and the suite runs 16-way parallel, so a timing assertion would
// measure scheduling noise. That is how both known flaky tests got that way.

var mods = require('./fixtures/wasm/modules.js');

var inst = new WebAssembly.Instance(new WebAssembly.Module(mods.ADD));
console.log('add', inst.exports.add(19, 23));

// A second, different module must get its own entry rather than colliding.
var fib = new WebAssembly.Instance(new WebAssembly.Module(mods.FIB), {
  env: { log: function () {} },
});
console.log('fib', fib.exports.fib(10));

// CHECK: add 42
// CHECK: fib 55
// ENTRY: {{w[0-9a-f]+}}
// CONFIG: recency: atime
// CONFIG: max_wasm_bytes: 268435456
// TRACE: wasm hit
// NONE: 0
// BUNDLE: add 42
// TWO: 2
// BADCFG: warning: {{.*}}/config: line 1: recency must be 'atime' or 'mtime', found 'banana'; using 'atime'
// BADCFG: warning: {{.*}}/config: line 2: max_wasm_bytes must be a decimal byte count, found '500MB'; using 268435456
// BADCFG: warning: {{.*}}/config: line 3: expected '<key>: <value>', found 'nonsense'
// NOWARN-NOT: warning
// FOUR: 4
