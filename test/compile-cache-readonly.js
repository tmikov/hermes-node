// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The compile cache is an optimization, and its contract is that no failure
// inside it reaches the program: a cache that cannot be used at all must
// leave the run indistinguishable from an uncached one. A read-only cache
// directory is the cheapest way to make every write fail at once -- the
// directory cannot be created, the config file cannot be published, and no
// entry can be stored -- so it stands in for the whole class here.
//
// This is the claim CLAUDE.md's "every failure in here is swallowed on
// purpose" bullet rests on, and nothing pinned it before.
//
// Running as root defeats the setup rather than the code: root ignores the
// permission bits, so the case degenerates into an ordinary cached run,
// which still passes. A test that goes red for whoever runs the suite in a
// container would be worse than one that occasionally proves less.

// RUN: rm -rf %t.ro %t.dir && mkdir -p %t.ro %t.dir
// RUN: echo 'module.exports = function () { return "dep"; };' > %t.dir/dep.js
// RUN: chmod 555 %t.ro
// The program runs, produces its output, and exits 0 with nothing on stderr.
// RUN: %hermes-node-cc --compile-cache=%t.ro %s %t.dir/dep.js 2>%t.err | %FileCheck %s
// RUN: %FileCheck --check-prefix=QUIET --allow-empty %s < %t.err
// A second run behaves identically -- there is no half-written state to trip
// over, which is the property a cache that fails to initialize must have.
// RUN: %hermes-node-cc --compile-cache=%t.ro %s %t.dir/dep.js 2>>%t.err | %FileCheck %s
// RUN: %FileCheck --check-prefix=QUIET --allow-empty %s < %t.err
// RUN: chmod 755 %t.ro

'use strict';

const dep = require(process.argv[2]);
console.log('VALUE ' + dep());
console.log('PASS');

// CHECK: VALUE dep
// CHECK: PASS
// The cache may not report its own failure. It is allowed to say nothing at
// all; it is not allowed to warn, error, or mention itself.
// QUIET-NOT: {{[Ww]arning}}
// QUIET-NOT: {{[Ee]rror}}
// QUIET-NOT: {{compile.cache}}
