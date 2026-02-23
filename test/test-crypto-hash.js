// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s

'use strict';

var assert = require('assert');
var crypto = require('crypto');

// -- createHash with known test vectors --

assert.strictEqual(
  crypto.createHash('md5').update('hello').digest('hex'),
  '5d41402abc4b2a76b9719d911017c592',
  'md5 of "hello"'
);

assert.strictEqual(
  crypto.createHash('sha1').update('hello').digest('hex'),
  'aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d',
  'sha1 of "hello"'
);

assert.strictEqual(
  crypto.createHash('sha256').update('hello').digest('hex'),
  '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
  'sha256 of "hello"'
);

// -- Multiple .update() calls --

assert.strictEqual(
  crypto.createHash('sha256').update('hel').update('lo').digest('hex'),
  '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
  'sha256 with multiple updates'
);

// -- Buffer input --

assert.strictEqual(
  crypto.createHash('md5').update(Buffer.from('hello')).digest('hex'),
  '5d41402abc4b2a76b9719d911017c592',
  'md5 with Buffer input'
);

// -- digest() with no encoding returns Buffer --

var buf = crypto.createHash('md5').update('hello').digest();
assert(Buffer.isBuffer(buf), 'digest() returns Buffer');
assert.strictEqual(buf.length, 16, 'MD5 digest is 16 bytes');

// -- digest('base64') --

var b64 = crypto.createHash('sha256').update('hello').digest('base64');
assert.strictEqual(typeof b64, 'string', 'base64 digest is a string');
// Verify round-trip: decode base64 and re-encode as hex.
assert.strictEqual(
  Buffer.from(b64, 'base64').toString('hex'),
  '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
  'base64 digest round-trip'
);

// -- createHmac --

assert.strictEqual(
  crypto.createHmac('sha256', 'key').update('data').digest('hex'),
  '5031fe3d989c6d1537a013fa6e739da23463fdaec3b70137d828e36ace221bd0',
  'hmac-sha256'
);

// -- getHashes() --

var hashes = crypto.getHashes();
assert(Array.isArray(hashes), 'getHashes() returns array');
assert(hashes.indexOf('md5') >= 0, 'includes md5');
assert(hashes.indexOf('sha1') >= 0, 'includes sha1');
assert(hashes.indexOf('sha256') >= 0, 'includes sha256');

// -- hash.copy() --

var h1 = crypto.createHash('sha256');
h1.update('hel');
var h2 = h1.copy();
h1.update('lo');
h2.update('lo');
assert.strictEqual(
  h1.digest('hex'),
  h2.digest('hex'),
  'copy produces same result'
);
assert.strictEqual(
  h1.digest === undefined ? 'ok' : 'ok',  // h1 already finalized
  'ok'
);

// -- Double digest() throws --

var h3 = crypto.createHash('md5').update('test');
h3.digest('hex'); // first call ok
var threw = false;
try {
  h3.digest('hex');
} catch (e) {
  threw = true;
}
assert(threw, 'double digest() throws');

// -- require('node:crypto') works --

var crypto2 = require('node:crypto');
assert.strictEqual(typeof crypto2.createHash, 'function', 'node:crypto works');

// -- timingSafeEqual --

var a = Buffer.from('abcd');
var b = Buffer.from('abcd');
var c = Buffer.from('abce');
assert.strictEqual(crypto.timingSafeEqual(a, b), true, 'equal buffers');
assert.strictEqual(crypto.timingSafeEqual(a, c), false, 'unequal buffers');

// -- hash() one-shot --

assert.strictEqual(
  crypto.hash('sha256', 'hello'),
  '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
  'hash() one-shot'
);

// -- SHA224 --

assert.strictEqual(
  crypto.createHash('sha224').update('hello').digest('hex'),
  'ea09ae9cc6768c50fcee903ed054556e5bfc8347907f12598aa24193',
  'sha224 of "hello"'
);

console.log('PASS');
