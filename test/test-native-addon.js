// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s %hello_addon | %FileCheck --match-full-lines %s

'use strict';

// The addon path is passed as a command-line argument.
const addonPath = process.argv[2];
if (!addonPath) {
  throw new Error('Usage: test-native-addon.js <path-to-hello_addon.node>');
}

// Test 1: require() of a .node file works (CJS loader calls process.dlopen).
const addon = require(addonPath);

// Test 2: addon exports are accessible.
console.log(typeof addon.hello);
// CHECK: function
console.log(addon.hello());
// CHECK-NEXT: world
console.log(typeof addon.add);
// CHECK-NEXT: function
console.log(addon.add(2, 3));
// CHECK-NEXT: 5
console.log(addon.add(-1, 1));
// CHECK-NEXT: 0
console.log(addon.add(1.5, 2.5));
// CHECK-NEXT: 4

// Test 3: Module caching — second require returns the same instance.
const addon2 = require(addonPath);
console.log(addon === addon2);
// CHECK-NEXT: true

console.log('PASS');
// CHECK-NEXT: PASS
