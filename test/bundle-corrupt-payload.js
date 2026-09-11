// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A container whose module payload Hermes will not load terminates the
// process. It does not throw, and it is not preempted by a clean exit that
// happens to be in progress.
//
// Throwing was the original behaviour and it is wrong here: the closed world
// deliberately supports `try { require(x) } catch {}` for an optional
// dependency -- a missing module throws MODULE_NOT_FOUND so that probe keeps
// working -- so throwing made a CORRUPT module indistinguishable from an
// absent one, and a program with such a probe carried on with its fallback.
// BundleReader::open() already treats a structurally invalid container as
// fatal; this is the same fault found later, because open() validates
// structure and never reads a payload.
//
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('PASS: ran');" > %t.tree/entry.js
// RUN: %hermes-node --build-bundle=%t.b %t.tree/entry.js > /dev/null
// RUN: %hermes-node --bundle=%t.b | %FileCheck --check-prefix=INTACT %s
// INTACT: PASS: ran

// Zero the first bytes of one module's bytecode so its magic no longer
// matches. Every offset and length in the container stays valid, which is
// the point: this is damage only the engine can see.
// RUN: %hermes-node %s %t.b entry.js
// RUN: %not %hermes-node --bundle=%t.b 2>&1 | %FileCheck --check-prefix=CORRUPT %s
// CORRUPT: error: {{.*}}entry.js: bundled bytecode failed to load
// CORRUPT: in container: {{.*}}
// CORRUPT: This container is damaged. Rebuild it with --build-bundle.

// Nothing should reach stdout: the process is gone before the module runs.
// RUN: %not %hermes-node --bundle=%t.b 2>/dev/null | %FileCheck --check-prefix=NOSTDOUT --allow-empty %s
// NOSTDOUT-NOT: PASS

// A fatal error outranks a clean exit already in progress. process.exit()
// flushes by running the event loop, so the queued require() below runs
// INSIDE that flush and finds the damaged module. Reporting a broken
// artifact and then exiting 0 would be the same silent success this whole
// rule exists to prevent.
// RUN: rm -rf %t.tree2 && mkdir -p %t.tree2
// RUN: echo "setImmediate(function () { require('./bad.js'); }); process.exit(0);" > %t.tree2/entry.js
// RUN: echo "console.log('PASS: bad ran');" > %t.tree2/bad.js
// RUN: %hermes-node --build-bundle=%t.b2 %t.tree2/entry.js > /dev/null
// RUN: %hermes-node %s %t.b2 bad.js
// RUN: %not %hermes-node --bundle=%t.b2 2>&1 | %FileCheck --check-prefix=PREEMPT %s
// PREEMPT: error: {{.*}}bad.js: bundled bytecode failed to load

'use strict';

const fs = require('fs');

const U32 = 4;

// BundleHeader (include/hermes/node-compat/bundle/bundle_format.h) is fixed
// width: 8 magic bytes then uint32 fields, in this order. Counted from the
// struct rather than hardcoded blind, and checked below, so a format change
// that moves them fails here instead of quietly zeroing something harmless.
//
// Format v6 inserts wasmTableOffset and wasmCount before containerFlags --
// when that lands, every index at or after CONTAINER_FLAGS moves up by two.
const F = {
  FORMAT_VERSION: 0,
  GENERATION_TAG: 1,
  ENTRY_MODULE: 2,
  STRINGS_OFFSET: 3,
  STRINGS_SIZE: 4,
  MODULE_TABLE_OFFSET: 5,
  MODULE_COUNT: 6,
  CONTAINER_FLAGS: 15,
  PAYLOAD_OFFSET: 16,
  PAYLOAD_SIZE: 17,
};

// BundleModuleRecord: identityString, kind, flags, payloadOffset, payloadSize.
const MODULE_RECORD_U32 = 5;

function field(buf, index) {
  return buf.readUInt32LE(8 + index * U32);
}

function stringAt(buf, stringsOffset, offset) {
  const at = stringsOffset + offset;
  const len = buf.readUInt32LE(at);
  return buf.toString('utf8', at + U32, at + U32 + len);
}

const file = process.argv[2];
const wantSuffix = process.argv[3];
const buf = fs.readFileSync(file);

const payloadOffset = field(buf, F.PAYLOAD_OFFSET);
const payloadSize = field(buf, F.PAYLOAD_SIZE);
const stringsOffset = field(buf, F.STRINGS_OFFSET);
const moduleTableOffset = field(buf, F.MODULE_TABLE_OFFSET);
const moduleCount = field(buf, F.MODULE_COUNT);

if (payloadOffset === 0 || payloadOffset + payloadSize !== buf.length ||
    moduleCount === 0 || moduleCount > 1000) {
  console.error(
    'FAIL: header layout moved: payloadOffset=' + payloadOffset +
    ' payloadSize=' + payloadSize + ' moduleCount=' + moduleCount +
    ' fileSize=' + buf.length +
    '\nUpdate the field indices in this test.');
  process.exit(1);
}

let patched = 0;
for (let i = 0; i < moduleCount; i++) {
  const rec = moduleTableOffset + i * MODULE_RECORD_U32 * U32;
  const identity = stringAt(buf, stringsOffset, buf.readUInt32LE(rec));
  if (!identity.endsWith(wantSuffix)) {
    continue;
  }
  const at = payloadOffset + buf.readUInt32LE(rec + 3 * U32);
  const size = buf.readUInt32LE(rec + 4 * U32);
  if (size < 16) {
    console.error('FAIL: ' + identity + ' has a ' + size + '-byte payload');
    process.exit(1);
  }
  buf.fill(0, at, at + 16);
  patched++;
}

if (patched !== 1) {
  console.error('FAIL: patched ' + patched + ' modules matching ' + wantSuffix);
  process.exit(1);
}

fs.writeFileSync(file, buf);
