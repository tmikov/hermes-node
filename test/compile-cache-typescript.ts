// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// TypeScript files compiled through the module loader are cached too, and a
// warm run produces identical output.

// RUN: rm -rf %t.cache
// RUN: %hermes-node-cc --compile-cache=%t.cache %s > %t.cold.txt
// RUN: %FileCheck %s < %t.cold.txt
// The TypeScript path goes through compileAndRunCallback, so the cold run
// must leave an entry behind. This is what fails before the change: output
// equality alone holds either way.
// RUN: find %t.cache -type f | wc -l | %FileCheck --check-prefix=POPULATED %s
// RUN: %hermes-node-cc --compile-cache=%t.cache %s > %t.warm.txt
// RUN: diff %t.cold.txt %t.warm.txt

function greet(name: string): string {
  return 'hello ' + name;
}

console.log(greet('world'));
console.log('PASS');

// CHECK: hello world
// CHECK: PASS
// POPULATED-NOT: {{^0$}}
