// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --include packages what static discovery cannot see. The dependency here
// is named by a string the program assembles, which is the shape Babel's
// preset loading has, and nothing in the source mentions it.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/node_modules/late
// RUN: echo '{ "main": "index.js" }' > %t.tree/node_modules/late/package.json
// RUN: echo "module.exports = { v: 11 };" > %t.tree/node_modules/late/index.js
// RUN: echo "const n = 'la' + 'te'; console.log('LATE', require(n).v);" > %t.tree/cli.js

// Without it, the module is simply not there.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/plain.hbb --dump | %FileCheck --check-prefix=WITHOUT --implicit-check-not=late %s
// WITHOUT: MODULES

// A bare specifier's suggestion is printed as written, with a line saying
// where --include resolves from -- not because that line is needed here
// (this is the flagship case: 'late' sits at the root node_modules, and
// --include=late is exactly right), but because the same code path prints
// an unfollowable bare suggestion when the importer is nested (see the
// %t.nest case below), so the message never claims more than it knows.
// RUN: %not %hermes-node --bundle=%t.tree/plain.hbb 2>&1 | %FileCheck --check-prefix=BARERR %s
// BARERR: Cannot find module 'late'
// BARERR-NEXT: required by cli.js
// BARERR-NEXT: Not in the bundle. Add it with:
// BARERR-NEXT: --include=late
// BARERR-NEXT: (--include resolves from the entry's directory.)

// With it, the module and its package.json are packaged, and the computed
// require finds them with the tree deleted.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --include=late %t.tree/cli.js
// RUN: rm -rf %t.tree/node_modules %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=WITH %s
// WITH: LATE 11

// An --include that does not resolve is a build error: the user named this
// one explicitly, so silence would be wrong.
// RUN: rm -rf %t.bad && mkdir -p %t.bad
// RUN: echo "console.log('x');" > %t.bad/cli.js
// RUN: %not %hermes-node --build-bundle=%t.bad/app.hbb --include=ghost %t.bad/cli.js 2>&1 | %FileCheck --check-prefix=BADINC %s
// BADINC: error: --include=ghost cannot be resolved

// The --include the not-found error prints has to be one that can be
// copied onto a command line. --include resolves from the ENTRY's
// directory, so a relative request from an importer somewhere else in the
// tree cannot be echoed back as written: require('./helper') inside
// node_modules/foo/index.js used to print `--include=./helper`, which
// fails the very next build with "--include=./helper cannot be resolved".
// This is the common shape, not an exotic one -- the design's worked
// example is a bare specifier, which is why it went unnoticed.
// RUN: rm -rf %t.rel && mkdir -p %t.rel/node_modules/foo
// RUN: echo '{ "main": "index.js" }' > %t.rel/node_modules/foo/package.json
// RUN: echo "module.exports = { v: 42 };" > %t.rel/node_modules/foo/helper.js
// RUN: echo "const n = 'hel' + 'per'; module.exports = require('./' + n);" > %t.rel/node_modules/foo/index.js
// RUN: echo "console.log('REL', require('foo').v);" > %t.rel/cli.js
// RUN: %hermes-node --build-bundle=%t.rel/app.hbb %t.rel/cli.js 2>&1 | %FileCheck --check-prefix=RELWARN %s
// RELWARN: warning: 1 computed require()/require.resolve() call
// RUN: %not %hermes-node --bundle=%t.rel/app.hbb 2>&1 | %FileCheck --check-prefix=RELERR %s
// RELERR: Cannot find module './helper'
// RELERR-NEXT: required by node_modules/foo/index.js
// RELERR-NEXT: Not in the bundle. Add it with:
// RELERR-NEXT: --include=./node_modules/foo/helper

// And that exact value, run. The two halves are the whole point: the
// message is only correct if the build it suggests succeeds and the
// program then works with the tree deleted.
// RUN: %hermes-node --build-bundle=%t.rel/fixed.hbb --include=./node_modules/foo/helper %t.rel/cli.js 2>&1 | %FileCheck --check-prefix=RELWARN %s
// RUN: rm -rf %t.rel/node_modules %t.rel/cli.js
// RUN: %hermes-node --bundle=%t.rel/fixed.hbb | %FileCheck --check-prefix=RELFIX %s
// RELFIX: REL 42

// A bare specifier's suggestion is unfollowable when the importer sits
// under a NESTED node_modules. Node's own resolution walks node_modules
// directories starting at the importer and climbing, so a dependency of a
// dependency can be shadowed one level down from where --include (entry-
// relative) would look for it: 'baz' is required by
// node_modules/foo/index.js, but the copy that satisfies it lives at
// node_modules/foo/node_modules/baz, not at the root. --include=baz -- the
// printed value -- only works when a copy sits in the ROOT node_modules,
// which is the flagship case above and not this one. The loader cannot
// compute the deeper value: the request never resolved, so there is no
// candidate directory to build it from. This is the case the old comment's
// "a bare specifier resolves the same way from any directory" got wrong.
// RUN: rm -rf %t.nest && mkdir -p %t.nest/node_modules/foo/node_modules/baz
// RUN: echo '{ "main": "index.js" }' > %t.nest/node_modules/foo/package.json
// RUN: echo '{ "main": "index.js" }' > %t.nest/node_modules/foo/node_modules/baz/package.json
// RUN: echo "module.exports = { v: 7 };" > %t.nest/node_modules/foo/node_modules/baz/index.js
// RUN: echo "const n = 'b' + 'az'; module.exports = require(n);" > %t.nest/node_modules/foo/index.js
// RUN: echo "console.log('NEST', require('foo').v);" > %t.nest/cli.js
// RUN: %hermes-node --build-bundle=%t.nest/plain.hbb %t.nest/cli.js 2>&1 | %FileCheck --check-prefix=NESTWARN %s
// NESTWARN: warning: 1 computed require()/require.resolve() call
// RUN: %not %hermes-node --bundle=%t.nest/plain.hbb 2>&1 | %FileCheck --check-prefix=NESTERR %s
// NESTERR: Cannot find module 'baz'
// NESTERR-NEXT: required by node_modules/foo/index.js
// NESTERR-NEXT: Not in the bundle. Add it with:
// NESTERR-NEXT: --include=baz
// NESTERR-NEXT: (--include resolves from the entry's directory.)

