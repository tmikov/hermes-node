// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The `require` a bundled module is handed must be Node's own -- the same
// object internal/modules/helpers.makeRequireFunction builds for a module
// compiled from disk. Identical source must not mean different semantics
// depending on whether the producer happened to package the file.
//
// The one deliberate difference is require.resolve, which answers from the
// container -- edge table first, then the container's resolver -- so it
// keeps answering after the source tree is gone, and throws
// MODULE_NOT_FOUND rather than consulting a filesystem when neither can
// place the specifier. See libjs/bundle-loader.js.

// RUN: rm -rf %t.req && mkdir -p %t.req/lib %t.req/node_modules/pkg
// RUN: echo "module.exports = { v: 1 };" > %t.req/lib/dep.js
// RUN: echo '{ "main": "main.js" }' > %t.req/node_modules/pkg/package.json
// RUN: echo "module.exports = { v: 2 };" > %t.req/node_modules/pkg/main.js
// RUN: echo "const dep = require('./lib/dep');" > %t.req/cli.js
// RUN: echo "require('pkg');" >> %t.req/cli.js
// RUN: echo "console.log('RESOLVE', require.resolve('./lib/dep'));" >> %t.req/cli.js
// RUN: echo "console.log('PKGDIR', require('path').dirname(require.resolve('pkg')));" >> %t.req/cli.js
// RUN: echo "console.log('BUILTIN', require.resolve('path'));" >> %t.req/cli.js
// RUN: echo "console.log('SHAPE', typeof require.cache, typeof require.extensions, typeof require.resolve.paths, typeof module.require);" >> %t.req/cli.js
// RUN: echo "console.log('MODREQ', module.require('./lib/dep') === dep);" >> %t.req/cli.js
// RUN: echo "console.log('INCACHE', require.cache[require.resolve('./lib/dep')].exports === dep);" >> %t.req/cli.js
// RUN: echo "console.log('PATHS', Array.isArray(require.resolve.paths('pkg')));" >> %t.req/cli.js
// RUN: %hermes-node --build-bundle=%t.req/app.bundle %t.req/cli.js

// Deleting the tree is the point: require.resolve('./lib/dep') has to answer
// with the file's real path, which only the edge table still knows. The old
// stub returned the specifier text itself ('./lib/dep'), so
// path.dirname(require.resolve('pkg')) -- the standard way to find a package
// root, used by examples/bufferutil-addon/mask.js -- yielded '.' and sent
// callers looking in the cwd.
// RUN: find %t.req -name '*.js' -delete && rm -rf %t.req/node_modules
// RUN: %hermes-node --bundle=%t.req/app.bundle | %FileCheck %s
// CHECK: RESOLVE {{.*}}.req/lib/dep.js
// CHECK: PKGDIR {{.*}}.req/node_modules/pkg
// A builtin is answered by the real resolver, which returns the bare name.
// CHECK: BUILTIN path
// CHECK: SHAPE object object function function
// CHECK: MODREQ true
// CHECK: INCACHE true
// CHECK: PATHS true

// module.children must list every module this one required, including the
// ones that were already in the cache: Node's updateChildren() runs on the
// cache-hit path too, so the second importer of a shared module still lists
// it. Both a.js and b.js require the same file, and b.js is the one that
// hits the cache.
//
// The entry's own count pins the other half of Node's throw handling: a
// module that throws is spliced out of its importer's children as well as
// deleted from the cache, so the three failed requires below leave the
// entry with exactly its two real children. Without the splice each attempt
// adds a dead loaded:false record and the count reads 5.
// RUN: rm -rf %t.kids && mkdir -p %t.kids
// RUN: echo "require('./shared'); module.exports = { kids: module.children.length };" > %t.kids/a.js
// RUN: echo "require('./shared'); module.exports = { kids: module.children.length };" > %t.kids/b.js
// RUN: echo "module.exports = {};" > %t.kids/shared.js
// RUN: echo "throw new Error('boom');" > %t.kids/bad.js
// RUN: echo "const a = require('./a'); const b = require('./b'); function t() { try { require('./bad'); } catch (e) {} } t(); t(); t(); console.log('KIDS', a.kids, b.kids, module.children.length);" > %t.kids/cli.js
// RUN: %hermes-node --build-bundle=%t.kids/app.bundle %t.kids/cli.js
// RUN: find %t.kids -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.kids/app.bundle | %FileCheck --check-prefix=KIDS %s
// KIDS: KIDS 1 1 2

