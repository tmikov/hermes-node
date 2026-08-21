// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// What the producer's static scanner can and cannot follow, what it says
// about the difference, and what a bundle does at run time with the part it
// could not follow.
//
// This file used to be test/bundle-fallback.js, and its answer to that last
// question used to be "reads it off the disk". A bundle is a closed world
// now: a specifier neither the edge table nor the container's resolver can
// place is an error naming the importer and --include. The scanner cases
// below are unchanged -- what the scan reports is a property of the scan,
// not of the loader -- and the three that turned on the fallback are
// rewritten around the container.

// A require() the scanner cannot follow is counted. Counted rather than
// listed by default (a large tree has many, and burying the actionable
// warnings under them would cost more than the positions are worth); the
// positions are under --verbose, pinned in test/bundle-verbose.js. The
// singular forms are asserted here because a count of one is the common
// case and "1 calls" is what a missing agreement looks like.
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "module.exports = { v: 7 };" > %t.tree/dyn.js
// RUN: echo "const n = 'dyn' + ''; try { console.log('GOT', require('./' + n).v); } catch (e) { console.log('CAUGHT', e.code); } require('path');" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.bundle %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=DYNWARN %s
// DYNWARN: warning: 1 computed require()/require.resolve() call in 1 file: not packaged; answered at run time only if the container already holds the target, else --include it

// dyn.js is still sitting on the disk, right where the specifier names it,
// and that must make no difference: nothing outside the container is a
// source of module code. A miss is reported, and it names both the
// specifier and the bundled importer that asked for it -- pinning the
// importer is what would catch a regression to the "from null" bug
// libjs/bundle-loader.js's `importer === undefined` guard exists to avoid.
//
// The debug log traces all three outcomes a bundled require() can have,
// not only a miss: an edge-table hit (dep.js, required literally by
// cli.js), a container-resolve hit (extra.js, required literally only by
// holder.js -- so it is packaged, but cli.js's own require of it has no
// edge -- and reached by cli.js through a computed specifier instead), and
// a miss (dyn.js, named only by a computed require and packaged nowhere).
// All three matter for building an --include set: a resolve hit today is
// one dependency change away from becoming a miss, and there is no way to
// tell that apart from an edge-table hit without logging both.
//
// Builtins are routed to the original loader before either lookup runs
// (see the same file), so the require('path') below must never produce a
// line of its own under any of the three outcomes: a log line for every
// non-bundled require would make the log useless for spotting the real
// ones. The log is now the trace a --include list is built from, so that
// matters more than it did.
//
// --implicit-check-not, not a trailing MISS-NOT: a trailing CHECK-NOT is
// scanned only between the last positive match and EOF, so a regression
// that logged the builtin would emit its line BEFORE the ./dyn one and
// slip past, whatever order the requires are written in. The implicit
// form scans the whole output and does not depend on ordering at all.
// RUN: rm -rf %t.mixed && mkdir -p %t.mixed
// RUN: echo "module.exports = { v: 1 };" > %t.mixed/dep.js
// RUN: echo "module.exports = { v: 2 };" > %t.mixed/extra.js
// RUN: echo "module.exports = require('./extra');" > %t.mixed/holder.js
// RUN: echo "module.exports = { v: 7 };" > %t.mixed/dyn.js
// RUN: echo "require('./holder'); console.log('DEP', require('./dep').v); console.log('EXTRA', require('./ex' + 'tra').v); try { console.log('GOT', require('./dy' + 'n').v); } catch (e) { console.log('CAUGHT', e.code); } require('path');" > %t.mixed/cli.js
// RUN: %hermes-node --build-bundle=%t.mixed/app.bundle %t.mixed/cli.js
// RUN: env HERMES_NODE_DEBUG_NATIVE=BUNDLE %hermes-node --bundle=%t.mixed/app.bundle 2>&1 | %FileCheck --check-prefix=MISS --implicit-check-not="miss: path" --implicit-check-not="edge: path" --implicit-check-not="resolve: path" %s
// MISS: [bundle] edge: ./dep from cli.js -> dep.js{{$}}
// MISS: [bundle] resolve: ./extra from cli.js -> extra.js{{$}}
// MISS: [bundle] miss: ./dyn from cli.js{{$}}
// MISS: CAUGHT MODULE_NOT_FOUND

