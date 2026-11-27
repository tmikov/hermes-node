// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The application's file viewer (examples/imgui-app/panels/viewer.js),
// without a window: it requires that module alone, which therefore loads
// neither hermes-node:ui nor hermes-node:imgui, and never calls draw().
// Every case drives createViewer({fs, log}).load(path) through an `fs` the
// test controls and reads `viewer.state` back.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  CAP           a file of 1 MiB + 1 byte through a wrapper that records
//                every method called on fs.promises and on the FileHandle,
//                and each read's length, position and returned byte count:
//                no readFile anywhere; every read stays inside the first
//                1 MiB (position + length <= 1 << 20); each read's
//                position is the sum of what the earlier reads RETURNED;
//                the bytes kept total exactly 1 << 20; close() once;
//                state.truncated true.
//  SHORT_READ    the same file through a handle whose read() returns at
//                most 1000 bytes whatever was asked: the same assertions,
//                and the text kept equals the file's first 1 MiB.
//  EXACT         a file of exactly 1 MiB: truncated false.
//  CLOSE_ON_ERROR a read() that rejects with EIO: status 'error', code
//                EIO, close() still called once.
//  SUPERSEDE     a fake fs whose open() promises the test settles by hand:
//                load('first'), load('second'); the second's chain settled
//                first, then the first's: state is 'second' with its
//                content. Then the first's settled first: still 'second'.
//  CLASSIFY      a.txt text, 3 lines; empty.txt text, 0 lines; binary.bin
//                hex, 16 rows; fffd.txt text (a literal U+FFFD is valid
//                UTF-8); an invalid byte sequence: hex; one U+0000: hex;
//                1 MiB + 3 bytes of valid UTF-8 with a three-byte
//                character across the cap: text, truncated, the cut
//                character dropped (no U+FFFD, no partial character);
//                one 1 MiB line: text, 1 line, cut for display at
//                LINE_LIMIT characters with the marker.
//  MISSING       a path that does not exist: 'error', ENOENT.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  the viewer using readFile: CAP fails on the recorded method;
//  the read loop not stopping at the cap: CAP fails on the sum;
//  close() moved out of `finally`: CLOSE_ON_ERROR fails;
//  the request counter removed: SUPERSEDE fails (the older request
//  finishing last);
//  the read position advanced by the requested length instead of the
//  returned one: SHORT_READ fails;
//  classification by searching the decoded text for U+FFFD: CLASSIFY
//  fails on fffd.txt;
//  the trailing-byte trim removed: CLASSIFY fails on the straddling file.
//
// RUN: rm -rf %t && mkdir -p %t && %hermes-node %s %S/fixtures/imgui/app-tree %t | %FileCheck %s
// CHECK: CAP ok
// CHECK-NEXT: SHORT_READ ok
// CHECK-NEXT: EXACT ok
// CHECK-NEXT: CLOSE_ON_ERROR ok
// CHECK-NEXT: SUPERSEDE ok
// CHECK-NEXT: CLASSIFY ok
// CHECK-NEXT: MISSING ok
// CHECK-NEXT: PASS

'use strict';

const fs = require('fs');
const path = require('path');
const {createViewer, CAP, LINE_LIMIT, CUT_MARKER} =
    require('../examples/imgui-app/panels/viewer.js');

const tree = process.argv[2];
const out = process.argv[3];

function fail(line) {
  console.log(line);
  process.exit(1);
}

/** Whether the module loaded either window module (it must not). */
function checkNoWindowModules() {
  const cache = Object.keys(require.cache);
  if (cache.some((k) => k.includes('hermes-node:')))
    fail('LOADED ' + cache.filter((k) => k.includes('hermes-node:')));
}

/**
 * A recording wrapper around `real` (fs.promises): every method called on
 * it, or on a FileHandle it opened, is recorded by name; each read()
 * records the length and position asked for and the bytes it returned.
 * `readLimit` caps what one read returns, as a short read would.
 */
function recordingFs(real, {readLimit = Infinity, readError = null} = {}) {
  const rec = {methods: [], reads: [], closes: 0};
  const wrapped = {};
  for (const name of Object.keys(real)) {
    if (typeof real[name] !== 'function')
      continue;
    wrapped[name] = (...args) => {
      rec.methods.push('fs.' + name);
      if (name !== 'open')
        return real[name](...args);
      return real.open(...args).then((fh) => wrapHandle(fh));
    };
  }
  function wrapHandle(fh) {
    const handle = {};
    const proto = Object.getPrototypeOf(fh);
    for (const name of Object.getOwnPropertyNames(proto)) {
      if (name === 'constructor' || typeof fh[name] !== 'function')
        continue;
      handle[name] = (...args) => {
        rec.methods.push('handle.' + name);
        return fh[name](...args);
      };
    }
    handle.read = async (buffer, offset, length, position) => {
      rec.methods.push('handle.read');
      if (readError !== null) {
        const e = new Error(readError + ': injected read failure');
        e.code = readError;
        throw e;
      }
      const asked = {length, position, returned: 0};
      rec.reads.push(asked);
      const r = await fh.read(
          buffer, offset, Math.min(length, readLimit), position);
      asked.returned = r.bytesRead;
      return r;
    };
    handle.close = () => {
      rec.methods.push('handle.close');
      ++rec.closes;
      return fh.close();
    };
    return handle;
  }
  return {fs: wrapped, rec};
}

