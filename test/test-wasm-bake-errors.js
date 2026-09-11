// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The bake-time error paths and the --record-wasm/--bake-wasm/--dump-wasm
// flag-conflict matrix. Deliberately ungated: every case here is refused by
// argument checking or by the bake step's own validation, and none needs
// WebAssembly to actually be enabled in this build or a real compiled Wasm
// module -- so this file runs on every checkout.
//
// A well-formed, zero-entry --record-wasm file is produced with `-e ""`,
// which flushes the recorder once, before any user code runs, and never
// compiles anything -- so the file it writes is a genuine record file
// carrying this build's own real version string, with none of the
// version-drift problems a checked-in fixture would have (this project's
// version is derived from `git describe` and moves on every commit). The
// truncated and version-mismatched inputs below are made by editing that
// same genuine file, in the spirit of "corrupt-file cases build their
// inputs by editing bytes" -- not by re-implementing the format by hand.

// RUN: rm -rf %t.d && mkdir -p %t.d
// RUN: echo "module.exports = 1;" > %t.entry.js

// A well-formed record file with 0 entries and this build's real version.
// RUN: %hermes-node --record-wasm=%t.rec0.bin -e ""

// --- Zero-entry warning: the build proceeds. ---
// RUN: %hermes-node --build-bundle=%t.d/warn.hbb --bake-wasm=%t.rec0.bin --verbose %t.entry.js 2>&1 >/dev/null | %FileCheck --check-prefix=ZEROWARN %s

// --- Absent record file: hard build error. ---
// RUN: %not %hermes-node --build-bundle=%t.d/absent.hbb --bake-wasm=%t.d/nosuch.bin %t.entry.js 2>&1 | %FileCheck --check-prefix=ABSENT %s

// --- Truncated record file: hard build error. ---
// RUN: head -c 20 %t.rec0.bin > %t.trunc.bin
// RUN: %not %hermes-node --build-bundle=%t.d/trunc.hbb --bake-wasm=%t.trunc.bin %t.entry.js 2>&1 | %FileCheck --check-prefix=TRUNC %s

// --- Version mismatch: hard build error naming both versions. ---
// RUN: cp %t.rec0.bin %t.badversion.bin
// RUN: %hermes-node %s %t.badversion.bin
// RUN: %not %hermes-node --build-bundle=%t.d/mismatch.hbb --bake-wasm=%t.badversion.bin %t.entry.js 2>&1 | %FileCheck --check-prefix=MISMATCH %s

// --- --bake-wasm without --build-bundle. ---
// RUN: %not %hermes-node --bake-wasm=%t.rec0.bin %t.entry.js 2>&1 | %FileCheck --check-prefix=BAKENOBUILD %s

// --- --record-wasm with a tool verb: nothing runs, so nothing to record. ---
// RUN: %not %hermes-node --record-wasm=%t.rec0.bin --dump 2>&1 | %FileCheck --check-prefix=RECVERB %s
// RUN: %not %hermes-node --dump --record-wasm=%t.rec0.bin 2>&1 | %FileCheck --check-prefix=RECVERB %s

// --- --dump-wasm with another verb: two verbs at once. ---
// RUN: %not %hermes-node --dump-wasm=%t.rec0.bin --dump 2>&1 | %FileCheck --check-prefix=DUMPWASMVERB %s
// RUN: %not %hermes-node --dump --dump-wasm=%t.rec0.bin 2>&1 | %FileCheck --check-prefix=DUMPWASMVERB %s

// --- --dump-wasm takes its own file, not a container. ---
// RUN: %not %hermes-node --dump-wasm=%t.rec0.bin --bundle=%t.d/x.hbb 2>&1 | %FileCheck --check-prefix=DUMPWASMBUNDLE %s
// RUN: %not %hermes-node --dump-wasm=%t.rec0.bin --build-bundle=%t.d/x.hbb %t.entry.js 2>&1 | %FileCheck --check-prefix=DUMPWASMBUILD %s

// --- --record-wasm with --build-bundle: the producer never runs. ---
// RUN: %not %hermes-node --record-wasm=%t.rec0.bin --build-bundle=%t.d/x.hbb %t.entry.js 2>&1 | %FileCheck --check-prefix=RECBUILD %s
// RUN: %not %hermes-node --build-bundle=%t.d/x.hbb --record-wasm=%t.rec0.bin %t.entry.js 2>&1 | %FileCheck --check-prefix=RECBUILD %s

