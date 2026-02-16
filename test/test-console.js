// Copyright (c) Tzvetan Mikov.
// Test: Node's real console module (loaded in bootstrap step 11c).
'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

// --- 1. globalThis.console is the real module, not the C++ stub ---
// The C++ stub only has log/info/warn/error. The real module has these too:
assert(typeof console.time === 'function', 'console.time');
assert(typeof console.timeEnd === 'function', 'console.timeEnd');
assert(typeof console.timeLog === 'function', 'console.timeLog');
assert(typeof console.dir === 'function', 'console.dir');
assert(typeof console.table === 'function', 'console.table');
assert(typeof console.count === 'function', 'console.count');
assert(typeof console.countReset === 'function', 'console.countReset');
assert(typeof console.group === 'function', 'console.group');
assert(typeof console.groupEnd === 'function', 'console.groupEnd');
assert(typeof console.trace === 'function', 'console.trace');
assert(typeof console.assert === 'function', 'console.assert');
assert(typeof console.clear === 'function', 'console.clear');
assert(typeof console.debug === 'function', 'console.debug');

// --- 2. Methods work without throwing ---
console.log('log', {a: 1, b: [2, 3]});
console.log('%s %d', 'test', 42);
console.info('info');
console.debug('debug');
console.warn('warn');
console.error('error');
console.dir({x: 1}, {depth: 0});
console.time('t');
console.timeLog('t');
console.timeEnd('t');
console.count('c');
console.count('c');
console.countReset('c');
console.group('g');
console.log('indented');
console.groupEnd();
console.assert(true, 'should not print');
console.assert(false, 'expected assertion');

// --- 3. Console constructor is available ---
var Console = require('internal/console/constructor').Console;
assert(typeof Console === 'function', 'Console constructor');

console.log('PASS');
