/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Test for the encoding_binding native binding.
// Run via: hermes-node --node-lib-path <project-root> test-encoding.js

var assert = globalThis.assert;
if (!assert) {
  assert = function(cond, msg) {
    if (!cond)
      throw new Error('Assertion failed: ' + (msg || ''));
  };
}

function assertEquals(a, b, msg) {
  if (a !== b)
    throw new Error(
      'assertEquals failed: ' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) +
      (msg ? ' (' + msg + ')' : ''));
}

var enc = internalBinding('encoding_binding');
var assertions = 0;

// --- Verify all expected exports exist ---

assert(typeof enc === 'object', 'encoding_binding is an object');
assertions++;

assert(typeof enc.encodeUtf8String === 'function', 'encodeUtf8String is a function');
assertions++;

assert(typeof enc.encodeInto === 'function', 'encodeInto is a function');
assertions++;

assert(enc.encodeIntoResults instanceof Uint32Array, 'encodeIntoResults is Uint32Array');
assertions++;
assertEquals(enc.encodeIntoResults.length, 2, 'encodeIntoResults has 2 elements');
assertions++;

assert(typeof enc.decodeUTF8 === 'function', 'decodeUTF8 is a function');
assertions++;

assert(typeof enc.decodeLatin1 === 'function', 'decodeLatin1 is a function');
assertions++;

assert(typeof enc.toASCII === 'function', 'toASCII is a function');
assertions++;

assert(typeof enc.toUnicode === 'function', 'toUnicode is a function');
assertions++;

// --- encodeUtf8String ---

// Simple ASCII
var encoded = enc.encodeUtf8String('hello');
assert(encoded instanceof Uint8Array, 'encodeUtf8String returns Uint8Array');
assertions++;
assertEquals(encoded.length, 5, 'encodeUtf8String length for ASCII');
assertions++;
assertEquals(encoded[0], 0x68, 'h');
assertions++;
assertEquals(encoded[1], 0x65, 'e');
assertions++;
assertEquals(encoded[2], 0x6C, 'l');
assertions++;
assertEquals(encoded[3], 0x6C, 'l');
assertions++;
assertEquals(encoded[4], 0x6F, 'o');
assertions++;

// Empty string
var empty = enc.encodeUtf8String('');
assert(empty instanceof Uint8Array, 'encodeUtf8String empty returns Uint8Array');
assertions++;
assertEquals(empty.length, 0, 'encodeUtf8String empty has length 0');
assertions++;

// Multi-byte UTF-8 (U+00E9 = e-acute = 0xC3 0xA9)
var accented = enc.encodeUtf8String('\u00e9');
assertEquals(accented.length, 2, 'encodeUtf8String 2-byte char');
assertions++;
assertEquals(accented[0], 0xC3, 'first byte of e-acute');
assertions++;
assertEquals(accented[1], 0xA9, 'second byte of e-acute');
assertions++;

// 3-byte UTF-8 (U+4E16 = CJK character = 0xE4 0xB8 0x96)
var cjk = enc.encodeUtf8String('\u4E16');
assertEquals(cjk.length, 3, 'encodeUtf8String 3-byte char');
assertions++;
assertEquals(cjk[0], 0xE4, 'first byte of CJK');
assertions++;
assertEquals(cjk[1], 0xB8, 'second byte of CJK');
assertions++;
assertEquals(cjk[2], 0x96, 'third byte of CJK');
assertions++;

// --- encodeInto ---

var dest = new Uint8Array(10);
enc.encodeInto('Hi!', dest);
var results = enc.encodeIntoResults;
assertEquals(results[0], 3, 'encodeInto: 3 chars read');
assertions++;
assertEquals(results[1], 3, 'encodeInto: 3 bytes written');
assertions++;
assertEquals(dest[0], 0x48, 'H');
assertions++;
assertEquals(dest[1], 0x69, 'i');
assertions++;
assertEquals(dest[2], 0x21, '!');
assertions++;

// Truncation test: dest too small for all chars
var smallDest = new Uint8Array(2);
enc.encodeInto('ABCDE', smallDest);
results = enc.encodeIntoResults;
assertEquals(results[0], 2, 'encodeInto truncated: 2 chars read');
assertions++;
assertEquals(results[1], 2, 'encodeInto truncated: 2 bytes written');
assertions++;
assertEquals(smallDest[0], 0x41, 'A');
assertions++;
assertEquals(smallDest[1], 0x42, 'B');
assertions++;

// Multi-byte truncation: dest too small for multi-byte char
var smallDest2 = new Uint8Array(1);
enc.encodeInto('\u00e9', smallDest2); // needs 2 bytes
results = enc.encodeIntoResults;
assertEquals(results[0], 0, 'encodeInto multi-byte truncated: 0 chars read');
assertions++;
assertEquals(results[1], 0, 'encodeInto multi-byte truncated: 0 bytes written');
assertions++;

// --- decodeUTF8 ---

// Simple ASCII decode
var asciiBytes = new Uint8Array([0x48, 0x65, 0x6C, 0x6C, 0x6F]);
var decoded = enc.decodeUTF8(asciiBytes, false, false);
assertEquals(decoded, 'Hello', 'decodeUTF8 ASCII');
assertions++;

// UTF-8 multi-byte decode
var utf8Bytes = new Uint8Array([0xC3, 0xA9]); // e-acute
decoded = enc.decodeUTF8(utf8Bytes, false, false);
assertEquals(decoded, '\u00e9', 'decodeUTF8 2-byte');
assertions++;

