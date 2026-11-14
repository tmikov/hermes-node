// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A "hermes-node:" specifier must pass through a bundle exactly like a
// builtin: never scanned into the edge table by the producer, never routed
// through the closed world's "Not in the bundle" error by the consumer, and
// never shadowed by a same-named node_modules package on either side. Two
// C++/JS pairs make that true and nothing forces them to agree --
// isBuiltinSpecifier (lib/bundle/bundle_resolve.cpp, the producer) paired
// with installBundleLoader's isEmbedderModuleSpecifier (libjs/bundle-loader.js,
// the consumer's Module._load, its resolve(), and its Module._resolveFilename,
// all three -- see the Task 5 review) -- so this test exercises the producer
// and the consumer in one file, the way test/bundle-builtins.js does for
// real builtins, and plants a decoy so a producer regression is caught by a
// positive assertion rather than by the absence of a warning:
//
//   - If the producer's skip regresses, resolveSpecifier() finds the decoy
//     at node_modules/hermes-node:test (planted below) and packages it --
//     silently, printing no warning at all, which is exactly why a mutation
//     of this check must be caught by what --dump's module table contains,
//     not by an assertion that a normally-quiet stream stayed quiet. DUMP
//     below asserts the container has exactly one module and that no
//     identity anywhere in the dump names the decoy.
//   - If any of the consumer's three checks regresses, the specifier --
//     still correctly unpackaged -- has no edge and does not resolve inside
//     the container either, so the affected entry point falls through to
//     the closed world's "Not in the bundle" error instead of forwarding to
//     the loader that actually resolves it. RUN below asserts require(),
//     require.resolve() and their round trip all answer correctly instead.
//
// REQUIRES: ui-enabled
//
// The gate is the `ui-enabled` feature in test/lit.cfg, decided by running
// hermes-node itself with --test-embedder-module and checking that it
// actually works (RULING 37), not a build-provided flag lit merely trusted.
// A build without HERMES_NODE_ENABLE_UI reports this UNSUPPORTED rather
// than failing it.

// RUN: rm -rf %t.dir && mkdir -p %t.dir/node_modules/hermes-node:test
// RUN: echo "module.exports = { ok: false, decoy: true };" > %t.dir/node_modules/hermes-node:test/index.js
// RUN: echo "var mod = require('hermes-node:test');" > %t.dir/main.js
// RUN: echo "console.log('OK', mod.ok === true);" >> %t.dir/main.js
// RUN: echo "console.log('RESOLVE', require.resolve('hermes-node:test') === 'hermes-node:test');" >> %t.dir/main.js
// RUN: echo "console.log('ROUNDTRIP', require(require.resolve('hermes-node:test')).ok === true);" >> %t.dir/main.js
// RUN: echo "console.log('CACHED', require('hermes-node:test') === mod);" >> %t.dir/main.js

// Producer side: build with --test-embedder-module so the module the entry
// requires actually exists, exactly as an ordinary run would need it to.
// (isBuiltinSpecifier's skip does not itself depend on this flag -- it is a
// pure specifier-shape check -- but a real embedder always registers what it
// publishes, so the build and run invocations register the same one.)
// RUN: %hermes-node --test-embedder-module --build-bundle=%t.dir/app.hbb %t.dir/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.dir/app.hbb --dump | %FileCheck --check-prefix=DUMP --implicit-check-not="hermes-node:test" %s
// DUMP: MODULES (1)

// Consumer side: delete the source tree -- decoy included -- so a run that
// quietly fell back to disk (rather than the embedder module) would fail
// outright instead of silently passing.
// RUN: rm -rf %t.dir/main.js %t.dir/node_modules
// RUN: %hermes-node --test-embedder-module --bundle=%t.dir/app.hbb | %FileCheck %s
// CHECK: OK true
// CHECK: RESOLVE true
// CHECK: ROUNDTRIP true
// CHECK: CACHED true
