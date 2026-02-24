// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s

'use strict';

var assert = require('assert');
var crypto = require('crypto');

// -- randomBytes(size) returns Buffer of correct length --

var buf16 = crypto.randomBytes(16);
assert(Buffer.isBuffer(buf16), 'randomBytes returns Buffer');
assert.strictEqual(buf16.length, 16, 'randomBytes(16) has length 16');

// -- randomBytes(0) returns empty Buffer --

var buf0 = crypto.randomBytes(0);
assert(Buffer.isBuffer(buf0), 'randomBytes(0) returns Buffer');
assert.strictEqual(buf0.length, 0, 'randomBytes(0) has length 0');

// -- randomBytes produces non-zero output (statistical check) --

var big = crypto.randomBytes(64);
var hasNonZero = false;
for (var i = 0; i < big.length; i++) {
  if (big[i] !== 0) { hasNonZero = true; break; }
}
assert(hasNonZero, 'randomBytes(64) should have some non-zero bytes');

// -- randomFillSync fills Buffer --

var fillBuf = Buffer.alloc(32);
var ret = crypto.randomFillSync(fillBuf);
assert.strictEqual(ret, fillBuf, 'randomFillSync returns the same buffer');
hasNonZero = false;
for (var i = 0; i < fillBuf.length; i++) {
  if (fillBuf[i] !== 0) { hasNonZero = true; break; }
}
assert(hasNonZero, 'randomFillSync should fill with some non-zero bytes');

// -- randomFillSync with offset and size --

var partialBuf = Buffer.alloc(32);
crypto.randomFillSync(partialBuf, 8, 8);
// First 8 bytes should still be zero.
for (var i = 0; i < 8; i++) {
  assert.strictEqual(partialBuf[i], 0, 'bytes before offset are untouched');
}
// Last 16 bytes should still be zero.
for (var i = 16; i < 32; i++) {
  assert.strictEqual(partialBuf[i], 0, 'bytes after offset+size are untouched');
}

// -- randomFillSync with TypedArray --

var u8 = new Uint8Array(16);
crypto.randomFillSync(u8);
hasNonZero = false;
for (var i = 0; i < u8.length; i++) {
  if (u8[i] !== 0) { hasNonZero = true; break; }
}
assert(hasNonZero, 'randomFillSync works with Uint8Array');

// -- randomBytes async form --

var asyncDone = false;
crypto.randomBytes(16, function(err, buf) {
  assert.ifError(err);
  assert(Buffer.isBuffer(buf), 'async randomBytes returns Buffer');
  assert.strictEqual(buf.length, 16, 'async randomBytes has correct length');
  asyncDone = true;
});

// -- randomInt(max) --

for (var i = 0; i < 100; i++) {
  var n = crypto.randomInt(10);
  assert(n >= 0 && n < 10, 'randomInt(10) in range [0,10): got ' + n);
}

// -- randomInt(min, max) --

for (var i = 0; i < 100; i++) {
  var n = crypto.randomInt(5, 10);
  assert(n >= 5 && n < 10, 'randomInt(5,10) in range [5,10): got ' + n);
}

// -- randomInt(min, max, callback) --

var asyncIntDone = false;
crypto.randomInt(0, 100, function(err, n) {
  assert.strictEqual(err, null, 'async randomInt callback err is null');
  assert.ifError(err);
  assert(n >= 0 && n < 100, 'async randomInt in range');
  asyncIntDone = true;
});

// -- randomFillSync with DataView --

var viewBuf = new ArrayBuffer(16);
var dv = new DataView(viewBuf);
crypto.randomFillSync(dv, 4, 8);
for (var i = 0; i < 4; i++) {
  assert.strictEqual(dv.getUint8(i), 0, 'DataView bytes before offset are untouched');
}
for (var i = 12; i < 16; i++) {
  assert.strictEqual(dv.getUint8(i), 0, 'DataView bytes after offset+size are untouched');
}

// -- Error cases --

// randomBytes with negative size.
assert.throws(function() { crypto.randomBytes(-1); }, RangeError);

// randomFillSync with bad offset.
assert.throws(function() { crypto.randomFillSync(Buffer.alloc(8), 9); }, RangeError);

// randomFillSync with bad size.
assert.throws(function() { crypto.randomFillSync(Buffer.alloc(8), 0, 9); }, RangeError);

// randomFillSync with non-object.
assert.throws(function() { crypto.randomFillSync(42); }, TypeError);

// randomInt with max <= min.
assert.throws(function() { crypto.randomInt(10, 5); }, RangeError);

// randomInt with range above 2^48 - 1.
assert.throws(function() {
  crypto.randomInt(Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);
}, RangeError);

// Verify async callbacks fire.
process.on('exit', function() {
  assert(asyncDone, 'async randomBytes callback must fire');
  assert(asyncIntDone, 'async randomInt callback must fire');
  console.log('PASS');
});
