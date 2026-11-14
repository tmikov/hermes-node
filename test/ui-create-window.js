// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The first test in this plan that opens a real window.
//
// Four assertions, which are Task 8 Step 4's whole list: createWindow's
// Promise RESOLVES, the window reports a positive framebuffer size, a
// SECOND createWindow throws, and the process exits 0.
//
// Two features, and they are different questions. `ui` is about the
// hermes-node-ui binary existing and running at all (decided in
// test/lit.cfg by running it). `display` is about this machine being able
// to start an X server -- RULING 34 introduces it here, in the first task
// that needs a window, rather than in Task 17, because a test deferred past
// its feature reports UNSUPPORTED instead of failing for its stated reason,
// and lit scores UNSUPPORTED as success.
//
// REQUIRES: ui
// REQUIRES: display

// --- Why the RUN line looks like this. --------------------------------
//
// `xvfb-run` rather than a bare invocation: RULING 34 requires the RUN line
// to actually START a display, not merely to have detected that xvfb-run is
// installed. `-a` picks a free display number, which is what makes this
// safe under the suite's 16-way parallelism -- several of these can be in
// flight at once.
//
// `timeout` is the load-bearing part, and it is here because of how the
// interesting mutations fail. Break the init_cb -> JS handshake and this
// script does not print something wrong; it prints nothing and waits
// forever on a Promise nobody will settle. lit configures no per-test
// timeout (see dz 01a0d797-e326), so an unbounded hang would stall a
// worker slot instead of failing -- and a mutation whose failure mode is a
// hang is not coverage. With the bound, a hang exits 124 and the EXITCODE
// check below goes red like any other wrong answer.
//
// stderr goes to its own file: HERMES_NODE_DEBUG_NATIVE tracing and any
// driver chatter land there, and folding them into stdout would make the
// ordered checks below depend on what Mesa felt like logging.
//
// RUN: timeout 120 xvfb-run -a %hermes-node-ui %s > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out

const ui = require('hermes-node:ui');

async function main() {
  const win = await ui.createWindow({
    width: 320,
    height: 240,
    title: 'hermes-node-ui create-window test',
  });

  console.log('RESOLVED');

  // Positive, not a specific number. The window manager -- here, none at
  // all under Xvfb -- is free to give a different size than asked for, and
  // a high-DPI display makes the framebuffer size differ from the window
  // size by the DPI scale. What must hold is that a real framebuffer was
  // measured and reported, which is what a zero would disprove: a struct
  // nobody filled in reads as 0 here.
  console.log('FBWIDTH_POSITIVE ' + (win.framebufferWidth > 0));
  console.log('FBHEIGHT_POSITIVE ' + (win.framebufferHeight > 0));

  // One window per process (spec: Lifecycle). Sokol supports no more
  // (sokol#437), so a second request cannot be honoured and must not be
  // quietly ignored either -- an ignored second call would return a handle
  // to the FIRST window, and the program would then drive two of its own
  // objects into one window without being told.
  let second = 'did-not-throw';
  try {
    await ui.createWindow({width: 100, height: 100});
  } catch (e) {
    second = 'threw';
  }
  console.log('SECOND ' + second);

  // Explicit, and required in this build rather than tidy. createWindow
  // takes a REFERENCED libuv handle (spec: Lifecycle, so the runtime cannot
  // drain while the window is being created) and nothing releases it yet:
  // the lifecycle hook that closes it before runtime cleanup is Task 15's.
  // So a script that simply returned here would reach
  // UvEventLoop::close()'s uv_run(UV_RUN_DEFAULT) with a live handle and
  // wait there forever. process.exit() is the path that works today, and
  // it is also the one the spec says ends a UI process normally.
  process.exit(0);
}

main().catch((e) => {
  // Printed to stdout on purpose: the OUT checks below run against stdout,
  // so a rejection produces a visible wrong answer there rather than a
  // silent empty file whose only symptom is the exit code.
  console.log('REJECTED ' + (e && e.message));
  process.exit(1);
});

// OUT: RESOLVED
// OUT-NEXT: FBWIDTH_POSITIVE true
// OUT-NEXT: FBHEIGHT_POSITIVE true
// OUT-NEXT: SECOND threw

// CODE: EXITCODE 0