// CJK decode
var cjkBytes = new Uint8Array([0xE4, 0xB8, 0x96]);
decoded = enc.decodeUTF8(cjkBytes, false, false);
assertEquals(decoded, '\u4E16', 'decodeUTF8 3-byte');
assertions++;

// BOM handling: BOM at start should be stripped by default (ignoreBOM=false)
var bomBytes = new Uint8Array([0xEF, 0xBB, 0xBF, 0x41, 0x42]);
decoded = enc.decodeUTF8(bomBytes, false, false);
assertEquals(decoded, 'AB', 'decodeUTF8 strips BOM');
assertions++;

// BOM handling: ignoreBOM=true should keep BOM
decoded = enc.decodeUTF8(bomBytes, true, false);
// When ignoreBOM is true, the BOM is NOT stripped (it remains as U+FEFF)
assertEquals(decoded.length, 3, 'decodeUTF8 keeps BOM when ignoreBOM=true');
assertions++;

// Empty input
decoded = enc.decodeUTF8(new Uint8Array(0), false, false);
assertEquals(decoded, '', 'decodeUTF8 empty');
assertions++;

// Fatal mode: valid UTF-8 should succeed
decoded = enc.decodeUTF8(asciiBytes, false, true);
assertEquals(decoded, 'Hello', 'decodeUTF8 fatal valid');
assertions++;

// Fatal mode: invalid UTF-8 should throw
var threw = false;
try {
  var invalidUtf8 = new Uint8Array([0xFF, 0xFE]);
  enc.decodeUTF8(invalidUtf8, false, true);
} catch (e) {
  threw = true;
}
assert(threw, 'decodeUTF8 fatal throws on invalid UTF-8');
assertions++;

// Non-fatal mode: invalid UTF-8 should use replacement characters
var invalidBytes = new Uint8Array([0x48, 0xFF, 0x69]); // H, invalid, i
decoded = enc.decodeUTF8(invalidBytes, false, false);
// Should contain H, replacement char, i
assert(decoded.length >= 3, 'decodeUTF8 non-fatal handles invalid bytes');
assertions++;
assertEquals(decoded[0], 'H', 'decodeUTF8 non-fatal: H preserved');
assertions++;
assertEquals(decoded[decoded.length - 1], 'i', 'decodeUTF8 non-fatal: i preserved');
assertions++;

// ArrayBuffer input
var ab = new ArrayBuffer(3);
var view = new Uint8Array(ab);
view[0] = 0x41; view[1] = 0x42; view[2] = 0x43;
decoded = enc.decodeUTF8(ab, false, false);
assertEquals(decoded, 'ABC', 'decodeUTF8 ArrayBuffer input');
assertions++;

// --- decodeLatin1 ---

// Simple Latin-1 decode (ASCII range)
var latin1Bytes = new Uint8Array([0x48, 0x65, 0x6C, 0x6C, 0x6F]);
decoded = enc.decodeLatin1(latin1Bytes, false, false);
assertEquals(decoded, 'Hello', 'decodeLatin1 ASCII range');
assertions++;

// Latin-1 high bytes (U+00E9 = 0xE9 in Latin-1)
var latin1High = new Uint8Array([0xE9]); // e-acute in Latin-1
decoded = enc.decodeLatin1(latin1High, false, false);
assertEquals(decoded, '\u00e9', 'decodeLatin1 high byte');
assertions++;

// Latin-1 0xFF (U+00FF = y-diaeresis)
var latin1FF = new Uint8Array([0xFF]);
decoded = enc.decodeLatin1(latin1FF, false, false);
assertEquals(decoded, '\u00FF', 'decodeLatin1 0xFF');
assertions++;

// Empty Latin-1
decoded = enc.decodeLatin1(new Uint8Array(0), false, false);
assertEquals(decoded, '', 'decodeLatin1 empty');
assertions++;

// --- toASCII / toUnicode ---

// ASCII domains should pass through
assertEquals(enc.toASCII('example.com'), 'example.com', 'toASCII ASCII domain');
assertions++;

assertEquals(enc.toUnicode('example.com'), 'example.com', 'toUnicode ASCII domain');
assertions++;

// IDN: internationalized domain names (now using Ada library)
// German umlaut domain
assertEquals(enc.toASCII('m\u00fcnchen.de'), 'xn--mnchen-3ya.de', 'toASCII German umlaut');
assertions++;

// Chinese domain
assertEquals(enc.toASCII('\u4e2d\u6587.com'), 'xn--fiq228c.com', 'toASCII Chinese domain');
assertions++;

// Mixed ASCII and non-ASCII labels
assertEquals(enc.toASCII('www.\u00e9xample.com'), 'www.xn--xample-9ua.com', 'toASCII mixed labels');
assertions++;

// toASCII of already-punycoded domain should be idempotent
assertEquals(enc.toASCII('xn--mnchen-3ya.de'), 'xn--mnchen-3ya.de', 'toASCII punycode idempotent');
assertions++;

// toUnicode should convert punycode back to Unicode
assertEquals(enc.toUnicode('xn--mnchen-3ya.de'), 'm\u00fcnchen.de', 'toUnicode German umlaut');
assertions++;

assertEquals(enc.toUnicode('xn--fiq228c.com'), '\u4e2d\u6587.com', 'toUnicode Chinese domain');
assertions++;

// Empty string
assertEquals(enc.toASCII(''), '', 'toASCII empty');
assertions++;

assertEquals(enc.toUnicode(''), '', 'toUnicode empty');
assertions++;

console.log('encoding_binding: ' + assertions + ' assertions passed');
console.log('PASS');
