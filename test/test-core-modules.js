// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test: core modules load and work (events, path, buffer, util)
// Step 24: Verify core modules load and work

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var assertionErrors = [];

// --- 1. Events module ---
var events = require('events');
var ee = new events.EventEmitter();
var emitted = false;
ee.on('test', function(msg) { emitted = true; assert(msg === 'hello', 'event msg'); });
ee.emit('test', 'hello');
assert(emitted, 'event emitted');

// EventEmitter basics
var order = [];
ee.on('multi', function() { order.push(1); });
ee.on('multi', function() { order.push(2); });
ee.emit('multi');
assert(order.length === 2 && order[0] === 1 && order[1] === 2, 'multi listeners order');

// once
var onceCount = 0;
ee.once('onceEvent', function() { onceCount++; });
ee.emit('onceEvent');
ee.emit('onceEvent');
assert(onceCount === 1, 'once fires exactly once');

// removeListener
var removed = false;
var handler = function() { removed = true; };
ee.on('removable', handler);
ee.removeListener('removable', handler);
ee.emit('removable');
assert(!removed, 'removeListener works');

// listenerCount
var ee2 = new events.EventEmitter();
ee2.on('x', function() {});
ee2.on('x', function() {});
assert(ee2.listenerCount('x') === 2, 'listenerCount');

console.log('events: OK');

// --- 2. Path module ---
var path = require('path');

assert(path.join('/foo', 'bar', 'baz') === '/foo/bar/baz', 'path.join');
assert(path.dirname('/foo/bar') === '/foo', 'path.dirname');
assert(path.extname('file.txt') === '.txt', 'path.extname');
assert(path.basename('/foo/bar/baz.txt') === 'baz.txt', 'path.basename');
assert(path.basename('/foo/bar/baz.txt', '.txt') === 'baz', 'path.basename with ext');
assert(path.isAbsolute('/foo') === true, 'path.isAbsolute /');
assert(path.isAbsolute('foo') === false, 'path.isAbsolute relative');
assert(path.normalize('/foo/bar/../baz') === '/foo/baz', 'path.normalize');
assert(path.resolve('/foo', 'bar') === '/foo/bar', 'path.resolve');

var parsed = path.parse('/home/user/file.txt');
assert(parsed.root === '/', 'path.parse root');
assert(parsed.dir === '/home/user', 'path.parse dir');
assert(parsed.base === 'file.txt', 'path.parse base');
assert(parsed.name === 'file', 'path.parse name');
assert(parsed.ext === '.txt', 'path.parse ext');

assert(path.sep === '/', 'path.sep');
assert(path.delimiter === ':', 'path.delimiter');

console.log('path: OK');

// --- 3. Buffer module ---
var buffer = require('buffer');
var Buffer = buffer.Buffer;

var buf = Buffer.from('hello world');
assert(buf.toString() === 'hello world', 'Buffer.from string');
assert(buf.length === 11, 'Buffer length');
assert(Buffer.isBuffer(buf) === true, 'Buffer.isBuffer');
assert(Buffer.isBuffer('not a buffer') === false, 'Buffer.isBuffer false');

// Buffer.alloc
var zbuf = Buffer.alloc(10);
assert(zbuf.length === 10, 'Buffer.alloc length');
for (var i = 0; i < 10; i++) {
  assert(zbuf[i] === 0, 'Buffer.alloc zeroed');
}

// Buffer.from array
var abuf = Buffer.from([1, 2, 3, 4, 5]);
assert(abuf.length === 5, 'Buffer.from array length');
assert(abuf[0] === 1 && abuf[4] === 5, 'Buffer.from array values');

// Buffer encoding
var hexBuf = Buffer.from('48656c6c6f', 'hex');
assert(hexBuf.toString() === 'Hello', 'Buffer hex decode');
assert(hexBuf.toString('hex') === '48656c6c6f', 'Buffer hex encode');

var b64Buf = Buffer.from('SGVsbG8=', 'base64');
assert(b64Buf.toString() === 'Hello', 'Buffer base64 decode');
assert(b64Buf.toString('base64') === 'SGVsbG8=', 'Buffer base64 encode');

// Buffer.concat
var concatBuf = Buffer.concat([Buffer.from('hel'), Buffer.from('lo')]);
assert(concatBuf.toString() === 'hello', 'Buffer.concat');

// Buffer.compare
assert(Buffer.compare(Buffer.from('abc'), Buffer.from('abc')) === 0, 'Buffer.compare equal');
assert(Buffer.compare(Buffer.from('abc'), Buffer.from('abd')) < 0, 'Buffer.compare less');

// Buffer slice
var sliced = buf.slice(0, 5);
assert(sliced.toString() === 'hello', 'Buffer.slice');

console.log('buffer: OK');

// --- 4. Util module ---
var util = require('util');

assert(util.format('%s %d', 'test', 42) === 'test 42', 'util.format');
assert(util.format('%j', {a: 1}) === '{"a":1}', 'util.format %j');
assert(util.format('hello') === 'hello', 'util.format no args');
assert(util.format('%s', 'world') === 'world', 'util.format %s');

// util.inspect
var inspected = util.inspect({a: 1, b: [2, 3]});
assert(typeof inspected === 'string', 'util.inspect returns string');
assert(inspected.indexOf('a') >= 0, 'util.inspect contains key');

// util.inherits
function Base() {}
Base.prototype.hello = function() { return 'world'; };
function Child() { Base.call(this); }
util.inherits(Child, Base);
var child = new Child();
assert(child.hello() === 'world', 'util.inherits');
assert(child instanceof Base, 'util.inherits instanceof');

// util.types
assert(typeof util.types === 'object', 'util.types exists');
assert(util.types.isDate(new Date()) === true, 'util.types.isDate');
assert(util.types.isRegExp(/test/) === true, 'util.types.isRegExp');
assert(util.types.isMap(new Map()) === true, 'util.types.isMap');

// util.promisify
var fn = function(a, b, cb) { cb(null, a + b); };
var promisified = util.promisify(fn);
assert(typeof promisified === 'function', 'util.promisify');

console.log('util: OK');

// --- 5. Integration: process.nextTick + timers ---
var ticked = false;
process.nextTick(function() { ticked = true; });

var timedOut = false;
setTimeout(function() {
  try {
    assert(ticked, 'nextTick ran before setTimeout');
    timedOut = true;
  } catch (e) {
    assertionErrors.push('nextTick/setTimeout: ' + e.message);
  }
}, 10);

setTimeout(function() {
  try {
    assert(timedOut, 'first setTimeout ran');

    if (assertionErrors.length > 0) {
      throw new Error('Errors: ' + assertionErrors.join('; '));
    }

    console.log('PASS');
  } catch (e) {
    console.error('FAIL:', e.message);
    process.exit(1);
  }
}, 100);