// Proof the caveat is load-bearing and not decorative: the printed value,
// built verbatim, is the same "--include=X cannot be resolved" build error
// as any other unfollowable suggestion. A message that let this through
// once must not be trusted on its text alone a second time.
// RUN: %not %hermes-node --build-bundle=%t.nest/bad.hbb --include=baz %t.nest/cli.js 2>&1 | %FileCheck --check-prefix=NESTBADINC %s
// NESTBADINC: error: --include=baz cannot be resolved

// The value that actually works, run with the tree deleted: join the
// request onto the importer's directory (node_modules/foo) and express the
// result relative to the entry's directory.
// RUN: %hermes-node --build-bundle=%t.nest/fixed.hbb --include=./node_modules/foo/node_modules/baz %t.nest/cli.js 2>&1 | %FileCheck --check-prefix=NESTWARN %s
// RUN: rm -rf %t.nest/node_modules %t.nest/cli.js
// RUN: %hermes-node --bundle=%t.nest/fixed.hbb | %FileCheck --check-prefix=NESTFIX %s
// NESTFIX: NEST 7

// A literal require.resolve() names a real dependency: its target is
// packaged like any other edge, so the call answers from the container with
// the tree deleted. Before this it resolved nothing and threw.
// RUN: rm -rf %t.rr && mkdir -p %t.rr
// RUN: echo '{ "v": 3 }' > %t.rr/data.json
// RUN: echo "const p = require.resolve('./data.json'); console.log('RR', require(p).v);" > %t.rr/cli.js
// RUN: %hermes-node --build-bundle=%t.rr/app.hbb %t.rr/cli.js
// RUN: rm -f %t.rr/cli.js %t.rr/data.json
// RUN: %hermes-node --bundle=%t.rr/app.hbb | %FileCheck --check-prefix=RREXEC %s
// RREXEC: RR 3

// --include dedup, in both directions: --include naming the same file
// twice, and --include naming something the entry's own graph already
// reaches. Both go through pathIndex (lib/bundle/bundle_build.cpp), the
// single discovery-order index shared by the --include seed loop and the
// require() walk, so a module ends up in the container's MODULES table
// exactly once no matter how many routes named it. --dump plus
// --implicit-check-not is what makes "exactly once" checkable: a single
// CHECK for the identity, guaranteed not to appear anywhere else in the
// output.

// Same file, two different --include spellings: a bare specifier and the
// resolved path it names.
// RUN: rm -rf %t.dedup && mkdir -p %t.dedup/node_modules/dup
// RUN: echo '{ "main": "index.js" }' > %t.dedup/node_modules/dup/package.json
// RUN: echo "module.exports = { v: 1 };" > %t.dedup/node_modules/dup/index.js
// RUN: echo "console.log('DEDUP');" > %t.dedup/cli.js
// RUN: %hermes-node --build-bundle=%t.dedup/app.hbb --include=dup --include=./node_modules/dup/index.js %t.dedup/cli.js
// RUN: %hermes-node --bundle=%t.dedup/app.hbb --dump | %FileCheck --check-prefix=DEDUP --implicit-check-not="node_modules/dup/index.js" %s
// DEDUP: node_modules/dup/index.js

// A file the entry's own literal require() already reaches, named again by
// --include: the walk (pass B, over the require() graph) and the --include
// seed loop feed the same pathIndex, so whichever one gets there first, the
// other reuses its slot instead of adding a second one. Run with the tree
// deleted too, so a container that quietly held two records for the same
// module -- which would still have worked here, since Module._cache is
// keyed by filename and both records would resolve to the identical path --
// cannot masquerade as correct; the --dump count is what actually catches
// that, not the run.
// RUN: rm -rf %t.dedup2 && mkdir -p %t.dedup2/node_modules/dup2
// RUN: echo '{ "main": "index.js" }' > %t.dedup2/node_modules/dup2/package.json
// RUN: echo "module.exports = { v: 2 };" > %t.dedup2/node_modules/dup2/index.js
// RUN: echo "console.log('DEDUP2', require('dup2').v);" > %t.dedup2/cli.js
// RUN: %hermes-node --build-bundle=%t.dedup2/app.hbb --include=dup2 %t.dedup2/cli.js
// RUN: %hermes-node --bundle=%t.dedup2/app.hbb --dump | %FileCheck --check-prefix=DEDUP2 --implicit-check-not="node_modules/dup2/index.js" %s
// DEDUP2: node_modules/dup2/index.js
// RUN: rm -rf %t.dedup2/node_modules %t.dedup2/cli.js
// RUN: %hermes-node --bundle=%t.dedup2/app.hbb | %FileCheck --check-prefix=DEDUP2EXEC %s
// DEDUP2EXEC: DEDUP2 2
