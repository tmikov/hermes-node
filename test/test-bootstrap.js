// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Step 17: Verify bootstrap modules load.
// Tests that the core internal modules can be loaded successfully.
'use strict';

var passed = 0;
var failed = 0;

function test(description, fn) {
  try {
    fn();
    passed++;
  } catch (e) {
    failed++;
    console.error('FAIL: ' + description);
    console.error('  ' + (e.stack || e.message || e));
  }
}

// --- internal/assert ---
test('require internal/assert', function() {
  var assert = require('internal/assert');
  if (typeof assert !== 'function')
    throw new Error('internal/assert should export a function, got ' + typeof assert);
});

// --- internal/errors ---
test('require internal/errors', function() {
  var errors = require('internal/errors');
  if (typeof errors.codes !== 'object' || errors.codes === null)
    throw new Error('errors.codes should be an object');
  if (typeof errors.codes.ERR_INVALID_ARG_TYPE !== 'function')
    throw new Error('ERR_INVALID_ARG_TYPE should be a function');
  if (typeof errors.codes.ERR_INVALID_ARG_VALUE !== 'function')
    throw new Error('ERR_INVALID_ARG_VALUE should be a function');
});

// --- internal/util ---
test('require internal/util', function() {
  var util = require('internal/util');
  if (typeof util.deprecate !== 'function')
    throw new Error('util.deprecate should be a function');
  if (typeof util.normalizeEncoding !== 'function')
    throw new Error('util.normalizeEncoding should be a function');
});

// --- internal/util/types ---
test('require internal/util/types', function() {
  var types = require('internal/util/types');
  if (typeof types.isDate !== 'function')
    throw new Error('types.isDate should be a function');
  if (typeof types.isMap !== 'function')
    throw new Error('types.isMap should be a function');
  if (typeof types.isPromise !== 'function')
    throw new Error('types.isPromise should be a function');
});

// --- internal/validators ---
test('require internal/validators', function() {
  var validators = require('internal/validators');
  if (typeof validators.validateString !== 'function')
    throw new Error('validateString should be a function');
  if (typeof validators.validateFunction !== 'function')
    throw new Error('validateFunction should be a function');
  if (typeof validators.validateInteger !== 'function')
    throw new Error('validateInteger should be a function');
});

// --- Functional tests ---
test('ErrorCaptureStackTrace works', function() {
  var obj = {};
  Error.captureStackTrace(obj);
  if (typeof obj.stack !== 'string')
    throw new Error('captureStackTrace should set .stack property');
});

test('internal/errors ERR_INVALID_ARG_TYPE creates proper error', function() {
  var errors = require('internal/errors');
  var ERR_INVALID_ARG_TYPE = errors.codes.ERR_INVALID_ARG_TYPE;
  var err = new ERR_INVALID_ARG_TYPE('arg1', 'string', 42);
  if (!(err instanceof TypeError))
    throw new Error('ERR_INVALID_ARG_TYPE should be a TypeError');
  if (err.code !== 'ERR_INVALID_ARG_TYPE')
    throw new Error('error.code should be ERR_INVALID_ARG_TYPE, got ' + err.code);
});

test('internal/validators.validateString works', function() {
  var validators = require('internal/validators');
  // Should not throw for valid string.
  validators.validateString('hello', 'name');
  // Should throw for non-string.
  var threw = false;
  try {
    validators.validateString(123, 'name');
  } catch (e) {
    threw = true;
    if (e.code !== 'ERR_INVALID_ARG_TYPE')
      throw new Error('Expected ERR_INVALID_ARG_TYPE, got ' + e.code);
  }
  if (!threw) throw new Error('validateString(123) should throw');
});

test('internal/util.normalizeEncoding works', function() {
  var util = require('internal/util');
  if (util.normalizeEncoding('UTF-8') !== 'utf8')
    throw new Error('normalizeEncoding("UTF-8") should return "utf8"');
  if (util.normalizeEncoding('ascii') !== 'ascii')
    throw new Error('normalizeEncoding("ascii") should return "ascii"');
});

// --- Summary ---
console.log('Passed: ' + passed);
console.log('Failed: ' + failed);
if (failed > 0) {
  throw new Error(failed + ' test(s) failed');
}
console.log('PASS');
