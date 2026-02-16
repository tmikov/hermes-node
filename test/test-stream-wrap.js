// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test for the stream_wrap binding stub.
'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var sw = internalBinding('stream_wrap');

// --- Constants ---
assert(sw.kReadBytesOrError === 0, 'kReadBytesOrError is 0');
assert(sw.kArrayBufferOffset === 1, 'kArrayBufferOffset is 1');
assert(sw.kBytesWritten === 2, 'kBytesWritten is 2');
assert(sw.kLastWriteWasAsync === 3, 'kLastWriteWasAsync is 3');

// --- streamBaseState: Int32Array ---
var state = sw.streamBaseState;
assert(state instanceof Int32Array, 'streamBaseState is Int32Array');
assert(state.length === 4, 'streamBaseState has 4 elements');
assert(state[0] === 0, 'streamBaseState initialized to 0');
assert(state[1] === 0, 'streamBaseState[1] initialized to 0');
assert(state[2] === 0, 'streamBaseState[2] initialized to 0');
assert(state[3] === 0, 'streamBaseState[3] initialized to 0');

// streamBaseState is writable
state[sw.kBytesWritten] = 42;
assert(state[sw.kBytesWritten] === 42, 'streamBaseState is writable');
state[sw.kBytesWritten] = 0;

// --- WriteWrap constructor ---
assert(typeof sw.WriteWrap === 'function', 'WriteWrap is a function');
var ww = new sw.WriteWrap();
assert(typeof ww === 'object', 'new WriteWrap() creates an object');
// JS side sets properties on the write wrap
ww.handle = {};
ww.oncomplete = function() {};
ww.async = false;
ww.bytes = 0;
ww.buffer = null;
ww.callback = function() {};
assert(ww.handle !== undefined, 'WriteWrap properties are settable');

// --- ShutdownWrap constructor ---
assert(typeof sw.ShutdownWrap === 'function', 'ShutdownWrap is a function');
var shw = new sw.ShutdownWrap();
assert(typeof shw === 'object', 'new ShutdownWrap() creates an object');
// JS side sets properties on the shutdown wrap
shw.oncomplete = function() {};
shw.handle = {};
shw.callback = function() {};
assert(shw.oncomplete !== undefined, 'ShutdownWrap properties are settable');

// Multiple instances are independent
var ww2 = new sw.WriteWrap();
ww2.bytes = 100;
assert(ww.bytes === 0, 'WriteWrap instances are independent');

console.log('PASS');
