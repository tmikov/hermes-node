// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Test that hermes-node with no script argument starts the REPL.
// Uses child_process to pipe input to hermes-node and verify output.

// RUN: %hermes-node %s

var { execSync } = require('child_process');
var assert = require('assert');
var hermesNode = process.argv[0];

// Test 1: Basic expression evaluation
var result = execSync(
  'printf "1 + 2\\n.exit\\n" | ' + hermesNode,
  { encoding: 'utf8', timeout: 10000 }
);
assert(result.includes('3'), 'Expected "3" in output, got: ' + result);

// Test 2: String evaluation
var result2 = execSync(
  'printf "\\"hello\\"\\n.exit\\n" | ' + hermesNode,
  { encoding: 'utf8', timeout: 10000 }
);
assert(result2.includes('hello'), 'Expected "hello" in output, got: ' + result2);

// Test 3: require works in REPL
var result3 = execSync(
  'printf "require(\\"path\\").basename(\\"/a/b/c\\")\\n.exit\\n" | ' + hermesNode,
  { encoding: 'utf8', timeout: 10000 }
);
assert(result3.includes('c'), 'Expected "c" in output, got: ' + result3);

// Test 4: Clean exit with .exit
execSync(
  'printf ".exit\\n" | ' + hermesNode + ' > /dev/null 2>&1',
  { timeout: 10000 }
);
