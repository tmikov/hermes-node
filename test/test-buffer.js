// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the buffer binding.
'use strict';

var b = internalBinding('buffer');

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}

function assertEq(actual, expected, msg) {
  if (actual !== expected)
    throw new Error('Assertion failed: ' + msg + ' (expected ' + expected + ', got ' + actual + ')');
}

// --- Constants ---
assert(typeof b.kMaxLength === 'number' && b.kMaxLength > 0, 'kMaxLength');
assert(typeof b.kStringMaxLength === 'number' && b.kStringMaxLength > 0, 'kStringMaxLength');

// --- byteLengthUtf8 ---
assertEq(b.byteLengthUtf8('hello'), 5, 'byteLengthUtf8 ascii');
assertEq(b.byteLengthUtf8(''), 0, 'byteLengthUtf8 empty');
// Multi-byte characters
assertEq(b.byteLengthUtf8('\u00e9'), 2, 'byteLengthUtf8 2-byte char');
assertEq(b.byteLengthUtf8('\u4e16'), 3, 'byteLengthUtf8 3-byte char');

// --- compare ---
var a1 = new Uint8Array([1, 2, 3]);
var a2 = new Uint8Array([1, 2, 4]);
var a3 = new Uint8Array([1, 2, 3]);
assert(b.compare(a1, a2) < 0, 'compare a1 < a2');
assert(b.compare(a2, a1) > 0, 'compare a2 > a1');
assertEq(b.compare(a1, a3), 0, 'compare a1 === a3');
// Different lengths
var short1 = new Uint8Array([1, 2]);
assert(b.compare(short1, a1) < 0, 'compare shorter < longer');
assert(b.compare(a1, short1) > 0, 'compare longer > shorter');

// --- compareOffset ---
var src = new Uint8Array([10, 20, 30, 40, 50]);
var tgt = new Uint8Array([20, 30, 40]);
assertEq(b.compareOffset(src, tgt, 0, 1, 3, 4), 0, 'compareOffset matching range');

// --- copy ---
var csrc = new Uint8Array([1, 2, 3, 4, 5]);
var cdst = new Uint8Array(5);
assertEq(b.copy(csrc, cdst, 0, 0, 5), 5, 'copy returns count');
assertEq(cdst[0], 1, 'copy byte 0');
assertEq(cdst[4], 5, 'copy byte 4');
// Partial copy
var cdst2 = new Uint8Array(5);
b.copy(csrc, cdst2, 1, 2, 2);
assertEq(cdst2[0], 0, 'partial copy byte 0 unchanged');
assertEq(cdst2[1], 3, 'partial copy byte 1');
assertEq(cdst2[2], 4, 'partial copy byte 2');

// --- fill ---
// Numeric fill
var fbuf = new Uint8Array(4);
b.fill(fbuf, 42, 0, 4);
assertEq(fbuf[0], 42, 'fill numeric byte 0');
assertEq(fbuf[3], 42, 'fill numeric byte 3');

// Buffer fill (pattern repeat)
var fbuf2 = new Uint8Array(6);
var fpat = new Uint8Array([0xAB, 0xCD]);
b.fill(fbuf2, fpat, 0, 6);
assertEq(fbuf2[0], 0xAB, 'fill buffer pattern byte 0');
assertEq(fbuf2[1], 0xCD, 'fill buffer pattern byte 1');
assertEq(fbuf2[2], 0xAB, 'fill buffer pattern byte 2');
assertEq(fbuf2[5], 0xCD, 'fill buffer pattern byte 5');

// OOB returns -2
var fbufOob = new Uint8Array(4);
assertEq(b.fill(fbufOob, 0, 5, 6), -2, 'fill OOB returns -2');

// --- swap16 ---
var s16 = new Uint8Array([1, 2, 3, 4]);
b.swap16(s16);
assertEq(s16[0], 2, 'swap16 byte 0');
assertEq(s16[1], 1, 'swap16 byte 1');
assertEq(s16[2], 4, 'swap16 byte 2');
assertEq(s16[3], 3, 'swap16 byte 3');

// --- swap32 ---
var s32 = new Uint8Array([1, 2, 3, 4]);
b.swap32(s32);
assertEq(s32[0], 4, 'swap32 byte 0');
assertEq(s32[1], 3, 'swap32 byte 1');
assertEq(s32[2], 2, 'swap32 byte 2');
assertEq(s32[3], 1, 'swap32 byte 3');

// --- swap64 ---
var s64 = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
b.swap64(s64);
assertEq(s64[0], 8, 'swap64 byte 0');
assertEq(s64[7], 1, 'swap64 byte 7');

