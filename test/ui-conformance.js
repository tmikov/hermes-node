// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The reference addon's conformance mode (spec: The reference addon): every
// hnui_* entry point the visual demo does not call, called once, with one
// printed line per observation -- returned values, statuses and counts read
// back, never a bare "ok" -- plus one frame rendered through the target's
// RESOLVE view from an MSAA image the addon owns.
//
// There is no code in this file. The conformance mode IS demo.js's
// (`demo.js <addon> conformance <x11_selection_owner> <capture.ppm>`), and
// it is lit-only by the spec's own description: it injects its events with
// ui.__injectEvent and captures with ui.__captureFrame, both test hooks, and
// starts and stops the x11_selection_owner fixture in `mute` mode for the
// clipboard timeout, exactly as test/ui-clipboard.js does. Running it here
// rather than from a copy is what keeps the copy from drifting.
//
// The MSAA frame. The addon clears its own 4x image to magenta and names the
// slot's resolve view as the pass's resolve attachment, so the resolved
// result is what the compositor presents. The capture is compared EXACTLY:
// every sample of a flat clear is the same value, so the resolve's average
// is that value, and magenta's channels are 0 and 255, which survive any
// float-to-unorm8 rounding; the blit is 1:1 (the agreement fields name equal
// target and framebuffer sizes). See test/ui-present.js for the full policy.
// (Mutation: attach the colour view where the resolve view belongs. Sokol's
// validation rejects the pass -- VALIDATE_BEGINPASS_RESOLVEATTACHMENTVIEW_TYPE
// -- and panics, and the panic terminates the process: CODE reads EXITCODE 1,
// stdout stops before the frame's lines, and stderr ends with the
// [sokol-produce:panic] line. Before the producer's logger terminated, the
// same mutation published a black frame and carried on.)
//
// Window-state values that depend on the X server (focus) are matched to
// each other -- the native read against the JS mirror -- rather than to a
// constant. Sizes are pinned to the 320x240 demo.js asks for: with no window
// manager, Xvfb gives a window exactly the size it requested, so a different
// one is a defect, and two variables bound to whatever came back would
// accept a wrong size as long as it was wrong consistently.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-present.js: redirections inside `sh -c`, so the
// trace file is the binary's own stderr whatever the display wrapper does,
// and `timeout` because lit has no per-test timeout (dz 01a0d797-e326).
// demo.js ends conformance mode with win.close(), the way a program would.
// A win.close() that did nothing leaves the run at its `timeout`; one that
// entered CLOSING and returned lets the loop drain and exit 0 through the
// runtime-return hook, which the implicit NOT on `runtime returned` catches.
//
// The backend (`%ui-backend`) is demo.js's last argument and selects the
// `CHECK-<backend>` and `TRACE-<backend>` lines. What names an X11 fact --
// `owns_selection`, the timeout against a mute selection owner -- is under
// `-gl`; on `gl` nothing is skipped.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %source_dir/test/fixtures/hnui-demo/demo.js %hnui_demo_addon conformance %x11_selection_owner %t.ppm %ui-backend > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefixes=CHECK,CHECK-%ui-backend %s < %t.out
// RUN: %FileCheck --check-prefixes=TRACE,TRACE-%ui-backend --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='runtime returned' %s < %t.err

// CODE: EXITCODE 0

// CHECK: hnui_api_version 1 (compiled against 1)
// CHECK-NEXT: hnui_get_window_state width=320 height=240 framebuffer=320x240 dpi_scale=1 focused=[[FOCUSED:[01]]] fullscreen=0 minimized=0
// CHECK-NEXT: hnui_get_window_state js mirror width=320 height=240 framebuffer=320x240 focused=[[FOCUSED]]
// CHECK-NEXT: hnui_frame_target size=320x240 sample_count=1 color_view_nonzero=1 resolve_view_nonzero=1 distinct=1
// CHECK-NEXT: hnui_frame_target msaa image sample_count=4 size=320x240 resolved into resolve_view
// CHECK-NEXT: hnui_frame_target capture matched=true publication=1
// CHECK-NEXT: hnui_frame_target capture top-left=(255,0,255) top-right=(255,0,255) bottom-left=(255,0,255) bottom-right=(255,0,255) centre=(255,0,255)
// CHECK-NEXT: hnui_cancel_animation_frame ids nonzero and distinct=1
// CHECK-NEXT: hnui_cancel_animation_frame ran=first
// CHECK-NEXT: hnui_remove_event_listener removed listener received 1 event(s), last key=293
// CHECK-NEXT: hnui_remove_event_listener remaining listener received keys=293,294
// CHECK-NEXT: hnui_event_dispatch_quiescent inside listener=0,0
// CHECK-NEXT: hnui_event_dispatch_quiescent next frame callback=1 (removed listener's user data freed there)
// CHECK-NEXT: hnui_cancel_close outside=0 inside=1
// CHECK-NEXT: hnui_set_cursor HNUI_CURSOR_POINTING_HAND (4) issued
// CHECK-NEXT: hnui_show_cursor 0 then 1 issued
// CHECK-NEXT: hnui_lock_pointer 1 then 0 issued
// CHECK-NEXT: hnui_write_clipboard "hnui conformance" issued
// CHECK-NEXT: hnui_read_clipboard_sync status=HNUI_OK text="hnui conformance"
// CHECK-NEXT: hnui_free released the HNUI_OK copy
// CHECK-gl-NEXT: hnui_read_clipboard_sync (owner never answers) status=HNUI_TIMEOUT out=NULL
// CHECK-NEXT: hnui_free(NULL) returned
// CHECK-NEXT: DONE

// What the platform thread APPLIED for the four void commands, and how it
// answered the two reads -- the observation the addon cannot make itself.
// TRACE: command cursor 4: sapp_mouse_cursor=4
// TRACE: command show cursor: sapp_mouse_shown=0
// TRACE: command show cursor: sapp_mouse_shown=1
// TRACE: command lock pointer: sapp_mouse_locked=1
// TRACE: command lock pointer: sapp_mouse_locked=0
// TRACE-gl: command clipboard: owns_selection=1 bytes=16
// TRACE: clipboard read sync #1: status=ok bytes=16
// TRACE-gl: clipboard read sync #2: status=timeout bytes=0
// TRACE: lifecycle: CLOSING (reason=close)
// TRACE: close: ending the process through process.exit()