// --- --dump-wasm with --inspect: nothing runs, so nothing to inspect. ---
// RUN: %not %hermes-node --dump-wasm=%t.rec0.bin --inspect 2>&1 | %FileCheck --check-prefix=DUMPWASMINSPECT %s

// --- Empty values name the flag, not a missing file with no name in it. ---
// RUN: %not %hermes-node --record-wasm= %t.entry.js 2>&1 | %FileCheck --check-prefix=RECEMPTY %s
// RUN: %not %hermes-node --bake-wasm= --build-bundle=%t.d/x.hbb %t.entry.js 2>&1 | %FileCheck --check-prefix=BAKEEMPTY %s
// RUN: %not %hermes-node --dump-wasm= 2>&1 | %FileCheck --check-prefix=DUMPWASMEMPTY %s

// --- The same-file refusal: recording onto the very file the run is
// --- reading would replace it out from under the run. ---
// RUN: cp %t.entry.js %t.same.js
// RUN: %not %hermes-node --record-wasm=%t.same.js --bundle=%t.same.js 2>&1 | %FileCheck --check-prefix=SAMEBUNDLE %s
// RUN: %not %hermes-node --record-wasm=%t.same.js %t.same.js 2>&1 | %FileCheck --check-prefix=SAMESCRIPT %s

// ZEROWARN: warning: {{.*}}rec0.bin records no WebAssembly modules
// ZEROWARN: wasm: 0 modules baked, 0 bytes
// ABSENT: error: cannot open {{.*}}nosuch.bin: {{.*}}
// TRUNC: error: {{.*}}trunc.bin: wasm record file: truncated (shorter than the header)
// MISMATCH: error: {{.*}}badversion.bin was recorded by hermes-node {{.*}}, but this is hermes-node {{.*}}
// BAKENOBUILD: Error: --bake-wasm requires --build-bundle.
// RECVERB: Error: --record-wasm cannot be combined with --dump.
// DUMPWASMVERB: Error: --dump-wasm cannot be combined with --dump.
// DUMPWASMBUNDLE: Error: --dump-wasm cannot be combined with --bundle.
// DUMPWASMBUILD: Error: --dump-wasm cannot be combined with --build-bundle.
// RECBUILD: Error: --record-wasm cannot be combined with --build-bundle.
// DUMPWASMINSPECT: Error: --dump-wasm cannot be combined with --inspect or --inspect-brk.
// RECEMPTY: Error: --record-wasm requires a file path.
// BAKEEMPTY: Error: --bake-wasm requires a file path.
// DUMPWASMEMPTY: Error: --dump-wasm requires a file path.
// SAMEBUNDLE: Error: --record-wasm={{.*}}same.js names the same file as --bundle={{.*}}same.js; recording onto the running container would replace it.
// SAMESCRIPT: Error: --record-wasm={{.*}}same.js names the same file as the script being run ({{.*}}same.js).

'use strict';

// Patches a genuine, well-formed --record-wasm file in place so its build
// version string no longer matches this binary's -- without touching any
// offset or length, and without needing to know what that string actually
// looks like (this project's version is `git describe` output and moves on
// every commit). Every byte of the version field is replaced with 'Z',
// which no real build version is ever entirely made of.

const fs = require('fs');

const HEADER_SIZE = 36; // WasmRecordHeader: magic[8] + 7 x uint32
const VERSION_OFFSET_FIELD = 16; // byte offset of header.versionOffset
const VERSION_LENGTH_FIELD = 20; // byte offset of header.versionLength

const file = process.argv[2];
const buf = fs.readFileSync(file);

if (buf.length < HEADER_SIZE) {
  console.error('FAIL: ' + file + ' is shorter than a wasm record header');
  process.exit(1);
}

const versionOffset = buf.readUInt32LE(VERSION_OFFSET_FIELD);
const versionLength = buf.readUInt32LE(VERSION_LENGTH_FIELD);

if (versionLength === 0) {
  console.error('FAIL: ' + file + ' has an empty build version string');
  process.exit(1);
}

buf.fill(0x5a /* 'Z' */, versionOffset, versionOffset + versionLength);

fs.writeFileSync(file, buf);