// --- isUtf8 ---
assert(b.isUtf8(new Uint8Array([0x68, 0x65, 0x6c, 0x6c, 0x6f])) === true, 'isUtf8 valid');
assert(b.isUtf8(new Uint8Array([0xff, 0xfe])) === false, 'isUtf8 invalid');
assert(b.isUtf8(new Uint8Array([])) === true, 'isUtf8 empty');
// Multi-byte valid UTF-8
assert(b.isUtf8(new Uint8Array([0xc3, 0xa9])) === true, 'isUtf8 2-byte');
assert(b.isUtf8(new Uint8Array([0xe4, 0xb8, 0x96])) === true, 'isUtf8 3-byte');

// --- isAscii ---
assert(b.isAscii(new Uint8Array([0x41, 0x42])) === true, 'isAscii valid');
assert(b.isAscii(new Uint8Array([0x80])) === false, 'isAscii invalid');
assert(b.isAscii(new Uint8Array([])) === true, 'isAscii empty');

// --- indexOfNumber ---
var ibuf = new Uint8Array([10, 20, 30, 20, 40]);
assertEq(b.indexOfNumber(ibuf, 20, 0, true), 1, 'indexOfNumber forward');
assertEq(b.indexOfNumber(ibuf, 20, 2, true), 3, 'indexOfNumber forward offset');
assertEq(b.indexOfNumber(ibuf, 20, 4, false), 3, 'indexOfNumber reverse');
assertEq(b.indexOfNumber(ibuf, 20, 2, false), 1, 'indexOfNumber reverse from offset 2');
assertEq(b.indexOfNumber(ibuf, 99, 0, true), -1, 'indexOfNumber not found');

// --- indexOfBuffer ---
var hbuf = new Uint8Array([1, 2, 3, 4, 5, 3, 4]);
var nbuf = new Uint8Array([3, 4]);
assertEq(b.indexOfBuffer(hbuf, nbuf, 0, 1, true), 2, 'indexOfBuffer forward');
assertEq(b.indexOfBuffer(hbuf, nbuf, 3, 1, true), 5, 'indexOfBuffer forward offset');
assertEq(b.indexOfBuffer(hbuf, nbuf, 6, 1, false), 5, 'indexOfBuffer reverse');
assertEq(b.indexOfBuffer(hbuf, nbuf, 4, 1, false), 2, 'indexOfBuffer reverse from 4');
// Empty needle
assertEq(b.indexOfBuffer(hbuf, new Uint8Array([]), 0, 1, true), 0, 'indexOfBuffer empty needle');

// --- atob ---
var decoded = b.atob('SGVsbG8=');
assertEq(decoded, 'Hello', 'atob basic');
// Error cases
assertEq(b.atob('A'), -1, 'atob single char remainder');
assertEq(b.atob('!!!'), -2, 'atob invalid char');

// --- btoa ---
var encoded = b.btoa('Hello');
assertEq(encoded, 'SGVsbG8=', 'btoa basic');

// --- String slice methods ---
// utf8Slice
var sliceBuf = new Uint8Array([0x48, 0x65, 0x6c, 0x6c, 0x6f]); // "Hello"
assertEq(b.utf8Slice.call(sliceBuf, 0, 5), 'Hello', 'utf8Slice');
assertEq(b.utf8Slice.call(sliceBuf, 1, 4), 'ell', 'utf8Slice partial');

// latin1Slice
assertEq(b.latin1Slice.call(sliceBuf, 0, 5), 'Hello', 'latin1Slice');

// asciiSlice
assertEq(b.asciiSlice.call(sliceBuf, 0, 5), 'Hello', 'asciiSlice');

// hexSlice
assertEq(b.hexSlice.call(sliceBuf, 0, 3), '48656c', 'hexSlice');

// base64Slice
assertEq(b.base64Slice.call(sliceBuf, 0, 3), 'SGVs', 'base64Slice');

// base64urlSlice
assertEq(b.base64urlSlice.call(sliceBuf, 0, 3), 'SGVs', 'base64urlSlice');

// ucs2Slice
var ucs2Buf = new Uint8Array([0x48, 0x00, 0x69, 0x00]); // "Hi" in UTF-16LE
assertEq(b.ucs2Slice.call(ucs2Buf, 0, 4), 'Hi', 'ucs2Slice');

// --- String write methods ---
// asciiWriteStatic(buf, string, offset, length)
var wbuf = new Uint8Array(10);
var written = b.asciiWriteStatic(wbuf, 'Hello', 0, 5);
assertEq(written, 5, 'asciiWriteStatic count');
assertEq(wbuf[0], 0x48, 'asciiWriteStatic byte 0');

