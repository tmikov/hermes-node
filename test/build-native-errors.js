// Refusals only. Deliberately NOT gated on linker-available or
// shermes-available: every case below is rejected before any toolchain is
// reached, so this file is the build-native coverage that survives a
// checkout with no kit.
//
// RUN: rm -rf %t && mkdir -p %t/src
// RUN: echo 'console.log("hi");' > %t/src/app.js
//
// RUN: %not %hermes-node build-native 2>&1 | %FileCheck --check-prefix=NOENTRY %s
// NOENTRY: requires an entry
//
// RUN: %not %hermes-node build-native %t/src/app.js 2>&1 | %FileCheck --check-prefix=NOOUT %s
// NOOUT: requires -o
//
// RUN: %not %hermes-node build-native -o %t/app %t/src/nope.js 2>&1 | %FileCheck --check-prefix=NOFILE %s
// NOFILE: nope.js
//
// RUN: %not %hermes-node build-native -o %t/app --jobs=0 %t/src/app.js 2>&1 | %FileCheck --check-prefix=JOBS %s
// JOBS: --jobs
//
// RUN: %not %hermes-node build-native -o %t/app --jobs=x %t/src/app.js 2>&1 | %FileCheck --check-prefix=JOBSX %s
// JOBSX: --jobs
//
// A value above UINT_MAX must not survive the narrowing cast to unsigned:
// 4294967296 is 2^32, which truncates to 0 -- the "unlimited" sentinel
// --jobs=0 above is refused for.
// RUN: %not %hermes-node build-native -o %t/app --jobs=4294967296 %t/src/app.js 2>&1 | %FileCheck --check-prefix=JOBSOVERFLOW %s
// JOBSOVERFLOW: --jobs
//
// RUN: %not %hermes-node build-native -o %t/app --record-wasm=%t/w.bin %t/src/app.js 2>&1 | %FileCheck --check-prefix=RECWASM %s
// RECWASM: --record-wasm
//
// RUN: %not %hermes-node build-native -o %t/app --nonsense %t/src/app.js 2>&1 | %FileCheck --check-prefix=UNKNOWN %s
// UNKNOWN: unknown option '--nonsense'
//
// RUN: %not %hermes-node build-native -o '' %t/src/app.js 2>&1 | %FileCheck --check-prefix=EMPTYOUT %s
// EMPTYOUT: -o
//
// RUN: %not %hermes-node build-native -o %t/app --kit= %t/src/app.js 2>&1 | %FileCheck --check-prefix=EMPTYKIT %s
// EMPTYKIT: --kit
//
// RUN: %not %hermes-node build-native -o %t/app %t/src/app.js %t/src/app.js 2>&1 | %FileCheck --check-prefix=TWO %s
// TWO: one entry
//
// A flag AFTER the entry is fine here, unlike --build-exe: a subcommand
// parses its own argv, so position carries no meaning.
// RUN: %not %hermes-node build-native %t/src/app.js -o %t/app --kit=%t/nokit 2>&1 | %FileCheck --check-prefix=AFTER %s
// AFTER: kit.manifest
//
// RUN: %hermes-node build-native --help 2>&1 | %FileCheck --check-prefix=HELP %s
// HELP: Usage:
// HELP: build-native

console.log('unused');
