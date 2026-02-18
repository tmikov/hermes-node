// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const assert = require('assert');
const domain = require('domain');

// Basic exports exist.
console.log(typeof domain.create); // CHECK: function
console.log(typeof domain.createDomain); // CHECK: function
console.log(typeof domain.Domain); // CHECK: function
console.log(domain.active); // CHECK: null

// create() returns a Domain instance.
const d = domain.create();
assert(d instanceof domain.Domain);
assert(d instanceof require('events'));
console.log('create ok'); // CHECK: create ok

// enter/exit changes process.domain.
assert.strictEqual(process.domain, null);
d.enter();
assert.strictEqual(process.domain, d);
assert.strictEqual(domain.active, d);
d.exit();
assert.strictEqual(process.domain, undefined);
assert.strictEqual(domain.active, undefined);
console.log('enter/exit ok'); // CHECK: enter/exit ok

// run() executes a function within the domain.
const d2 = domain.create();
let ranInDomain = false;
d2.run(function() {
  assert.strictEqual(process.domain, d2);
  ranInDomain = true;
});
assert(ranInDomain);
console.log('run ok'); // CHECK: run ok

// bind() wraps a callback to run within the domain.
const d3 = domain.create();
const bound = d3.bind(function(a, b) {
  assert.strictEqual(process.domain, d3);
  return a + b;
});
assert.strictEqual(bound(3, 4), 7);
console.log('bind ok'); // CHECK: bind ok

// add/remove members.
const d4 = domain.create();
const ee = new (require('events'))();
d4.add(ee);
assert.strictEqual(ee.domain, d4);
assert.strictEqual(d4.members.length, 1);
d4.remove(ee);
assert.strictEqual(ee.domain, null);
assert.strictEqual(d4.members.length, 0);
console.log('add/remove ok'); // CHECK: add/remove ok

// error handler via domain.
const d5 = domain.create();
let errorCaught = false;
d5.on('error', function(err) {
  assert.strictEqual(err.message, 'test error');
  errorCaught = true;
});
d5._errorHandler(new Error('test error'));
assert(errorCaught);
console.log('error handler ok'); // CHECK: error handler ok

// intercept() strips first error arg.
const d6 = domain.create();
const intercepted = d6.intercept(function(val) {
  assert.strictEqual(val, 42);
  return val;
});
assert.strictEqual(intercepted(null, 42), 42);
console.log('intercept ok'); // CHECK: intercept ok

// intercept() emits error for Error first arg.
const d7 = domain.create();
let interceptError = false;
d7.on('error', function(err) {
  assert.strictEqual(err.message, 'intercept error');
  interceptError = true;
});
const intercepted2 = d7.intercept(function() {
  throw new Error('should not reach');
});
intercepted2(new Error('intercept error'));
assert(interceptError);
console.log('intercept error ok'); // CHECK: intercept error ok

console.log('PASS'); // CHECK: PASS
