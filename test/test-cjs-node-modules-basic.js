// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/node-modules-basic/main.js | %FileCheck %s
// CHECK: PASS
// Test that basic node_modules/ resolution works.
// The actual test logic is in fixtures/node-modules-basic/main.js because
// node_modules resolution starts from the requiring file's directory.
