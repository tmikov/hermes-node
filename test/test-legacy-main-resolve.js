// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Tests the fs binding legacyMainResolve implementation.
'use strict';

var path = require('path');
var assert = require('assert');
var binding = internalBinding('fs');

var fixturesDir = path.join(__dirname, 'fixtures', 'legacy-main-resolve');

// --- Test 1: main field resolves to exact file (index 0) ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-with-main');
  var result = binding.legacyMainResolve(pkgPath, 'lib/entry.js');
  assert.strictEqual(result, 0, 'Expected index 0 for exact main match');
}

// --- Test 2: main field + .js extension (index 1) ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-with-main');
  var result = binding.legacyMainResolve(pkgPath, 'lib/entry');
  assert.strictEqual(result, 1, 'Expected index 1 for main + .js');
}

// --- Test 3: main field resolves to directory/index.js (index 4) ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-main-dir');
  var result = binding.legacyMainResolve(pkgPath, 'entry');
  assert.strictEqual(result, 4, 'Expected index 4 for main + /index.js');
}

// --- Test 4: main field + .json extension (index 2) ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-main-json');
  var result = binding.legacyMainResolve(pkgPath, 'data');
  assert.strictEqual(result, 2, 'Expected index 2 for main + .json');
}

// --- Test 5: No main, fallback to index.js (index 7) ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-with-index');
  var result = binding.legacyMainResolve(pkgPath);
  assert.strictEqual(result, 7, 'Expected index 7 for fallback index.js');
}

// --- Test 6: main not found, fallback to index.js (index 7) ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-with-index');
  var result = binding.legacyMainResolve(pkgPath, 'nonexistent');
  assert.strictEqual(result, 7, 'Expected index 7 for fallback after main miss');
}

// --- Test 7: No file found throws ERR_MODULE_NOT_FOUND ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-empty');
  var threw = false;
  try {
    binding.legacyMainResolve(pkgPath, 'nonexistent', 'file:///test.js');
  } catch (e) {
    threw = true;
    assert(e.message.includes('Cannot find package'), 'Expected ERR_MODULE_NOT_FOUND message, got: ' + e.message);
  }
  assert(threw, 'Expected legacyMainResolve to throw for missing files');
}

// --- Test 8: No main, no index — throws ---
{
  var pkgPath = path.join(fixturesDir, 'pkg-empty');
  var threw = false;
  try {
    binding.legacyMainResolve(pkgPath);
  } catch (e) {
    threw = true;
    assert(e.message.includes('Cannot find package'), 'Expected error for empty pkg');
  }
  assert(threw, 'Expected throw for empty package');
}

console.log('PASS');
