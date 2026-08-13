// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A genuine SyntaxError in the user's source is still reported, and the two
// runs produce byte-identical output. Only failures on the cached-bytecode
// path are swallowed.
//
// Note what is and is not warm here. The failing file is never cached at
// all: save() is only reached after hermes_compile_to_bytecode succeeds, so
// a file that will not compile leaves no entry behind and misses on every
// run. What the second run exercises is the entry script, which did compile
// and is a genuine cache hit. So this guards against the cache changing how
// a compile error surfaces, not against a stale cached error being replayed
// -- the latter cannot happen because no such entry is ever written.

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
