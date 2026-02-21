// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Tests the contextify binding compileFunctionForCJSLoader implementation.
'use strict';

var assert = require('assert');
var binding = internalBinding('contextify');

// --- Test 1: Basic compilation returns a callable function ---
{
  var result = binding.compileFunctionForCJSLoader(
    'return exports;', 'test1.js', false, false);
  assert(result !== undefined, 'Expected result object');
  assert.strictEqual(typeof result.function, 'function',
    'Expected function property');
  assert.strictEqual(result.canParseAsESM, false);
  assert.strictEqual(result.cachedDataRejected, false);
}

// --- Test 2: Function has correct CJS parameters ---
{
  var result = binding.compileFunctionForCJSLoader(
    'return { e: exports, r: typeof require, m: module, f: __filename, d: __dirname };',
    'test2.js', false, false);
  var fn = result.function;

  var fakeExports = { hello: 'world' };
  var fakeModule = { id: 'test' };
  var ret = fn(fakeExports, require, fakeModule, '/foo/test2.js', '/foo');

  assert.strictEqual(ret.e, fakeExports, 'exports should be passed through');
  assert.strictEqual(ret.r, 'function', 'require should be a function');
  assert.strictEqual(ret.m, fakeModule, 'module should be passed through');
  assert.strictEqual(ret.f, '/foo/test2.js', '__filename should match');
  assert.strictEqual(ret.d, '/foo', '__dirname should match');
}

// --- Test 3: Module.exports assignment pattern ---
{
  var result = binding.compileFunctionForCJSLoader(
    'module.exports = 42;', 'test3.js', false, false);
  var fn = result.function;
  var mod = { exports: {} };
  fn(mod.exports, require, mod, 'test3.js', '.');
  assert.strictEqual(mod.exports, 42,
    'module.exports should be assignable');
}

// --- Test 4: SyntaxError throws ---
{
  var threw = false;
  try {
    binding.compileFunctionForCJSLoader(
      'function {{{', 'bad.js', false, false);
  } catch (e) {
    threw = true;
    assert(e instanceof SyntaxError, 'Expected SyntaxError');
  }
  assert(threw, 'Expected compilation to throw SyntaxError');
}

// --- Test 5: Empty source compiles fine ---
{
  var result = binding.compileFunctionForCJSLoader(
    '', 'empty.js', false, false);
  assert.strictEqual(typeof result.function, 'function');
  // Calling with no args should not throw
  result.function({}, function(){}, {}, '', '');
}

// --- Test 6: Source with require() call ---
{
  var result = binding.compileFunctionForCJSLoader(
    'var path = require("path"); module.exports = path.join("a", "b");',
    'test6.js', false, false);
  var fn = result.function;
  var mod = { exports: {} };
  fn(mod.exports, require, mod, 'test6.js', '.');
  assert.strictEqual(mod.exports, 'a/b',
    'require should work inside compiled function');
}

console.log('PASS');
