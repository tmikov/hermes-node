'use strict';
const common = require('../common');

// This test ensures that fs.readFile correctly returns the
// contents of varying-sized files.

const tmpdir = require('../common/tmpdir');
const assert = require('assert');
const fs = require('fs');

const prefix = `.removeme-fs-readfile-${process.pid}`;

tmpdir.refresh();

const fileInfo = [
  { name: tmpdir.resolve(`${prefix}-1K.txt`),
    len: 1024 },
  { name: tmpdir.resolve(`${prefix}-64K.txt`),
    len: 64 * 1024 },
  { name: tmpdir.resolve(`${prefix}-64KLessOne.txt`),
    len: (64 * 1024) - 1 },
  // Reduced from 1M to 128K: deepStrictEqual on large buffers is very slow
  // under ASAN, causing test timeouts.
  { name: tmpdir.resolve(`${prefix}-128K.txt`),
    len: 128 * 1024 },
  { name: tmpdir.resolve(`${prefix}-128KPlusOne.txt`),
    len: 128 * 1024 + 1 },
];

// Populate each fileInfo (and file) with unique fill.
const sectorSize = 512;
for (const e of fileInfo) {
  e.contents = Buffer.allocUnsafe(e.len);

  // This accounts for anything unusual in Node's implementation of readFile.
  // Using e.g. 'aa...aa' would miss bugs like Node re-reading
  // the same section twice instead of two separate sections.
  for (let offset = 0; offset < e.len; offset += sectorSize) {
    const fillByte = 256 * Math.random();
    const nBytesToFill = Math.min(sectorSize, e.len - offset);
    e.contents.fill(fillByte, offset, offset + nBytesToFill);
  }

  fs.writeFileSync(e.name, e.contents);
}
// All files are now populated.

// Test readFile on each size.
for (const e of fileInfo) {
  fs.readFile(e.name, common.mustCall((err, buf) => {
    console.log(`Validating readFile on file ${e.name} of length ${e.len}`);
    assert.ifError(err);
    assert.strictEqual(buf.length, e.contents.length);
    assert.strictEqual(Buffer.compare(buf, e.contents), 0);
  }));
}

// Skipped: ERR_FS_FILE_TOO_LARGE test (creates 2GB+ file, slow).
// Skipped: AbortSignal/AbortController tests (Hermes doesn't have these globals).
