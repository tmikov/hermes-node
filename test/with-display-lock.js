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
// CLEANER is macOS's tmp_cleaner, which can unlink a lock file under its
// holder (test/fixtures/with-display/cleaner_check.py): the holder of the
// unlinked file and the creator of its replacement overlap -- asserted
// honestly, nothing can prevent it -- but a later run queues behind the
// new holder, and so does a waiter that had already opened the OLD file:
// after flock succeeds, the descriptor is compared with the path, and a
// mismatch reopens. A lock file 10 days old is refreshed by its holder.
// (Mutations: remove the inode comparison -- W after-B=0; remove the
// refresh -- refreshed=0.)
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
// The three fixtures are independent (each has its own directory and lock
// files), so they run at once; each one's status is still checked, and its
// output is checked by a FileCheck run of its own.
//
// RUN: rm -rf %t.d %t.c %t.h && mkdir -p %t.d %t.c %t.h
// RUN: %python %S/fixtures/with-display/lock_check.py %source_dir/utils/with-display.py %t.d/display.lock %t.d > %t.d.out & D=$!; %python %S/fixtures/with-display/cleaner_check.py %source_dir/utils/with-display.py %t.c > %t.c.out & C=$!; %python %S/fixtures/with-display/lock_hardening.py %source_dir/utils/with-display.py %t.h > %t.h.out & H=$!; wait $D && wait $C && wait $H
// RUN: %FileCheck %s < %t.d.out
// RUN: %FileCheck --check-prefix=CLEANER %s < %t.c.out
// RUN: %FileCheck --check-prefix=HARD %s < %t.h.out

// CHECK: {{^}}serialised{{$}}
// CHECK-NEXT: {{^}}A status=0{{$}}
// CHECK-NEXT: {{^}}B status=0{{$}}
// CHECK-NEXT: {{^}}C status=0 ran=1 waited-past-limit=1{{$}}
// CHECK-NEXT: {{^}}D status=125 ran=0 bounded=1{{$}}
// CHECK-NEXT: {{^}}D said: with-display.py: gave up waiting for the display lock {{.*}}display.lock after 1 s{{$}}

// HARD: {{^}}symlink status=125 ran=0 target-mode=600{{$}}
// HARD-NEXT: {{^}}symlink said: with-display.py: refusing the display lock {{.*}}link-to-victim: it is a symbolic link (owner: {{[^ ]+}} (uid [[UID:[0-9]+]])); remove it (as that user or with sudo), or set HERMES_NODE_UI_DISPLAY_LOCK to another path{{$}}
// HARD-NEXT: {{^}}dangling status=125 ran=0 target-created=0{{$}}
// HARD-NEXT: {{^}}fifo status=125 ran=0 prompt=1{{$}}
// HARD-NEXT: {{^}}fifo said: with-display.py: refusing the display lock {{.*}}fifo: it is not a regular file (owner: {{[^ ]+}} (uid [[UID]])); remove it (as that user or with sudo), or set HERMES_NODE_UI_DISPLAY_LOCK to another path{{$}}
// HARD-NEXT: {{^}}readonly status=0 serialised=1 mode=444{{$}}
// HARD-NEXT: {{^}}umask 022 status=0 ran=1 mode=666{{$}}
// HARD-NEXT: {{^}}umask 077 status=0 ran=1 mode=666{{$}}
// HARD-NEXT: {{^}}budget abc status=125 ran=0 said: with-display.py: HERMES_NODE_UI_DISPLAY_LOCK_WAIT must be a finite, non-negative number of seconds, not 'abc'{{$}}
// HARD-NEXT: {{^}}budget nan status=125 ran=0 said: {{.*}}not 'nan'{{$}}
// HARD-NEXT: {{^}}budget inf status=125 ran=0 said: {{.*}}not 'inf'{{$}}
// HARD-NEXT: {{^}}budget -1 status=125 ran=0 said: {{.*}}not '-1'{{$}}
// HARD-NEXT: {{^}}default (TMPDIR=one): /tmp/hermes-node-ui-display.lock{{$}}
// HARD-NEXT: {{^}}default (TMPDIR=two): /tmp/hermes-node-ui-display.lock{{$}}

// CLEANER: {{^}}A-B overlap=1 (the unlinked file: unavoidable){{$}}
// CLEANER-NEXT: {{^}}W after-B=1 (W status 0){{$}}
// CLEANER-NEXT: {{^}}W reopened=1{{$}}
// CLEANER-NEXT: {{^}}C after-B=1 launched-while-B=1 (C status 0){{$}}
// CLEANER-NEXT: {{^}}refreshed=1{{$}}
