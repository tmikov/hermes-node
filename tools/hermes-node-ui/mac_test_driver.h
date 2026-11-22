/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_MAC_TEST_DRIVER_H
#define HERMES_NODE_COMPAT_UI_MAC_TEST_DRIVER_H

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// macOS only (`mac_test_driver.mm`): the in-process window driver, the
/// macOS half of test/fixtures/hnui-demo/window_driver.js. Exported as
/// `ui.__driveWindow(command)` only when the test hooks are on.
///
/// **JS thread** (NAPI). Runs one command on the **main thread** -- every
/// AppKit call it makes is main-thread only -- through `dispatch_async` to
/// the main queue, and blocks the JS thread until the answer arrives. The
/// main thread never waits on the JS thread, so this cannot deadlock;
/// throws if the main thread did not run the command within 10 s, or for a
/// command it does not know.
///
/// Commands, and the answer each returns (window_driver.js maps the answer
/// onto the X11 driver's line; an answer starting `ERROR` is a refusal):
///
///   key <c>          `first_responder_is_view=<0|1>`
///   resize <w> <h>   `content=<w>x<h>`
///   close            `visible=<0|1>`
///   miniaturize      `miniaturized=<0|1>`
///   deminiaturize    `miniaturized=<0|1>`
///   terminate        `requested` (Task 16e is what gives it a meaning)
///   state            `viewable=<0|1> <w>x<h>`
///   gpu-hold         `held=<0|1>`
///   gpu-release      `in_flight=<n>`, read when the command runs: the
///                    frames are released after the worker's next loop
///                    turn, from another thread
///
/// The answers are read back after the action, never echoed from the
/// request -- for the two animated ones, `miniaturize` and
/// `deminiaturize`, after AppKit's notification that the animation is
/// over. Two are weaker than that, and say less than they seem to:
/// `key`'s `first_responder_is_view` is a precondition, read after the
/// event was sent -- that the view was where a key event goes, not that
/// this one arrived (the `keydown` event the script receives is the
/// evidence of that); and `terminate`'s `requested` is not a read-back at
/// all (Task 16e replaces it).
///
/// `gpu-hold` and `gpu-release` are not about the window: they are the test
/// hooks' hold on the producer's GPU work (`metal_test_hooks.h`), here
/// because this is the one in-process command channel a macOS test has.
///
/// Each command except `state` is traced as
/// `drive: <command> applied (<answer>)`. Nothing here posts a CGEvent or
/// needs Accessibility or Automation consent: the events are NSEvents sent
/// to our own window, and the rest are calls on our own NSWindow.
napi_value driveWindow(napi_env env, napi_callback_info info);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_MAC_TEST_DRIVER_H
