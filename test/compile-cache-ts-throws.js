// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A required .ts module that throws at top level goes through
// compileAndRunCallback. Its exception must propagate, and its side effects
// must run exactly once per run -- warm as well as cold. A cached-bytecode
// failure is swallowed and recompiled by design; a genuine exception from
// the module body must never be, or the body would execute twice.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'console.log("SIDE EFFECT"); throw new Error("boom");' > %t.dir/thrower.ts
// RUN: %not %hermes-node-cc --compile-cache=%t.cache %s %t.dir/thrower.ts > %t.cold.txt 2>&1
// RUN: grep -c "SIDE EFFECT" %t.cold.txt | %FileCheck --check-prefix=ONCE %s
// RUN: grep -c "boom" %t.cold.txt | %FileCheck --check-prefix=ATLEASTONE %s
// RUN: %not %hermes-node-cc --compile-cache=%t.cache %s %t.dir/thrower.ts > %t.warm.txt 2>&1
// RUN: grep -c "SIDE EFFECT" %t.warm.txt | %FileCheck --check-prefix=ONCE %s
// RUN: grep -c "boom" %t.warm.txt | %FileCheck --check-prefix=ATLEASTONE %s

'use strict';

require(process.argv[2]);
console.log('SHOULD NOT REACH');

// ONCE: {{^1$}}
// ATLEASTONE-NOT: {{^0$}}
