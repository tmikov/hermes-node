// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The classifier behind stage 1 of the macOS `display` lit feature
// (utils/macos_console_session.py, called from test/lit.cfg): whether the
// console session is usable, decided from `ioreg -n Root -d1 -a` before any
// window is opened. It runs on every platform, because the decision is pure
// parsing and the only machine where the real input exists is the one where
// a mistake would be hardest to see -- a wrong "locked" turns every window
// test UNSUPPORTED there, and the canary then has to explain why.
//
// Each case is checked by its verdict AND its reason, since the reason is
// what lit prints and what the `display-expected` canary repeats. A key
// that is ABSENT means not locked (`usable`): it is present only while the
// screen is locked, so treating absence as failure would turn the feature
// off on every unlocked Mac.
//
// RUN: %python %source_dir/utils/macos_console_session.py --self-test | %FileCheck %s

// CHECK: {{^}}locked: usable=0 reason=console session is locked{{$}}
// CHECK-NEXT: {{^}}usable: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}usable-nested: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}unlocked-flag-false: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}not-on-console: usable=0 reason=session is not on console{{$}}
// CHECK-NEXT: {{^}}no-users: usable=0 reason=no console session (IOConsoleUsers is empty){{$}}
// CHECK-NEXT: {{^}}no-key: usable=0 reason=ioreg output has no IOConsoleUsers{{$}}
// CHECK-NEXT: {{^}}garbage: usable=0 reason=ioreg output could not be parsed as a plist{{$}}
