// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// **The forcing function for the window tests' stderr rule.** Every RUN
// line that opens a window (`%with-display`) must have its stderr read by
// a FileCheck run carrying
//
//   --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'
//
// so that a Sokol error in a run that otherwise behaves is a failure.
// utils/check-ui-stderr-rule.py is the rule; its docstring gives the two
// exceptions and why. Without this test the rule holds until the next
// window test is written from a file that predates it: Task 17 found it
// missing from 23 RUN lines in nine files.
//
// `[sapp:warning` is not excluded: a healthy GL run prints one,
// LINUX_X11_QUERY_SYSTEM_DPI_FAILED.
//
// Mutation: delete the two exclusions from one RUN line of
// test/ui-events.js; this test fails naming that file and line.
//
// RUN: %python %source_dir/utils/check-ui-stderr-rule.py %source_dir/test | %FileCheck %s

// CHECK-NOT: {{^.*}}.js:{{[0-9]+}}:
// CHECK: {{^}}ui stderr rule: {{[1-9][0-9]*}} file(s), 0 violation(s){{$}}
