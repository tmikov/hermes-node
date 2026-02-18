// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s

'use strict';

// Test that the contextify binding loads and vm module works.

const binding = internalBinding('contextify');

// Verify binding exports exist.
console.log(typeof binding.ContextifyScript);
// CHECK: function
console.log(typeof binding.makeContext);
// CHECK: function
console.log(typeof binding.startSigintWatchdog);
// CHECK: function
console.log(typeof binding.stopSigintWatchdog);
// CHECK: function
console.log(typeof binding.compileFunction);
// CHECK: function
console.log(typeof binding.constants.measureMemory.mode.SUMMARY);
// CHECK: number

// Test startSigintWatchdog/stopSigintWatchdog stubs.
console.log(binding.startSigintWatchdog());
// CHECK: true
console.log(binding.stopSigintWatchdog());
// CHECK: false

// Test ContextifyScript directly.
var script = new binding.ContextifyScript('1 + 2', 'test.js', 0, 0);
console.log(script.runInContext(null, -1, true, false, false));
// CHECK: 3

// Test that global context is used (runInThisContext semantics).
var script2 = new binding.ContextifyScript(
    'var __testContextifyVar = 42', 'test2.js', 0, 0);
script2.runInContext(null, -1, true, false, false);
console.log(globalThis.__testContextifyVar);
// CHECK: 42

// Test compileFunction stub.
var result = binding.compileFunction(
    'return a + b',  // code
    'compiled.js',   // filename
    0,               // lineOffset
    0,               // columnOffset
    undefined,       // cachedData
    false,           // produceCachedData
    undefined,       // parsingContext
    undefined,       // contextExtensions
    ['a', 'b'],      // params
    undefined        // hostDefinedOptionId
);
console.log(typeof result.function);
// CHECK: function
console.log(result.function(10, 20));
// CHECK: 30

console.log('PASS');
// CHECK: PASS
