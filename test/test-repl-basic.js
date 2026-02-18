// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// R17: Integration test -- REPL loads and works with programmatic streams.
// Verifies that require('repl') loads, repl.start() works, eval produces
// correct results, and the REPL exits cleanly.

// RUN: %hermes-node %s

var assert = require('assert');
var { Readable, Writable } = require('stream');

// Test 1: require('repl') loads without error
var repl = require('repl');
assert(typeof repl.start === 'function', 'repl.start should be a function');
assert(typeof repl.REPLServer === 'function', 'repl.REPLServer should be a function');

// Helper to run a REPL session with given input lines and collect output.
function runREPL(lines, opts, callback) {
  var input = new Readable({ read() {} });
  var output = '';
  var outputStream = new Writable({
    write(chunk, enc, cb) { output += chunk.toString(); cb(); }
  });
  var r = repl.start(Object.assign({
    input: input,
    output: outputStream,
    useGlobal: false,
    terminal: false,
    prompt: '> ',
  }, opts));
  r.on('exit', function() {
    callback(output, r);
  });
  for (var i = 0; i < lines.length; i++) {
    input.push(lines[i] + '\n');
  }
  input.push(null);
}

var testsRemaining = 5;
var testsPassed = 0;

function testDone(name) {
  testsPassed++;
  testsRemaining--;
  if (testsRemaining === 0) {
    assert.strictEqual(testsPassed, 5, 'Expected all 5 tests to pass');
    console.log('PASS');
  }
}

// Test 2: Basic arithmetic evaluation
runREPL(['1 + 2', '.exit'], {}, function(output) {
  assert(output.includes('3'), 'Test 2: Expected "3" in output, got: ' + output);
  testDone('arithmetic');
});

// Test 3: String evaluation
runREPL(['"hello" + " world"', '.exit'], {}, function(output) {
  assert(output.includes("'hello world'"), 'Test 3: Expected "hello world" in output, got: ' + output);
  testDone('string');
});

// Test 4: Variable declarations persist within session (var)
runREPL(['var x = 42', 'x * 2', '.exit'], {}, function(output) {
  assert(output.includes('84'), 'Test 4: Expected "84" in output, got: ' + output);
  testDone('var persist');
});

// Test 5: Error recovery -- syntax errors don't crash the REPL
runREPL([')', '1 + 1', '.exit'], {}, function(output) {
  assert(output.includes('2'), 'Test 5: Expected "2" after error recovery, got: ' + output);
  testDone('error recovery');
});

// Test 6: require() works inside REPL
runREPL(['require("path").basename("/a/b/c.js")', '.exit'], {}, function(output) {
  assert(output.includes('c.js'), 'Test 6: Expected "c.js" in output, got: ' + output);
  testDone('require in repl');
});