// `delete require.cache[require.resolve(x)]` followed by a fresh require()
// is the standard reload idiom, and it reaches only Module._cache. That is
// why Module._cache is the loader's only cache: a private identity-keyed one
// would still hold the record, so the reload would silently hand back the
// stale module -- worse than the TypeError it used to throw when
// require.cache did not exist at all. Node and hermes-node from disk both
// print "RELOAD 1 2 false" here.
//
// The tree is deleted first, so the re-execution demonstrably comes from the
// container rather than from a fresh disk compile.
// RUN: rm -rf %t.reload && mkdir -p %t.reload
// RUN: echo "globalThis.__hits = (globalThis.__hits || 0) + 1; module.exports = { hit: globalThis.__hits };" > %t.reload/state.js
// RUN: echo "const a = require('./state'); delete require.cache[require.resolve('./state')]; const b = require('./state'); console.log('RELOAD', a.hit, b.hit, a === b);" > %t.reload/cli.js
// RUN: %hermes-node --build-bundle=%t.reload/app.bundle %t.reload/cli.js
// RUN: find %t.reload -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.reload/app.bundle | %FileCheck --check-prefix=RELOAD %s
// RELOAD: RELOAD 1 2 false

// A program can put anything in require.cache, and Node's _load tests the
// slot with `!== undefined` rather than for truthiness. `require.cache[f] =
// null` followed by require(f) therefore throws a TypeError under Node and
// from disk; a truthiness test here would quietly re-instantiate from the
// container instead, which is a wrong answer where Node gives an error.
// RUN: rm -rf %t.sentinel && mkdir -p %t.sentinel
// RUN: echo "module.exports = { v: 1 };" > %t.sentinel/dep.js
// RUN: echo "const f = require.resolve('./dep'); require('./dep'); require.cache[f] = null; var out; try { require('./dep'); out = 'NO THROW'; } catch (e) { out = e.constructor.name; } console.log('SENTINEL', out);" > %t.sentinel/cli.js
// RUN: %hermes-node --build-bundle=%t.sentinel/app.bundle %t.sentinel/cli.js
// RUN: find %t.sentinel -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.sentinel/app.bundle | %FileCheck --check-prefix=SENTINEL %s
// SENTINEL: SENTINEL TypeError

// A circular require must terminate and hand the second module the first
// one's partially populated exports, which is what publishing the record
// before the body runs buys. Pinned here because it is a property of the
// cache, and the cache has been reshaped twice.
// RUN: rm -rf %t.cycle && mkdir -p %t.cycle
// RUN: echo "exports.name = 'A'; const b = require('./b'); exports.sawB = b.name;" > %t.cycle/a.js
// RUN: echo "const a = require('./a'); exports.name = 'B'; exports.sawAPartial = a.name; exports.sawASaw = a.sawB;" > %t.cycle/b.js
// RUN: echo "const a = require('./a'); console.log('CYCLE', a.name, a.sawB, require('./b').sawAPartial, require('./b').sawASaw);" > %t.cycle/cli.js
// RUN: %hermes-node --build-bundle=%t.cycle/app.bundle %t.cycle/cli.js
// RUN: find %t.cycle -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.cycle/app.bundle | %FileCheck --check-prefix=CYCLE %s
// CYCLE: CYCLE A B A undefined