// latin1WriteStatic
var wbuf2 = new Uint8Array(10);
written = b.latin1WriteStatic(wbuf2, 'AB', 2, 2);
assertEq(written, 2, 'latin1WriteStatic count');
assertEq(wbuf2[2], 0x41, 'latin1WriteStatic byte 2');
assertEq(wbuf2[3], 0x42, 'latin1WriteStatic byte 3');

// utf8WriteStatic
var wbuf3 = new Uint8Array(10);
written = b.utf8WriteStatic(wbuf3, 'Hi', 0, 10);
assertEq(written, 2, 'utf8WriteStatic count');
assertEq(wbuf3[0], 0x48, 'utf8WriteStatic byte 0');
assertEq(wbuf3[1], 0x69, 'utf8WriteStatic byte 1');

// hexWrite (method-style: this.hexWrite(string, offset, length))
var wbuf4 = new Uint8Array(4);
written = b.hexWrite.call(wbuf4, '48656c6c', 0, 4);
assertEq(written, 4, 'hexWrite count');
assertEq(wbuf4[0], 0x48, 'hexWrite byte 0');
assertEq(wbuf4[3], 0x6c, 'hexWrite byte 3');

// base64Write
var wbuf5 = new Uint8Array(5);
written = b.base64Write.call(wbuf5, 'SGVsbG8=', 0, 5);
assertEq(written, 5, 'base64Write count');
assertEq(wbuf5[0], 0x48, 'base64Write byte 0');

// ucs2Write
var wbuf6 = new Uint8Array(4);
written = b.ucs2Write.call(wbuf6, 'Hi', 0, 4);
assertEq(written, 4, 'ucs2Write count');
assertEq(wbuf6[0], 0x48, 'ucs2Write byte 0');
assertEq(wbuf6[1], 0x00, 'ucs2Write byte 1');

// --- setBufferPrototype (no-op, just should not throw) ---
b.setBufferPrototype({});

// --- copyArrayBuffer ---
var srcAb = new ArrayBuffer(4);
var srcView = new Uint8Array(srcAb);
srcView[0] = 10; srcView[1] = 20; srcView[2] = 30; srcView[3] = 40;
var dstAb = new ArrayBuffer(4);
b.copyArrayBuffer(dstAb, 0, srcAb, 0, 4);
var dstView = new Uint8Array(dstAb);
assertEq(dstView[0], 10, 'copyArrayBuffer byte 0');
assertEq(dstView[3], 40, 'copyArrayBuffer byte 3');

// Partial copy with offset
var dstAb2 = new ArrayBuffer(6);
b.copyArrayBuffer(dstAb2, 2, srcAb, 1, 2);
var dstView2 = new Uint8Array(dstAb2);
assertEq(dstView2[2], 20, 'copyArrayBuffer offset dst byte 2');
assertEq(dstView2[3], 30, 'copyArrayBuffer offset dst byte 3');

// --- createUnsafeArrayBuffer ---
var uab = b.createUnsafeArrayBuffer(16);
assert(uab instanceof ArrayBuffer, 'createUnsafeArrayBuffer returns ArrayBuffer');
assertEq(uab.byteLength, 16, 'createUnsafeArrayBuffer correct size');

// --- All functions exist ---
var expectedFns = [
  'byteLengthUtf8', 'compare', 'compareOffset', 'copy', 'fill',
  'indexOfBuffer', 'indexOfNumber', 'indexOfString',
  'swap16', 'swap32', 'swap64', 'isUtf8', 'isAscii', 'atob', 'btoa',
  'setBufferPrototype', 'copyArrayBuffer', 'createUnsafeArrayBuffer',
  'asciiSlice', 'base64Slice', 'base64urlSlice', 'latin1Slice',
  'hexSlice', 'ucs2Slice', 'utf8Slice',
  'asciiWriteStatic', 'latin1WriteStatic', 'utf8WriteStatic',
  'base64Write', 'base64urlWrite', 'hexWrite', 'ucs2Write',
];
expectedFns.forEach(function(name) {
  assert(typeof b[name] === 'function', name + ' is a function');
});

// --- Edge cases: simdutf code paths ---

