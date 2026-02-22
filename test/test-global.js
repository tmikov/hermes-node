// Copyright (c) Tzvetan Mikov.
// Test that the Node.js 'global' alias exists and equals globalThis.
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

if (typeof global === 'object' && global === globalThis) {
  print('PASS');
} else {
  print('FAIL');
}

// CHECK: PASS
