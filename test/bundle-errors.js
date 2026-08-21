// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Hard-error paths around --build-bundle and --bundle: an entry point the
// build cannot compile, a corrupt or truncated container at run time, and
// argument combinations that are mutually exclusive with --bundle. None of
// these should ever silently fall back to something else -- a bundle is a
// deliverable, and a bad one should say what is wrong with it.
//
// What is NOT here: a require() that resolves to nothing, and a required
// file this engine cannot compile. Both are warnings, because neither stops
// the program from running. See test/bundle-tolerant.js.

// The entry is the one file the program is certain to load, so a build that
// cannot read it has produced nothing runnable and fails. Both phases that
// can reject it are pinned: the scan that collects require() calls, and the
// compile that follows the walk. Everywhere else these two are warnings, so
// a regression that made them uniform would go unnoticed without this.
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "function ( {" > %t.tree/unparseable.js
// RUN: %not %hermes-node --build-bundle=%t.b %t.tree/unparseable.js 2>&1 | %FileCheck --check-prefix=ENTRYPARSE %s
// ENTRYPARSE: error: failed to parse {{.*}}unparseable.js

// RUN: echo "module.exports = function (f) { return import(f); };" > %t.tree/uncompilable.js
// RUN: %not %hermes-node --build-bundle=%t.b %t.tree/uncompilable.js 2>&1 | %FileCheck --check-prefix=ENTRYCOMPILE %s
// ENTRYCOMPILE: error: failed to compile {{.*}}uncompilable.js

// A corrupt container is a hard error, not a fallback.
// RUN: echo "console.log('ok');" > %t.tree/ok.js
// RUN: %hermes-node --build-bundle=%t.good %t.tree/ok.js
// RUN: cp %t.good %t.bad && printf 'X' | dd of=%t.bad bs=1 seek=0 count=1 conv=notrunc 2>/dev/null
// RUN: %not %hermes-node --bundle=%t.bad 2>&1 | %FileCheck --check-prefix=CORRUPT %s
// CORRUPT: not a hermes-node bundle

// Truncation is a hard error too, and distinct from a bad-magic corruption:
// assert the actual truncation diagnostic, not just the presence of the
// word "error".
// RUN: head -c 40 %t.good > %t.trunc
// RUN: %not %hermes-node --bundle=%t.trunc 2>&1 | %FileCheck --check-prefix=TRUNC %s
// TRUNC: truncated (shorter than the header)

// The inspector is refused.
// RUN: %not %hermes-node --inspect --bundle=%t.good 2>&1 | %FileCheck --check-prefix=INSPECT %s
// INSPECT: --bundle cannot be combined with --inspect

// --bundle and --build-bundle are mutually exclusive: one consumes a
// container, the other produces one, and picking a winner silently would
// hide the mistake rather than reject it.
// RUN: %not %hermes-node --bundle=%t.good --build-bundle=%t.tree/out.bundle %t.tree/ok.js 2>&1 | %FileCheck --check-prefix=BOTHFLAGS %s
// BOTHFLAGS: --bundle cannot be combined with --build-bundle

// --bundle and -e/--eval are mutually exclusive: there is no entry point
// left for the bundle to supply once eval code wins.
// RUN: %not %hermes-node --bundle=%t.good -e "1" 2>&1 | %FileCheck --check-prefix=EVALFLAGS %s
// EVALFLAGS: --bundle cannot be combined with -e or --eval

// -r/--require is refused in bundle mode. A bundle carries its own
// preloads (--preload at build time); the operator of a sealed artifact
// does not get to insert code into it. This is also what makes the
// injection point unreachable: a preload running before the bundle loader
// was installed could plant Module._cache[<root>/<identity>] and replace a
// bundled module's exports, and there is now no such phase to occupy.
// RUN: rm -rf %t.rtree && mkdir -p %t.rtree
// RUN: echo "console.log('ok');" > %t.rtree/cli.js
// RUN: %hermes-node --build-bundle=%t.rtree/app.hbb %t.rtree/cli.js
// RUN: echo "console.log('PRELOAD RAN');" > %t.rtree/pre.js
// RUN: %not %hermes-node --bundle=%t.rtree/app.hbb -r %t.rtree/pre.js 2>&1 | %FileCheck --check-prefix=NORFLAG --implicit-check-not="PRELOAD RAN" %s
// NORFLAG: --bundle cannot be combined with -r or --require
