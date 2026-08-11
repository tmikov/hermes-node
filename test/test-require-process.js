// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test: require('process') returns the global process object.

'use strict';

var assert = function (cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var proc = require('process');
assert(proc === process, "require('process') === process");

var prefixed = require('node:process');
assert(prefixed === process, "require('node:process') === process");

// The module is cached like any other builtin.
assert(require('process') === proc, 'require is cached');

// Sanity-check that the object really is the process object.
assert(typeof proc.cwd === 'function', 'process.cwd is a function');
assert(typeof proc.version === 'string', 'process.version is a string');

console.log('PASS');
