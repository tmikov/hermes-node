// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib
// RUN: echo "const u = require('./lib/util'); const c = require('./cfg.json'); console.log('D', u.v + c.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 2 }' > %t.tree/cfg.json
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js

// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck %s
// CHECK: bundle: {{.*}}app.hbb   format v3  generation 0x{{[0-9a-f]+}}
// CHECK: entry:  [0] cli.js
// CHECK: MODULES (3)
// CHECK-DAG: js {{.*}} cli.js
// CHECK-DAG: js {{.*}} lib/util.js
// CHECK-DAG: json {{.*}} cfg.json
// CHECK: EDGES (2)
// CHECK-DAG: cli.js {{.*}}'./lib/util'{{.*}}->
// CHECK-DAG: cli.js {{.*}}'./cfg.json'{{.*}}->
// CHECK: SECTIONS
// CHECK: total {{[0-9]+}} bytes

// Dumping must not run the program. The prefixes below deliberately avoid
// spelling "NORUN": lit looks for its command keyword anywhere in a line,
// and a check line starting with that prefix contains it, so lit would take
// the check text for a shell command and execute it.
// The exclusion is --implicit-check-not rather than a NOT line, because a
// NOT line only covers the region before the next positive match: the
// program's output appearing after the table would go undetected. Same
// reasoning as bundle-verbose.js's quiet check.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=NOEXEC --implicit-check-not="D 3" %s
// NOEXEC: MODULES

// A container from another generation still dumps, and says so.
// RUN: cp %t.tree/app.hbb %t.tree/old.hbb
// RUN: printf '\xff\xff\xff\xff' | dd of=%t.tree/old.hbb bs=1 seek=12 count=4 conv=notrunc 2>/dev/null
// RUN: %hermes-node --bundle=%t.tree/old.hbb --dump | %FileCheck --check-prefix=MISMATCH %s
// MISMATCH: MISMATCH (this binary requires 0x{{[0-9a-f]+}})
// MISMATCH: MODULES (3)

// But it still refuses to execute it.
// RUN: %not %hermes-node --bundle=%t.tree/old.hbb 2>&1 | %FileCheck --check-prefix=OLDGEN %s
// OLDGEN: generation mismatch
