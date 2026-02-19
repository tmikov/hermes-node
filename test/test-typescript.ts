// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test that hermes-node can run a .ts file directly as an entry point.
'use strict';

// Basic type annotations.
const x: number = 42;
const s: string = 'hello';

// Function with typed parameters and return type.
function add(a: number, b: number): number {
  return a + b;
}

// Generic function.
function identity<T>(val: T): T {
  return val;
}

// Class with typed properties.
class Greeter {
  name: string;
  constructor(name: string) {
    this.name = name;
  }
  greet(): string {
    return 'Hello, ' + this.name + '!';
  }
}

// Verify values are correct at runtime.
if (x !== 42) throw new Error('x');
if (s !== 'hello') throw new Error('s');
if (add(3, 4) !== 7) throw new Error('add');
if (identity(99) !== 99) throw new Error('identity');

const g = new Greeter('World');
if (g.greet() !== 'Hello, World!') throw new Error('greet');

console.log('PASS');
