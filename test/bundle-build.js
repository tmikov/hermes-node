// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib %t.tree/node_modules/dep
// RUN: echo "const d = require('dep'); const u = require('./lib/util'); const c = require('./cfg.json'); console.log(d.v + u.v + c.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 100 }' > %t.tree/cfg.json
// RUN: echo '{ "main": "main.js" }' > %t.tree/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 10 };" > %t.tree/node_modules/dep/main.js
// The producer prints the computed build root, and the container starts with
// the magic. Both are asserted -- checking only that a file appeared would
// pass on an empty file.
// RUN: %hermes-node --build-bundle=%t.bundle %t.tree/cli.js | %FileCheck --check-prefix=ROOT %s
// ROOT: bundle root: {{.*}}.tree
// RUN: head -c 8 %t.bundle | %FileCheck --check-prefix=MAGIC %s
// MAGIC: HNBUNDLE

// A require() that resolves to a non-packageable extension (e.g. a native
// addon) must not fail the build: it is a warning, the module is left out
// of the container, and the runtime's on-disk fallback handles it at load
// time. Assert both that the warning is printed AND that the build still
// succeeds and still writes a real bundle -- the failure mode this guards
// against is treating a skip as an error.
// RUN: rm -rf %t.warn && mkdir -p %t.warn
// RUN: touch %t.warn/native.node
// RUN: echo "require('./native.node'); console.log('ok');" > %t.warn/entry.js
// RUN: %hermes-node --build-bundle=%t.warn.bundle %t.warn/entry.js 2>&1 | %FileCheck --check-prefix=WARN %s
// WARN: warning: skipping {{.*}}native.node (.node is not packageable)
// WARN: bundle root: {{.*}}.warn
// RUN: head -c 8 %t.warn.bundle | %FileCheck --check-prefix=WARNMAGIC %s
// WARNMAGIC: HNBUNDLE

// "JavaScript" is not the same set as "*.js". A package with
// "type": "module" ships its CommonJS half as .cjs, and an extensionless
// file is a perfectly ordinary package entry point (examples/yargs-cli
// depends on both shapes through yargs). Both must be packaged, or a bundle
// only works while node_modules is still on disk -- which is the whole
// property the feature exists to provide. .mjs is the deliberate exception:
// its import/export syntax is a syntax error inside the CommonJS wrapper, so
// packaging one would fail the whole build over a module this runtime could
// not have executed anyway (require() of an .mjs throws with or without a
// bundle -- there is no working fallback being preserved here).
//
// The require() of the .mjs sits in a function nobody calls: the scanner
// walks the AST, so the specifier is discovered at build time, while the
// runtime half of this test never has to load it.
// RUN: rm -rf %t.kinds && mkdir -p %t.kinds/node_modules/bare
// RUN: echo "const a = require('./mod.cjs'); const b = require('bare'); function unused() { require('./esm.mjs'); } console.log('SUM', a.v + b.v);" > %t.kinds/cli.js
// RUN: echo "module.exports = { v: 2 };" > %t.kinds/mod.cjs
// RUN: echo "export const v = 3;" > %t.kinds/esm.mjs
// RUN: echo '{ "main": "bare" }' > %t.kinds/node_modules/bare/package.json
// RUN: echo "module.exports = { v: 5 };" > %t.kinds/node_modules/bare/bare
// RUN: %hermes-node --build-bundle=%t.kinds/app.bundle %t.kinds/cli.js 2>&1 | %FileCheck --check-prefix=KINDS %s
// KINDS: warning: skipping {{.*}}esm.mjs (.mjs is ESM, not packageable)
// KINDS: bundle root: {{.*}}.kinds

// Delete every source file, leaving only the bundle: the .cjs and the
// extensionless module have to come out of the container. Without them this
// prints a "Cannot find module" stack instead of a sum.
// RUN: find %t.kinds -name '*.js' -delete && find %t.kinds -name '*.cjs' -delete && rm -rf %t.kinds/node_modules
// RUN: %hermes-node --bundle=%t.kinds/app.bundle | %FileCheck --check-prefix=KINDSOUT %s
// KINDSOUT: SUM 7

