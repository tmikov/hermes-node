// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/node-modules-exports/main.js | %FileCheck %s
// CHECK: PASS
// Test that package.json "exports" field is respected during node_modules resolution.
// The actual test logic is in fixtures/node-modules-exports/main.js because
// node_modules resolution starts from the requiring file's directory.
