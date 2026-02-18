// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// R16: Verify that repl.js line 216 -- vm.runInNewContext at module load time
// -- works correctly. The REPL uses:
//   const globalBuiltins = new SafeSet(
//     vm.runInNewContext('Object.getOwnPropertyNames(globalThis)'));
// This must succeed at require('repl') time.

// RUN: %hermes-node %s

var assert = require('assert');
var vm = require('vm');

// Test 1: vm.runInNewContext returns global property names
var names = vm.runInNewContext('Object.getOwnPropertyNames(globalThis)');
assert(Array.isArray(names), 'Expected array from runInNewContext');
assert(names.length > 0, 'Expected non-empty array of global property names');
// Should include well-known global properties
assert(names.includes('Object'), 'Expected "Object" in global names');
assert(names.includes('Array'), 'Expected "Array" in global names');
assert(names.includes('String'), 'Expected "String" in global names');

// Test 2: require('repl') loads without error (this exercises line 216)
var repl = require('repl');
assert(typeof repl.start === 'function', 'Expected repl.start to be a function');
assert(typeof repl.REPLServer === 'function', 'Expected repl.REPLServer to be a function');

// Test 3: REPL can be started with programmatic streams
var { Readable, Writable } = require('stream');
var input = new Readable({ read() {} });
var output = '';
var outputStream = new Writable({
  write(chunk, enc, cb) { output += chunk.toString(); cb(); }
});

var r = repl.start({
  input: input,
  output: outputStream,
  useGlobal: true,
  terminal: false,
  prompt: '> ',
});

r.on('exit', function() {
  // Verify eval worked -- output should contain the result "3"
  assert(output.includes('3'), 'Expected "3" in REPL output, got: ' + output);
  console.log('PASS');
});

input.push('1 + 2\n');
input.push('.exit\n');
