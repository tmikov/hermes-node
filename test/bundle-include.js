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
// RELWARN: warning: 1 computed require() call
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
