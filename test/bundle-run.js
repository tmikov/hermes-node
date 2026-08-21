// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib %t.tree/node_modules/dep
// The entry's output sits inside the standard `require.main === module`
// guard, so nothing below can pass unless the bundle's entry really is the
// main module. A CLI whose whole body sits in that guard is the normal case,
// not a corner one.
// RUN: echo "const d = require('dep'); const u = require('./lib/util'); const c = require('./cfg.json'); if (require.main === module && process.mainModule === module && module.id === '.') { console.log('MAIN OK'); } if (require.main === module) { console.log('SUM', d.v + u.v + c.v); }" > %t.tree/cli.js
// A non-entry module sees the same main module through its own require.
// RUN: echo "if (require.main && require.main.id === '.') { console.log('CHILD SEES MAIN'); } module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 100 }' > %t.tree/cfg.json
// RUN: echo '{ "main": "main.js" }' > %t.tree/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 10 };" > %t.tree/node_modules/dep/main.js
// RUN: %hermes-node --build-bundle=%t.tree/app.bundle %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.bundle | %FileCheck %s

// Delete every source file, keeping only the bundle, and run again. This is
// the test that actually demonstrates self-sufficiency.
// RUN: find %t.tree -name '*.js' -delete && find %t.tree -name '*.json' -delete
// RUN: %hermes-node --bundle=%t.tree/app.bundle | %FileCheck %s

// CHECK: CHILD SEES MAIN
// CHECK: MAIN OK
// CHECK: SUM 111

// A require() the edge table has no row for is answered by the container's
// resolver, from the requesting module's own identity -- so two bundled
// modules in different directories doing the SAME computed require must get
// two different modules. The specifier text is identical in both, which is
// what makes this a real test: anything that keys an answer on the request
// alone, rather than on (importer, request), hands the second importer the
// first one's module and nothing else notices. --include is how the two
// targets get into the container at all, since nothing names them
// literally. The tree is deleted before the run, so both answers can only
// have come from the container.
// RUN: rm -rf %t.coll && mkdir -p %t.coll/a %t.coll/b
// RUN: echo "console.log('RESULT', require('./a/mod').name, require('./b/mod').name);" > %t.coll/cli.js
// RUN: echo "module.exports = require('./' + 'dyn');" > %t.coll/a/mod.js
// RUN: echo "module.exports = require('./' + 'dyn');" > %t.coll/b/mod.js
// RUN: echo "module.exports = { name: 'A' };" > %t.coll/a/dyn.js
// RUN: echo "module.exports = { name: 'B' };" > %t.coll/b/dyn.js
// RUN: %hermes-node --build-bundle=%t.coll/app.bundle --include=./a/dyn --include=./b/dyn %t.coll/cli.js
// RUN: find %t.coll -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.coll/app.bundle | %FileCheck --check-prefix=COLLIDE %s
// COLLIDE: RESULT A B

// A computed require() of a BARE specifier is answered the same way, which
// means the container's resolver has to run the node_modules walk itself:
// there is no Module._resolveLookupPaths behind it any more and no disk to
// walk. The package's package.json is packaged too (the resolver reads
// "main" out of it), which is the format-v2 half of this working at all.
// RUN: rm -rf %t.bare && mkdir -p %t.bare/node_modules/dep2
// RUN: echo "const n = 'dep' + '2'; console.log('RESULT', require(n).v);" > %t.bare/cli.js
// RUN: echo '{ "main": "main.js" }' > %t.bare/node_modules/dep2/package.json
// RUN: echo "module.exports = { v: 42 };" > %t.bare/node_modules/dep2/main.js
// RUN: %hermes-node --build-bundle=%t.bare/app.bundle --include=dep2 %t.bare/cli.js
// RUN: rm -rf %t.bare/node_modules %t.bare/cli.js
// RUN: %hermes-node --bundle=%t.bare/app.bundle | %FileCheck --check-prefix=BARE %s
// BARE: RESULT 42

// A module must not exist twice. It used to be possible for one file to be
// instantiated once from the container and once from disk, so this pinned
// the two halves that prevented it; in a closed world both routes end at
// the same identity and the assertion holds trivially, which is exactly why
// it is worth keeping -- it is the statement that the two routes really did
// collapse into one. './state' has an edge; './state' + '.js' has none and
// goes through the container's resolver. Module-level state, singletons and
// instanceof all break silently if those produce two records.
// RUN: rm -rf %t.single && mkdir -p %t.single
// RUN: echo "module.exports = { n: 0 };" > %t.single/state.js
// RUN: echo "const s1 = require('./state'); s1.n = 41; const s2 = require('./state' + '.js'); console.log('SINGLE', s1 === s2, s2.n);" > %t.single/cli.js
// RUN: %hermes-node --build-bundle=%t.single/app.bundle %t.single/cli.js
// RUN: find %t.single -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.single/app.bundle | %FileCheck --check-prefix=SINGLE %s
// SINGLE: SINGLE true 41

