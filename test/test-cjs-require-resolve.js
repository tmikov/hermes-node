// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/require-resolve/main.js | %FileCheck %s
// CHECK: PASS
// Test that require.resolve() and require.resolve.paths() work correctly.
// The actual test logic is in fixtures/require-resolve/main.js because
// node_modules resolution starts from the requiring file's directory.
