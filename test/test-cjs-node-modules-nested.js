// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/node-modules-nested/main.js | %FileCheck %s
// CHECK: PASS
// Test that nested node_modules/ resolution works correctly.
// The actual test logic is in fixtures/node-modules-nested/main.js because
// node_modules resolution starts from the requiring file's directory.
