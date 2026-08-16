// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Hard-error paths around --build-bundle and --bundle: an unresolvable
// require() at build time, a corrupt or truncated container at run time, and
// argument combinations that are mutually exclusive with --bundle. None of
// these should ever silently fall back to something else -- a bundle is a
// deliverable, and a bad one should say what is wrong with it.

// Unresolvable specifier is a hard build error.
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "require('nope-not-here');" > %t.tree/cli.js
// RUN: %not %hermes-node --build-bundle=%t.b %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=UNRESOLVED %s
// UNRESOLVED: cannot resolve 'nope-not-here'

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