// A `bin/` CLI script starts with a hashbang, which is legal only at the
// very start of a Program -- inside the CommonJS wrapper it is a syntax
// error. The producer has to strip it, the way libjs/loader.js and
// node_contextify.cpp already do, or a module that runs fine from disk fails
// the build outright. Both the entry and a dependency carry one here, and
// the dependency is extensionless because that is the shape a bin script
// usually has.
// RUN: rm -rf %t.hashbang && mkdir -p %t.hashbang/node_modules/tool
// RUN: printf '#!/usr/bin/env node\nconst t = require("tool");\nconsole.log("HB", t.v);\nt.boom();\n' > %t.hashbang/cli.js
// RUN: printf '{ "main": "tool" }\n' > %t.hashbang/node_modules/tool/package.json
// RUN: printf '#!/usr/bin/env node\nmodule.exports = { v: 4, boom: function() { throw new Error("kaboom"); } };\n' > %t.hashbang/node_modules/tool/tool
// RUN: %hermes-node --build-bundle=%t.hashbang/app.bundle %t.hashbang/cli.js
// RUN: find %t.hashbang -name '*.js' -delete && rm -rf %t.hashbang/node_modules
// RUN: %not %hermes-node --bundle=%t.hashbang/app.bundle 2>&1 | %FileCheck --check-prefix=HASHBANG %s
// HASHBANG: HB 4
// The stripped hashbang leaves its newline behind, so line numbers still
// match the original file: the throw is on line 2 of tool, and the call that
// reaches it is on line 4 of cli.js. Without that, both shift by one.
// HASHBANG: at {{.*}}node_modules/tool/tool:2:{{[0-9]+}}
// HASHBANG: at {{.*}}cli.js:4:{{[0-9]+}}

// The specifiers "." and ".." are relative in Node (Module._resolveLookupPaths
// treats a leading '.' followed by nothing, '.' or '/' as relative), and both
// appear in real trees -- `require('..')` is how a package's bin/ script
// reaches its own root. Classified as bare instead, ".." probes
// "<dir>/node_modules/.." which normalizes back to "<dir>", so it silently
// packages the requiring file's OWN directory, and "." fails the build
// outright. The sub/ directory below is itself resolvable (it has an
// index.js), which is what makes the wrong answer reachable.
// RUN: rm -rf %t.dots && mkdir -p %t.dots/a/sub
// RUN: echo "module.exports = 'A_INDEX';" > %t.dots/a/index.js
// RUN: echo "module.exports = 'SUB_INDEX';" > %t.dots/a/sub/index.js
// RUN: echo "module.exports = { up: require('..'), here: require('.') };" > %t.dots/a/sub/mod.js
// RUN: echo "const m = require('./a/sub/mod'); console.log('DOTS', m.up, m.here);" > %t.dots/cli.js
// RUN: %hermes-node --build-bundle=%t.dots/app.bundle %t.dots/cli.js
// RUN: find %t.dots -name '*.js' -delete
// RUN: %hermes-node --bundle=%t.dots/app.bundle | %FileCheck --check-prefix=DOTS %s
// DOTS: DOTS A_INDEX SUB_INDEX

// TypeScript is in scope for bundling: discovery parses .ts with the
// TypeScript front end, the producer compiles it with enable_ts, and the
// identity keeps the .ts extension. A .ts entry with a .ts dependency and a
// .json alongside covers all three, and running with the sources deleted is
// what proves the bytecode in the container was produced with types
// stripped -- nothing at run time could do it, since there is no compiler
// pass left.
// RUN: rm -rf %t.ts && mkdir -p %t.ts
// RUN: printf 'interface Dep { v: number }\nconst d: Dep = { v: 5 };\nmodule.exports = d;\n' > %t.ts/dep.ts
// RUN: echo '{ "v": 100 }' > %t.ts/cfg.json
// RUN: printf 'const dep = require("./dep");\nconst cfg = require("./cfg.json");\nconst sum: number = dep.v + cfg.v;\nconsole.log("TSSUM", sum);\n' > %t.ts/cli.ts
// RUN: %hermes-node --build-bundle=%t.ts/app.bundle %t.ts/cli.ts
// RUN: rm -f %t.ts/cli.ts %t.ts/dep.ts %t.ts/cfg.json
// RUN: %hermes-node --bundle=%t.ts/app.bundle | %FileCheck --check-prefix=TS %s
// TS: TSSUM 105

