// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A tree with a shared dependency (both cli.js and dep.js require util),
// so discovery must report the second reference as already known.
// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib
// RUN: echo "const d = require('./lib/dep'); const u = require('./lib/util'); console.log('V', d.v + u.v);" > %t.tree/cli.js
// RUN: echo "const u = require('./util'); module.exports = { v: u.v + 1 };" > %t.tree/lib/dep.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: printf 'not really an addon\n' > %t.tree/lib/native.node
// RUN: echo "require('./native.node'); module.exports = {};" >> %t.tree/lib/dep.js

// Verbose output goes to stderr and names discovery, provenance and totals.
// The configuration block names the output path absolutely (the working
// directory it was relative to is recorded nowhere else in this output) and
// spells out the four inputs folded into the generation tag, so a later
// MISMATCH from --dump can be read against the build that stamped it.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --verbose %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=VERB %s
// VERB: entry: {{.*}}cli.js
// VERB: output: {{/.*}}app.hbb
// VERB: generation: 0x{{[0-9a-f]+}} (hermes-node {{.*}}, bytecode {{[0-9]+}}, optimized)
// VERB: optimize: on
// VERB: discover {{.*}}cli.js
// VERB: require './lib/dep'
// VERB: discover {{.*}}lib/dep.js
// VERB: skip {{.*}}native.node
// VERB: known './util'
// VERB: compile {{.*}}src -> {{[0-9]+}} bc ({{[0-9]+\.[0-9]}}x) {{[0-9]+\.[0-9]+}} ms
//
// The summary describes the finished container. Every line carries the
// number that answers a question the module table alone does not: how the
// modules split by kind, how many of the edges name the same specifier,
// what the string table costs, and which single module is the big one.
// VERB: modules: 3 (3 js, 0 json)
// VERB: edges: 3 (3 distinct specifiers)
// VERB: strings: {{[0-9]+}} entries, {{[0-9]+}} bytes
// VERB: payload: {{[0-9]+}} bytes
// VERB: bytecode: {{[0-9]+}} bytes
// VERB: largest: {{.*}}.js {{[0-9]+}} bytes
// VERB: total: {{[0-9]+}} bytes
// VERB: compile: {{[0-9]+\.[0-9]+}} ms

// `total:` is the size of the file that was just written, not a number
// summed from the sections (which would miss the header and the payload
// padding). Comparing it against the file itself is what makes it a fact
// rather than a plausible-looking integer.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --verbose %t.tree/cli.js 2>&1 | sed -n 's/^total: *\([0-9]*\) bytes$/\1/p' > %t.total
// RUN: wc -c < %t.tree/app.hbb | tr -d ' ' > %t.size
// RUN: cmp %t.total %t.size

// A second tree where two modules require the same specifier string, which
// is the only shape that can tell `distinct specifiers` apart from the edge
// count: in the tree above the two numbers agree by coincidence. The .json
// module is deliberately far bigger than any module's bytecode, so
// `largest:` has one right answer and it is not the entry point.
// RUN: rm -rf %t.two && mkdir -p %t.two/lib
// RUN: echo "const a = require('./lib/a'); const b = require('./lib/b'); const c = require('./big.json'); console.log('V', a.v + b.v + c.v);" > %t.two/cli.js
// RUN: echo "const u = require('./util'); module.exports = { v: u.v };" > %t.two/lib/a.js
// RUN: echo "const u = require('./util'); module.exports = { v: u.v + 1 };" > %t.two/lib/b.js
// RUN: echo "module.exports = { v: 1 };" > %t.two/lib/util.js
// RUN: printf '{ "v": 2, "pad": "' > %t.two/big.json
// RUN: head -c 4000 /dev/zero | tr '\\0' 'x' >> %t.two/big.json
// RUN: printf '" }' >> %t.two/big.json
// RUN: %hermes-node --build-bundle=%t.two/app.hbb --verbose %t.two/cli.js 2>&1 | %FileCheck --check-prefix=TWO %s
// TWO: modules: 5 (4 js, 1 json)
// TWO: edges: 5 (4 distinct specifiers)
// TWO: largest: big.json 4021 bytes

// A lone module, which is the one bundle whose string table holds a single
// entry: "1 entry", not "1 entries".
// RUN: rm -rf %t.one && mkdir -p %t.one
// RUN: echo "console.log('V', 1);" > %t.one/cli.js
// RUN: %hermes-node --build-bundle=%t.one/app.hbb --verbose %t.one/cli.js 2>&1 | %FileCheck --check-prefix=ONE %s
// ONE: modules: 1 (1 js, 0 json)
// ONE: edges: 0 (0 distinct specifiers)
// ONE: strings: 1 entry, {{[0-9]+}} bytes

// The discovery lines are flat, all three verbs in column 0 like `discover`
// itself. Indenting them under the nearest `discover` line above would
// claim a parentage that is not there: a require is reported before the
// module it discovers, so the second and later require lines of a file
// would appear nested under a module they have nothing to do with.
// --strict-whitespace is what makes this an assertion about columns at all;
// FileCheck canonicalizes horizontal whitespace otherwise. CHECK-NEXT pins
// the adjacency, so a require line indented under the discover line above
// it fails here.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --verbose %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=FLAT --strict-whitespace %s
// FLAT:{{^}}discover [0] {{.*}}cli.js
// FLAT-NEXT:{{^}}require './lib/dep' -> {{.*}}lib/dep.js
// FLAT-NEXT:{{^}}discover [1] {{.*}}lib/dep.js

// Without --verbose none of it appears: the default output is the warning
// and the root line, exactly as before. --implicit-check-not (unlike
// CHECK-NOT, which only covers the region before the first positive match)
// scans the whole output, so a leak anywhere -- including after
// `bundle root:` -- is caught. `skip` alone would false-match the
// legitimate "warning: skipping" text, hence `skip '`.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=QUIET \
// RUN:   --implicit-check-not=entry: --implicit-check-not=output: --implicit-check-not=generation: \
// RUN:   --implicit-check-not=optimize: --implicit-check-not=discover --implicit-check-not=compile \
// RUN:   --implicit-check-not=modules: --implicit-check-not=edges: --implicit-check-not=bytecode: \
// RUN:   --implicit-check-not=require --implicit-check-not=known --implicit-check-not="skip '" \
// RUN:   --implicit-check-not=strings: --implicit-check-not=payload: --implicit-check-not=largest: \
// RUN:   --implicit-check-not=total: --implicit-check-not="cannot read back" %s
// QUIET: warning: skipping
// QUIET: bundle root:

// The artifact must not depend on the diagnostics.
// RUN: cmp %t.tree/app.hbb %t.tree/plain.hbb
