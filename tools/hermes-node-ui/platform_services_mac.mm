/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The macOS implementation of `platform_services.h`. Platform thread --
/// the main thread, AppKit's -- only, every function.
///
/// Task 16b implements what a window needs to appear: the application
/// subclass before `sapp_run()`, putting the window on screen without
/// activating the application, and the title read-back. The clipboard, both
/// directions, and the icon read-back are Task 16e's: until then they
/// answer with an error or a sentence beginning "not implemented", never
/// with something that could pass for a real read-back.

#include "platform_services.h"

#include "mac_app.h"
#include "ui_trace.h"

#import <Cocoa/Cocoa.h>

#include <cstdio>

namespace hermes {
namespace node_compat {

ClipboardResult uiPlatformReadClipboard(int timeoutMs, size_t maxBytes) {
  // Task 16e: [NSPasteboard pasteboardWithName:]. Until then every read is
  // an error, never stale text.
  (void)timeoutMs;
  (void)maxBytes;
  ClipboardResult result;
  result.status = ClipboardStatus::kError;
  return result;
}

void uiPlatformWriteClipboard(const std::string &utf8) {
  // Task 16e: clearContents and setString:forType: on the pasteboard the
  // test wrapper names. Until then nothing is written -- in particular not
  // the general pasteboard of whoever is using the Mac.
  (void)utf8;
}

std::string uiPlatformDescribeClipboardWrite(size_t bytesWritten) {
  // One line per write, whatever the text: test/ui-abi-sizes.js counts
  // them.
  char buffer[96];
  std::snprintf(
      buffer,
      sizeof buffer,
      "not implemented until Task 16e (bytes=%zu)",
      bytesWritten);
  return buffer;
}

bool uiPlatformWindowTitle(std::string &out) {
  // Read back from the window AppKit holds, not from the title requested.
  // Only ever called after init_cb, when the window exists; the nil test
  // is for a build where Sokol's assert is compiled out (mac_app.h).
  NSWindow *window = (__bridge NSWindow *)uiMacWindow();
  if (window == nil)
    return false;
  NSString *title = window.title;
  out = title != nil ? std::string([title UTF8String]) : std::string();
  return true;
}

std::string uiPlatformDescribeIcon() {
  // Task 16e: the dock tile's image, read back.
  return "not implemented until Task 16e";
}

void uiPlatformBeforeRun() {
  uiMacInstallApplication();
}

/// NSApp's activation policy, by name, for the trace.
static const char *activationPolicyName(NSApplicationActivationPolicy policy) {
  switch (policy) {
    case NSApplicationActivationPolicyRegular:
      return "regular";
    case NSApplicationActivationPolicyAccessory:
      return "accessory";
    case NSApplicationActivationPolicyProhibited:
      return "prohibited";
  }
  return "unknown";
}

void uiPlatformAfterInit() {
  // After Sokol's launch code has set its policy: read back from NSApp, so
  // this is the setActivationPolicy: override's effect rather than the
  // mode it was asked for. Printed in either mode.
  uiTrace(
      "app: activation policy=%s",
      activationPolicyName(NSApp.activationPolicy));
  if (!uiMacRefusesActivation())
    return;
  // Sokol's makeKeyAndOrderFront: orders the window front within an
  // application that, under the no-activate mode, never became active --
  // so another application's windows may cover it. orderFrontRegardless
  // puts it above them without activating anything (Mac probe P1). Only
  // for whoever watches a test run: a window covered from the moment it
  // opens still gets a valid swapchain and a drawable -- measured on every
  // one of 600 frames, in two runs, with another application's window on
  // top throughout -- so no test can see this line go.
  NSWindow *window = (__bridge NSWindow *)uiMacWindow();
  [window orderFrontRegardless];
}

} // namespace node_compat
} // namespace hermes
