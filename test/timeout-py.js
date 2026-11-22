// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// utils/timeout.py is what `%timeout` expands to on macOS, and what
// utils/with-display.py applies its limit with there. On Linux `%timeout`
// is GNU `timeout`, so without this test nothing in the suite would ever
// run it. Ungated: it needs only python3, sh and sleep, and runs on both
// platforms.
//
// Each line is one property, with GNU `timeout`'s answer: 124 on expiry,
// within a bound; the command's own status otherwise; 128+N for a command
// killed by signal N (SIGABRT, 6); a command that ignores SIGTERM is
// killed by the SIGKILL escalation after the grace (KILL_GRACE_SECONDS, 5);
// the timed-out command's whole process group is gone afterwards, a
// grandchild `sleep` included; and a command that does not exist is 127.
//
// (Mutations: return 123 on expiry -- expiry and ignores-term lines go
// red; signal the child rather than its group -- grandchild-gone=0; drop
// the SIGKILL escalation -- the ignores-term case runs its 30 s and fails
// the bound;
// return 125 for a missing command -- missing status=125.)
//
// RUN: %python %S/fixtures/timeout/check_timeout.py %source_dir/utils/timeout.py %t.d | %FileCheck %s

// CHECK: {{^}}expiry status=124 bounded=1{{$}}
// CHECK-NEXT: {{^}}exit status=7{{$}}
// CHECK-NEXT: {{^}}signal status=134{{$}}
// CHECK-NEXT: {{^}}ignores-term status=124 escalated=1 bounded=1{{$}}
// CHECK-NEXT: {{^}}group status=124 grandchild-gone=1{{$}}
// CHECK-NEXT: {{^}}missing status=127{{$}}
