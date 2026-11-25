/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "mac_test_driver.h"

#include "mac_app.h"
#include "ui_trace.h"

#include <metal_test_hooks.h>

#import <Cocoa/Cocoa.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace hermes {
namespace node_compat {

namespace {

/// How long the JS thread waits for the main thread to run a command:
/// the same bound __waitPlatformFrames uses.
constexpr auto kDriveTimeout = std::chrono::seconds(10);

/// One command's hand-off between the two threads. Shared, because a
/// command the JS thread gave up on may still run on the main thread later
/// and must have somewhere to write.
struct Pending {
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;
  bool known = true;
  std::string answer;
};

/// The ANSI virtual key code for \p c (HIToolbox's kVK_ANSI_*), or -1.
/// Sokol translates an NSEvent's keyCode, not its characters, into a
/// sapp_keycode (_sapp_translate_key), so the code is what makes `KeyA`.
int keyCodeFor(char c) {
  switch (c) {
    case 'a':
      return 0x00;
    case 's':
      return 0x01;
    case 'd':
      return 0x02;
    case 'f':
      return 0x03;
    case 'h':
      return 0x04;
    case 'g':
      return 0x05;
    case 'z':
      return 0x06;
    case 'x':
      return 0x07;
    case 'c':
      return 0x08;
    case 'v':
      return 0x09;
    case 'b':
      return 0x0B;
    case 'q':
      return 0x0C;
    case 'w':
      return 0x0D;
    case 'e':
      return 0x0E;
    case 'r':
      return 0x0F;
    case 'y':
      return 0x10;
    case 't':
      return 0x11;
    case 'o':
      return 0x1F;
    case 'u':
      return 0x20;
    case 'i':
      return 0x22;
    case 'p':
      return 0x23;
    case 'l':
      return 0x25;
    case 'j':
      return 0x26;
    case 'k':
      return 0x28;
    case 'n':
      return 0x2D;
    case 'm':
      return 0x2E;
    default:
      return -1;
  }
}

/// A key event for our own window, as AppKit would deliver one.
NSEvent *keyEvent(
    NSEventType type,
    NSWindow *window,
    char c,
    int keyCode,
    NSEventModifierFlags modifiers = 0) {
  NSString *characters = [NSString stringWithFormat:@"%c", c];
  return [NSEvent keyEventWithType:type
                          location:NSZeroPoint
                     modifierFlags:modifiers
                         timestamp:[[NSProcessInfo processInfo] systemUptime]
                      windowNumber:window.windowNumber
                           context:nil
                        characters:characters
       charactersIgnoringModifiers:characters
                         isARepeat:NO
                           keyCode:static_cast<unsigned short>(keyCode)];
}

/// The window's content size in points, read from AppKit.
std::string contentSize(NSWindow *window) {
  const NSSize size = window.contentView.bounds.size;
  return std::to_string(static_cast<int>(size.width)) + "x" +
      std::to_string(static_cast<int>(size.height));
}

/// The answer to one command, handed back to the JS thread. Called exactly
/// once, on the main thread; \p known is false for a command this driver
/// does not have.
using Finish = std::function<void(const std::string &answer, bool known)>;

/// Main thread. Traces an applied action and hands its answer back.
void applied(
    const std::string &command,
    const std::string &answer,
    const Finish &finish) {
  uiTrace("drive: %s applied (%s)", command.c_str(), answer.c_str());
  finish(answer, true);
}

/// Main thread. Miniaturizes or deminiaturizes the window, and answers once
/// AppKit has finished doing it. Both are animated: read straight after
/// the call, `isMiniaturized` still reports the state before it (measured:
/// `miniaturize` answered `miniaturized=0`). So the read-back waits for
/// AppKit's own did-(de)miniaturize notification, run synchronously as it
/// is posted (`queue:nil`, on the main thread); if the window is already
/// in the requested state no notification will come, and it answers at
/// once. A notification that never arrives leaves the JS thread to time
/// out, as for any other command.
void changeMiniaturized(
    NSWindow *window,
    bool miniaturize,
    const std::string &command,
    const Finish &finish) {
  auto answerNow = [window]() {
    return std::string("miniaturized=") + (window.isMiniaturized ? "1" : "0");
  };
  if (static_cast<bool>(window.isMiniaturized) == miniaturize) {
    applied(command, answerNow(), finish);
    return;
  }
  // Copies, not the parameters: a block captures a C++ reference as a
  // reference, and the notification arrives after this frame is gone.
  const std::string commandCopy = command;
  const Finish finishCopy = finish;
  __block id observer = nil;
  observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:(miniaturize ? NSWindowDidMiniaturizeNotification
                                      : NSWindowDidDeminiaturizeNotification)
                  object:window
                   queue:nil
              usingBlock:^(NSNotification *) {
                [[NSNotificationCenter defaultCenter] removeObserver:observer];
                observer = nil;
                applied(commandCopy, answerNow(), finishCopy);
              }];
  if (miniaturize)
    [window miniaturize:nil];
  else
    [window deminiaturize:nil];
}

/// Main thread. Runs \p command against the window and calls \p finish
/// with what AppKit reports afterwards -- before returning, except for
/// `miniaturize` and `deminiaturize` (changeMiniaturized()).
void runOnMain(const std::string &command, const Finish &finish) {
  if (command == "terminate") {
    // What a Dock Quit or the menu's Quit item sends, and allowed before the
    // window exists: HnuiApplication's terminate: (mac_app.mm) forwards it
    // either way. Read back after the call returns, which it does once
    // terminate: is ours: the window's visibility, or that there is none.
    [NSApp terminate:nil];
    NSWindow *after = (__bridge NSWindow *)uiMacWindow();
    applied(
        command,
        after == nil ? std::string("window=0")
                     : std::string("visible=") + (after.isVisible ? "1" : "0"),
        finish);
    return;
  }

  NSWindow *window = (__bridge NSWindow *)uiMacWindow();
  if (window == nil) {
    finish("ERROR no window", true);
    return;
  }

  std::istringstream in(command);
  std::string verb;
  in >> verb;
  std::string answer;

  if (verb == "state") {
    // Not an action, so not traced: the answer is the read-back.
    const bool viewable =
        (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    finish(
        std::string("viewable=") + (viewable ? "1" : "0") + " " +
            contentSize(window),
        true);
    return;
  }

  if (verb == "key") {
    std::string arg;
    in >> arg;
    const int keyCode = arg.size() == 1 ? keyCodeFor(arg[0]) : -1;
    if (keyCode < 0) {
      finish("ERROR key: no key code for '" + arg + "'", true);
      return;
    }
    // Through the window, as AppKit dispatches a real key: sendEvent:
    // hands a key event to the first responder, which for Sokol is its
    // view. The read-back is of the responder chain, not of focus -- in an
    // Accessory application that was never activated, focus as AppKit
    // reports it is evidence of nothing (Task 16b as built).
    [window sendEvent:keyEvent(NSEventTypeKeyDown, window, arg[0], keyCode)];
    [window sendEvent:keyEvent(NSEventTypeKeyUp, window, arg[0], keyCode)];
    applied(
        command,
        std::string("first_responder_is_view=") +
            (window.firstResponder == window.contentView ? "1" : "0"),
        finish);
  } else if (verb == "cmd-key" || verb == "menu-key") {
    std::string arg;
    in >> arg;
    const int keyCode = arg.size() == 1 ? keyCodeFor(arg[0]) : -1;
    if (keyCode < 0) {
      finish("ERROR " + verb + ": no key code for '" + arg + "'", true);
      return;
    }
    NSEvent *event = keyEvent(
        NSEventTypeKeyDown,
        window,
        arg[0],
        keyCode,
        NSEventModifierFlagCommand);
    if (verb == "cmd-key") {
      // A Command key-down handed to the application, where AppKit's
      // event loop hands a real one: -[NSApplication sendEvent:] offers it
      // to the main menu as a key equivalent before any window gets it.
      // Nothing is returned to read, so the answer is what the menu's
      // action would have changed; the trace says whether an action ran.
      [NSApp sendEvent:event];
      applied(
          command,
          std::string("visible=") + (window.isVisible ? "1" : "0"),
          finish);
    } else {
      // The same event handed to the main menu directly, one step lower:
      // it skips -[NSApplication sendEvent:]'s routing, and its answer is
      // the menu's own, whether an item took the key.
      const BOOL handled = [NSApp.mainMenu performKeyEquivalent:event];
      applied(command, std::string("handled=") + (handled ? "1" : "0"), finish);
    }
  } else if (verb == "resize") {
    int width = 0;
    int height = 0;
    in >> width >> height;
    if (width <= 0 || height <= 0) {
      finish("ERROR resize: needs a positive width and height", true);
      return;
    }
    [window setContentSize:NSMakeSize(width, height)];
    applied(command, "content=" + contentSize(window), finish);
  } else if (verb == "close") {
    // performClose: asks the delegate, which is Sokol's: it raises
    // QUIT_REQUESTED and, unless cancelled, closes.
    [window performClose:nil];
    applied(
        command,
        std::string("visible=") + (window.isVisible ? "1" : "0"),
        finish);
  } else if (verb == "miniaturize" || verb == "deminiaturize") {
    changeMiniaturized(window, verb == "miniaturize", command, finish);
  } else if (verb == "gpu-hold") {
    // Not about the window: the test hooks' hold on the producer's GPU
    // work (metal_test_hooks.h), here because this is the one in-process
    // command channel a macOS test has.
    applied(
        command,
        std::string("held=") + (metalTestHoldProducerGpu() ? "1" : "0"),
        finish);
  } else if (verb == "gpu-release") {
    applied(
        command,
        "in_flight=" + std::to_string(metalTestReleaseProducerGpu()),
        finish);
  } else {
    finish("", false);
  }
}

} // namespace

napi_value driveWindow(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  char buffer[256];
  size_t length = 0;
  if (argc < 1 ||
      napi_get_value_string_utf8(
          env, argv[0], buffer, sizeof buffer, &length) != napi_ok) {
    napi_throw_type_error(
        env, nullptr, "__driveWindow: the argument must be a command string.");
    return nullptr;
  }
  const std::string command(buffer, length);

  auto pending = std::make_shared<Pending>();
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      runOnMain(command, [pending](const std::string &answer, bool known) {
        {
          std::lock_guard<std::mutex> lock(pending->mutex);
          pending->answer = answer;
          pending->known = known;
          pending->finished = true;
        }
        pending->done.notify_all();
      });
    }
  });

  bool finished;
  bool known;
  std::string answer;
  {
    std::unique_lock<std::mutex> lock(pending->mutex);
    finished = pending->done.wait_for(
        lock, kDriveTimeout, [&] { return pending->finished; });
    known = pending->known;
    answer = pending->answer;
  }
  if (!finished) {
    napi_throw_error(
        env,
        nullptr,
        ("__driveWindow: the main thread did not run '" + command +
         "' within 10 s.")
            .c_str());
    return nullptr;
  }
  if (!known) {
    napi_throw_error(
        env,
        nullptr,
        ("__driveWindow: unknown command '" + command + "'.").c_str());
    return nullptr;
  }

  napi_value result = nullptr;
  napi_create_string_utf8(env, answer.c_str(), answer.size(), &result);
  return result;
}

} // namespace node_compat
} // namespace hermes
