// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The cache directory is created when the cache is enabled, and not created
// when it is disabled. Detection is through the filesystem because the cache
// is deliberately not observable from JavaScript.

// RUN: rm -rf %t.cache %t.off %t.env
// RUN: %hermes-node-cc --compile-cache=%t.cache %s | %FileCheck %s
// RUN: test -d %t.cache/v1
// RUN: %hermes-node-cc --no-compile-cache --compile-cache=%t.off %s | %FileCheck %s
// RUN: test ! -d %t.off
// RUN: env HERMES_NODE_DISABLE_COMPILE_CACHE=1 %hermes-node --compile-cache=%t.off %s | %FileCheck %s
// RUN: test ! -d %t.off
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t.env %hermes-node %s | %FileCheck %s
// RUN: test -d %t.env/v1

'use strict';

// The JS API must keep reporting that there is no caching, whatever the
// native cache is doing.
const mod = require('module');
if (mod.enableCompileCache().status !== 0) throw new Error('status changed');
if (mod.getCompileCacheDir() !== undefined) throw new Error('dir exposed');

console.log('PASS');
// CHECK: PASS
