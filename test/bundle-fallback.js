// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A module reached only through a computed require() is invisible to the
// static scanner that builds the edge table, so it cannot be in the bundle.
// It must still run by falling back to disk, and the fallback must be
// observable through the debug log. See test/bundle-run.js for the
// COLLIDE/BARE/THROW fallback regressions this builds on; those stay there.

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "const n = 'dyn' + ''; const m = require('./' + n); require('path'); console.log('GOT', m.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 7 };" > %t.tree/dyn.js
// RUN: %hermes-node --build-bundle=%t.tree/app.bundle %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.bundle | %FileCheck %s
// CHECK: GOT 7

// With the tree present, the miss falls back to disk and says so, naming
// both the specifier and the bundled importer that requested it -- pinning
// the importer is what would catch a regression to the "from null" bug
// libjs/bundle-loader.js's `importer === undefined` guard exists to avoid.
//
// Builtins are routed to the original loader before the edge-table lookup
// (see the same file), so the require('path') above must never produce a
// miss line of its own: a log line for every non-bundled require would
// make the log useless for spotting real fallbacks.
// RUN: env HERMES_NODE_DEBUG_NATIVE=BUNDLE %hermes-node --bundle=%t.tree/app.bundle 2>&1 | %FileCheck --check-prefix=MISS %s
// MISS: [bundle] miss: ./dyn from cli.js
// MISS-NOT: miss: path

// The build says so as well, and before any of that: the scanner knows it
// gave up on this require(), and saying nothing about it is what let a
// container with a hole in it look complete. Counted rather than listed by
// default (a large tree has many, and burying the actionable warnings under
// them would cost more than the positions are worth); the positions are
// under --verbose, pinned in test/bundle-verbose.js. The plural forms are
// asserted here because a count of one is the common case and "1 calls" is
// what a missing agreement looks like.
// RUN: %hermes-node --build-bundle=%t.tree/again.bundle %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=DYNWARN %s
// DYNWARN: warning: 1 computed require() call in 1 file: not packaged, resolved from disk at run time

// A literal require() is followed, so it must never be counted as one: a
// warning that fires for every module would say nothing. The same run also
// pins that the module wrapper's own `require` parameter is not reported as
// a use of require -- it is a declaration, and reporting it would fire in
// every module ever built.
// RUN: rm -rf %t.static && mkdir -p %t.static
// RUN: echo "module.exports = { v: 1 };" > %t.static/dep.js
// RUN: echo "console.log('STATIC', require('./dep').v);" > %t.static/cli.js
// RUN: %hermes-node --build-bundle=%t.static/app.bundle %t.static/cli.js 2>&1 | %FileCheck --check-prefix=NODYN --implicit-check-not="computed require" --implicit-check-not="require used as a value" %s
// NODYN: bundle root:

// `require` passed somewhere instead of called. Nothing it goes on to load
// can be discovered -- there is no require() call in the source at all --
// so the count is all the build can offer. @babel/core does exactly this,
// which is what makes a bundle of it quietly need node_modules on disk.
// RUN: rm -rf %t.escape && mkdir -p %t.escape
// RUN: echo "module.exports = { v: 4 };" > %t.escape/dep.js
// RUN: echo "const load = (function (r) { return r; })(require); console.log('ESCAPE', load('./dep').v);" > %t.escape/cli.js
// RUN: %hermes-node --build-bundle=%t.escape/app.bundle %t.escape/cli.js 2>&1 | %FileCheck --check-prefix=ESCWARN %s
// ESCWARN: warning: require used as a value in 1 place in 1 file: whatever it goes on to load is not packaged
// RUN: %hermes-node --bundle=%t.escape/app.bundle | %FileCheck --check-prefix=ESCEXEC %s
// ESCEXEC: ESCAPE 4

// Reading a property of require loads nothing, so it is not a use the
// build has to warn about. Without this, require.resolve() and
// require.cache -- both ordinary -- would produce a warning each.
// RUN: rm -rf %t.prop && mkdir -p %t.prop
// RUN: echo "module.exports = { v: 6 };" > %t.prop/dep.js
// RUN: echo "require.resolve('./dep'); void require.cache; console.log('PROP', require('./dep').v);" > %t.prop/cli.js
// RUN: %hermes-node --build-bundle=%t.prop/app.bundle %t.prop/cli.js 2>&1 | %FileCheck --check-prefix=PROP --implicit-check-not="require used as a value" %s
// PROP: bundle root:

