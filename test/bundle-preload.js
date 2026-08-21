// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --preload records a module that runs before the entry. The fixture's
// setup.js is required by NOTHING -- which is what a register or polyfill
// module looks like -- so it is in the container only because the flag put
// it there, and the walk would never have found it.

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "globalThis.SETUP = 'ran';" > %t.tree/setup.js
// RUN: echo "console.log('ENTRY sees', globalThis.SETUP);" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --preload=./setup %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=DUMP %s
// DUMP: setup.js
// DUMP: PRELOADS (1)
// DUMP: setup.js
// The preload table is a section with real bytes, not a free extra folded
// into "header and padding" -- SECTIONS says so instead of only PRELOADS
// counting the module.
// DUMP: preloads {{[1-9][0-9]*}} B

// Without the flag it is not in the container at all.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/plain.hbb --dump | %FileCheck --check-prefix=PLAIN --implicit-check-not=setup.js --implicit-check-not=PRELOADS %s
// PLAIN: MODULES

// A --preload that does not resolve is a build error: the user named this
// one explicitly, so silence would be wrong.
// RUN: %not %hermes-node --build-bundle=%t.tree/bad.hbb --preload=./ghost %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=BADPRE %s
// BADPRE: error: --preload=./ghost cannot be resolved

// --preload without --build-bundle is a flag conflict, reported after the
// whole parse so flag order cannot change the outcome.
// RUN: %not %hermes-node --preload=./setup %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=NOBUILD %s
// NOBUILD: --preload requires --build-bundle

// A --preload naming something unpackageable is a build error too, and says
// which reason: the user named this file explicitly, so skipping it with a
// warning the way the walk does would leave a container that cannot run.
// (An .mjs, not a .node: a .node addon IS packageable now -- as a kNative
// module whose bytes ship beside the bundle -- and preloading one is
// meaningful, since running a preload means requiring it, which for a
// native means dlopen. An .mjs is the extension this loader still cannot
// execute at all.)
// RUN: printf 'export const v = 1;\n' > %t.tree/native.mjs
// RUN: %not %hermes-node --build-bundle=%t.tree/bad2.hbb --preload=./native.mjs %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=BADKIND %s
// BADKIND: error: --preload=./native.mjs resolves to {{.*}}native.mjs, which is not packageable

// A --preload that resolves but does not parse or compile is a hard build
// error too, not the tolerant "package as a throwing stub" fallback
// test/bundle-tolerant.js pins for an ordinary required-but-maybe-unreached
// file. A preload is not such a file: run() always loads it, before the
// entry, so a stub would let the build succeed and hand back a container
// that can never start (the SyntaxError would fire on every run, past the
// point the operator could still act on it). Both phases that can reject a
// preload are pinned here, mirroring the entry pins in test/bundle-errors.js:
// the scan that collects require() calls, and the compile that follows the
// walk.
// RUN: rm -rf %t.badparse && mkdir -p %t.badparse
// RUN: echo "function ( {" > %t.badparse/setup.js
// RUN: echo "console.log('should not build');" > %t.badparse/cli.js
// RUN: %not %hermes-node --build-bundle=%t.badparse/app.hbb --preload=./setup %t.badparse/cli.js 2>&1 | %FileCheck --check-prefix=PREPARSE %s
// PREPARSE: error: failed to parse preload {{.*}}setup.js

// RUN: rm -rf %t.badcompile && mkdir -p %t.badcompile
// RUN: echo "module.exports = function (f) { return import(f); };" > %t.badcompile/setup.cjs
// RUN: echo "console.log('should not build');" > %t.badcompile/cli.js
// RUN: %not %hermes-node --build-bundle=%t.badcompile/app.hbb --preload=./setup.cjs %t.badcompile/cli.js 2>&1 | %FileCheck --check-prefix=PRECOMPILE %s
// PRECOMPILE: error: failed to compile preload {{.*}}setup.cjs

