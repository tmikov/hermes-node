// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The reference demo's close button (F8 of the Slice C fix wave), from a
// REAL close request: test/fixtures/hnui-demo/demo.js ends with status 0,
// by the default action of a `close` nobody listens to -- the demo has no
// `close` listener at all. The X11 driver's `--close` waits for the demo's
// window to be mapped (no sleeping) and may therefore send the request
// before the demo's script has run past its `await`; that it still arrives
// is the early-delivery guarantee (spec: JS API -- no event is delivered
// before the window's Promise continuation has run). (Mutation: delete the
// uiCloseWindow call in deliver; the demo stays up and the run ends at its
// time limit with 124.)
//
// Split out of test/ui-real-events.js, and GL-only: the demo runs with the
// test hooks OFF, so the request must come from a second process, and
// nothing on macOS can close a hooks-off program's window from outside
// without a consent the session may not have given.
//
// REQUIRES: ui, ui-gpu, ui-backend-gl, display
//
// RUN: %with-display 120 sh -c '%hermes-node-ui %source_dir/test/fixtures/hnui-demo/demo.js %hnui_demo_addon js > %t.demo.out 2> %t.demo.err & %x11_window_driver --close "hermes-node-ui demo"; wait $!'; echo "DEMOEXIT $?" | %FileCheck --check-prefix=DEMO %s
// RUN: %FileCheck --check-prefix=DEMOOUT %s < %t.demo.out

// DEMO: DEMOEXIT 0
// DEMOOUT: MODE js
