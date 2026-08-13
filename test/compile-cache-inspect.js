// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The cache is not enabled under --inspect, so that debugging keeps the
// napi_run_script path, which compiles with full debug info.

// The -cc substitution matters here: with the suite-wide disable still in
// effect this test would pass for the wrong reason, asserting nothing.
// RUN: rm -rf %t.cache
// RUN: %hermes-node-cc --inspect --compile-cache=%t.cache %s | %FileCheck %s
// RUN: test ! -d %t.cache

'use strict';

console.log('PASS');
// CHECK: PASS