// require.resolve(request, { paths }) is the caller replacing the search
// path outright -- a different question from the one the edge table answers,
// which is where THIS importer's specifier resolved at build time. Node
// honours the option, so the edge table is skipped when one is present and
// the container's resolver runs the option's own algorithm instead: resolve
// as if from each entry in turn, first hit wins.
//
// In a closed world that resolver is the last word. It used to be bounded
// to each entry's own nearest node_modules, because a climb past an
// ancestor that merely had not been PACKAGED could land on a different,
// real package of the same name while the disk fallback behind it knew
// better. There is no such thing as "merely unpackaged" now: the far copy
// below is not in the container, so it does not exist, and the walk climbs
// past its directory exactly as Node's would past one that is not there --
// landing on the near copy, which IS in the container. Answering NEAR here
// is the closed world's correct answer, and the old FAR expectation was
// the disk's.
//
// The whole tree is deleted before the run, so neither answer can be the
// filesystem's, and a specifier the container cannot place under any paths
// throws rather than falling through to a resolver that would go looking.
// RUN: rm -rf %t.optpaths && mkdir -p %t.optpaths/node_modules/pkg %t.optpaths/other/node_modules/pkg
// RUN: echo "module.exports = { who: 'NEAR' };" > %t.optpaths/node_modules/pkg/index.js
// RUN: echo "module.exports = { who: 'FAR' };" > %t.optpaths/other/node_modules/pkg/index.js
// RUN: echo "const p = require('pkg'); var miss; try { require.resolve('ghost-pkg', { paths: [__dirname + '/other'] }); miss = 'NO THROW'; } catch (e) { miss = e.code; } console.log('OPTPATHS', p.who, require.resolve('pkg', { paths: [__dirname + '/other'] }), miss);" > %t.optpaths/cli.js
// RUN: %hermes-node --build-bundle=%t.optpaths/app.bundle %t.optpaths/cli.js
// RUN: rm -rf %t.optpaths/node_modules %t.optpaths/other %t.optpaths/cli.js
// RUN: %hermes-node --bundle=%t.optpaths/app.bundle | %FileCheck --check-prefix=OPTPATHS %s
// OPTPATHS: OPTPATHS NEAR {{.*}}.optpaths/node_modules/pkg/index.js MODULE_NOT_FOUND

// A `paths` that is present but not an array is neither "absent" (use the
// edge table/container) nor "a real search list" (use it) -- it is
// malformed, and Node's own Module._resolveFilename throws
// ERR_INVALID_ARG_VALUE for it before ever touching a filesystem. That
// validation must still happen for a bundled module: makeRequire's
// resolve() must not mistake a malformed paths for "no paths" and quietly
// answer from the container or the edge table instead of raising. The tree
// is deleted before the run: the throw does not depend on anything being
// on disk, so a pass here cannot be masked by the tree still being present.
// RUN: rm -rf %t.badpaths && mkdir -p %t.badpaths/node_modules/pkg
// RUN: echo "module.exports = {};" > %t.badpaths/node_modules/pkg/index.js
// RUN: echo "try { require.resolve('pkg', { paths: 'not-an-array' }); console.log('BADPATHS NO THROW'); } catch (e) { console.log('BADPATHS', e.constructor.name, e.code); }" > %t.badpaths/cli.js
// RUN: %hermes-node --build-bundle=%t.badpaths/app.bundle %t.badpaths/cli.js
// RUN: rm -rf %t.badpaths/node_modules %t.badpaths/cli.js
// RUN: %hermes-node --bundle=%t.badpaths/app.bundle | %FileCheck --check-prefix=BADPATHS %s
// BADPATHS: BADPATHS TypeError ERR_INVALID_ARG_VALUE

// A non-string request is Node's to reject. The closed world throws
// MODULE_NOT_FOUND where nothing can place a specifier, and 123 is
// certainly not in the container -- but "not found" is the wrong answer to
// "that is not a specifier", and it is the answer this loader would invent
// if it tried to place the argument before checking it. Node's own
// validateString() lives inside the require.resolve makeRequireFunction
// builds, so reaching it means passing the request through untouched.
// RUN: rm -rf %t.argtype && mkdir -p %t.argtype
// RUN: echo "try { require(123); } catch (e) { console.log('ARGT', e.code); } try { require.resolve(123); } catch (e) { console.log('ARGR', e.code); }" > %t.argtype/cli.js
// RUN: %hermes-node --build-bundle=%t.argtype/app.bundle %t.argtype/cli.js
// RUN: find %t.argtype -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.argtype/app.bundle | %FileCheck --check-prefix=ARGTYPE %s
// ARGTYPE: ARGT ERR_INVALID_ARG_TYPE
// ARGTYPE: ARGR ERR_INVALID_ARG_TYPE

// This file is a lit driver only; the RUN lines above are the test.
