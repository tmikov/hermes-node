// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// R20: Verify REPL features -- multi-line input, commands, error recovery,
// require(), variable persistence, util.inspect output.

// RUN: %hermes-node %s

var assert = require('assert');
var { Readable, Writable } = require('stream');
var repl = require('repl');

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

var totalTests = 10;
var testsRemaining = totalTests;
var testsPassed = 0;

function testDone(name) {
  testsPassed++;
  testsRemaining--;
  if (testsRemaining === 0) {
    assert.strictEqual(testsPassed, totalTests,
      'Expected all ' + totalTests + ' tests to pass');
    console.log('PASS');
  }
}

// Test 1: Multi-line input -- incomplete expression gets continuation
runREPL(['(1 +', '2)', '.exit'], {}, function(output) {
  assert(output.includes('3'),
    'Test 1: Expected "3" from multi-line expression, got: ' + output);
  // Should have continuation prompt '... ' for the second line
  assert(output.includes('... '),
    'Test 1: Expected continuation prompt "... ", got: ' + output);
  testDone('multi-line');
});

// Test 2: Multi-line object literal
runREPL(['var obj = {', '  a: 1,', '  b: 2', '}', 'obj.a + obj.b', '.exit'],
  {}, function(output) {
  assert(output.includes('3'),
    'Test 2: Expected "3" from obj.a + obj.b, got: ' + output);
  testDone('multi-line object');
});

// Test 3: .help command output
runREPL(['.help', '.exit'], {}, function(output) {
  assert(output.includes('.break'),
    'Test 3: Expected .help to mention .break, got: ' + output);
  assert(output.includes('.clear'),
    'Test 3: Expected .help to mention .clear, got: ' + output);
  assert(output.includes('.exit'),
    'Test 3: Expected .help to mention .exit, got: ' + output);
  assert(output.includes('.help'),
    'Test 3: Expected .help to mention .help, got: ' + output);
  testDone('.help command');
});

// Test 4: .break command cancels multi-line input
runREPL(['(1 +', '.break', '5', '.exit'], {}, function(output) {
  assert(output.includes('5'),
    'Test 4: Expected "5" after .break, got: ' + output);
  testDone('.break command');
});

// Test 5: Error recovery -- multiple errors and normal eval
runREPL(['invalid syntax!!!', '}{', '2 + 2', '.exit'], {}, function(output) {
  assert(output.includes('4'),
    'Test 5: Expected "4" after errors, got: ' + output);
  testDone('error recovery');
});

// Test 6: require() works inside REPL -- path and os modules
runREPL([
  'require("path").join("a", "b", "c")',
  'typeof require("os").cpus',
  '.exit'
], {}, function(output) {
  assert(output.includes("'a/b/c'") || output.includes("a/b/c"),
    'Test 6: Expected path.join result, got: ' + output);
  assert(output.includes("'function'"),
    'Test 6: Expected typeof os.cpus to be function, got: ' + output);
  testDone('require in REPL');
});

// Test 7: var declarations persist across lines (useGlobal: false)
runREPL([
  'var x = 10',
  'var y = 20',
  'x + y',
  '.exit'
], {}, function(output) {
  assert(output.includes('30'),
    'Test 7: Expected "30" from x + y, got: ' + output);
  testDone('var persistence');
});

// Test 8: util.inspect output for objects, arrays, and special values
runREPL([
  '({name: "test", count: 42})',
  '[1, 2, 3]',
  'null',
  'undefined',
  '.exit'
], {}, function(output) {
  assert(output.includes('name:') && output.includes("'test'"),
    'Test 8: Expected object inspect with name, got: ' + output);
  assert(output.includes('42'),
    'Test 8: Expected 42 in object inspect, got: ' + output);
  assert(output.includes('[ 1, 2, 3 ]'),
    'Test 8: Expected array inspect, got: ' + output);
  assert(output.includes('null'),
    'Test 8: Expected null in output, got: ' + output);
  assert(output.includes('undefined'),
    'Test 8: Expected undefined in output, got: ' + output);
  testDone('util.inspect output');
});

// Test 9: Function definition and invocation
runREPL([
  'function add(a, b) { return a + b; }',
  'add(3, 4)',
  '.exit'
], {}, function(output) {
  assert(output.includes('7'),
    'Test 9: Expected "7" from add(3,4), got: ' + output);
  testDone('function definition');
});

// Test 10: Exceptions show error message, REPL continues
runREPL([
  'throw new Error("test error")',
  '99',
  '.exit'
], {}, function(output) {
  assert(output.includes('test error'),
    'Test 10: Expected "test error" in output, got: ' + output);
  assert(output.includes('99'),
    'Test 10: Expected "99" after exception, got: ' + output);
  testDone('exception handling');
});
