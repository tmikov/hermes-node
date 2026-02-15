// Copyright (c) Tzvetan Mikov.
// Test the string_decoder binding.
'use strict';

var sd = internalBinding('string_decoder');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

// --- Constants ---
assertEqual(sd.kIncompleteCharactersStart, 0, 'kIncompleteCharactersStart');
assertEqual(sd.kIncompleteCharactersEnd, 4, 'kIncompleteCharactersEnd');
assertEqual(sd.kMissingBytes, 4, 'kMissingBytes');
assertEqual(sd.kBufferedBytes, 5, 'kBufferedBytes');
assertEqual(sd.kEncodingField, 6, 'kEncodingField');
assertEqual(sd.kNumFields, 7, 'kNumFields');
assertEqual(sd.kSize, 7, 'kSize');

// --- Encodings array ---
assert(Array.isArray(sd.encodings), 'encodings is array');
assertEqual(sd.encodings[0], 'ascii', 'encodings[0]');
assertEqual(sd.encodings[1], 'utf8', 'encodings[1]');
assertEqual(sd.encodings[2], 'base64', 'encodings[2]');
assertEqual(sd.encodings[3], 'utf16le', 'encodings[3]');
assertEqual(sd.encodings[4], 'latin1', 'encodings[4]');
assertEqual(sd.encodings[5], 'hex', 'encodings[5]');
assertEqual(sd.encodings[6], 'buffer', 'encodings[6]');
assertEqual(sd.encodings[7], 'base64url', 'encodings[7]');

// --- Functions ---
assertEqual(typeof sd.decode, 'function', 'decode is function');
assertEqual(typeof sd.flush, 'function', 'flush is function');

// --- UTF-8 decoding (encoding index 1) ---
// Create a decoder state buffer (7 bytes).
var state = new Uint8Array(7);
state[6] = 1; // UTF8 encoding

// Simple ASCII via UTF-8 decoder.
var input = new Uint8Array([72, 101, 108, 108, 111]); // "Hello"
var result = sd.decode(state, input);
assertEqual(result, 'Hello', 'UTF-8 decode simple ASCII');

// Multi-byte UTF-8: euro sign U+20AC = E2 82 AC
state = new Uint8Array(7);
state[6] = 1; // UTF8
var euro = new Uint8Array([0xE2, 0x82, 0xAC]);
result = sd.decode(state, euro);
assertEqual(result, '\u20AC', 'UTF-8 decode euro sign');

// Split multi-byte character across two chunks.
state = new Uint8Array(7);
state[6] = 1; // UTF8
// First chunk: first 2 bytes of a 3-byte sequence (euro sign).
var chunk1 = new Uint8Array([0xE2, 0x82]);
result = sd.decode(state, chunk1);
assertEqual(result, '', 'UTF-8 split char: first chunk empty');
// State should show buffered/missing bytes.
assertEqual(state[5], 2, 'UTF-8 split: bufferedBytes=2');
assertEqual(state[4], 1, 'UTF-8 split: missingBytes=1');

// Second chunk: last byte of euro sign + "A".
var chunk2 = new Uint8Array([0xAC, 0x41]);
result = sd.decode(state, chunk2);
assertEqual(result, '\u20ACA', 'UTF-8 split char: second chunk');

// Flush with no buffered data returns empty string.
state = new Uint8Array(7);
state[6] = 1; // UTF8
result = sd.flush(state);
assertEqual(result, '', 'flush empty UTF-8');

// Flush with buffered incomplete character.
state = new Uint8Array(7);
state[6] = 1; // UTF8
var incomplete = new Uint8Array([0xE2, 0x82]); // incomplete euro
sd.decode(state, incomplete);
result = sd.flush(state);
// Flushing incomplete UTF-8 produces replacement character(s).
assert(typeof result === 'string', 'flush incomplete UTF-8 returns string');

// --- Latin1 decoding (encoding index 4) ---
state = new Uint8Array(7);
state[6] = 4; // LATIN1
var latin = new Uint8Array([0xC0, 0xE9, 0xF1]); // various latin1 chars
result = sd.decode(state, latin);
assertEqual(result.length, 3, 'Latin1 decode length');
assertEqual(result.charCodeAt(0), 0xC0, 'Latin1 byte 0xC0');
assertEqual(result.charCodeAt(1), 0xE9, 'Latin1 byte 0xE9');
assertEqual(result.charCodeAt(2), 0xF1, 'Latin1 byte 0xF1');

// --- ASCII decoding (encoding index 0) ---
state = new Uint8Array(7);
state[6] = 0; // ASCII
var ascii = new Uint8Array([65, 66, 67]); // "ABC"
result = sd.decode(state, ascii);
assertEqual(result, 'ABC', 'ASCII decode');

// --- Hex decoding (encoding index 5) ---
state = new Uint8Array(7);
state[6] = 5; // HEX
var hexInput = new Uint8Array([0xDE, 0xAD, 0xBE, 0xEF]);
result = sd.decode(state, hexInput);
assertEqual(result, 'deadbeef', 'Hex decode');

// --- Base64 decoding (encoding index 2) ---
state = new Uint8Array(7);
state[6] = 2; // BASE64
// "Hello" in ASCII = [72, 101, 108, 108, 111]
var b64Input = new Uint8Array([72, 101, 108, 108, 111, 33]); // "Hello!"
result = sd.decode(state, b64Input);
assertEqual(result, 'SGVsbG8h', 'Base64 decode "Hello!"');

// Base64 split: 1 leftover byte (needs 2 more to form a triple).
state = new Uint8Array(7);
state[6] = 2; // BASE64
var b64Chunk1 = new Uint8Array([65]); // 1 byte: not a multiple of 3
result = sd.decode(state, b64Chunk1);
assertEqual(result, '', 'Base64 split: first chunk empty');
assertEqual(state[5], 1, 'Base64 split: bufferedBytes=1');
assertEqual(state[4], 2, 'Base64 split: missingBytes=2');

// Second chunk completes the triple.
var b64Chunk2 = new Uint8Array([66, 67]);
result = sd.decode(state, b64Chunk2);
assertEqual(result, 'QUJD', 'Base64 split: second chunk');

// --- UCS2/UTF16LE decoding (encoding index 3) ---
state = new Uint8Array(7);
state[6] = 3; // UCS2
// "Hi" in UTF-16LE: H=0x0048, i=0x0069
var ucs2Input = new Uint8Array([0x48, 0x00, 0x69, 0x00]);
result = sd.decode(state, ucs2Input);
assertEqual(result, 'Hi', 'UCS2 decode "Hi"');

// UCS2 split: odd number of bytes.
state = new Uint8Array(7);
state[6] = 3; // UCS2
var ucs2Chunk1 = new Uint8Array([0x48, 0x00, 0x69]);
result = sd.decode(state, ucs2Chunk1);
assertEqual(result, 'H', 'UCS2 split: first chunk');
assertEqual(state[5], 1, 'UCS2 split: bufferedBytes=1');
assertEqual(state[4], 1, 'UCS2 split: missingBytes=1');

var ucs2Chunk2 = new Uint8Array([0x00]);
result = sd.decode(state, ucs2Chunk2);
assertEqual(result, 'i', 'UCS2 split: second chunk');

console.log('PASS');
