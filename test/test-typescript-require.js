// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test that require() can load .ts modules.
'use strict';

// Require with explicit .ts extension.
const mod = require('./test-typescript-module.ts');
if (mod.VERSION !== 1) throw new Error('VERSION');
if (mod.greet('World') !== 'Hello, World!') throw new Error('greet');

// Require without extension (auto-resolve should find .ts).
const mod2 = require('./test-typescript-module');
// Should be the same cached module.
if (mod2 !== mod) throw new Error('cache');

console.log('PASS');
