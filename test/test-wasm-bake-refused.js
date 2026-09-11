// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A baked entry Hermes refuses is fatal -- the container is treated as
// damaged, exactly like a JavaScript module whose bytecode will not load
// (bundle-corrupt-payload.js). A disk cache entry Hermes refuses is the
// opposite: the cache is best effort and falls back to compiling, silently.
// This asymmetry is the point of the feature, and this is the only place a
// real hermes_set_wasm_cache round trip can prove it -- a unit test cannot
// reach either path.
//
// Both cases patch bytes that make Hermes refuse the bytecode (zeroing its
// magic) while leaving every offset and length in the surrounding format
// valid, so the corruption is invisible to anything that only checks
// structure.

// REQUIRES: wasm

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.tree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.tree/app.js

// --- Case 1: a baked container entry Hermes refuses is fatal.
// RUN: %hermes-node --record-wasm=%t.rec %t.tree/app.js > /dev/null
// RUN: %hermes-node --build-bundle=%t.b --bake-wasm=%t.rec %t.tree/app.js
// RUN: %hermes-node --bundle=%t.b | %FileCheck --check-prefix=INTACT %s
// RUN: %hermes-node %s %t.b
// RUN: %not %hermes-node --bundle=%t.b > %t.rout 2> %t.rerr
// RUN: %FileCheck --check-prefix=REFUSED %s < %t.rerr
// Nothing should reach stdout: the process is gone before the module's
// result is ever printed.
// RUN: %FileCheck --check-prefix=NOSTDOUT --allow-empty %s < %t.rout

// --- Case 2: the same corruption in a disk cache entry is NOT fatal. The
// cache is best effort and has a right answer to fall back on, so a refused
// disk hit just means the module compiles again, exactly as an ordinary
// miss would.
// RUN: rm -rf %t.dcache && mkdir -p %t.dcache
// RUN: %hermes-node-cc --compile-cache=%t.dcache %t.tree/app.js | %FileCheck --check-prefix=INTACT %s
// The compile-cache entry header is 24 bytes (EntryHeader); the compiled
// Wasm bytecode starts right after it. Zeroing 16 bytes there corrupts the
// bytecode's own magic while leaving the header -- and therefore the
// lookup's CRC/size guard -- untouched, so the entry is still found as a
// HIT and handed to Hermes, which is the refused-hit path this case exists
// to reach.
// RUN: for f in $(find %t.dcache -name 'w*'); do \
// RUN:   dd if=/dev/zero of="$f" bs=1 count=16 seek=24 conv=notrunc 2>/dev/null; \
// RUN: done
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc --compile-cache=%t.dcache %t.tree/app.js > %t.dout 2> %t.derr
// RUN: %FileCheck --check-prefix=INTACT %s < %t.dout
// RUN: %FileCheck --check-prefix=DISKREFUSED %s < %t.derr

// INTACT: add 42
// REFUSED: error: baked WebAssembly bytecode failed to load
// REFUSED: sha256: {{[0-9a-f]+}}
// REFUSED: in container: {{.*}}
// REFUSED: This container is damaged. Rebuild it with --build-bundle.
// NOSTDOUT-NOT: add
// DISKREFUSED: wasm hit
// DISKREFUSED: wasm store
// DISKREFUSED-NOT: error:
// DISKREFUSED-NOT: container refused

'use strict';

// Patches a container's sole baked Wasm entry so its bytecode magic no
// longer matches, without touching any offset or length. Fails loudly if
// the container does not look like exactly what this test built, so a
// format change is caught here instead of silently patching the wrong
// bytes.

const fs = require('fs');

const U32 = 4;

// BundleHeader field indices after the 8-byte magic (see
// bundle_format.h). Format v6 added wasmTableOffset/wasmCount before
// containerFlags.
const WASM_TABLE_OFFSET = 15;
const WASM_COUNT = 16;
const PAYLOAD_OFFSET = 18;

// BundleWasmRecord: digest[32] + payloadOffset(u32) + payloadSize(u32).
const DIGEST_BYTES = 32;

function field(buf, index) {
  return buf.readUInt32LE(8 + index * U32);
}

const file = process.argv[2];
const buf = fs.readFileSync(file);

const wasmTableOffset = field(buf, WASM_TABLE_OFFSET);
const wasmCount = field(buf, WASM_COUNT);
const payloadOffset = field(buf, PAYLOAD_OFFSET);

if (wasmCount !== 1) {
  console.error(
    'FAIL: expected exactly 1 baked Wasm entry, found ' + wasmCount +
    '\nUpdate the field indices or entry count in this test.');
  process.exit(1);
}

const recordAt = wasmTableOffset; // entry 0
const recordPayloadOffset = buf.readUInt32LE(recordAt + DIGEST_BYTES);
const recordPayloadSize = buf.readUInt32LE(recordAt + DIGEST_BYTES + U32);

if (recordPayloadSize < 16) {
  console.error(
    'FAIL: baked Wasm payload is only ' + recordPayloadSize + ' bytes');
  process.exit(1);
}

const at = payloadOffset + recordPayloadOffset;
buf.fill(0, at, at + 16);

fs.writeFileSync(file, buf);