// A vendored package ('ws') is embedded in the binary and served by the
// runtime when no node_modules copy exists -- test/test-vendored-ws.js pins
// that. It is not in the producer's builtin skip set (see the Task 6
// anti-shadowing decision: an installed copy must be packaged and must win),
// so with nothing on disk to resolve, the specifier used to fail the build
// outright and no program using ws could be bundled at all. It has to warn
// and skip instead: the runtime serves the embedded copy, which survives the
// tree being deleted exactly like a builtin does.
// RUN: rm -rf %t.vendored && mkdir -p %t.vendored
// RUN: echo "const a = require('ws'); const b = require('node:ws'); console.log('VENDORED', typeof a.WebSocketServer, a === b);" > %t.vendored/cli.js
// RUN: %hermes-node --build-bundle=%t.vendored/app.bundle %t.vendored/cli.js 2>&1 | %FileCheck --check-prefix=VENDORED %s
// VENDORED-DAG: warning: not packaging 'ws' from {{.*}}cli.js (vendored package, served by the runtime)
// VENDORED-DAG: warning: not packaging 'node:ws' from {{.*}}cli.js (vendored package, served by the runtime)
// RUN: rm %t.vendored/cli.js
// RUN: %hermes-node --bundle=%t.vendored/app.bundle | %FileCheck --check-prefix=VENDOREDOUT %s
// VENDOREDOUT: VENDORED function true

// The companion direction, unchanged by the above and pinned here so it
// stays that way: when a node_modules copy of a vendored package IS
// installed, it is an ordinary dependency -- packaged like any other, and
// still served from the container once the tree is gone.
// RUN: rm -rf %t.wsinstalled && mkdir -p %t.wsinstalled/node_modules/ws
// RUN: echo '{ "main": "index.js" }' > %t.wsinstalled/node_modules/ws/package.json
// RUN: echo "module.exports = { mark: 'INSTALLED' };" > %t.wsinstalled/node_modules/ws/index.js
// RUN: echo "console.log('WSCOPY', require('ws').mark);" > %t.wsinstalled/cli.js
// RUN: %hermes-node --build-bundle=%t.wsinstalled/app.bundle %t.wsinstalled/cli.js
// RUN: rm -rf %t.wsinstalled/node_modules %t.wsinstalled/cli.js
// RUN: %hermes-node --bundle=%t.wsinstalled/app.bundle | %FileCheck --check-prefix=WSCOPY %s
// WSCOPY: WSCOPY INSTALLED

// An .mjs entry is rejected for the same reason a .json one is: the consumer
// has no way to execute it as the CommonJS entry point. The message says
// CommonJS, because an .mjs file is JavaScript and a message that only said
// "JavaScript" would read like a bug.
// RUN: echo "export const v = 1;" > %t.mjs-entry.mjs
// RUN: %not %hermes-node --build-bundle=%t.mjs-entry.bundle %t.mjs-entry.mjs 2>&1 | %FileCheck --check-prefix=MJSENTRY %s
// MJSENTRY: error: entry must be a CommonJS JavaScript or TypeScript file: {{.*}}mjs-entry.mjs

// A .json entry cannot be executed as a CommonJS module by the consumer, so
// it must be rejected at build time rather than silently producing an
// unrunnable bundle.
// RUN: echo '{ "v": 1 }' > %t.json-entry.json
// RUN: %not %hermes-node --build-bundle=%t.json-entry.bundle %t.json-entry.json 2>&1 | %FileCheck --check-prefix=JSONENTRY %s
// JSONENTRY: error: entry must be a CommonJS JavaScript or TypeScript file: {{.*}}json-entry.json

// This file is a lit driver only; the RUN lines above are the test.
