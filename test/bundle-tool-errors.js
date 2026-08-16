// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Every row of the flag-surface table in
// history/plans/2026-08-15-bundle-tooling-design.md, plus the two
// malformed-value cases. Each case asserts the specific diagnostic rather
// than the presence of the word "error": a check that matches any message
// passes when the binary fails for a reason that has nothing to do with the
// combination under test.
//
// The fixture is a real container and a real bytecode file, so every
// rejection below is a rejection of the combination and not of a missing
// file.

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "const u = require('./util'); console.log('T', u.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/util.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=util.js --out=%t.util.hbc

// A verb that reads a container needs one named.
// RUN: %not %hermes-node --dump 2>&1 | %FileCheck --check-prefix=NOBUNDLE %s
// NOBUNDLE: Error: --dump requires --bundle

// RUN: %not %hermes-node --extract-module=util.js --out=%t.x 2>&1 | %FileCheck --check-prefix=EXTNOBUNDLE %s
// EXTNOBUNDLE: Error: --extract-module requires --bundle

// Two verbs in one invocation is a mistake, not a precedence question, and
// the answer must not depend on which was typed first.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --dump --extract-module=util.js --out=%t.x 2>&1 | %FileCheck --check-prefix=TWOVERBS %s
// TWOVERBS: Error: --dump cannot be combined with --extract-module
// RUN: %not %hermes-node --extract-module=util.js --out=%t.x --dump --bundle=%t.tree/app.hbb 2>&1 | %FileCheck --check-prefix=TWOVERBS %s

// RUN: %not %hermes-node --dump --dump-bytecode=%t.util.hbc 2>&1 | %FileCheck --check-prefix=BCDUMP %s
// BCDUMP: Error: --dump-bytecode cannot be combined with --dump

// RUN: %not %hermes-node --extract-module=util.js --out=%t.x --dump-bytecode=%t.util.hbc 2>&1 | %FileCheck --check-prefix=BCEXT %s
// BCEXT: Error: --dump-bytecode cannot be combined with --extract-module

// Extraction writes a file, and the file it writes is always one the user
// named.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --extract-module=util.js 2>&1 | %FileCheck --check-prefix=NOOUT %s
// NOOUT: Error: --extract-module requires --out

// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --out=%t.x 2>&1 | %FileCheck --check-prefix=OUTALONE %s
// OUTALONE: Error: --out requires --extract-module

// --verbose has exactly three consumers. Naming it anywhere else asks for
// output that will never come, so it is refused rather than ignored.
// RUN: %not %hermes-node --verbose %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=VERBALONE %s
// VERBALONE: Error: --verbose requires --build-bundle, --dump or --dump-bytecode

// RUN: %not %hermes-node --verbose --bundle=%t.tree/app.hbb 2>&1 | %FileCheck --check-prefix=VERBALONE %s

// A bytecode file is not a container and does not come from one: naming
// both a container and a bytecode file describes two different jobs.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --dump-bytecode=%t.util.hbc 2>&1 | %FileCheck --check-prefix=BCBUNDLE %s
// BCBUNDLE: Error: --dump-bytecode cannot be combined with --bundle

// RUN: %not %hermes-node --build-bundle=%t.tree/other.hbb --dump-bytecode=%t.util.hbc %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=BCBUILD %s
// BCBUILD: Error: --dump-bytecode cannot be combined with --build-bundle

// None of the verbs runs a program, so there would be nothing for an
// inspector session to attach to. Each names itself: the pre-existing
// --bundle refusal is about bytecode that lacks debug info, which is a
// different reason and would be the wrong explanation here.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --dump --inspect 2>&1 | %FileCheck --check-prefix=DUMPINSPECT %s
// DUMPINSPECT: Error: --dump cannot be combined with --inspect

// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --extract-module=util.js --out=%t.x --inspect-brk 2>&1 | %FileCheck --check-prefix=EXTINSPECT %s
// EXTINSPECT: Error: --extract-module cannot be combined with --inspect

// RUN: %not %hermes-node --dump-bytecode=%t.util.hbc --inspect 2>&1 | %FileCheck --check-prefix=BCINSPECT %s
// BCINSPECT: Error: --dump-bytecode cannot be combined with --inspect

// A flag given an empty value names a file that cannot exist. Reporting it
// as a missing file leaves the user reading a diagnostic with no filename
// in it and no flag in it either.
// RUN: %not %hermes-node --dump-bytecode= 2>&1 | %FileCheck --check-prefix=BCEMPTY %s
// BCEMPTY: Error: --dump-bytecode requires a file path

// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --extract-module=util.js --out= 2>&1 | %FileCheck --check-prefix=OUTEMPTY %s
// OUTEMPTY: Error: --out requires a file path

// The matrix must reject only the combinations above: the valid ones still
// work, and --extract-module wrote the fixture this test opened with.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=OKDUMP %s
// OKDUMP: bundle: {{.*}}app.hbb
// OKDUMP: MODULES (2)
