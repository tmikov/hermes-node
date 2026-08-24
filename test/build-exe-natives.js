// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A native addon in a produced executable. The bundle root moves when a
// container becomes an executable -- it is the executable's own directory,
// not the container's -- and everything about sidecars follows from that:
// the addon has to sit beside the executable, and an executable that shipped
// without one has to say so.
//
// REQUIRES: linker-available

// The container is built in one directory and the executable is linked into
// another, empty one. Keeping the two apart is what makes the assertions
// below mean anything: a loader still reaching for the container's directory
// would find the addon there and pass every check.
// RUN: rm -rf %t.src %t.out && mkdir -p %t.src %t.out
// RUN: cp %hello_addon %t.src/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.src/main.js
// RUN: %hermes-node --build-bundle=%t.src/app.hbb %t.src/main.js | %FileCheck --check-prefix=BUILD %s
// BUILD: native: hello_addon.node (from hello_addon.node)
// BUILD: note: this bundle requires 1 native addon alongside it; ship them together.
// The link reports the addon too, and where it now has to sit. Linking is
// the moment "alongside" stops meaning the container's directory and starts
// meaning the executable's, so a build that said nothing here would let a
// container that passed --verify-natives be shipped as an executable that
// throws on its first run.
// RUN: %hermes-node --build-exe=%t.out/app.exe --kit=%kit_dir %t.src/app.hbb | %FileCheck --check-prefix=WROTE %s
// WROTE: wrote {{.*}}app.exe
// WROTE-NEXT: native: hello_addon.node (from hello_addon.node)
// WROTE-NEXT: note: this executable requires 1 native addon alongside it; ship them together.
// WROTE-NEXT: note: they must sit beside {{.*}}app.exe, not beside the container.

// Nothing has been shipped beside the executable yet, and the addon IS
// still sitting beside the container. The recorded native is therefore
// missing as far as this executable is concerned, and the error says which
// file to ship and where -- naming a path under the executable's directory,
// which is the whole claim this file exists to check.
//
// MODULE_NOT_FOUND rather than ERR_DLOPEN_FAILED, because what handles an
// unavailable addon in the wild (an optional-dependency probe, a napi-rs
// try/catch chain) branches on that code.
// RUN: ls %t.src/hello_addon.node
// RUN: %not %t.out/app.exe 2>&1 | %FileCheck --check-prefix=MISSING %s
// MISSING: Cannot find module 'hello_addon.node'
// MISSING: its file is not beside the bundle
// MISSING: Expected: {{.*}}.out/hello_addon.node
// MISSING: copy it there

// Ship it, and the addon loads and runs: main.js calls into it and only
// prints PASS if both exported functions return what they should.
// RUN: cp %t.src/hello_addon.node %t.out/hello_addon.node
// RUN: %t.out/app.exe | %FileCheck --check-prefix=RUNS %s
// RUNS: PASS

// The pair -- executable plus sidecar -- is the whole deliverable: delete
// the container, the source and the addon's original copy, and it still runs.
// RUN: rm -rf %t.src
// RUN: %t.out/app.exe | %FileCheck --check-prefix=RUNS %s

// And the addon really is loaded from the sidecar rather than from anything
// the container remembers: truncating the file beside the executable breaks
// the run. Without this, "it works after the copy" could be satisfied by a
// loader that had already found the addon somewhere else.
// RUN: cp %t.out/hello_addon.node %t.good_addon
// RUN: head -c 64 %t.good_addon > %t.out/hello_addon.node
// RUN: %not %t.out/app.exe 2>&1 | %FileCheck --check-prefix=BROKEN %s
// BROKEN: Error: Cannot open {{.*}}.out/hello_addon.node
// BROKEN: at dlopen
// RUN: cp %t.good_addon %t.out/hello_addon.node
// RUN: %t.out/app.exe | %FileCheck --check-prefix=RUNS %s

// The produced executables are 185 MB apiece under ASAN, so they go when
// they are no longer needed. This is the LAST line deliberately: lit stops
// at the first failing RUN line, so a failure leaves every artifact in place
// for post-mortem and only a passing run cleans up after itself.
// RUN: rm -f %t.out/app.exe %t.good_addon

// This file is a lit driver only; the RUN lines above are the test.
