// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Every way --vm= can be refused. Nothing here needs a linker or a kit, so
// this file is deliberately not gated on any lit feature.

// RUN: echo "console.log('RAN');" > %t.js

// A ConsoleHost-only flag is refused by name, and says why -- rather than
// parsing cleanly and doing nothing.
// RUN: %not %hermes-node --vm=-Xdump-jitcode=1 %t.js 2>&1 | %FileCheck --check-prefix=CONSOLEHOST %s
// CONSOLEHOST: Xdump-jitcode
// CONSOLEHOST: ConsoleHost

// The three ConsoleHost-only flags that set a real RuntimeConfig/GCConfig
// bit are refused too, and by the same route: the bit costs real work and
// nothing in this runtime reports the result. -gc-print-stats is the one
// whose name promises printed output, which is why accepting it silently
// would be the worst of the three.
// RUN: %not %hermes-node --vm=-gc-print-stats %t.js 2>&1 | %FileCheck --check-prefix=STATS %s
// STATS: gc-print-stats
// STATS: ConsoleHost

// RUN: %not %hermes-node --vm=-track-io %t.js 2>&1 | %FileCheck --check-prefix=TRACKIO %s
// TRACKIO: track-io
// TRACKIO: ConsoleHost

// RUN: %not %hermes-node --vm=-sample-profiling %t.js 2>&1 | %FileCheck --check-prefix=SAMPLE %s
// SAMPLE: sample-profiling
// SAMPLE: ConsoleHost

// An unknown flag is our error, not LLVM's.
// RUN: %not %hermes-node --vm=-gc-max-hep=1g %t.js 2>&1 | %FileCheck --check-prefix=UNKNOWN %s
// UNKNOWN: unknown VM option
// UNKNOWN: gc-max-hep

// -help must never reach llvh::cl's help printer, which would dump the
// entire option registry and exit. A separate prefix from UNKNOWN above:
// FileCheck applies every directive under one prefix to each invocation it
// checks, and this invocation's output has no "gc-max-hep" in it.
// RUN: %not %hermes-node --vm=-help %t.js 2>&1 | %FileCheck --check-prefix=NOHELP %s
// NOHELP: unknown VM option

// A bad value for an honoured flag is reported, and the process does not
// die inside llvh::cl.
// RUN: %not %hermes-node --vm=-gc-max-heap=banana %t.js 2>&1 | %FileCheck --check-prefix=BADVAL %s
// BADVAL: gc-max-heap

// An empty value names the flag rather than reporting a parse failure with
// nothing in it.
// RUN: %not %hermes-node --vm= %t.js 2>&1 | %FileCheck --check-prefix=EMPTY %s
// EMPTY: Error: --vm requires a value

// The space form reaches the same check, because the check lives in
// checkToolOptions() with every other empty-value flag rather than in the
// parse loop, which only ever saw the --vm= spelling.
// RUN: %not %hermes-node --vm "" %t.js 2>&1 | %FileCheck --check-prefix=EMPTY %s

// The conflict matrix. Each message names both flags.
// RUN: %not %hermes-node --vm=-Xjit=on --build-exe=%t.out %t.hbb 2>&1 | %FileCheck --check-prefix=EXE %s
// EXE: --vm cannot be combined with --build-exe

// RUN: %not %hermes-node --vm=-Xjit=on --bundle=%t.hbb --dump 2>&1 | %FileCheck --check-prefix=DUMP %s
// DUMP: --vm cannot be combined with --dump

// RUN: %not %hermes-node --vm=-Xjit=on --dump-bytecode=%t.hbc 2>&1 | %FileCheck --check-prefix=DUMPBC %s
// DUMPBC: --vm cannot be combined with --dump-bytecode

// RUN: %not %hermes-node --vm=-Xjit=on --bundle=%t.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=VERIFY %s
// VERIFY: --vm cannot be combined with --verify-natives

// RUN: %not %hermes-node --vm=-Xjit=on --bundle=%t.hbb --extract-module=x --out=%t.o 2>&1 | %FileCheck --check-prefix=EXTRACT %s
// EXTRACT: --vm cannot be combined with --extract-module
