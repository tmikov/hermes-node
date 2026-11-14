// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The "hermes-node:" specifier prefix resolves to a native module the
// embedder registered directly -- no embedded bytecode, no file on disk --
// which is what a later task uses to publish "hermes-node:ui". Exercised
// here through --test-embedder-module, which registers a fixed
// "hermes-node:test" module exporting { ok: true } for exactly this test.
//
// require() and require.resolve() must agree, exactly as they do for a real
// builtin (require.resolve('fs') === 'fs', no filesystem lookup either).
// The DECOY case below is the containment property that agreement buys: a
// same-named node_modules package must not win over the embedder module for
// either one. It is also a real reproduction, not a hypothetical -- the
// Task 5 review found require('hermes-node:test') returning the embedder
// module while require.resolve('hermes-node:test') returned the decoy's
// on-disk path, so require(require.resolve(x)) executed the wrong file.
//
// REQUIRES: ui-enabled
//
// The gate is the `ui-enabled` feature in test/lit.cfg, decided by running
// hermes-node itself with --test-embedder-module and checking that it
// actually works (RULING 37) -- not a build-provided flag lit merely
// trusted, which is what let this exact feature go undetectably UNSUPPORTED
// forever the first time that flag was tried and reverted. A build without
// HERMES_NODE_ENABLE_UI reports this UNSUPPORTED rather than failing it,
// keeping CLAUDE.md's "a build without the option is unaffected" true.

// RUN: %hermes-node --test-embedder-module %s | %FileCheck %s

var mod = require('hermes-node:test');
console.log('OK', mod.ok === true);

// require.resolve() must agree with require(): for a real builtin,
// require.resolve('fs') === 'fs', because Module._resolveFilename answers a
// builtin on its own first line with no filesystem lookup. An embedder
// module gets the same treatment -- the specifier text itself, never a
// path derived from probing node_modules.
console.log('RESOLVE', require.resolve('hermes-node:test') === 'hermes-node:test');

// The module is cached like any other: a second require() returns the exact
// same exports object rather than running the native init a second time.
console.log('CACHED', require('hermes-node:test') === mod);

// A name under the same prefix that nothing registered is a plain
// MODULE_NOT_FOUND, not a crash and not a silent undefined -- an optional-
// dependency probe (a try/catch around require()) has to see the code it
// expects, the same contract every other module kind gives it.
try {
  require('hermes-node:missing');
  console.log('MISSING unreachable');
} catch (e) {
  console.log('MISSING', e.code === 'MODULE_NOT_FOUND');
}

// RULING 36: "hermes-node:" is a distinct scheme from Node's own "node:",
// not an alternate spelling that stacks with it -- a doubled scheme is a
// spelling with no meaning, so it is rejected rather than treated as
// 'hermes-node:test' with an optional "node:" wrapper. The module-scoped
// require() goes through Module._load, wrapped in __initCJS; its own
// raw-prefix check does not match 'node:hermes-node:test', so it falls to
// Node's real _load, which throws ERR_UNKNOWN_BUILTIN_MODULE for any
// unrecognized "node:"-prefixed request. (The other entry point --
// global.require, which IS requireModule the bootstrap loader, reachable
// directly and unwrapped, the same escape hatch libjs/bundle-loader.js
// documents against -- has its own independent check, pinned by the
// DOUBLESCHEME_GLOBAL case below in a fresh process, where the cache is
// not already warm for 'hermes-node:test'.)
try {
  require('node:hermes-node:test');
  console.log('DOUBLESCHEME_CJS unreachable');
} catch (e) {
  console.log('DOUBLESCHEME_CJS', e.code === 'ERR_UNKNOWN_BUILTIN_MODULE');
}

console.log('PASS');
// CHECK: OK true
// CHECK: RESOLVE true
// CHECK: CACHED true
// CHECK: MISSING true
// CHECK: DOUBLESCHEME_CJS true
// CHECK: PASS

// Containment: a same-named node_modules package must not win over the
// embedder module, for require() or require.resolve() -- the property
// test/bundle-build.js's WSDECOY case pins for the vendored package 'ws'.
// Run from a real file (not -e) so require() has a directory to probe
// node_modules from, the same way the review's repro did.
// RUN: rm -rf %t.decoy && mkdir -p %t.decoy/node_modules/hermes-node:test
// RUN: echo "module.exports = { ok: false, decoy: true };" > %t.decoy/node_modules/hermes-node:test/index.js
// RUN: echo "console.log('DECOY_REQUIRE', require('hermes-node:test').ok === true);" > %t.decoy/main.js
// RUN: echo "console.log('DECOY_RESOLVE', require.resolve('hermes-node:test') === 'hermes-node:test');" >> %t.decoy/main.js
// RUN: echo "console.log('DECOY_ROUNDTRIP', require(require.resolve('hermes-node:test')).ok === true);" >> %t.decoy/main.js
// RUN: %hermes-node --test-embedder-module %t.decoy/main.js | %FileCheck --check-prefix=DECOY %s
// DECOY: DECOY_REQUIRE true
// DECOY: DECOY_RESOLVE true
// DECOY: DECOY_ROUNDTRIP true

// A module registered under a name that does not start with
// kEmbedderModulePrefix would be unreachable by any require() -- a silent
// no-op registration -- so registerEmbedderModule() refuses it outright;
// and a second registration under a name already taken is refused the same
// way rather than silently replacing or shadowing the first (see
// registerEmbedderModule() in hermes_node_runtime.h). The CLI has no flag
// that constructs a bad name, so only the duplicate half is reachable from
// here -- --test-embedder-module registers "hermes-node:test" once, and
// passing the flag twice registers it again.
// RUN: %not %hermes-node --test-embedder-module --test-embedder-module -e "" 2>&1 | %FileCheck --check-prefix=DUPLICATE %s
// DUPLICATE: Error: embedder module 'hermes-node:test' is already registered

// global.require IS requireModule (the bootstrap loader) -- reachable
// directly and unwrapped, the same escape hatch libjs/bundle-loader.js
// documents against ((0, eval)('require'), global.require) -- and it has
// its OWN independent RULING 36 check. Run in a fresh process, with nothing
// having required 'hermes-node:test' yet: requireModule() strips a leading
// 'node:' internally (so require('node:fs') === require('fs')), and if the
// embedder-module check ever moved to after that stripping, or tested the
// stripped name instead of the raw one, 'node:hermes-node:test' would
// resolve to the same cache entry (and, on a cold cache, the same lookup)
// as 'hermes-node:test' and wrongly succeed. A warm cache from an earlier
// require('hermes-node:test') in the same process would mask exactly that
// regression -- both names would already answer from the shared 'node:'
// cache-sharing behavior real builtins rely on -- which is why this needs
// its own process rather than reusing the one above.
// RUN: rm -rf %t.doublescheme && mkdir -p %t.doublescheme
// RUN: echo "try { var d = global.require('node:hermes-node:test'); console.log('DOUBLESCHEME_GLOBAL unexpectedly succeeded', d); } catch (e) { console.log('DOUBLESCHEME_GLOBAL', true); }" > %t.doublescheme/main.js
// RUN: %hermes-node --test-embedder-module %t.doublescheme/main.js | %FileCheck --check-prefix=DOUBLEGLOBAL %s
// DOUBLEGLOBAL: DOUBLESCHEME_GLOBAL true