// Testing whether require exists is not using it: `typeof require` and an
// equality comparison against it yield a boolean and can load nothing. The
// idiom is in essentially every UMD-flavored file a package ships, so
// reporting it drowns the real ones -- yargs reported five escapes before
// this and one of them was real.
// RUN: rm -rf %t.guard && mkdir -p %t.guard
// RUN: echo "module.exports = { v: 3 };" > %t.guard/dep.js
// RUN: echo "if (typeof require !== 'undefined' && null !== require) { console.log('GUARD', require('./dep').v); }" > %t.guard/cli.js
// RUN: %hermes-node --build-bundle=%t.guard/app.bundle %t.guard/cli.js 2>&1 | %FileCheck --check-prefix=GUARD --implicit-check-not="require used as a value" %s
// GUARD: bundle root:
// RUN: rm -f %t.guard/cli.js %t.guard/dep.js
// RUN: %hermes-node --bundle=%t.guard/app.bundle | %FileCheck --check-prefix=GUARDEXEC %s
// GUARDEXEC: GUARD 3

// A module that declares its own `require` is not talking about the
// module's require: its specifiers were only ever meaningful inside
// whatever bundle produced it, and looking for them on disk is how a
// pre-bundled dist/ file used to fail the build. Matching the binding
// rather than the name is what tells the two apart.
// RUN: rm -rf %t.shadow && mkdir -p %t.shadow
// RUN: echo "(function (require) { require('./only-inside-that-bundle'); })(function () {});" > %t.shadow/dep.js
// RUN: echo "module.exports = { v: 8 };" >> %t.shadow/dep.js
// RUN: echo "console.log('SHADOW', require('./dep').v);" > %t.shadow/cli.js
// RUN: %hermes-node --build-bundle=%t.shadow/app.bundle %t.shadow/cli.js 2>&1 | %FileCheck --check-prefix=SHADOW --implicit-check-not="cannot be resolved" --implicit-check-not="require used as a value" %s
// SHADOW: bundle root:
// RUN: %hermes-node --bundle=%t.shadow/app.bundle | %FileCheck --check-prefix=SHADOWEXEC %s
// SHADOWEXEC: SHADOW 8

// A module must not exist twice, once from the container and once from disk.
// The fallback resolves through Node's loader, which checks Module._cache, so
// a bundled module has to be published there under its filename -- and
// loadIdentity has to consult Module._cache for the same reason in reverse.
// Without both halves the two importers below get two independent copies:
// module-level state, singletons and instanceof all break silently across
// that boundary, and this is the configuration the design expects to be
// common, since .node addons and computed require()s take the fallback.
//
// The tree stays in place here, because the fallback is the whole point: the
// computed specifier ('./state' + '.js') is invisible to the edge table and
// is resolved and compiled from disk.
// RUN: rm -rf %t.single && mkdir -p %t.single
// RUN: echo "module.exports = { n: 0 };" > %t.single/state.js
// RUN: echo "const s1 = require('./state'); s1.n = 41; const s2 = require('./state' + '.js'); console.log('SINGLE', s1 === s2, s2.n);" > %t.single/cli.js
// RUN: %hermes-node --build-bundle=%t.single/app.bundle %t.single/cli.js
// RUN: %hermes-node --bundle=%t.single/app.bundle | %FileCheck --check-prefix=SINGLE %s
// SINGLE: SINGLE true 41

// The same thing in the other order: the disk copy is instantiated first,
// through the fallback, and the bundled edge that follows must hand back
// that copy rather than instantiating the container's.
// RUN: rm -rf %t.rev && mkdir -p %t.rev
// RUN: echo "module.exports = { n: 0 };" > %t.rev/state.js
// RUN: echo "const s1 = require('./state' + '.js'); s1.n = 7; const s2 = require('./state'); console.log('REVERSE', s1 === s2, s2.n);" > %t.rev/cli.js
// RUN: %hermes-node --build-bundle=%t.rev/app.bundle %t.rev/cli.js
// RUN: %hermes-node --bundle=%t.rev/app.bundle | %FileCheck --check-prefix=REVERSE %s
// REVERSE: REVERSE true 7