// isUtf8: truncated multi-byte sequences
assert(b.isUtf8(new Uint8Array([0xc3])) === false, 'isUtf8 truncated 2-byte');
assert(b.isUtf8(new Uint8Array([0xe4, 0xb8])) === false, 'isUtf8 truncated 3-byte');
assert(b.isUtf8(new Uint8Array([0xf0, 0x9f, 0x98])) === false, 'isUtf8 truncated 4-byte');
// Overlong encoding
assert(b.isUtf8(new Uint8Array([0xc0, 0x80])) === false, 'isUtf8 overlong NUL');
// 4-byte emoji
assert(b.isUtf8(new Uint8Array([0xf0, 0x9f, 0x98, 0x80])) === true, 'isUtf8 4-byte emoji');

// base64 edge cases
// Empty string decode
assertEq(b.atob(''), '', 'atob empty string');
// Padding-only
assertEq(b.atob('===='), -2, 'atob padding-only');
// base64url slice of empty buffer
var emptyBuf = new Uint8Array(0);
assertEq(b.base64urlSlice.call(emptyBuf, 0, 0), '', 'base64urlSlice empty');
assertEq(b.base64Slice.call(emptyBuf, 0, 0), '', 'base64Slice empty');
// Single byte base64 encode/decode roundtrip
var oneByteBuf = new Uint8Array([0xff]);
var b64One = b.base64Slice.call(oneByteBuf, 0, 1);
var wbuf_b64 = new Uint8Array(1);
b.base64Write.call(wbuf_b64, b64One, 0, 1);
assertEq(wbuf_b64[0], 0xff, 'base64 roundtrip single byte');

// utf8WriteStatic: UTF-8 boundary truncation edge cases
// 2-byte char (é = 0xc3 0xa9) that doesn't fit in 1 byte
var wbuf_utf8_1 = new Uint8Array(1);
written = b.utf8WriteStatic(wbuf_utf8_1, '\u00e9', 0, 1);
assertEq(written, 0, 'utf8WriteStatic truncates 2-byte char at 1 byte');

// 3-byte char (世 = 0xe4 0xb8 0x96) that doesn't fit in 2 bytes
var wbuf_utf8_2 = new Uint8Array(2);
written = b.utf8WriteStatic(wbuf_utf8_2, '\u4e16', 0, 2);
assertEq(written, 0, 'utf8WriteStatic truncates 3-byte char at 2 bytes');

// 4-byte emoji (😀 = 0xf0 0x9f 0x98 0x80) that doesn't fit in 3 bytes
var wbuf_utf8_3 = new Uint8Array(3);
written = b.utf8WriteStatic(wbuf_utf8_3, '\uD83D\uDE00', 0, 3);
assertEq(written, 0, 'utf8WriteStatic truncates 4-byte char at 3 bytes');

// Mixed ASCII + multi-byte: "A\u00e9" = [0x41, 0xc3, 0xa9], maxLength=2 -> only 'A'
var wbuf_utf8_4 = new Uint8Array(2);
written = b.utf8WriteStatic(wbuf_utf8_4, 'A\u00e9', 0, 2);
assertEq(written, 1, 'utf8WriteStatic keeps ASCII, truncates partial 2-byte');
assertEq(wbuf_utf8_4[0], 0x41, 'utf8WriteStatic partial keeps A');

// Exact fit: "A\u00e9" in 3 bytes
var wbuf_utf8_5 = new Uint8Array(3);
written = b.utf8WriteStatic(wbuf_utf8_5, 'A\u00e9', 0, 3);
assertEq(written, 3, 'utf8WriteStatic exact fit A+2-byte');
assertEq(wbuf_utf8_5[0], 0x41, 'utf8WriteStatic exact A');
assertEq(wbuf_utf8_5[1], 0xc3, 'utf8WriteStatic exact é byte 1');
assertEq(wbuf_utf8_5[2], 0xa9, 'utf8WriteStatic exact é byte 2');

// Empty string write
var wbuf_utf8_6 = new Uint8Array(4);
written = b.utf8WriteStatic(wbuf_utf8_6, '', 0, 4);
assertEq(written, 0, 'utf8WriteStatic empty string');

// hexWrite edge cases
var wbuf_hex = new Uint8Array(4);
written = b.hexWrite.call(wbuf_hex, '', 0, 4);
assertEq(written, 0, 'hexWrite empty string');
// Odd-length hex string (trailing nibble ignored)
var wbuf_hex2 = new Uint8Array(4);
written = b.hexWrite.call(wbuf_hex2, '4865f', 0, 4);
assertEq(written, 2, 'hexWrite odd-length string');
assertEq(wbuf_hex2[0], 0x48, 'hexWrite odd byte 0');
assertEq(wbuf_hex2[1], 0x65, 'hexWrite odd byte 1');

console.log('PASS');