function newViewer(fsImpl) {
  const logged = [];
  const viewer = createViewer({fs: fsImpl, log: (t) => logged.push(t)});
  return {viewer, logged};
}

/** The assertions CAP and SHORT_READ share; returns a failure or null. */
function checkCappedReads(rec, viewer) {
  if (rec.methods.some((m) => /readFile/.test(m)))
    return 'readFile used: ' + rec.methods.join(',');
  if (rec.reads.length === 0)
    return 'no read';
  let sum = 0;
  for (const r of rec.reads) {
    if (typeof r.position !== 'number')
      return 'a read without an explicit position: ' + JSON.stringify(r);
    if (r.position + r.length > CAP)
      return 'a read past the cap: ' + JSON.stringify(r);
    if (r.position !== sum)
      return 'position ' + r.position + ' after ' + sum + ' bytes returned';
    sum += r.returned;
  }
  if (sum !== CAP)
    return 'kept ' + sum + ' bytes, want ' + CAP;
  if (rec.closes !== 1)
    return 'close() called ' + rec.closes + ' times';
  if (viewer.state.truncated !== true)
    return 'truncated ' + viewer.state.truncated;
  return null;
}

/** Lines of `count` bytes in total, ASCII, line lengths not aligned. */
function textOfLength(count) {
  let s = '';
  let n = 0;
  while (s.length < count)
    s += 'line ' + (n++) + ' of the generated text, padded out\n';
  return s.slice(0, count);
}

async function caseCap() {
  const file = path.join(out, 'cap.txt');
  fs.writeFileSync(file, textOfLength(CAP + 1));
  const {fs: wrapped, rec} = recordingFs(fs.promises);
  const {viewer} = newViewer(wrapped);
  await viewer.load(file);
  const problem = checkCappedReads(rec, viewer);
  if (problem !== null || viewer.state.status !== 'text')
    fail('CAP WRONG ' + (problem || viewer.state.status));
  console.log('CAP ok');
}

async function caseShortRead() {
  const file = path.join(out, 'cap.txt');
  const {fs: wrapped, rec} = recordingFs(fs.promises, {readLimit: 1000});
  const {viewer} = newViewer(wrapped);
  await viewer.load(file);
  const problem = checkCappedReads(rec, viewer);
  if (problem !== null)
    fail('SHORT_READ WRONG ' + problem);
  // The first 1 MiB, as lines: what the viewer kept, rejoined.
  const want =
      fs.readFileSync(file).subarray(0, CAP).toString('latin1').split('\n');
  if (want[want.length - 1] === '')
    want.pop();
  const got = viewer.state.lines;
  if (rec.reads.some((r) => r.returned > 1000) || got.length !== want.length ||
      got.join('\n') !== want.join('\n'))
    fail('SHORT_READ WRONG content: ' + got.length + ' lines, want ' +
        want.length);
  console.log('SHORT_READ ok');
}

async function caseExact() {
  const file = path.join(out, 'exact.txt');
  fs.writeFileSync(file, textOfLength(CAP));
  const {viewer} = newViewer(fs.promises);
  await viewer.load(file);
  if (viewer.state.status !== 'text' || viewer.state.truncated !== false)
    fail('EXACT WRONG ' + JSON.stringify(
        {status: viewer.state.status, truncated: viewer.state.truncated}));
  console.log('EXACT ok');
}

async function caseCloseOnError() {
  const {fs: wrapped, rec} = recordingFs(fs.promises, {readError: 'EIO'});
  const {viewer, logged} = newViewer(wrapped);
  await viewer.load(path.join(tree, 'a.txt'));
  const s = viewer.state;
  if (s.status !== 'error' || s.code !== 'EIO' || rec.closes !== 1 ||
      !logged.some((l) => l.includes('EIO')))
    fail('CLOSE_ON_ERROR WRONG ' + JSON.stringify(
        {status: s.status, code: s.code, closes: rec.closes, logged}));
  console.log('CLOSE_ON_ERROR ok');
}

/**
 * A fake fs.promises over in-memory files whose open() returns a promise
 * the test settles: `release(path)` resolves the pending open of `path`.
 */
