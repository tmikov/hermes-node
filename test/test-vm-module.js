// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s

'use strict';

// Test that require('vm') loads and the public API works.

const vm = require('vm');

// --- vm.runInThisContext ---
console.log(vm.runInThisContext('2 + 3'));
// CHECK: 5

// Global side effects persist.
vm.runInThisContext('var __vmTestGlobal = 99');
console.log(globalThis.__vmTestGlobal);
// CHECK: 99

// --- vm.Script ---
const script = new vm.Script('globalThis.__vmScriptVar = 42');
script.runInThisContext();
console.log(globalThis.__vmScriptVar);
// CHECK: 42

// Script with filename option.
const script2 = new vm.Script('1 + 1', { filename: 'myfile.js' });
console.log(script2.runInThisContext());
// CHECK: 2

// --- vm.createContext / vm.isContext ---
var plainObj = {};
console.log(vm.isContext(plainObj));
// CHECK: false

var ctx = vm.createContext(plainObj);
console.log(ctx === plainObj);
// CHECK: true
console.log(vm.isContext(ctx));
// CHECK: true

// Already-contextified object returns same object.
var ctx2 = vm.createContext(ctx);
console.log(ctx2 === ctx);
// CHECK: true

// --- vm.runInNewContext ---
// Our stub evaluates in the global context, but should not throw.
var result = vm.runInNewContext('typeof Object');
console.log(result);
// CHECK: function

// runInNewContext with context object.
var result2 = vm.runInNewContext('Object.getOwnPropertyNames(globalThis).length > 0');
console.log(result2);
// CHECK: true

// --- vm.compileFunction ---
var fn = vm.compileFunction('return a + b', ['a', 'b']);
console.log(fn(10, 20));
// CHECK: 30

// compileFunction with no params.
var fn2 = vm.compileFunction('return 123');
console.log(fn2());
// CHECK: 123

console.log('PASS');
// CHECK: PASS
