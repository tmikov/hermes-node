/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "mac_app.h"

#include "ui_module.h"
#include "ui_trace.h"

#include <sokol_app.h>

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>

/// The process's `NSApplication`, created before Sokol's
/// `[NSApplication sharedApplication]` so that call returns this instance.
///
/// It overrides the two activation entry points, and `terminate:`, which
/// turns a termination request into the cancelable close (the plan's
/// Design (d)).
@interface HnuiApplication : NSApplication
/// Set once, before `sapp_run()`, from HERMES_NODE_UI_TEST_NO_ACTIVATE.
@property(nonatomic) BOOL hnuiRefusesActivation;
@end

@implementation HnuiApplication

/// Sokol calls this with YES once its window exists
/// (`applicationDidFinishLaunching:`). Refused under the no-activate mode:
/// the window is put on screen by `orderFrontRegardless` instead
/// (`uiPlatformAfterInit`), which does not make the application active.
///
/// **No trace can show this refusal working.** Sokol posts itself a
/// synthetic application-activated event right after
/// `makeKeyAndOrderFront:`, so `NSApp.isActive` reads true whether or not
/// this override ran, and key and focus state follow it. Whether the
/// application took focus is only visible from outside the process
/// (`lsappinfo front`), which is where the plan's checks look.
- (void)activateIgnoringOtherApps:(BOOL)flag {
  if (self.hnuiRefusesActivation)
    return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  [super activateIgnoringOtherApps:flag];
#pragma clang diagnostic pop
}

/// Sokol sets `NSApp.activationPolicy = NSApplicationActivationPolicyRegular`
/// before creating its window. Under the no-activate mode every policy
/// becomes Accessory: no Dock icon and no menu bar, and an Accessory
/// application is not made active by showing a window. Mac probe P1: a
/// window shown this way stays visible to occlusion tracking, yields a
/// drawable, and never made its application frontmost in 9 of 9 runs.
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)policy {
  if (self.hnuiRefusesActivation)
    policy = NSApplicationActivationPolicyAccessory;
  return [super setActivationPolicy:policy];
}

/// What a Dock Quit, Cmd-Q or `[NSApp terminate:nil]` reaches. **Never
/// calls super**: AppKit's terminate: ends the process from inside the call
/// (measured before this existed: `cleanup_cb`, then status 0 or a
/// SIGTRAP), so the program's `close` listeners and exit code would be
/// bypassed. Instead the request becomes the close request a click on the
/// close button makes, which the shared code cancels and forwards to the
/// JS thread, whose listeners decide.
///
/// Through the window -- `performClose:`, so Sokol's delegate raises
/// QUIT_REQUESTED -- once Sokol delivers events: it has a window
/// (sapp_isvalid()) and has called `init_cb`, which a completed frame
/// implies. Before that, performClose: would be wrong rather than merely
/// early: Sokol sends no event before `init_cb` (`_sapp_events_enabled`),
/// so nothing would cancel the quit, and `windowShouldClose:` would let
/// the window close. So the request is then handed to the shared code
/// directly.
///
/// Each trace line is printed after the call it describes returned, and
/// says what was read back rather than what was intended: for
/// performClose:, how many quit requests the shared code cancelled during
/// the call (uiQuitRequestsCancelled(), before and after) -- 1 when Sokol's
/// delegate raised one, 0 when performClose: did not reach it; for the
/// direct hand-off, whether there was no window or a window that had drawn
/// no frame. The shared `quit requested` line is printed inside either.
- (void)terminate:(id)sender {
  using namespace hermes::node_compat;
  NSWindow *window = (__bridge NSWindow *)uiMacWindow();
  if (window != nil && sapp_frame_count() > 0) {
    const uint64_t before = uiQuitRequestsCancelled();
    [window performClose:sender];
    uiTrace(
        "app: terminate requested; through performClose:, %llu quit "
        "request(s) cancelled",
        static_cast<unsigned long long>(uiQuitRequestsCancelled() - before));
    return;
  }
  uiHandleQuitRequestDirectly();
  uiTrace(
      "app: terminate requested; %s, forwarded directly",
      window == nil ? "no window yet" : "no frame yet");
}

@end

namespace hermes {
namespace node_compat {

namespace {

/// Set (non-empty and not "0") by utils/with-display.py for every window
/// test on macOS.
bool noActivateRequested() {
  const char *value = std::getenv("HERMES_NODE_UI_TEST_NO_ACTIVATE");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

} // namespace

void uiMacInstallApplication() {
  static bool installed = false;
  if (installed)
    return;
  installed = true;

  @autoreleasepool {
    // Before anything else in the process touches NSApp: the first
    // +sharedApplication call decides the class, and every later one --
    // Sokol's included -- returns that instance.
    [HnuiApplication sharedApplication];
    if (![NSApp isKindOfClass:[HnuiApplication class]]) {
      // Something created a plain NSApplication first. The overrides would
      // then be absent, which is a test machine's focus being taken rather
      // than a broken window, so it is said rather than refused.
      uiTrace(
          "app: NSApp is %s, not HnuiApplication",
          [NSStringFromClass([NSApp class]) UTF8String]);
      return;
    }
    ((HnuiApplication *)NSApp).hnuiRefusesActivation = noActivateRequested();
    uiTrace(
        "app: HnuiApplication installed (activate=%d)",
        uiMacRefusesActivation() ? 0 : 1);
  }
}

bool uiMacRefusesActivation() {
  if (![NSApp isKindOfClass:[HnuiApplication class]])
    return false;
  return ((HnuiApplication *)NSApp).hnuiRefusesActivation;
}

void *uiMacWindow() {
  // sapp_isvalid() turns true in applicationDidFinishLaunching:, once the
  // window and its delegate exist; sapp_macos_get_window() asserts before.
  if (!sapp_isvalid())
    return nullptr;
  return const_cast<void *>(sapp_macos_get_window());
}

} // namespace node_compat
} // namespace hermes
