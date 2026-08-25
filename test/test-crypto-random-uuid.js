// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s

// crypto.randomUUID(), an RFC 4122 version 4 UUID.
//
// Every expectation here was checked against node v24.13.1 before it was
// written: the accepted and rejected argument shapes, the error code, and the
// two nibbles the RFC fixes. The batching cases exist because Node generates
// entropy 128 UUIDs at a time and hands out one 16-byte slice per call; an
// off-by-one in that bookkeeping repeats a UUID or reads past the buffer, and
// neither shows up in a test that only calls randomUUID once.

'use strict';

var assert = require('assert');
var crypto = require('crypto');

var UUID_RE =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

// -- shape --

assert.strictEqual(typeof crypto.randomUUID, 'function', 'randomUUID exists');

var u = crypto.randomUUID();
assert.strictEqual(typeof u, 'string', 'returns a string');
assert.strictEqual(u.length, 36, 'is 36 characters');
assert(UUID_RE.test(u), 'is lowercase hex in 8-4-4-4-12 form: ' + u);

// -- the two nibbles RFC 4122 fixes --

assert.strictEqual(u[14], '4', 'version nibble is 4');
assert('89ab'.indexOf(u[19]) !== -1, 'variant nibble is one of 8,9,a,b: ' + u[19]);

// -- uniqueness across a batch boundary --
//
// 500 > 128, so this crosses the batch refill three times. A refill that
// forgets to re-randomize, or an index that revisits a slice, collides here.

var seen = new Set();
var badVersion = 0;
var badVariant = 0;
for (var i = 0; i < 500; i++) {
  var id = crypto.randomUUID();
  assert(UUID_RE.test(id), 'well-formed at iteration ' + i + ': ' + id);
  if (id[14] !== '4') badVersion++;
  if ('89ab'.indexOf(id[19]) === -1) badVariant++;
  seen.add(id);
}
assert.strictEqual(seen.size, 500, 'all 500 UUIDs distinct');
assert.strictEqual(badVersion, 0, 'every UUID is version 4');
assert.strictEqual(badVariant, 0, 'every UUID has the RFC variant');

// -- the entropy cache can be turned off, and still produces valid UUIDs --

var unbuffered = new Set();
for (var j = 0; j < 200; j++) {
  var un = crypto.randomUUID({ disableEntropyCache: true });
  assert(UUID_RE.test(un), 'unbuffered UUID well-formed: ' + un);
  unbuffered.add(un);
}
assert.strictEqual(unbuffered.size, 200, 'unbuffered UUIDs distinct');

// Buffered and unbuffered draw from the same generator but must not repeat
// each other either.
var mixed = new Set();
for (var k = 0; k < 100; k++) {
  mixed.add(crypto.randomUUID());
  mixed.add(crypto.randomUUID({ disableEntropyCache: true }));
}
assert.strictEqual(mixed.size, 200, 'buffered and unbuffered do not collide');

// -- accepted argument shapes --

assert(UUID_RE.test(crypto.randomUUID(undefined)), 'undefined options allowed');
assert(UUID_RE.test(crypto.randomUUID({})), 'empty options allowed');
assert(UUID_RE.test(crypto.randomUUID({ nope: 1 })), 'unknown keys ignored');
assert(
  UUID_RE.test(crypto.randomUUID({ disableEntropyCache: false })),
  'disableEntropyCache:false allowed',
);

// -- rejected argument shapes, with Node's error code --

function rejects(fn, what) {
  var threw = false;
  try {
    fn();
  } catch (e) {
    threw = true;
    assert.strictEqual(e.code, 'ERR_INVALID_ARG_TYPE', what + ' code');
  }
  assert(threw, what + ' throws');
}

rejects(function () { crypto.randomUUID(null); }, 'null options');
rejects(function () { crypto.randomUUID('x'); }, 'string options');
rejects(function () { crypto.randomUUID(42); }, 'number options');
rejects(
  function () { crypto.randomUUID({ disableEntropyCache: 'x' }); },
  'non-boolean disableEntropyCache (string)',
);
rejects(
  function () { crypto.randomUUID({ disableEntropyCache: 1 }); },
  'non-boolean disableEntropyCache (number)',
);

console.log('PASS');
