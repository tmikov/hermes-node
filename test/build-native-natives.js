// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A native addon in a natively compiled executable. Same case as
// test/build-exe-natives.js -- an addon packaged as a kNative record with
// its bytes copied to a flat sidecar beside the executable, loaded by
// process.dlopen at run time, and a recorded addon whose sidecar is missing
// throwing MODULE_NOT_FOUND rather than ERR_DLOPEN_FAILED -- ported case for
// case. None of this is new code: Task 11 reuses the producer's addon
// handling unchanged, which is exactly why it needs a test.
//
// One structural change from the source file: build-native compiles and
// links directly from the entry, so the --build-bundle-then---build-exe
// pair collapses into one build-native invocation, and the source file's
// separate BUILD/WROTE checks (one per step, since a container's addon
// starts out "beside the container" and only "beside the executable" after
// linking) collapse into the single WROTE check below -- there is no
// container step here for an addon to be beside first. The "note: they
// must sit beside <exe>, not beside the container" line also has no
// equivalent: that line exists in --build-exe to announce a relocation
// build-native never does.
//
// REQUIRES: linker-available, shermes-available

// The entry and the addon live in one directory; the build runs into a
// separate, empty one -- so a loader still reaching for the entry's own
// directory would find the addon there and pass every check.
// RUN: rm -rf %t.src %t.out && mkdir -p %t.src %t.out
// RUN: cp %hello_addon %t.src/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.src/main.js
// RUN: %hermes-node build-native %t.src/main.js -o %t.out/app.exe --kit=%kit_dir | %FileCheck --check-prefix=WROTE %s
// WROTE: native: hello_addon.node (from hello_addon.node)
// WROTE-NEXT: note: this executable requires 1 native addon alongside it; ship them together.
// WROTE-NEXT: wrote {{.*}}app.exe

// The build already copied the sidecar beside the executable (build-native
// performs the copy itself, unlike --build-exe which needs a separate run
// to move it from beside the container). Delete it to exercise the missing
// case: the error names the file to ship and where.
//
// MODULE_NOT_FOUND rather than ERR_DLOPEN_FAILED, because what handles an
// unavailable addon in the wild (an optional-dependency probe, a napi-rs
// try/catch chain) branches on that code.
// RUN: rm %t.out/hello_addon.node
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
// the entry tree and the addon's original copy, and it still runs.
// RUN: rm -rf %t.src
// RUN: %t.out/app.exe | %FileCheck --check-prefix=RUNS %s

// And the addon really is loaded from the sidecar rather than from anything
// remembered elsewhere: truncating the file beside the executable breaks
// the run. Without this, "it works after the copy" could be satisfied by a
// loader that had already found the addon somewhere else.
// RUN: cp %t.out/hello_addon.node %t.good_addon
// RUN: head -c 64 %t.good_addon > %t.out/hello_addon.node
// RUN: %not %t.out/app.exe 2>&1 | %FileCheck --check-prefix=BROKEN %s
// BROKEN: Error: Cannot open {{.*}}.out/hello_addon.node
// BROKEN: at dlopen
// RUN: cp %t.good_addon %t.out/hello_addon.node
// RUN: %t.out/app.exe | %FileCheck --check-prefix=RUNS %s

// Step 4b: a build that fails AFTER compilation -- the reason sidecar
// copying was deferred until after the link (Task 11). Without this test, a
// later refactor moving the copy back into the shared producer would go
// unnoticed until somebody's rebuild half-updated an artifact.
//
// Build once so a known-good executable and sidecar exist, in their own
// directory so the checks above cannot leave stray state behind.
// RUN: rm -rf %t.n && mkdir -p %t.n/src
// RUN: cp %hello_addon %t.n/src/binding.node
// RUN: echo "var a = require('./binding.node'); if (a.hello() !== 'world') throw new Error('bad'); console.log('PASS');" > %t.n/src/app.js
// RUN: %hermes-node build-native %t.n/src/app.js -o %t.n/app --kit=%kit_dir
// RUN: cp %t.n/app %t.n/app.first
// RUN: cp %t.n/binding.node %t.n/sidecar.first
//
// Make the source differ, so a copy performed during the failing build
// below would change the sidecar's bytes. Without this the assertion is
// vacuous: recopying the same file yields the same bytes either way.
// RUN: printf 'x' >> %t.n/src/binding.node
//
// %false resolves and runs, then fails every compile -- a build that fails
// AFTER the shared producer would have copied sidecars, which is the case
// the deferral exists for.
// RUN: %not %hermes-node build-native %t.n/src/app.js -o %t.n/app --kit=%kit_dir --cc=%false
//
// The old executable and sidecar are named explicitly rather than globbed:
// the producer prints "native: <sidecar> (from <identity>)", so the
// fixture knows what it is called, and a glob that matches two files
// silently compares the wrong one.
// RUN: cmp %t.n/app %t.n/app.first
// RUN: cmp %t.n/binding.node %t.n/sidecar.first

// Step 5: the entry directory and the output directory are the SAME
// directory, and the addon already sits there -- the most ordinary
// invocation there is (`build-native app.js -o app` run inside a project
// that already contains its own binding.node). The addon's sidecar
// destination is then its own path: nothing to copy, so the build must
// not "copy" it onto itself, but it is still a required addon and must
// still get its `native:` line and be counted in the note below. Before
// this test existed, an in-place addon was silently dropped from the
// build's own bookkeeping instead: no `native:` line, and the "requires N
// native addon(s)" count did not include it. Neither test above catches
// this, because both build into a directory other than the entry's.
// RUN: rm -rf %t.ip && mkdir -p %t.ip
// RUN: cp %hello_addon %t.ip/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.ip/main.js
// RUN: %hermes-node build-native %t.ip/main.js -o %t.ip/app.exe --kit=%kit_dir | %FileCheck --check-prefix=INPLACE %s
// INPLACE: native: hello_addon.node (from hello_addon.node)
// INPLACE-NEXT: note: this executable requires 1 native addon alongside it; ship them together.
// INPLACE-NEXT: wrote {{.*}}app.exe

// The addon survived the build (a "copy onto itself" would still leave the
// bytes correct, but this is what a real copy-elision bug would break) and
// the executable actually loads and runs it.
// RUN: cmp %hello_addon %t.ip/hello_addon.node
// RUN: %t.ip/app.exe | %FileCheck --check-prefix=RUNS %s
// RUN: rm -rf %t.ip

// The produced executables are large under ASAN, so they go when they are
// no longer needed. This is the LAST line deliberately: lit stops at the
// first failing RUN line, so a failure leaves every artifact in place for
// post-mortem and only a passing run cleans up after itself.
// RUN: rm -f %t.out/app.exe %t.good_addon %t.n/app

// This file is a lit driver only; the RUN lines above are the test.
