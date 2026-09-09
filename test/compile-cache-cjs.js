// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A cold run populates the cache and a warm run produces identical output.
// Editing the required file invalidates its entry.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'module.exports = function () { return "first"; };' > %t.dir/dep.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js > %t.cold.txt
// RUN: %FileCheck --check-prefix=FIRST %s < %t.cold.txt
// `-not -name config` matters: the cache directory's own configuration
// file is written on every run whether or not a single entry is, so an
// unfiltered count is never 0 and this check could no longer fail.
// RUN: find %t.cache -type f -not -name config | wc -l | tr -d ' ' | %FileCheck --check-prefix=POPULATED %s
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js > %t.warm.txt
// RUN: diff %t.cold.txt %t.warm.txt
// RUN: echo 'module.exports = function () { return "second"; };' > %t.dir/dep.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js | %FileCheck --check-prefix=SECOND %s

'use strict';

const dep = require(process.argv[2]);
console.log('VALUE ' + dep());
console.log('PASS');

// FIRST: VALUE first
// FIRST: PASS
// SECOND: VALUE second
// SECOND: PASS
// POPULATED-NOT: {{^0$}}
