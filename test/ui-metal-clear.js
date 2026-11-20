// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The macOS toolchain, end to end (Task 16b): hermes-node-ui opens a window
// through sokol_app's Metal backend, and the presenting sokol_gfx instance
// puts its clear colour on screen -- while the worker's drawing half, which
// is Task 16c's, refuses and rejects createWindow with a sentence saying so.
//
// Every line checked below is a trace of something that was executed or read
// back, never of what was requested:
//  - `route: metal (device=...)` names the MTLDevice that
//    MTLCreateSystemDefaultDevice() actually returned;
//  - `app: HnuiApplication installed` is printed after NSApp was read back
//    and found to be the subclass, and `activate=0` after the activation
//    policy was read back -- the no-activate arrangement %with-display asks
//    for, which keeps the suite's windows from taking focus;
//  - `presenting instance ready` follows sgp_setup() and the MSL blit
//    shader's compilation, with the colour format the swapchain reported
//    (28 is SG_PIXELFORMAT_BGRA8, what sokol_app gives a CAMetalLayer);
//  - `present: drawable acquired (320x240)` is the size of the drawable's
//    own texture, not sapp_width();
//  - `present: first clear committed` follows sgp_commit()'s return.
//
// `FRAMES true` is the platform-thread barrier: three frames ran after the
// rejection was delivered, so the clear is still being presented once the
// worker has refused -- a FAILED window keeps its platform thread alive
// until the process exits.
//
// (Mutations, each made and reverted once: skip sgp_commit() in
// MetalPresenter::clear() -- `first clear committed` is missing; drop the
// orderFrontRegardless in uiPlatformAfterInit() -- no frame is ever drawn,
// `drawable acquired` is missing and the run ends at its timeout; return
// true from MetalPresenter::acquireSwapchain() without calling
// sglue_swapchain() -- no valid drawable reaches the clear.)
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 60 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=ERR %s < %t.err

const ui = require('hermes-node:ui');

ui.createWindow({width: 320, height: 240, title: 'metal clear'}).then(
    () => {
      // The drawing half is not built, so nothing can have resolved this.
      console.log('RESOLVED');
      process.exit(1);
    },
    (e) => {
      const message = (e && e.message) || '';
      console.log(
          /not built yet/.test(message) ? 'REJECTED ok' :
                                          'REJECTED wrong: ' + message);
      console.log('FRAMES ' + ui.__waitPlatformFrames(3));
      process.exit(0);
    });

// CODE: EXITCODE 0

// OUT: REJECTED ok
// OUT-NEXT: FRAMES true

// ERR: [ui] route: metal (device={{.+}})
// ERR: [ui] app: HnuiApplication installed (activate=0)
// ERR: [ui] presenting instance ready: color_format 28, sample_count 1
// ERR: [ui] present: drawable acquired (320x240)
// ERR: [ui] present: first clear committed
