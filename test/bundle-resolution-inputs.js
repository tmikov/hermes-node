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

// A package.json read while probing a specifier that never resolved is not
// a reason to move the bundle root. Here `foo` resolves to nothing (its
// package.json main points at a file that does not exist), and it sits
// OUTSIDE the directory every packaged module shares -- so before this it
// pulled the root up a level and changed every identity in the container.
// RUN: rm -rf %t.wide && mkdir -p %t.wide/app %t.wide/node_modules/foo
// RUN: echo '{ "main": "nope.js" }' > %t.wide/node_modules/foo/package.json
// RUN: echo "module.exports = { v: 1 };" > %t.wide/app/dep.js
// RUN: echo "try { require('foo'); } catch (e) {} console.log('W', require('./dep').v);" > %t.wide/app/cli.js
// RUN: %hermes-node --build-bundle=%t.wide/app/app.hbb %t.wide/app/cli.js 2>&1 | %FileCheck --check-prefix=WIDE %s
// WIDE: bundle root: {{.*}}/app
//
// And the identities stay relative to that root. Anchored to the start of
// the identity column -- the byte count and the padding before it -- since
// a bare `cli.js` is a substring of the `app/cli.js` this case exists to
// rule out, and so would pass whether the bug was fixed or not.
// RUN: %hermes-node --bundle=%t.wide/app/app.hbb --dump | %FileCheck --check-prefix=WIDEDUMP %s
// WIDEDUMP: {{[0-9]+ +cli\.js$}}

// The flip side: a package.json outside the modules-only common ancestor
// is not always safe to drop. Here every packaged module lives under
// node_modules/foo/lib/, so node_modules/foo/package.json sits one level
// ABOVE all of them -- an ancestor of every packaged module's directory,
// not of none -- and dropping it unconditionally (the first cut of this
// fix) would leave the container unable to answer a dynamic
// require.resolve('foo', {paths}) lookup, which does not go through the
// edge table and needs foo's package.json "main" to find it by name.
// Widening the root to node_modules/foo/ is exactly what makes that
// lookup work; a plain require('./helper') needs none of this, since a
// static edge answers it without consulting any package.json again.
// RUN: rm -rf %t.deep && mkdir -p %t.deep/node_modules/foo/lib
// RUN: echo '{ "main": "lib/index.js" }' > %t.deep/node_modules/foo/package.json
// RUN: echo "module.exports = { v: 7 };" > %t.deep/node_modules/foo/lib/helper.js
// RUN: echo "var path = require('path'); var dir = path.join(__dirname, '..', '..', '..'); var resolved = require.resolve('foo', { paths: [dir] }); console.log('DEEP', resolved === __filename, require('./helper').v);" > %t.deep/node_modules/foo/lib/index.js
// RUN: %hermes-node --build-bundle=%t.deep/node_modules/foo/app.hbb %t.deep/node_modules/foo/lib/index.js 2>&1 | %FileCheck --check-prefix=DEEPROOT %s
// DEEPROOT: bundle root: {{.*}}/foo{{$}}
//
// The source tree is gone; only the container can answer the dynamic,
// paths-qualified lookup of `foo` by name.
// RUN: rm -rf %t.deep/node_modules/foo/lib %t.deep/node_modules/foo/package.json
// RUN: %hermes-node --bundle=%t.deep/node_modules/foo/app.hbb | %FileCheck --check-prefix=DEEPEXEC %s
// DEEPEXEC: DEEP true 7