// The same value twice records ONE entry: the second load would be a
// Module._cache hit and execute nothing, so a second table row would
// promise something the loader cannot do. Pinned two ways: the dump shows
// one table entry, and an actual run shows the module body ran once, not
// twice -- N is 1, not 2.
// RUN: rm -rf %t.dup && mkdir -p %t.dup
// RUN: echo "globalThis.N = (globalThis.N || 0) + 1;" > %t.dup/setup.js
// RUN: echo "console.log('DUP N =', globalThis.N);" > %t.dup/cli.js
// RUN: %hermes-node --build-bundle=%t.dup/app.hbb --preload=./setup --preload=./setup %t.dup/cli.js
// RUN: %hermes-node --bundle=%t.dup/app.hbb --dump | %FileCheck --check-prefix=DUP %s
// DUP: PRELOADS (1)
// RUN: rm -f %t.dup/setup.js %t.dup/cli.js
// RUN: %hermes-node --bundle=%t.dup/app.hbb | %FileCheck --check-prefix=DUPEXEC %s
// DUPEXEC: DUP N = 1

// A --preload that the entry already reaches is packaged once and recorded
// once: seeding a root that is already on the worklist must not duplicate
// the module.
// RUN: rm -rf %t.both && mkdir -p %t.both
// RUN: echo "module.exports = { v: 1 };" > %t.both/setup.js
// RUN: echo "console.log('BOTH', require('./setup').v);" > %t.both/cli.js
// RUN: %hermes-node --build-bundle=%t.both/app.hbb --preload=./setup %t.both/cli.js
// RUN: %hermes-node --bundle=%t.both/app.hbb --dump | %FileCheck --check-prefix=BOTH %s
// BOTH: MODULES (2)
// BOTH: PRELOADS (1)

// The preload runs before the entry, and its own require() resolves from
// the container like any bundled module's.
// RUN: rm -rf %t.run && mkdir -p %t.run
// RUN: echo "module.exports = { v: 3 };" > %t.run/dep.js
// RUN: echo "globalThis.SETUP = require('./dep').v; console.log('PRELOAD ran');" > %t.run/setup.js
// RUN: echo "console.log('ENTRY ran, SETUP =', globalThis.SETUP);" > %t.run/cli.js
// RUN: %hermes-node --build-bundle=%t.run/app.hbb --preload=./setup %t.run/cli.js
// RUN: rm -f %t.run/setup.js %t.run/cli.js %t.run/dep.js
// RUN: %hermes-node --bundle=%t.run/app.hbb | %FileCheck --check-prefix=ORDER %s
// ORDER: PRELOAD ran
// ORDER-NEXT: ENTRY ran, SETUP = 3

// Two preloads run in flag order.
// RUN: rm -rf %t.two && mkdir -p %t.two
// RUN: echo "console.log('FIRST');" > %t.two/one.js
// RUN: echo "console.log('SECOND');" > %t.two/two.js
// RUN: echo "console.log('THIRD');" > %t.two/cli.js
// RUN: %hermes-node --build-bundle=%t.two/app.hbb --preload=./one --preload=./two %t.two/cli.js
// RUN: rm -f %t.two/one.js %t.two/two.js %t.two/cli.js
// RUN: %hermes-node --bundle=%t.two/app.hbb | %FileCheck --check-prefix=TWO %s
// TWO: FIRST
// TWO-NEXT: SECOND
// TWO-NEXT: THIRD

// A preload is not the main module. It runs before the entry exists, so
// require.main is not yet set -- the same thing Node's -r observes, and for
// the same reason.
// RUN: rm -rf %t.main && mkdir -p %t.main
// RUN: echo "console.log('PRE main is', typeof require.main);" > %t.main/setup.js
// RUN: echo "console.log('ENTRY main is', require.main === module);" > %t.main/cli.js
// RUN: %hermes-node --build-bundle=%t.main/app.hbb --preload=./setup %t.main/cli.js
// RUN: rm -f %t.main/setup.js %t.main/cli.js
// RUN: %hermes-node --bundle=%t.main/app.hbb | %FileCheck --check-prefix=MAIN %s
// MAIN: PRE main is undefined
// MAIN: ENTRY main is true

// A throwing preload stops the run before the entry executes.
// RUN: rm -rf %t.throw && mkdir -p %t.throw
// RUN: echo "throw new Error('preload exploded');" > %t.throw/setup.js
// RUN: echo "console.log('ENTRY SHOULD NOT RUN');" > %t.throw/cli.js
// RUN: %hermes-node --build-bundle=%t.throw/app.hbb --preload=./setup %t.throw/cli.js
// RUN: rm -f %t.throw/setup.js %t.throw/cli.js
// RUN: %not %hermes-node --bundle=%t.throw/app.hbb 2>&1 | %FileCheck --check-prefix=THROW --implicit-check-not="ENTRY SHOULD NOT RUN" %s
// THROW: preload exploded
