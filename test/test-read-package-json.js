// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Tests the modules binding readPackageJSON implementation.
'use strict';

var path = require('path');
var assert = require('assert');
var binding = internalBinding('modules');

var fixturesDir = path.join(__dirname, 'fixtures');

// --- Test 1: Full package.json with all fields ---
{
  var result = binding.readPackageJSON(
    path.join(fixturesDir, 'test-package.json'));
  assert(Array.isArray(result), 'Expected array result');
  assert.strictEqual(result.length, 6);

  // [name, main, type, imports, exports, file_path]
  assert.strictEqual(result[0], 'my-test-package');
  assert.strictEqual(result[1], 'lib/index.js');
  assert.strictEqual(result[2], 'module');

  // imports should be a JSON string (object)
  assert.strictEqual(typeof result[3], 'string');
  var imports = JSON.parse(result[3]);
  assert.strictEqual(imports['#internal'], './src/internal.js');

  // exports should be a JSON string (object)
  assert.strictEqual(typeof result[4], 'string');
  var exports_ = JSON.parse(result[4]);
  assert.deepStrictEqual(exports_['.'], {
    require: './cjs.js',
    import: './esm.js',
  });
  assert.strictEqual(exports_['./utils'], './utils.js');

  // file_path echoed back
  assert.strictEqual(result[5], path.join(fixturesDir, 'test-package.json'));
}

// --- Test 2: CJS package with type "commonjs" ---
{
  var result = binding.readPackageJSON(
    path.join(fixturesDir, 'test-package-cjs.json'));
  assert(Array.isArray(result));
  assert.strictEqual(result[0], 'cjs-package');
  assert.strictEqual(result[1], 'index.js');
  assert.strictEqual(result[2], 'commonjs');
  assert.strictEqual(result[3], undefined); // no imports
  assert.strictEqual(result[4], undefined); // no exports
}

// --- Test 3: Minimal package (no main, no type, no imports/exports) ---
{
  var result = binding.readPackageJSON(
    path.join(fixturesDir, 'test-package-minimal.json'));
  assert(Array.isArray(result));
  assert.strictEqual(result[0], 'minimal');
  assert.strictEqual(result[1], undefined); // no main
  assert.strictEqual(result[2], 'none');    // default type
  assert.strictEqual(result[3], undefined);
  assert.strictEqual(result[4], undefined);
}

// --- Test 4: Non-existent file returns undefined ---
{
  var result = binding.readPackageJSON(
    path.join(fixturesDir, 'does-not-exist.json'));
  assert.strictEqual(result, undefined);
}

// --- Test 5: String exports (not object) ---
{
  var result = binding.readPackageJSON(
    path.join(fixturesDir, 'test-package-string-exports.json'));
  assert(Array.isArray(result));
  assert.strictEqual(result[4], './main.js');
}

// --- Test 6: Invalid type field is normalized to "none" ---
{
  var result = binding.readPackageJSON(
    path.join(fixturesDir, 'test-package-bad-type.json'));
  assert(Array.isArray(result));
  assert.strictEqual(result[2], 'none');
}

console.log('PASS');
