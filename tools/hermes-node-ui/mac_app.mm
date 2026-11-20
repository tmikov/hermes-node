/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "mac_app.h"

#include "ui_trace.h"

#include <sokol_app.h>

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>

/// The process's `NSApplication`, created before Sokol's
/// `[NSApplication sharedApplication]` so that call returns this instance.
///
/// In Task 16b it overrides only the two activation entry points; Task 16e
/// adds `terminate:`, which turns a termination request into the
/// cancelable close (the plan's Design (d)).
@interface HnuiApplication : NSApplication
/// Set once, before `sapp_run()`, from HERMES_NODE_UI_TEST_NO_ACTIVATE.
@property(nonatomic) BOOL hnuiRefusesActivation;
@end

@implementation HnuiApplication

/// Sokol calls this with YES once its window exists
/// (`applicationDidFinishLaunching:`). Refused under the no-activate mode:
/// the window is put on screen by `orderFrontRegardless` instead
/// (`uiPlatformAfterInit`), which does not make the application active.
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
  return const_cast<void *>(sapp_macos_get_window());
}

} // namespace node_compat
} // namespace hermes
