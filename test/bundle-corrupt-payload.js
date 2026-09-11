// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A container whose module payload Hermes will not load terminates the
// process. It does not throw.
//
// The distinction matters because the closed world deliberately supports
// `try { require(x) } catch {}` for an optional dependency -- a missing
// module throws MODULE_NOT_FOUND so that probe keeps working. Throwing here
// too would make a CORRUPT module indistinguishable from an absent one, and
// a program with such a probe would carry on with its fallback and never
// report that its artifact is damaged. BundleReader::open() already treats a
// structurally invalid container as fatal; this is the same fault found
// later, because open() validates structure and never reads a payload.
//
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('PASS: ran');" > %t.tree/entry.js
// RUN: %hermes-node --build-bundle=%t.b %t.tree/entry.js > /dev/null
// RUN: %hermes-node --bundle=%t.b | %FileCheck --check-prefix=INTACT %s
// INTACT: PASS: ran

// Zero the first bytes of the payload region, which is where the one and
// only module's bytecode starts, so its magic no longer matches. Every
// offset and length in the container stays valid, which is the point: this
// is damage that only the engine can see.
// RUN: %hermes-node %s %t.b
// RUN: %not %hermes-node --bundle=%t.b 2>&1 | %FileCheck --check-prefix=CORRUPT %s
// CORRUPT: error: {{.*}}entry.js: bundled bytecode failed to load
// CORRUPT: in container: {{.*}}
// CORRUPT: This container is damaged. Rebuild it with --build-bundle.

// Nothing should reach stdout: the process is gone before the module runs.
// RUN: %not %hermes-node --bundle=%t.b 2>/dev/null | %FileCheck --check-prefix=NOSTDOUT --allow-empty %s
// NOSTDOUT-NOT: PASS

'use strict';

const fs = require('fs');

const file = process.argv[2];
const buf = fs.readFileSync(file);

// BundleHeader (include/hermes/node-compat/bundle/bundle_format.h) is fixed
// width: 8 magic bytes then uint32 fields. payloadOffset is the second to
// last. Counted from the struct rather than hardcoded blind, and asserted
// below, so a format change that moves it fails here instead of quietly
// zeroing something harmless.
const U32 = 4;
const kMagicBytes = 8;
const kFieldsBeforePayloadOffset = 16;
const payloadOffsetAt = kMagicBytes + kFieldsBeforePayloadOffset * U32;

const payloadOffset = buf.readUInt32LE(payloadOffsetAt);
const payloadSize = buf.readUInt32LE(payloadOffsetAt + U32);

if (payloadOffset === 0 || payloadOffset + payloadSize !== buf.length) {
  console.error(
    'FAIL: header layout moved: payloadOffset=' + payloadOffset +
    ' payloadSize=' + payloadSize + ' fileSize=' + buf.length +
    '\nUpdate kFieldsBeforePayloadOffset in this test.');
  process.exit(1);
}

buf.fill(0, payloadOffset, payloadOffset + 16);
fs.writeFileSync(file, buf);
