// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `%display-reason` cannot break or run the shell line it is printed on.
// test/ui-display-feature.js and test/ui-display-busy.js print it inside
// single quotes, and a reason can quote a path. test/lit.cfg therefore
// registers it through utils/display_probe.py's sanitize_reason(), and
// `%display-reason-selftest` is a hostile string -- a single quote, a
// double quote, a backslash, `$(...)`, backquotes, `%t`, `%%` and a newline
// -- through the same function, used in the same shell context. It must
// arrive with each of those replaced by `?`: if the substitution executed
// anything, `EXECUTED` would appear without its `(echo`; if lit expanded
// `%t`, a path would appear; if the quote broke the line, the RUN fails.
//
// (Mutation: register the hostile string unsanitised in test/lit.cfg --
// the RUN line breaks.)
//
// RUN: echo 'reason %display-reason-selftest' | %FileCheck %s

// CHECK: {{^}}reason it?s ?x? ? ?(echo EXECUTED) ?echo EXECUTED? ?t ??{{[?]}}next{{$}}
