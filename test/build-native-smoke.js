// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The two build-native cases that need a real toolchain: a bare output
// name (Task 11's Critical bug was fs::path("app").parent_path() being
// empty, which routed every native addon's sidecar into the temp directory
// that then gets deleted) and the same-file refusal for -o.
//
// REQUIRES: linker-available, shermes-available

// Case 1 also runs the produced executable now that the run path can load a
// native-unit container (Task 13), but that is a bonus check, not the
// point: this is a regression test for the Critical bug Task 11 fixed, and
// that bug is about where a file ends up, not about running anything. The
// build/placement assertions below stand on their own even if the run
// assertion is ever removed.
//
// The entry and its addon live in %t.src; the build is run from %t.out, a
// separate, otherwise-empty directory, with a BARE -o app -- no directory
// component. If the sidecar were misrouted into the (deleted) temp
// directory instead of resolving "app" against the cwd, hello_addon.node
// would never appear in %t.out at all.
// RUN: rm -rf %t.src %t.out && mkdir -p %t.src %t.out
// RUN: cp %hello_addon %t.src/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.src/main.js
// RUN: cd %t.out && %hermes-node build-native %t.src/main.js -o app --kit=%kit_dir | %FileCheck --check-prefix=BUILD %s
// BUILD: native: hello_addon.node (from hello_addon.node)
// BUILD: note: this executable requires 1 native addon alongside it; ship them together.
// BUILD: wrote {{.*}}app ({{[0-9]+}} bytes)

// The sidecar landed beside the bare-named executable in the cwd it was
// built from, not in some temp directory, and not beside the entry in
// %t.src.
// RUN: ls %t.out/app
// RUN: ls %t.out/hello_addon.node

// The addon loads and the entry's own assertions pass, so the executable
// this case built is not just present but actually runs.
// RUN: %t.out/app | %FileCheck --check-prefix=RUNS %s
// RUNS: PASS

// -o naming the entry itself must be refused rather than linking a native
// executable over the user's own source file.
// RUN: %not %hermes-node build-native %t.src/main.js -o %t.src/main.js --kit=%kit_dir 2>&1 | %FileCheck --check-prefix=SAMEFILE %s
// SAMEFILE: names the same file as the entry

// This file is a lit driver only; the RUN lines above are the test.
