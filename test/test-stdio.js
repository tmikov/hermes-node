// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s > %t.out 2> %t.err
// RUN: %FileCheck --check-prefix=STDOUT %s < %t.out
// RUN: %FileCheck --check-prefix=STDERR %s < %t.err
// RUN: ! grep stderr-test %t.out
// STDOUT: stdout-test
// STDOUT: console-test
// STDOUT: PASS
// STDERR: stderr-test
// Test process.stdout and process.stderr (minimal writable streams).
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
}

// --- process.stdout ---
assert(process.stdout !== undefined, 'process.stdout exists');
assert(typeof process.stdout === 'object', 'process.stdout is an object');
assert(process.stdout.fd === 1, 'process.stdout.fd === 1');
assert(typeof process.stdout.write === 'function', 'process.stdout.write is a function');
assert(typeof process.stdout.isTTY === 'boolean', 'process.stdout.isTTY is a boolean');
assert(process.stdout._isStdio === true, 'process.stdout._isStdio is true');

// Event emitter stubs.
assert(typeof process.stdout.on === 'function', 'process.stdout.on');
assert(typeof process.stdout.once === 'function', 'process.stdout.once');
assert(typeof process.stdout.removeListener === 'function', 'process.stdout.removeListener');
assert(typeof process.stdout.listenerCount === 'function', 'process.stdout.listenerCount');
assert(process.stdout.listenerCount('error') === 0, 'listenerCount returns 0');

// --- process.stderr ---
assert(process.stderr !== undefined, 'process.stderr exists');
assert(typeof process.stderr === 'object', 'process.stderr is an object');
assert(process.stderr.fd === 2, 'process.stderr.fd === 2');
assert(typeof process.stderr.write === 'function', 'process.stderr.write is a function');
assert(typeof process.stderr.isTTY === 'boolean', 'process.stderr.isTTY is a boolean');
assert(process.stderr._isStdio === true, 'process.stderr._isStdio is true');

// --- Write tests ---

// process.stdout.write returns true (synchronous).
var ret = process.stdout.write('stdout-test\n');
assert(ret === true, 'stdout.write returns true');

// process.stderr.write returns true.
ret = process.stderr.write('stderr-test\n');
assert(ret === true, 'stderr.write returns true');

// Write with callback.
var callbackCalled = false;
process.stdout.write('callback-test\n', function() {
  callbackCalled = true;
});
assert(callbackCalled, 'write callback called synchronously');

// console.log still works (through our minimal console).
console.log('console-test');

// --- Binding tests (internalBinding('stdio')) ---
var stdio = internalBinding('stdio');
assert(typeof stdio.writeString === 'function', 'stdio.writeString exists');
assert(typeof stdio.writeBuffer === 'function', 'stdio.writeBuffer exists');
assert(typeof stdio.getHandleType === 'function', 'stdio.getHandleType exists');

// getHandleType returns a string.
var handleType = stdio.getHandleType(1);
assert(typeof handleType === 'string', 'getHandleType returns string');
// Valid handle types.
var validTypes = ['TCP', 'TTY', 'UDP', 'FILE', 'PIPE', 'UNKNOWN'];
assert(validTypes.indexOf(handleType) >= 0, 'getHandleType returns valid type: ' + handleType);

// isTTY should match getHandleType result.
assert(
  process.stdout.isTTY === (stdio.getHandleType(1) === 'TTY'),
  'isTTY matches getHandleType'
);

console.log('PASS');
