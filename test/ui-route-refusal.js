// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Stage 1 of startup refuses a context route it cannot deliver, and the
// refusal REACHES JAVASCRIPT.
//
// Two invariants from Task 8 Step 5, and neither had a test:
//
//  1. "Refuse an unsupported route with a message rather than degrading."
//     Silently substituting the route that does work is the
//     swallow-and-continue shape this codebase removes on sight, and it is
//     especially bad here: the person who set this variable set it because
//     something was already wrong, which is exactly when being quietly
//     given the default is least useful.
//
//  2. "On shared-context failure, post the rejection and keep going until
//     JS has observed it." Stage 1 fails BEFORE sapp_run(), so there is no
//     Sokol loop to keep alive -- what has to hold instead is that
//     uiRunPlatformLoop() posts through OwnerDispatch and UiHost::run()
//     then joins the JS thread rather than returning out from under it.
//     Every case below only prints REJECTED if that whole path worked, so
//     this file pins the delivery mechanism as much as the message.
//
// HERMES_NODE_UI_GL_ROUTE is the hook, and it is not a test-only backdoor:
// it exists so an operator on a driver nobody here has met can name a
// route and be told plainly that it is not built.
//
// No window is ever opened -- stage 1 fails before sapp_run() -- so this
// needs no display, which is why it runs where ui-create-window.js cannot.
// `ui-gpu` is still required: a build with no GPU backend has no route to
// refuse, and UiHost reports "no window support" instead.
//
// REQUIRES: ui, ui-gpu

// --- An unknown route name. -------------------------------------------
//
// `timeout` for the same reason ui-create-window.js has one: the failure
// mode of a broken rejection path is a Promise nobody settles, and lit
// configures no per-test timeout (dz 01a0d797-e326). A hang would stall a
// worker slot instead of failing.
//
// RUN: timeout 60 env HERMES_NODE_UI_GL_ROUTE=bogus %hermes-node-ui %s > %t.bogus.out 2> %t.bogus.err; echo "EXITCODE $?" | %FileCheck --check-prefix=BOGUSCODE %s
// RUN: %FileCheck --check-prefix=BOGUS %s < %t.bogus.out
// RUN: %FileCheck --check-prefix=BOGUSERR %s < %t.bogus.err

// --- A route that is real but not built in v1. ------------------------
//
// pbuffer is the documented contingency for a driver that refuses a
// surfaceless make-current. Asking for it must say so, not fall back to
// surfaceless and appear to work -- on the driver that needed it, the
// fallback is precisely what would fail later and somewhere else.
//
// RUN: timeout 60 env HERMES_NODE_UI_GL_ROUTE=pbuffer %hermes-node-ui %s > %t.pb.out 2> %t.pb.err; echo "EXITCODE $?" | %FileCheck --check-prefix=PBCODE %s
// RUN: %FileCheck --check-prefix=PB %s < %t.pb.out
// RUN: %FileCheck --check-prefix=PBERR %s < %t.pb.err

// --- A handled rejection, and then the script simply returns. ----------
//
// A rejected createWindow leaves the window FAILED, and FAILED must not
// keep the process alive: rejectWith() gives back the referenced handle
// createWindow took, so the loop drains, runHermesNode reaches its end, and
// the pre-cleanup hook exits with the program's own process.exitCode.
// (Mutation: remove that release from rejectWith, and the run hangs until
// `timeout` kills it, status 124.) Its 'exit' listener prints what
// requestAnimationFrame answers, which is `RAF 0` only if the hook entered
// CLOSING (from FAILED) before the listener ran. (Mutation: move
// runHermesNode's kRuntimeReturn hook call below the 'exit' emit, and the
// listener prints `RAF threw: ... no window exists yet ...`.)
//
// RUN: timeout 60 env HERMES_NODE_UI_GL_ROUTE=bogus HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s return > %t.ret.out 2> %t.ret.err; echo "EXITCODE $?" | %FileCheck --check-prefix=RETCODE %s
// RUN: %FileCheck --check-prefix=RET %s < %t.ret.out
// RUN: %FileCheck --check-prefix=RETERR %s < %t.ret.err

const ui = require('hermes-node:ui');

const returnAfterRejection = process.argv[2] === 'return';

if (returnAfterRejection) {
  // In CLOSING requestAnimationFrame answers 0; in FAILED, which is where the
  // window is until the runtime-return hook enters CLOSING, it throws "no
  // window exists yet". So this line says which of the two the 'exit'
  // listener ran in.
  process.on('exit', () => {
    try {
      console.log('RAF ' + ui.requestAnimationFrame(() => {}));
    } catch (e) {
      console.log('RAF threw: ' + e.message);
    }
  });
}

ui.createWindow({width: 320, height: 240, title: 'route refusal'}).then(
    () => {
      // Reached only if the route was accepted, which for both values
      // below means it was silently degraded to the one that works.
      console.log('RESOLVED');
      process.exit(1);
    },
    (e) => {
      console.log('REJECTED ' + (e && e.message));
      if (returnAfterRejection) {
        process.exitCode = 5;
        return;
      }
      process.exit(3);
    });

// RET: REJECTED {{.*}}unknown value for HERMES_NODE_UI_GL_ROUTE: 'bogus'
// RET-NEXT: RAF 0
// RETCODE: EXITCODE 5
// RETERR: lifecycle: FAILED
// RETERR: lifecycle: event-loop handle released
// RETERR: lifecycle: runtime returned, exiting with status 5

// The message names the variable, the bad value, and both routes with
// their status, because "unknown route" alone leaves the reader to guess
// what the alternatives were.
// BOGUS: REJECTED {{.*}}unknown value for HERMES_NODE_UI_GL_ROUTE: 'bogus'
// BOGUS-SAME: surfaceless (built)
// BOGUS-SAME: pbuffer (not built in v1)
// BOGUSCODE: EXITCODE 3

// Also on stderr, before any of the JS ran: a route refusal must be
// legible even to someone whose script swallows the rejection.
// BOGUSERR: hermes-node-ui: unknown value for HERMES_NODE_UI_GL_ROUTE

// PB: REJECTED {{.*}}pbuffer GL context route is not built in v1
// PBCODE: EXITCODE 3
//
// The native half of the refusal, checked for the same reason BOGUSERR is:
// the JS side above only proves the promise was rejected with some message,
// while this proves the refusal was printed by the backend that made the
// decision. Without it this arm captured %t.pb.err and never looked at it,
// so the pbuffer branch could have stopped saying anything on stderr and
// nothing would have noticed.
// PBERR: hermes-node-ui: the pbuffer GL context route is not built in v1
