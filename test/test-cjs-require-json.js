// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %source_dir/test/fixtures/require-json/main.js | %FileCheck %s
// CHECK: PASS
// Test that require() of .json files works correctly.
// Tests: relative .json require, cached references, require.resolve for .json,
// .json from node_modules (index.json), and package.json "main" pointing to .json.
