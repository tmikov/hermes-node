// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// utils/display_probe.py, which test/lit.cfg uses for the `display`
// feature, with utils/with-display.py's macOS branch forced so it runs on
// any POSIX machine (test/fixtures/with-display/probe_check.py).
//
//  - The outer timeout around the wrapper is DERIVED: lock wait + limit +
//    the kill escalation + a margin, so the wrapper always outlives its
//    own limit and kills its own command.
//  - A display lock held longer than the probe's lock-wait budget reads as
//    `display busy: ...` (lit.cfg then turns test/ui-display-busy.js red),
//    and the probe's command never starts.
//  - If the outer timeout fires anyway (forced here at 3 s), the wrapper's
//    whole session is killed: the command, in a process group of its own,
//    does not survive with a window open. In two steps: SIGTERM to the
//    wrapper alone, which with-display.py forwards to the command's group
//    -- so a command that dies on SIGTERM is gone with no external tool
//    (term-nopkill: pkill replaced by a nonexistent binary) -- and then
//    `pkill -KILL -s <sid>` for one that ignores it (ignore-pkill). Without
//    a working pkill such a command can survive, and the reason must say
//    so (ignore-nopkill), so the lit note and %display-reason carry it.
//
// (Mutations: drop HERMES_NODE_UI_DISPLAY_LOCK_WAIT from the probe's env --
// the wrapper waits its default 1800 s, the holder lets go after 4 s, and
// the command starts: started=1; kill only the wrapper on the outer
// timeout -- command-survived=1; skip the SIGTERM step -- term-nopkill
// command-survived=1; swallow the pkill failure -- unkillable=0 where it
// must be 1.)
//
// RUN: rm -rf %t.d && mkdir -p %t.d
// RUN: %python %S/fixtures/with-display/probe_check.py %source_dir %t.d | %FileCheck %s

// CHECK: {{^}}outer for defaults: 200 (lock wait 120 + limit 60 + grace 5 + margin 15){{$}}
// CHECK-NEXT: {{^}}busy status=125 started=0 is_busy=1{{$}}
// CHECK-NEXT: {{^}}busy reason: display busy: another run held the display lock for 1 s{{$}}
// CHECK-NEXT: {{^}}outer status=None command-started=1 command-survived=0{{$}}
// CHECK-NEXT: {{^}}outer reason: display probe killed after 3 s; the wrapper never returned{{$}}
// CHECK-NEXT: {{^}}term-nopkill command-started=1 command-survived=0 unkillable=1{{$}}
// CHECK-NEXT: {{^}}ignore-pkill command-started=1 command-survived=0 unkillable=0{{$}}
// CHECK-NEXT: {{^}}ignore-nopkill command-started=1 command-survived=1 unkillable=1{{$}}
