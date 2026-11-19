// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// utils/with-display.py's macOS display lock is MACHINE-wide: two window
// tests never have windows up at once, whichever lit invocation (or hand
// run) started them. Sokol centres every window, so two of the same size
// cover each other exactly; the covered one's swapchain goes invalid and
// its capture never matches.
//
// The lock used to live under $TMPDIR, and lit sets TMPDIR to a fresh
// lit_tmp_* directory per invocation (hermes/external/llvh/utils/lit/lit/
// main.py), so check-hermes-node and a concurrent utils/lit-run.sh took
// different locks. So the two runs here get DIFFERENT TMPDIRs and must
// still serialise.
//
// The queue for the display is a property of the machine, not of a test,
// so waiting for the lock is NOT charged to the test's limit: a run whose
// limit is shorter than the wait (C) still runs and succeeds, its limit
// starting once it holds the lock. The wait has its own budget
// (HERMES_NODE_UI_DISPLAY_LOCK_WAIT, default 1800 s); a run that exhausts
// it (D, budget 1 s) exits 125 -- never 124, which would read as the test
// timing out -- without running its command, naming the lock.
//
// The darwin branch is forced (test/fixtures/with-display/run_as_darwin.py),
// so this runs everywhere; it uses only flock and process groups. The lock
// path is pointed into %t through HERMES_NODE_UI_DISPLAY_LOCK, so a real
// Mac's lock is never touched.
//
// HARD is the lock file's hardening (test/fixtures/with-display/
// lock_hardening.py): the default path is in world-writable /tmp, so what
// another local user can plant there must not be turned against the user
// running the tests. A symlink is not followed (its 0600 target keeps its
// mode, a missing target is not created), a FIFO is refused at once rather
// than blocking the open, each with 125 and without running the command;
// a read-only existing lock still serialises; a file the wrapper creates is
// 0666 under umask 022 and 077. Lock-wait budgets that are not a finite,
// non-negative number are refused with 125. And the DEFAULT path, with no
// override, is the fixed machine-wide one whatever TMPDIR says.
//
// (Mutations: derive the lock path from TMPDIR again -- OVERLAP; charge
// the wait to the limit again -- C status=124 ran=0; ignore the budget --
// D waits for the holder and runs: status=0 ran=1; open with O_RDWR|O_CREAT
// and fchmod as before -- symlink status=0 target-mode=666, the dangling
// target is created; drop O_NONBLOCK -- the FIFO hangs (status=None); drop
// the fchmod -- umask 077 mode=600; accept nan -- budget nan runs;
// derive the default from TMPDIR -- default lines differ.)
//
// RUN: rm -rf %t.d && mkdir -p %t.d
// RUN: %python %S/fixtures/with-display/lock_check.py %source_dir/utils/with-display.py %t.d/display.lock %t.d | %FileCheck %s

// RUN: rm -rf %t.h && mkdir -p %t.h
// RUN: %python %S/fixtures/with-display/lock_hardening.py %source_dir/utils/with-display.py %t.h | %FileCheck --check-prefix=HARD %s

// CHECK: {{^}}serialised{{$}}
// CHECK-NEXT: {{^}}A status=0{{$}}
// CHECK-NEXT: {{^}}B status=0{{$}}
// CHECK-NEXT: {{^}}C status=0 ran=1 waited-past-limit=1{{$}}
// CHECK-NEXT: {{^}}D status=125 ran=0 bounded=1{{$}}
// CHECK-NEXT: {{^}}D said: with-display.py: gave up waiting for the display lock {{.*}}display.lock after 1 s{{$}}

// HARD: {{^}}symlink status=125 ran=0 target-mode=600{{$}}
// HARD-NEXT: {{^}}symlink said: with-display.py: refusing the display lock {{.*}}link-to-victim: it is a symbolic link{{$}}
// HARD-NEXT: {{^}}dangling status=125 ran=0 target-created=0{{$}}
// HARD-NEXT: {{^}}fifo status=125 ran=0 prompt=1{{$}}
// HARD-NEXT: {{^}}fifo said: with-display.py: refusing the display lock {{.*}}fifo: it is not a regular file{{$}}
// HARD-NEXT: {{^}}readonly status=0 serialised=1 mode=444{{$}}
// HARD-NEXT: {{^}}umask 022 status=0 ran=1 mode=666{{$}}
// HARD-NEXT: {{^}}umask 077 status=0 ran=1 mode=666{{$}}
// HARD-NEXT: {{^}}budget abc status=125 ran=0 said: with-display.py: HERMES_NODE_UI_DISPLAY_LOCK_WAIT must be a finite, non-negative number of seconds, not 'abc'{{$}}
// HARD-NEXT: {{^}}budget nan status=125 ran=0 said: {{.*}}not 'nan'{{$}}
// HARD-NEXT: {{^}}budget inf status=125 ran=0 said: {{.*}}not 'inf'{{$}}
// HARD-NEXT: {{^}}budget -1 status=125 ran=0 said: {{.*}}not '-1'{{$}}
// HARD-NEXT: {{^}}default (TMPDIR=one): /tmp/hermes-node-ui-display.lock{{$}}
// HARD-NEXT: {{^}}default (TMPDIR=two): /tmp/hermes-node-ui-display.lock{{$}}
