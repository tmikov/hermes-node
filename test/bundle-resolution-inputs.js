// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The consumer resolves with the same algorithm the producer used, so it
// needs the same inputs: every package.json the producer read to answer a
// `main` has to be in the container. Nothing require()s these, so without
// this they were simply absent -- a container of the yargs example held
// zero package.json records.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/node_modules/dep
// RUN: echo '{ "main": "lib/entry.js" }' > %t.tree/node_modules/dep/package.json
// RUN: mkdir -p %t.tree/node_modules/dep/lib
// RUN: echo "module.exports = { v: 5 };" > %t.tree/node_modules/dep/lib/entry.js
// RUN: echo "console.log('GOT', require('dep').v);" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
// It is packaged for the resolver, not for the program: the dump marks it
// resolve-only in the kind column, which is what keeps
// require('dep/package.json') failing exactly where Node fails. The marker
// precedes the identity on the row, so this is one CHECK line, not a
// CHECK-SAME after the identity.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=DUMP %s
// DUMP: resolve-only{{.*}}node_modules/dep/package.json

// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=EXEC %s
// EXEC: GOT 5

// One package.json, one record -- even when the path the resolver used to
// reach it carried a trailing slash. require('..') is enough to produce
// one: lexically_normal() turns "<pkg>/lib/.." into "<pkg>/", and
// DiskFileSource then recorded "<pkg>//package.json", a string the
// producer's pathIndex does not recognize as the "<pkg>/package.json" it
// already had. Both paths normalize to one identity, so the container
// ended up with the same file packaged twice -- once requirable, once
// resolve-only.
//
// --implicit-check-not is the assertion: the two rows differed only in
// their kind column, so a positive CHECK for either one would have passed
// happily while both were there.
// RUN: rm -rf %t.dot && mkdir -p %t.dot/node_modules/pkg/lib
// RUN: echo '{ "main": "index.js" }' > %t.dot/node_modules/pkg/package.json
// RUN: echo "module.exports = { v: 3, sub: require('./lib/sub') };" > %t.dot/node_modules/pkg/index.js
// RUN: echo "module.exports = require('..');" > %t.dot/node_modules/pkg/lib/sub.js
// RUN: echo "console.log('DOT', require('pkg').v);" > %t.dot/cli.js
// RUN: %hermes-node --build-bundle=%t.dot/app.hbb %t.dot/cli.js
// RUN: %hermes-node --bundle=%t.dot/app.hbb --dump | %FileCheck --check-prefix=DOTDUMP --implicit-check-not="json{{.*}}node_modules/pkg/package.json" %s
// DOTDUMP: MODULES (4)
// DOTDUMP: json resolve-only{{.*}}node_modules/pkg/package.json

// RUN: rm -rf %t.dot/node_modules %t.dot/cli.js
// RUN: %hermes-node --bundle=%t.dot/app.hbb | %FileCheck --check-prefix=DOTEXEC %s
// DOTEXEC: DOT 3
