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

// A require() that misses the edge table falls back to disk, and must fall
// back the way it would with no bundle at all. Node's Module._load keys its
// relativeResolveCache on `${parent.path}\x00${request}`, so two bundled
// modules in different directories doing the same computed require must not
// collide: without a `path` on the module record both key as "undefined" and
// the second one is silently handed the first one's module. The bundle sits
// inside the tree because identities are resolved against the directory that
// holds it.
// RUN: rm -rf %t.coll && mkdir -p %t.coll/a %t.coll/b
// RUN: echo "console.log('RESULT', require('./a/mod').name, require('./b/mod').name);" > %t.coll/cli.js
// RUN: echo "module.exports = require('./' + 'dyn');" > %t.coll/a/mod.js
// RUN: echo "module.exports = require('./' + 'dyn');" > %t.coll/b/mod.js
// RUN: echo "module.exports = { name: 'A' };" > %t.coll/a/dyn.js
// RUN: echo "module.exports = { name: 'B' };" > %t.coll/b/dyn.js
// RUN: %hermes-node --build-bundle=%t.coll/app.bundle %t.coll/cli.js
// RUN: %hermes-node --bundle=%t.coll/app.bundle | %FileCheck --check-prefix=COLLIDE %s
// COLLIDE: RESULT A B

// A computed require() of a bare specifier falls back too, and has to search
// the importer's node_modules chain: Module._resolveLookupPaths reads
// parent.paths, so a record without one reaches only the global paths and
// the require throws.
// RUN: rm -rf %t.bare && mkdir -p %t.bare/node_modules/dep2
// RUN: echo "const n = 'dep' + '2'; console.log('RESULT', require(n).v);" > %t.bare/cli.js
// RUN: echo '{ "main": "main.js" }' > %t.bare/node_modules/dep2/package.json
// RUN: echo "module.exports = { v: 42 };" > %t.bare/node_modules/dep2/main.js
// RUN: %hermes-node --build-bundle=%t.bare/app.bundle %t.bare/cli.js
// RUN: %hermes-node --bundle=%t.bare/app.bundle | %FileCheck --check-prefix=BARE %s
// BARE: RESULT 42

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
// that never finished running.
// RUN: rm -rf %t.throw && mkdir -p %t.throw
// RUN: echo "try { require('./boom'); } catch (e) { console.log('FIRST', e.message); } try { require('./boom'); } catch (e) { console.log('SECOND', e.message); } console.log('END');" > %t.throw/cli.js
// RUN: echo "throw new Error('boom');" > %t.throw/boom.js
// RUN: %hermes-node --build-bundle=%t.throw/app.bundle %t.throw/cli.js
// RUN: %hermes-node --bundle=%t.throw/app.bundle | %FileCheck --check-prefix=THROW %s
// THROW: FIRST boom
// THROW: SECOND boom
// THROW: END
