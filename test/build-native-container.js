// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The reader-side refusal for a native container (kBundleFlagNativeUnits),
// deferred by Task 8 to here because only a real build-native run can
// produce such a container to point --bundle= at. --keep-temp is the one
// way one reaches a filesystem: the format exists because .incbin takes a
// path, so the container is always serialized to a temp file first, and
// --keep-temp is what keeps that file around instead of deleting it.
//
// Gated (needs the toolchain to produce a container at all), so this case
// lives here rather than in the ungated test/build-native-errors.js --
// gating that whole file for one case would cost the coverage that
// survives a kitless checkout, which is the reason that file exists.
//
// REQUIRES: linker-available, shermes-available

// RUN: rm -rf %t/src %t/keep && mkdir -p %t/src %t/keep
// RUN: echo 'console.log("hi");' > %t/src/app.js

// --keep-temp prints the retained directory on one line: "build-native:
// temp directory: <path>", to stderr, so a script can find it without
// guessing the mkdtemp-generated name.
// RUN: %hermes-node build-native %t/src/app.js -o %t/keep/app \
// RUN:     --kit=%kit_dir --keep-temp 2>%t/keep/err.txt
// RUN: sed -n 's/^build-native: temp directory: //p' %t/keep/err.txt > %t/keep/dir.txt

// The retained container is well-formed -- its code is just linked into
// the executable rather than carried as bytecode -- so pointing --bundle=
// at it must not be confused with a damaged artifact.
// RUN: %not %hermes-node --bundle="$(cat %t/keep/dir.txt)/container.hbb" 2>&1 \
// RUN:     | %FileCheck --check-prefix=NATIVE %s
// NATIVE: holds no bytecode

// --extract-module on the same container must not "succeed" by writing a
// zero-byte file and calling it app.js's payload: the module's code was
// compiled ahead of time to a Static Hermes unit and linked into the
// executable, so there is no bytecode in the container to extract.
// RUN: %not %hermes-node --bundle="$(cat %t/keep/dir.txt)/container.hbb" \
// RUN:     --extract-module=app.js --out=%t/keep/extracted.bin 2>&1 \
// RUN:     | %FileCheck --check-prefix=NOEXTRACT %s
// NOEXTRACT: compiled to native code
// RUN: test ! -e %t/keep/extracted.bin

// RUN: rm -f %t/keep/app

// This file is a lit driver only; the RUN lines above are the test.