function handSettledFs(files) {
  const pending = new Map();
  const fake = {
    open(p) {
      return new Promise((resolve) => {
        pending.set(p, () => resolve(memoryHandle(Buffer.from(files[p]))));
      });
    },
  };
  function memoryHandle(bytes) {
    return {
      stat: async () => ({size: bytes.length, isFile: () => true}),
      read: async (buffer, offset, length, position) => {
        const n = Math.max(0, Math.min(length, bytes.length - position));
        bytes.copy(buffer, offset, position, position + n);
        return {bytesRead: n, buffer};
      },
      close: async () => {},
    };
  }
  return {
    fs: fake,
    release(p) {
      pending.get(p)();
    },
  };
}

async function caseSupersede() {
  const files = {first: 'first content\n', second: 'second content\n'};
  for (const order of ['older-last', 'older-first']) {
    const hand = handSettledFs(files);
    const {viewer} = newViewer(hand.fs);
    const p1 = viewer.load('first');
    const p2 = viewer.load('second');
    if (order === 'older-last') {
      hand.release('second');
      await p2;
      hand.release('first');
      await p1;
    } else {
      hand.release('first');
      await p1;
      hand.release('second');
      await p2;
    }
    const s = viewer.state;
    if (s.path !== 'second' || s.status !== 'text' ||
        JSON.stringify(s.lines) !== '["second content"]')
      fail('SUPERSEDE WRONG ' + order + ' ' +
          JSON.stringify({path: s.path, status: s.status, lines: s.lines}));
  }
  console.log('SUPERSEDE ok');
}

async function caseClassify() {
  const {viewer} = newViewer(fs.promises);
  const wrong = [];
  async function expect(label, file, check) {
    await viewer.load(file);
    const s = viewer.state;
    if (!check(s))
      wrong.push(label + '=' + JSON.stringify({
        status: s.status,
        lines: s.lines && s.lines.length,
        rows: s.rows && s.rows.length,
        truncated: s.truncated,
        code: s.code,
      }));
  }
  const write = (name, bytes) => {
    const file = path.join(out, name);
    fs.writeFileSync(file, bytes);
    return file;
  };

  await expect('a.txt', path.join(tree, 'a.txt'),
      (s) => s.status === 'text' && s.lines.length === 3 &&
          s.lines[0] === 'one' && !s.truncated);
  await expect('empty.txt', path.join(tree, 'empty.txt'),
      (s) => s.status === 'text' && s.lines.length === 0);
  await expect('binary.bin', path.join(tree, 'binary.bin'),
      (s) => s.status === 'hex' && s.rows.length === 16);
  await expect('fffd.txt', path.join(tree, 'fffd.txt'),
      (s) => s.status === 'text' && s.lines.length === 1 &&
          s.lines[0].includes('�'));
  await expect('invalid',
      write('invalid.txt', Buffer.from([0x61, 0x62, 0xff, 0xfe, 0x63, 0x0a])),
      (s) => s.status === 'hex' && s.rows.length === 1);
  await expect('nul', write('nul.txt', Buffer.from('abc\u0000def\n')),
      (s) => s.status === 'hex');

  // A three-byte character (U+20AC, E2 82 AC) starting at the cap's last
  // byte: only its first byte is read, and it must be dropped.
  const straddle = Buffer.concat([
    Buffer.from(textOfLength(CAP - 1)),
    Buffer.from('€'),
    Buffer.from('b'),
  ]);
  if (straddle.length !== CAP + 3 || straddle[CAP - 1] !== 0xe2)
    fail('CLASSIFY setup ' + straddle.length);
  await expect('straddle', write('straddle.txt', straddle), (s) => {
    if (s.status !== 'text' || s.truncated !== true)
      return false;
    const kept = Buffer.from(s.lines.join('\n'));
    const want = Buffer.from(textOfLength(CAP - 1).replace(/\n$/, ''));
    return kept.equals(want);
  });

  await expect('longline', write('longline.txt', 'x'.repeat(CAP)),
      (s) => s.status === 'text' && s.lines.length === 1 &&
          !s.truncated &&
          s.lines[0] === 'x'.repeat(LINE_LIMIT) + CUT_MARKER);

  if (wrong.length !== 0)
    fail('CLASSIFY WRONG ' + wrong.join(' '));
  console.log('CLASSIFY ok');
}

async function caseMissing() {
  const {viewer, logged} = newViewer(fs.promises);
  const file = path.join(out, 'does-not-exist');
  await viewer.load(file);
  const s = viewer.state;
  if (s.status !== 'error' || s.code !== 'ENOENT' || s.path !== file ||
      !logged.some((l) => l.includes('ENOENT')))
    fail('MISSING WRONG ' + JSON.stringify({status: s.status, code: s.code}));
  console.log('MISSING ok');
}

async function main() {
  checkNoWindowModules();
  await caseCap();
  await caseShortRead();
  await caseExact();
  await caseCloseOnError();
  await caseSupersede();
  await caseClassify();
  await caseMissing();
  checkNoWindowModules();
  console.log('PASS');
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack || e));
  process.exit(1);
});
