// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s > %t.out 2> %t.err
// RUN: %FileCheck --check-prefix=STDOUT %s < %t.out
// RUN: %FileCheck --check-prefix=STDERR %s < %t.err
// RUN: ! grep stderr-test %t.out
// STDOUT: stdout-test
// STDOUT: console-test
// STDOUT: PASS
// STDERR: stderr-test
// Test process.stdout, process.stderr, and process.stdin (proper streams).
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
}

// --- process.stdout ---
assert(process.stdout !== undefined, 'process.stdout exists');
assert(typeof process.stdout === 'object', 'process.stdout is an object');
assert(process.stdout.fd === 1, 'process.stdout.fd === 1');
assert(typeof process.stdout.write === 'function', 'process.stdout.write is a function');
assert(process.stdout._isStdio === true, 'process.stdout._isStdio is true');

// isTTY: true when TTY, undefined otherwise (Node behavior).
// When piped to a file, stdout is not a TTY so isTTY is undefined.
assert(
  process.stdout.isTTY === true || process.stdout.isTTY === undefined,
  'process.stdout.isTTY is true or undefined'
);

// Proper streams have event emitter methods.
assert(typeof process.stdout.on === 'function', 'process.stdout.on');
assert(typeof process.stdout.once === 'function', 'process.stdout.once');
assert(typeof process.stdout.removeListener === 'function', 'process.stdout.removeListener');

// --- process.stderr ---
assert(process.stderr !== undefined, 'process.stderr exists');
assert(typeof process.stderr === 'object', 'process.stderr is an object');
assert(process.stderr.fd === 2, 'process.stderr.fd === 2');
assert(typeof process.stderr.write === 'function', 'process.stderr.write is a function');
assert(process.stderr._isStdio === true, 'process.stderr._isStdio is true');

// --- process.stdin ---
assert(process.stdin !== undefined, 'process.stdin exists');
assert(typeof process.stdin === 'object', 'process.stdin is an object');
assert(process.stdin.fd === 0, 'process.stdin.fd === 0');
// stdin is a readable stream.
assert(typeof process.stdin.on === 'function', 'process.stdin.on');
assert(
  typeof process.stdin.read === 'function' ||
  typeof process.stdin.resume === 'function',
  'process.stdin has read or resume'
);

// --- Write tests ---

// process.stdout.write returns boolean.
var ret = process.stdout.write('stdout-test\n');
assert(typeof ret === 'boolean', 'stdout.write returns boolean');

// process.stderr.write returns boolean.
ret = process.stderr.write('stderr-test\n');
assert(typeof ret === 'boolean', 'stderr.write returns boolean');

// console.log still works.
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

console.log('PASS');
