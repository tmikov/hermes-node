// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The macOS toolchain, end to end (Task 16b): hermes-node-ui opens a window
// through sokol_app's Metal backend, and the presenting sokol_gfx instance
// puts its clear colour on screen. Since Task 16c the window also resolves
// -- the worker's drawing half is built -- but this script draws nothing,
// so nothing is ever published and the presenter keeps clearing.
//
// Every line checked below is a trace of something that was executed or read
// back, never of what was requested:
//  - `route: metal (device=...)` names the MTLDevice that
//    MTLCreateSystemDefaultDevice() actually returned;
//  - `app: HnuiApplication installed` is printed after NSApp was read back
//    and found to be the subclass; its `activate=0` says the two
//    no-activate overrides are armed, which is the request, not the effect;
//  - `presenting instance ready` follows sgp_setup() and the MSL blit
//    shader's compilation, with the colour format the swapchain reported
//    (28 is SG_PIXELFORMAT_BGRA8, what sokol_app gives a CAMetalLayer);
//  - `app: activation policy=accessory` is the effect: NSApp's policy read
//    back after Sokol's launch code set it to Regular, which the
//    setActivationPolicy: override turned into Accessory -- the
//    no-activate arrangement %with-display asks for, which keeps the
//    suite's windows out of the Dock. The other override,
//    activateIgnoringOtherApps:, has no read-back in-process (Sokol posts
//    itself an application-activated event, so NSApp.isActive reads true
//    either way) and nothing here checks it;
//  - `present: drawable acquired (320x240)` is the size of the drawable's
//    own texture, not sapp_width();
//  - `present: first clear committed` is read back from the frame's
//    command buffer, held across sgp_commit(): Sokol has let go of it and
//    its status is Committed or later.
//
// `FRAMES true` is the platform-thread barrier: three frames ran after the
// window resolved, so the clear is still what is presented while nothing
// has been published.
//
// (Mutations, each made and reverted: skip sgp_commit() in
// MetalPresenter::clear() -- `first clear NOT committed` is printed and
// `first clear committed` is missing; delete the setActivationPolicy:
// override in mac_app.mm -- the policy reads `regular`; return true from
// MetalPresenter::acquireSwapchain() without calling sglue_swapchain() --
// under lit this breaks the display probe first, so this test reports
// UNSUPPORTED and what goes red is test/ui-display-feature.js (with
// HERMES_NODE_UI_EXPECT_DISPLAY=1); only a direct run shows Sokol's
// validation panic in sg_begin_pass. No test observes the
// orderFrontRegardless in uiPlatformAfterInit(): with it removed the window
// sits behind another application's and every check here still passes.)
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 60 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=ERR %s < %t.err

const ui = require('hermes-node:ui');

ui.createWindow({width: 320, height: 240, title: 'metal clear'}).then(
    () => {
      console.log('RESOLVED ok');
      console.log('FRAMES ' + ui.__waitPlatformFrames(3));
      process.exit(0);
    },
    (e) => {
      console.log('REJECTED ' + ((e && e.message) || ''));
      process.exit(1);
    });

// CODE: EXITCODE 0

// OUT: RESOLVED ok
// OUT-NEXT: FRAMES true

// ERR: [ui] route: metal (device={{.+}})
// ERR: [ui] app: HnuiApplication installed (activate=0)
// ERR: [ui] presenting instance ready: color_format 28, sample_count 1
// ERR: [ui] app: activation policy=accessory
// ERR: [ui] present: drawable acquired (320x240)
// ERR: [ui] present: first clear committed
