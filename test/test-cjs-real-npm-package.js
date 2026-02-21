// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/real-npm-package/main.js | %FileCheck %s
// CHECK: PASS
// Test that a real npm package (minimist) installed via npm loads and works correctly.
// The actual test logic is in fixtures/real-npm-package/main.js because
// node_modules resolution starts from the requiring file's directory.
