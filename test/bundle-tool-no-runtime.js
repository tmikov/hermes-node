// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The three read-only verbs answer before any runtime exists. runToolVerb()
// in tools/hermes-node/hermes-node.cpp dispatches them from main() ahead of
// runHermesNode(), which is what keeps a diagnostic tool from failing for
// reasons that have nothing to do with the file being diagnosed.
//
// That property has no direct observable, so this pins a consequence of it:
// the compile cache directory is created during runtime setup, so a verb
// that names one and returns first never creates it. Moving the dispatch
// inside runHermesNode -- while still not executing the bundled program --
// would pass every other test in the suite and fail this one.
//
// %hermes-node-cc clears the suite-wide HERMES_NODE_DISABLE_COMPILE_CACHE
// (see test/lit.cfg). Without it nothing would create the directory in any
// run, and every assertion below would hold for the wrong reason.

// A container and a bytecode file to point the verbs at.
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('B');" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=cli.js --out=%t.cli.hbc

// The positive control, first, and on the same file the verbs below are
// pointed at: running that container boots a runtime, and the runtime
// creates the directory it was pointed at. This is what makes the three
// assertions below mean "the runtime never started" rather than "bundle
// mode does not use the cache" or "the cache is off".
// RUN: rm -rf %t.cc-control
// RUN: %hermes-node-cc --compile-cache=%t.cc-control --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=CONTROL %s
// CONTROL: B
// RUN: test -d %t.cc-control

// RUN: rm -rf %t.cc-dump
// RUN: %hermes-node-cc --compile-cache=%t.cc-dump --bundle=%t.tree/app.hbb --dump > /dev/null
// RUN: test ! -e %t.cc-dump

// RUN: rm -rf %t.cc-extract
// RUN: %hermes-node-cc --compile-cache=%t.cc-extract --bundle=%t.tree/app.hbb --extract-module=cli.js --out=%t.again.hbc
// RUN: test ! -e %t.cc-extract

// RUN: rm -rf %t.cc-disasm
// RUN: %hermes-node-cc --compile-cache=%t.cc-disasm --dump-bytecode=%t.cli.hbc > /dev/null
// RUN: test ! -e %t.cc-disasm

// This file is a lit driver only; the RUN lines above are the test.
