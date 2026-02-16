// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test: Step 26 -- Verify streams work

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var testsExpected = 7;
var testsPassed = 0;
var testsDone = false;

function passTest(name) {
  testsPassed++;
  console.log('  OK: ' + name);
  if (testsPassed === testsExpected && !testsDone) {
    testsDone = true;
    console.log('All ' + testsPassed + ' stream tests passed');
    console.log('PASS');
  }
}

// ---- Test 1: Require stream module and check exports ----
var stream = require('stream');
assert(typeof stream === 'function' || typeof stream === 'object', 'stream module loaded');
assert(typeof stream.Readable === 'function', 'Readable is a function');
assert(typeof stream.Writable === 'function', 'Writable is a function');
assert(typeof stream.Transform === 'function', 'Transform is a function');
assert(typeof stream.Duplex === 'function', 'Duplex is a function');
assert(typeof stream.PassThrough === 'function', 'PassThrough is a function');
assert(typeof stream.pipeline === 'function', 'pipeline is a function');
assert(typeof stream.finished === 'function', 'finished is a function');
passTest('stream module exports');

// ---- Test 2: Writable stream ----
(function() {
  var Writable = stream.Writable;
  var chunks = [];
  var w = new Writable({
    write: function(chunk, encoding, callback) {
      chunks.push(chunk.toString());
      callback();
    }
  });
  w.on('finish', function() {
    assert(chunks.length === 2, 'got 2 chunks');
    assert(chunks[0] === 'hello', 'first chunk is hello');
    assert(chunks[1] === 'world', 'second chunk is world');
    passTest('Writable stream');
  });
  w.write('hello');
  w.write('world');
  w.end();
})();

// ---- Test 3: Transform stream ----
(function() {
  var Transform = stream.Transform;
  var transformed = '';
  var upper = new Transform({
    transform: function(chunk, encoding, callback) {
      callback(null, chunk.toString().toUpperCase());
    }
  });
  upper.on('data', function(chunk) {
    transformed += chunk.toString();
  });
  upper.on('end', function() {
    assert(transformed === 'HELLO', 'transform produced HELLO, got: ' + transformed);
    passTest('Transform stream');
  });
  upper.write('hello');
  upper.end();
})();

// ---- Test 4: Readable stream ----
(function() {
  var Readable = stream.Readable;
  var data = ['one', 'two', 'three', null];
  var idx = 0;
  var r = new Readable({
    read: function() {
      this.push(data[idx++]);
    }
  });
  var result = [];
  r.on('data', function(chunk) {
    result.push(chunk.toString());
  });
  r.on('end', function() {
    assert(result.length === 3, 'got 3 chunks');
    assert(result.join(',') === 'one,two,three', 'data is correct');
    passTest('Readable stream');
  });
})();

// ---- Test 5: Pipeline ----
(function() {
  var Readable = stream.Readable;
  var Writable = stream.Writable;
  var pipeline = stream.pipeline;

  var readable = Readable.from(['hello', ' ', 'world']);
  var chunks = [];
  var writable = new Writable({
    write: function(chunk, enc, cb) {
      chunks.push(chunk.toString());
      cb();
    }
  });
  pipeline(readable, writable, function(err) {
    assert(!err, 'pipeline completed without error');
    assert(chunks.join('') === 'hello world', 'pipeline data correct: ' + chunks.join(''));
    passTest('pipeline');
  });
})();

// ---- Test 6: PassThrough ----
(function() {
  var PassThrough = stream.PassThrough;
  var pt = new PassThrough();
  var result = '';
  pt.on('data', function(chunk) {
    result += chunk.toString();
  });
  pt.on('end', function() {
    assert(result === 'abc', 'passthrough data correct');
    passTest('PassThrough stream');
  });
  pt.write('a');
  pt.write('b');
  pt.write('c');
  pt.end();
})();

// ---- Test 7: stream.finished ----
(function() {
  var Writable = stream.Writable;
  var finished = stream.finished;
  var w = new Writable({
    write: function(chunk, enc, cb) { cb(); }
  });
  finished(w, function(err) {
    assert(!err, 'finished without error');
    passTest('stream.finished');
  });
  w.end();
})();
