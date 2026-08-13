// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A cache file whose header is intact but whose bytecode Hermes cannot load
// must not surface as an error. The run recompiles from source and succeeds.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'module.exports = 7;' > %t.dir/dep.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js | %FileCheck %s
// Zero 64 payload bytes in place, starting right after the 24-byte header.
// That leaves our header (and so our own validation) intact and destroys the
// Hermes bytecode magic, which is what forces the failure onto the
// cached-bytecode path rather than a header rejection. conv=notrunc keeps
// the file length unchanged so the fstat size check still passes.
//
// dd is used rather than head/tail with `stat -c%s`: that spelling of stat
// is a GNU extension and CI also builds macOS, where it is `stat -f%z`.
// RUN: for f in $(find %t.cache -type f); do \
// RUN:   dd if=/dev/zero of="$f" bs=1 seek=24 count=64 conv=notrunc 2>/dev/null; \
// RUN: done
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js | %FileCheck %s

'use strict';

console.log('VALUE ' + require(process.argv[2]));
console.log('PASS');

// CHECK: VALUE 7
// CHECK: PASS
