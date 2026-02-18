// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Test that hermes-node with no script argument starts the REPL and works
// with piped input. Covers R18 (pipe mode entry point) of the REPL plan.

// RUN: %hermes-node %s

var { execSync } = require('child_process');
var assert = require('assert');
var hermesNode = process.argv[0];

function replExec(input) {
  return execSync(
    'printf ' + JSON.stringify(input) + ' | ' + hermesNode,
    { encoding: 'utf8', timeout: 10000 }
  );
}

// Test 1: Basic expression evaluation
var result = replExec('1 + 2\n.exit\n');
assert(result.includes('3'), 'Expected "3" in output, got: ' + result);

// Test 2: String evaluation
var result2 = replExec('"hello"\n.exit\n');
assert(result2.includes("'hello'"), 'Expected quoted hello in output, got: ' + result2);

// Test 3: require works in REPL
var result3 = replExec('require("path").basename("/a/b/c")\n.exit\n');
assert(result3.includes("'c'"), "Expected 'c' in output, got: " + result3);

// Test 4: Clean exit with .exit
execSync(
  'printf ".exit\\n" | ' + hermesNode + ' > /dev/null 2>&1',
  { timeout: 10000 }
);

// Test 5: var declarations persist across lines
var result5 = replExec('var x = 42\nx + 8\n.exit\n');
assert(result5.includes('50'), 'Expected "50" in output, got: ' + result5);

// Test 6: Error recovery -- syntax error doesn't crash REPL
var result6 = replExec('}\n1 + 1\n.exit\n');
assert(result6.includes('2'), 'Expected "2" after error recovery, got: ' + result6);

// Test 7: undefined expressions show 'undefined'
var result7 = replExec('var a = 1\n.exit\n');
assert(result7.includes('undefined'), 'Expected "undefined" for var decl, got: ' + result7);

// Test 8: .help command produces output
var result8 = replExec('.help\n.exit\n');
assert(result8.includes('.exit'), 'Expected .help to mention .exit, got: ' + result8);

// Test 9: Object inspection
var result9 = replExec('({a: 1, b: 2})\n.exit\n');
assert(result9.includes('a:') && result9.includes('b:'),
  'Expected object inspection, got: ' + result9);

// Test 10: Multi-expression session
var result10 = replExec('var sum = 0\nsum += 10\nsum += 20\nsum\n.exit\n');
assert(result10.includes('30'), 'Expected "30" for accumulated sum, got: ' + result10);
