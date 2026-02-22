// Copyright (c) Tzvetan Mikov.
// Test that files with shebang lines can be loaded via require().
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

var mod = require('./fixtures/shebang-script.js');

if (mod.loaded === true) {
  print('PASS');
} else {
  print('FAIL: loaded =', mod.loaded);
}

// CHECK: PASS
