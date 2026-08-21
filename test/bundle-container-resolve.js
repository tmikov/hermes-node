// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A computed require() has no edge, so it used to go straight to disk.
// The container can answer it instead, with the same algorithm the
// producer used. The tree is deleted before the run, so a pass here cannot
// be the disk fallback answering.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/node_modules/dep
// RUN: echo '{ "main": "entry.js" }' > %t.tree/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 6 };" > %t.tree/node_modules/dep/entry.js
//
// pull.js names `dep` in a branch the run never takes. That is what puts it
// in the container -- the walk follows every literal require() whether or
// not the run reaches it -- while the only require that actually executes
// is the computed one in cli.js, which has no edge and must therefore be
// answered by the container's resolver.
// RUN: echo "if (globalThis.never) { require('dep'); }" > %t.tree/pull.js
// RUN: echo "require('./pull'); const n = 'd' + 'ep'; console.log('DYN', require(n).v);" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
//
// The tree goes before the run, so a pass here cannot be the disk fallback.
// RUN: rm -rf %t.tree/node_modules %t.tree/cli.js %t.tree/pull.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=DYN %s
// DYN: DYN 6


// The plain (no-`paths`) container resolve must not be bounded to a single
// node_modules level: on an ordinary hoisted npm tree, a package's own
// dependencies live in an ANCESTOR's node_modules, not a nested one of its
// own, so answering only the nearest level would make the container answer
// essentially nothing on real trees. node_modules/a/index.js does a
// computed require('b'), and 'b' sits at the tree root's node_modules, two
// levels up from node_modules/a (climbing past node_modules/a's own,
// nonexistent, nested node_modules and skipping the node_modules segment
// itself, exactly like Module._nodeModulePaths). pull2.js's dead branch is
// what puts 'b' in the container, the same trick as DYN above. The tree is
// deleted before the run.
// RUN: rm -rf %t.hoist && mkdir -p %t.hoist/node_modules/a %t.hoist/node_modules/b
// RUN: echo '{ "main": "index.js" }' > %t.hoist/node_modules/a/package.json
// RUN: echo "const n = 'b'; module.exports = require(n);" > %t.hoist/node_modules/a/index.js
// RUN: echo '{ "main": "index.js" }' > %t.hoist/node_modules/b/package.json
// RUN: echo "module.exports = { v: 9 };" > %t.hoist/node_modules/b/index.js
// RUN: echo "if (globalThis.never) { require('b'); }" > %t.hoist/pull2.js
// RUN: echo "require('./pull2'); console.log('HOIST', require('a').v);" > %t.hoist/cli.js
// RUN: %hermes-node --build-bundle=%t.hoist/app.hbb %t.hoist/cli.js
// RUN: rm -rf %t.hoist/node_modules %t.hoist/cli.js %t.hoist/pull2.js
// RUN: %hermes-node --bundle=%t.hoist/app.hbb | %FileCheck --check-prefix=HOIST %s
// HOIST: HOIST 9

// The `paths` form of the container resolve must be able to answer a hit,
// not just decline (test/bundle-require.js's OPTPATHS only exercises the
// decline direction, where the container's bounded search intentionally
// misses so the disk fallback -- still present, tree still on disk there --
// finds the real answer). Here `dep` is packaged only under extra/, reached
// by an explicit `paths` entry that names extra/ itself: the container's
// single-level probe from that entry hits directly. pull3.js's dead branch
// is what puts extra/node_modules/dep in the container in the first place,
// same trick as above. The tree is deleted before the run, so the resolved
// path can only have come from the container.
// RUN: rm -rf %t.pathhit && mkdir -p %t.pathhit/extra/node_modules/dep
// RUN: echo '{ "main": "entry.js" }' > %t.pathhit/extra/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 11 };" > %t.pathhit/extra/node_modules/dep/entry.js
// RUN: echo "if (globalThis.never) { require('dep'); }" > %t.pathhit/extra/pull3.js
// RUN: echo "require('./extra/pull3'); console.log('PATHSHIT', require.resolve('dep', { paths: [__dirname + '/extra'] }));" > %t.pathhit/cli.js
// RUN: %hermes-node --build-bundle=%t.pathhit/app.hbb %t.pathhit/cli.js
// RUN: rm -rf %t.pathhit/extra %t.pathhit/cli.js
// RUN: %hermes-node --bundle=%t.pathhit/app.hbb | %FileCheck --check-prefix=PATHSHIT %s
// PATHSHIT: PATHSHIT {{.*}}.pathhit/extra/node_modules/dep/entry.js
