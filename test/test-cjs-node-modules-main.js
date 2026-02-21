// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/node-modules-main/main.js | %FileCheck %s
// CHECK: PASS
// Test that package.json "main" field is respected during node_modules resolution.
// The actual test logic is in fixtures/node-modules-main/main.js because
// node_modules resolution starts from the requiring file's directory.
