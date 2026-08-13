// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The cache is not enabled under --inspect, so that debugging keeps the
// napi_run_script path, which compiles with full debug info.

// The -cc substitution matters here: with the suite-wide disable still in
// effect this test would pass for the wrong reason, asserting nothing.
// RUN: rm -rf %t.cache %t.xdg && mkdir -p %t.xdg
// RUN: env XDG_CACHE_HOME=%t.xdg %hermes-node-cc --inspect --compile-cache=%t.cache %s | %FileCheck %s
// RUN: test ! -d %t.cache
// The inspector spawns a second runtime on its own thread. It must not
// enable a cache of its own at the default root either -- it inherits none
// of the parent's compile-cache config, so it has to be disabled explicitly.
// RUN: test ! -d %t.xdg/hermes-node

'use strict';

console.log('PASS');
// CHECK: PASS
