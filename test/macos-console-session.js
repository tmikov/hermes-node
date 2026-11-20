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
// With fast user switching IOConsoleUsers holds one entry per GUI session
// and ours need not be first: the entry is chosen by
// `kCGSSessionUserIDKey == getuid()` (the self-test fixes the uid at 501),
// else the single on-console entry, and anything ambiguous is refused with
// a reason. A lock flag of an unexpected type (integer 1) reads as locked.
// The two `real-` samples are in exactly the shape `ioreg -n Root -d1 -a`
// printed on macOS 27.2 (a top-level dict holding IOConsoleUsers and
// IOConsoleLocked; unlocked, the entry has NO CGSSessionScreenIsLocked;
// locked, it is true beside CGSSessionScreenLockedTime). The top-level
// IOConsoleLocked is a cross-check: true means locked whatever the entry
// says (`console is locked`), false adds nothing.
//
// The `display` lines are the third, advisory signal, CGDisplayIsAsleep
// through ctypes, with the probe stubbed so they run anywhere: asleep
// turns a usable verdict into `display is asleep`; awake, unknown or a
// raising probe leaves it unchanged. That the real call returns true for
// a sleeping display is UNVERIFIED on a Mac.
//
// (Mutations: ignore IOConsoleLocked -- ioconsolelocked-only reads usable;
// let a raising probe through -- the self-test dies; take IOConsoleUsers[0] -- console-user-second and
// mine-locked-other-not go red; test the lock flag with `is True` --
// integer-lock-flag reads usable.)
//
// RUN: %python %source_dir/utils/macos_console_session.py --self-test | %FileCheck %s

// CHECK: {{^}}locked: usable=0 reason=console session is locked{{$}}
// CHECK-NEXT: {{^}}usable: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}usable-nested: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}unlocked-flag-false: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}not-on-console: usable=0 reason=session is not on console{{$}}
// CHECK-NEXT: {{^}}console-user-second: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}mine-locked-other-not: usable=0 reason=console session is locked{{$}}
// CHECK-NEXT: {{^}}no-uid-one-on-console: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}no-uid-none-on-console: usable=0 reason=no console session for uid 501, and 0 sessions on console{{$}}
// CHECK-NEXT: {{^}}two-entries-for-uid: usable=0 reason=2 console sessions for uid 501{{$}}
// CHECK-NEXT: {{^}}real-unlocked: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}real-locked: usable=0 reason=console session is locked{{$}}
// CHECK-NEXT: {{^}}ioconsolelocked-only: usable=0 reason=console is locked{{$}}
// CHECK-NEXT: {{^}}integer-lock-flag: usable=0 reason=console session is locked{{$}}
// CHECK-NEXT: {{^}}no-users: usable=0 reason=no console session (IOConsoleUsers is empty){{$}}
// CHECK-NEXT: {{^}}no-key: usable=0 reason=ioreg output has no IOConsoleUsers{{$}}
// CHECK-NEXT: {{^}}garbage: usable=0 reason=ioreg output could not be parsed as a plist{{$}}
// CHECK-NEXT: {{^}}display asleep: usable=0 reason=display is asleep{{$}}
// CHECK-NEXT: {{^}}display awake: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}display unknown: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}display raising: usable=1 reason=console session is unlocked and on console{{$}}
// CHECK-NEXT: {{^}}display asleep, already locked: usable=0 reason=console session is locked{{$}}
