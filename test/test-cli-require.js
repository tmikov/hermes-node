// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node -r %source_dir/test/fixtures/preload/set-global.js --require %source_dir/test/fixtures/preload/second.js %s | %FileCheck %s
// RUN: %not %hermes-node -r %source_dir/test/fixtures/preload/no-such-module.js %s 2>&1 | %FileCheck --check-prefix=MISSING %s
// CHECK: PASS
// MISSING: Cannot find module
// Test: -r/--require preloads modules before the main script.

'use strict';

var assert = function (cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

// Both modules ran, in the order given on the command line, before this
// script.
assert(
  Array.isArray(globalThis.__preloadOrder),
  'preloaded modules ran before the main script',
);
assert(
  globalThis.__preloadOrder.join(',') === 'set-global,second',
  'preload order follows the command line, got: ' + globalThis.__preloadOrder,
);

// A preloaded module resolves its own relative requires.
assert(globalThis.__preloadHelper === 'helper', 'relative require worked');

// The main script is still the entry point.
assert(
  process.argv[1].indexOf('test-cli-require.js') !== -1,
  'process.argv[1] is the main script, not a preload: ' + process.argv[1],
);

console.log('PASS');
