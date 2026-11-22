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
// `%with-display <seconds>` rather than a bare invocation: RULING 34
// requires the RUN line to actually START a display, not merely to have
// detected that Xvfb is installed. On Linux the wrapper
// (utils/with-display.py) runs utils/with-xvfb.py under `timeout`, and
// that has the server claim a free display itself through `-displayfd`,
// which is what makes this safe under the suite's 16-way parallelism --
// several of these can be in flight at once. (`xvfb-run -a`, which this
// replaced, chose the number before starting the server, and two runs
// could be handed the same one.) On macOS it serialises window tests on
// the one real display instead.
//
// The time limit is the load-bearing part, and it is here because of how the
// interesting mutations fail. Break the init_cb -> JS handshake and this
// script does not print something wrong; it prints nothing and waits
// forever on a Promise nobody will settle. lit configures no per-test
// timeout (see dz 01a0d797-e326), so an unbounded hang would stall a
// worker slot instead of failing -- and a mutation whose failure mode is a
// hang is not coverage. With the bound, a hang exits 124 and the EXITCODE
// check below goes red like any other wrong answer.
//
// stderr goes to its own file, and that file is read: the run is traced
// (HERMES_NODE_DEBUG_NATIVE=UI) so that there is a positive line, `window
// ready`, for the suite-wide stderr rule to hang on -- the ERR run refuses a
// sokol_app error and anything at all from either sokol_gfx instance
// (test/ui-stderr-rule.js). Folding the trace and any driver chatter into
// stdout instead would make the ordered checks below depend on what Mesa
// felt like logging. Redirections inside `sh -c`, as in every later window
// test, so the files are the binary's own streams whatever the display
// wrapper does.
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
// RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err

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

  // Explicit, and required rather than tidy. createWindow takes a
  // REFERENCED libuv handle (spec: Lifecycle, so the runtime cannot drain
  // while the window is being created), and only entering CLOSING -- or a
  // rejected createWindow -- releases it. So a script that simply returned
  // here would keep running with its window open, as a program with a
  // window should. process.exit() ends it; win.close() would too, and is
  // test/ui-close.js's subject.
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

// ERR: window ready