// The same thing in the other order: the computed specifier is resolved
// first and instantiates the module, and the bundled edge that follows must
// hand back that copy rather than instantiating the container's a second
// time. The two orders reach Module._cache through different branches of
// the loader, so one does not cover the other.
// RUN: rm -rf %t.rev && mkdir -p %t.rev
// RUN: echo "module.exports = { n: 0 };" > %t.rev/state.js
// RUN: echo "const s1 = require('./state' + '.js'); s1.n = 7; const s2 = require('./state'); console.log('REVERSE', s1 === s2, s2.n);" > %t.rev/cli.js
// RUN: %hermes-node --build-bundle=%t.rev/app.bundle %t.rev/cli.js
// RUN: find %t.rev -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.rev/app.bundle | %FileCheck --check-prefix=REVERSE %s
// REVERSE: REVERSE true 7

// __dirname is a real directory under the bundle root, not a synthetic
// prefix -- that is the single reason the design rejected a virtual
// filesystem for module identities, so it needs a test. A module whose
// JavaScript is served from the container reads a data file that sits next
// to where its source used to be, with the source deleted and the process
// running from an unrelated cwd (lit's, not the bundle's), so nothing but
// __dirname could find it.
// RUN: rm -rf %t.assets && mkdir -p %t.assets/sub
// RUN: echo "ASSET-OK" > %t.assets/sub/data.txt
// RUN: echo "const fs = require('fs'); const path = require('path'); module.exports = { read: function() { return fs.readFileSync(path.join(__dirname, 'data.txt'), 'utf8').trim(); }, dir: __dirname };" > %t.assets/sub/reader.js
// RUN: echo "const r = require('./sub/reader'); console.log('ASSET', r.read(), r.dir !== process.cwd());" > %t.assets/cli.js
// RUN: %hermes-node --build-bundle=%t.assets/app.bundle %t.assets/cli.js
// RUN: find %t.assets -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.assets/app.bundle | %FileCheck --check-prefix=ASSET %s
// ASSET: ASSET ASSET-OK true

// A bundled module that throws while loading must not stay cached: requiring
// it again has to throw again, not hand back the empty exports of a module
// that never finished running. The tree is deleted before the run, so the
// SECOND line is the container re-running the module rather than a fresh
// disk compile of it -- with a fallback behind the loader, a cache entry
// that had been dropped could be re-satisfied off the disk and this would
// pass without the container ever being asked twice.
// RUN: rm -rf %t.throw && mkdir -p %t.throw
// RUN: echo "try { require('./boom'); } catch (e) { console.log('FIRST', e.message); } try { require('./boom'); } catch (e) { console.log('SECOND', e.message); } console.log('END');" > %t.throw/cli.js
// RUN: echo "throw new Error('boom');" > %t.throw/boom.js
// RUN: %hermes-node --build-bundle=%t.throw/app.bundle %t.throw/cli.js
// RUN: find %t.throw -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.throw/app.bundle | %FileCheck --check-prefix=THROW %s
// THROW: FIRST boom
// THROW: SECOND boom
// THROW: END

// __bundleLoad refuses a module packaged only so the resolver could read
// it (kRequirable clear -- here node_modules/dep/package.json, kept
// because it is an ancestor of the packaged entry.js but never itself
// require()d). bundle.lookup()'s edge-table hit and bundle.resolve()'s
// container hit never hand such an identity to require() in the first
// place -- the latter checks explicitly -- so this pins the guard at
// __bundleLoad itself, the point the bytes are actually handed out,
// reached directly rather than through either of its two normal callers.
// pull.js's dead branch is what packages dep's package.json and entry.js
// in the first place, the same trick test/bundle-container-resolve.js
// uses for the same reason.
// RUN: rm -rf %t.guard && mkdir -p %t.guard/node_modules/dep
// RUN: echo '{ "main": "entry.js" }' > %t.guard/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 1 };" > %t.guard/node_modules/dep/entry.js
// RUN: echo "if (globalThis.never) { require('dep'); }" > %t.guard/pull.js
// RUN: echo "require('./pull'); try { globalThis.__bundleLoad('node_modules/dep/package.json'); console.log('NOTHROWN'); } catch (e) { console.log('GUARD', e.message); }" > %t.guard/cli.js
// RUN: %hermes-node --build-bundle=%t.guard/app.bundle %t.guard/cli.js
// RUN: rm -rf %t.guard/node_modules %t.guard/cli.js %t.guard/pull.js
// RUN: %hermes-node --bundle=%t.guard/app.bundle | %FileCheck --check-prefix=GUARD %s
// GUARD: GUARD {{.*}}node_modules/dep/package.json
