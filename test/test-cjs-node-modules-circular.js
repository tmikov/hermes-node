// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/node-modules-circular/main.js | %FileCheck %s
// CHECK: PASS
// Test that circular dependencies across node_modules packages work correctly.
// The actual test logic is in fixtures/node-modules-circular/main.js because
// node_modules resolution starts from the requiring file's directory.
