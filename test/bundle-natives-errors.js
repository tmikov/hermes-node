// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// What a bundled program sees when a native addon is missing, and when one
// was never packaged at all.

// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.dir/main.js
// RUN: %hermes-node --build-bundle=%t.dir/app.hbb %t.dir/main.js > /dev/null

// The recorded addon, deleted after the build: the message names the file
// to ship and the code stays MODULE_NOT_FOUND so a probe still works.
// RUN: rm %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb 2>&1 | %FileCheck --check-prefix=MISSING %s
// MISSING: Cannot find module 'hello_addon.node'
// MISSING: its file is not beside the bundle
// MISSING: Expected: {{.*}}/hello_addon.node
// MISSING: copy it there

// An addon nothing packaged: the ordinary not-in-the-bundle error, with the
// --include suggestion that now actually works.
// RUN: rm -rf %t.two && mkdir -p %t.two
// RUN: cp %source_dir/test/fixtures/bundle-natives/computed.js %t.two/main.js
// RUN: cp %hello_addon %t.two/hello_addon.node
// RUN: %hermes-node --build-bundle=%t.two/app.hbb %t.two/main.js > /dev/null 2>&1
// RUN: %not %hermes-node --bundle=%t.two/app.hbb 2>&1 | %FileCheck --check-prefix=UNRECORDED %s
// UNRECORDED: Cannot find module './hello_addon.node'
// UNRECORDED: Not in the bundle. Add it with:
// UNRECORDED: --include=./hello_addon.node
// UNRECORDED-NOT: Native addons are not supported

// The catchable form of the same miss: e.code stays MODULE_NOT_FOUND, same
// as any other not-in-the-bundle specifier, because a probing caller (an
// optional-dependency check, a napi-rs try/catch chain) branches on exactly
// that value and must still see the "no" it expects. UNRECORDED above lets
// the require throw uncaught, so it pins the message text but never reads
// .code -- an uncaught Error prints "Error: <message>" and never the code
// property. Uses its own computed require, same trick as the fixture, so
// this build does not accidentally package the addon and turn the miss
// into a hit.
// RUN: echo "var n = './hello_addon' + '.node'; try { require(n); } catch (e) { console.log('CODE', e.code); }" > %t.two/catch.js
// RUN: %hermes-node --build-bundle=%t.two/catch.hbb %t.two/catch.js > /dev/null 2>&1
// RUN: %hermes-node --bundle=%t.two/catch.hbb | %FileCheck --check-prefix=CODE %s
// CODE: CODE MODULE_NOT_FOUND

// And the suggestion the previous case printed actually resolves.
// RUN: %hermes-node --build-bundle=%t.two/app.hbb --include=./hello_addon.node %t.two/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.two/app.hbb | %FileCheck --check-prefix=INCLUDED %s
// INCLUDED: PASS

// --preload of an addon needs no special case: the preload table holds
// module indices and running one means requiring it, which for a native
// means dlopen. Pinned here so nobody later "fixes" it with a refusal.
// quiet.js does not require the addon itself -- it only reports whether
// require.cache already holds it by the time its own body runs, which is
// true exactly when a preload dlopen'd it first.
// RUN: rm -rf %t.pre && mkdir -p %t.pre
// RUN: cp %hello_addon %t.pre/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/quiet.js %t.pre/main.js
// RUN: %hermes-node --build-bundle=%t.pre/app.hbb --preload=./hello_addon.node %t.pre/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.pre/app.hbb --dump | %FileCheck --check-prefix=DUMPPRELOAD %s
// DUMPPRELOAD: PRELOADS
// DUMPPRELOAD: hello_addon.node
// RUN: %hermes-node --bundle=%t.pre/app.hbb | %FileCheck --check-prefix=PRELOADED %s
// PRELOADED: PRELOADED true

// The discriminator: the identical entry, built with no --preload at all,
// must NOT show the addon in require.cache. Without this half, PRELOADED
// above could not tell "the preload table ran dlopen" apart from "nothing
// loaded the addon and require.cache is merely empty of it for some other
// reason" -- the two would look the same if quiet.js printed only 'true'.
// RUN: rm -rf %t.nopre && mkdir -p %t.nopre
// RUN: cp %hello_addon %t.nopre/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/quiet.js %t.nopre/main.js
// RUN: %hermes-node --build-bundle=%t.nopre/app.hbb %t.nopre/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.nopre/app.hbb | %FileCheck --check-prefix=NOPRELOAD %s
// NOPRELOAD: PRELOADED false

// --extract-module on a native says where the bytes actually are: there is
// nothing to write, since the addon's bytes were never in the container
// (see the sidecar note above extractModule() in bundle_tools.cpp).
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --extract-module=hello_addon.node --out=%t.dir/x.bin 2>&1 | %FileCheck --check-prefix=EXTRACT %s
// EXTRACT: is a native addon
// EXTRACT: ships alongside the bundle as hello_addon.node