// The same specifier, with the remedy applied: --include puts the file in
// the container and the computed require() finds it there, with the tree
// deleted. The two runs together are the whole statement -- the failure
// says what to do, and doing it works.
// RUN: %hermes-node --build-bundle=%t.tree/fixed.bundle --include=./dyn %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=DYNWARN %s
// RUN: rm -f %t.tree/cli.js %t.tree/dyn.js
// RUN: %hermes-node --bundle=%t.tree/fixed.bundle | %FileCheck --check-prefix=FIXED --implicit-check-not=CAUGHT %s
// FIXED: GOT 7

// The uncaught form, which is what a user actually sees. The message is the
// deliverable of the closed world, so it is pinned in full: the specifier,
// the importer's identity (not a build-machine path -- a bundled module has
// no source on disk for a stack trace to point at), and the remedy spelled
// out as a flag that can be copied.
//
// './ghost' is what lib/mod.js wrote, but './lib/ghost' is what the flag
// has to say: --include resolves its value from the ENTRY's directory, not
// from the importer's, so echoing the request back would print an
// invocation that fails with "--include=./ghost cannot be resolved". The
// LOUDFIX run below is the point of pinning the exact text -- it copies
// this value onto a command line and requires it to work.
// RUN: rm -rf %t.loud && mkdir -p %t.loud/lib
// RUN: echo "module.exports = { v: 1 };" > %t.loud/lib/ghost.js
// RUN: echo "const n = 'gh' + 'ost'; module.exports = require('./' + n);" > %t.loud/lib/mod.js
// RUN: echo "console.log('LOUD', require('./lib/mod').v);" > %t.loud/cli.js
// RUN: %hermes-node --build-bundle=%t.loud/app.bundle %t.loud/cli.js 2>&1 | %FileCheck --check-prefix=LOUDWARN %s
// LOUDWARN: warning: 1 computed require()/require.resolve() call
// RUN: %not %hermes-node --bundle=%t.loud/app.bundle 2>&1 | %FileCheck --check-prefix=LOUD %s
// LOUD: Cannot find module './ghost'
// LOUD-NEXT: required by lib/mod.js
// LOUD-NEXT: Not in the bundle. Add it with:
// LOUD-NEXT: --include=./lib/ghost
//
// The advice, run verbatim. A suggestion that cannot be copy-pasted is
// worse than none, so the flag the message printed is the flag this builds
// with, and the tree is deleted before the run.
// RUN: %hermes-node --build-bundle=%t.loud/fixed.bundle --include=./lib/ghost %t.loud/cli.js 2>&1 | %FileCheck --check-prefix=LOUDWARN %s
// RUN: rm -rf %t.loud/lib %t.loud/cli.js
// RUN: %hermes-node --bundle=%t.loud/fixed.bundle | %FileCheck --check-prefix=LOUDFIX %s
// LOUDFIX: LOUD 1

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
// which is what makes a bundle of it need an --include or two.
// RUN: rm -rf %t.escape && mkdir -p %t.escape
// RUN: echo "module.exports = { v: 4 };" > %t.escape/dep.js
// RUN: echo "const load = (function (r) { return r; })(require); console.log('ESCAPE', load('./dep').v);" > %t.escape/cli.js
// RUN: %hermes-node --build-bundle=%t.escape/app.bundle --include=./dep %t.escape/cli.js 2>&1 | %FileCheck --check-prefix=ESCWARN %s
// ESCWARN: warning: require used as a value in 1 place in 1 file: whatever it goes on to load is not packaged
//
// The escaped require is still a real require: what --include put in the
// container is reached through it, with the tree gone. Named explicitly
// because an escape is the one case the scan cannot narrow down at all, so
// --include is the only answer there is.
// RUN: rm -f %t.escape/cli.js %t.escape/dep.js
// RUN: %hermes-node --bundle=%t.escape/app.bundle | %FileCheck --check-prefix=ESCEXEC %s
// ESCEXEC: ESCAPE 4

// Reading a property of require loads nothing, so it is not a use the
// build has to warn about. Without this, `require.cache` and any other
// property read -- all ordinary -- would each be reported as require
// escaping as a value. (`require.resolve('./dep')` is a recognized call
// shape now and contributes a discovery edge, so it is doubly not an
// escape; it stays in the case because the property read is what is
// being tested and a literal resolve is the commonest one.)
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
