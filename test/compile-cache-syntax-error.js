// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A genuine SyntaxError in the user's source is still reported, and reports
// the same file both cold and warm. Only failures on the cached-bytecode path
// are swallowed.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'function ( { oops' > %t.dir/bad.js
// RUN: %not %hermes-node-cc --compile-cache=%t.cache %s %t.dir/bad.js > %t.cold.txt 2>&1
// RUN: %FileCheck %s < %t.cold.txt
// The identical failure must be reported on a warm run. This is the property
// the cache could actually break: a real compile error must never be cached,
// never swallowed, and never replaced by a stale success.
// RUN: %not %hermes-node-cc --compile-cache=%t.cache %s %t.dir/bad.js > %t.warm.txt 2>&1
// RUN: diff %t.cold.txt %t.warm.txt

'use strict';

require(process.argv[2]);
console.log('SHOULD NOT REACH');

// Hermes formats parse errors as "<line>:<col>:<message>" and does not
// include the filename (SimpleDiagHandler::getErrorString in the hermes
// submodule), so this asserts on the diagnostic text. The column is past the
// end of the one-line source because the CJS wrapper prefix is prepended
// before compiling.
// CHECK: SyntaxError: {{[0-9]+}}:{{[0-9]+}}:{{.*}}'identifier' expected after 'function'
// CHECK-NOT: SHOULD NOT REACH
