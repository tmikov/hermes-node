// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The inspector runs on a second runtime in the same process. That runtime
// must execute only the inspector server, never the user's program.
//
// Config describing what a runtime should run is deliberately NOT inherited
// by it; only HermesNodeProcessConfig is. If the inspector config were ever
// built by copying the parent outright, scriptPath would take precedence
// over evalCode where the runtime picks what to execute, and the user's
// script would run a second time on the inspector thread -- concurrently,
// with its own event loop. This asserts it runs exactly once.
//
// Uses --inspect=0 for an OS-assigned port: compile-cache-inspect.js takes
// the fixed 9229, and lit runs tests in parallel.

// RUN: %hermes-node --inspect=0 %s > %t.txt 2>&1
// RUN: grep -c MARKER %t.txt | tr -d ' ' | %FileCheck %s

'use strict';

console.log('MARKER');

// CHECK: {{^1$}}
