/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The macOS implementation of `platform_services.h`. Platform thread --
/// the main thread, AppKit's -- only, every function.
///
/// The application subclass before `sapp_run()`, putting the window on
/// screen without activating the application, the pasteboard both ways,
/// and the read-backs the command traces print.

#include "platform_services.h"

#include "mac_app.h"
#include "ui_trace.h"

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <cstdlib>

namespace hermes {
namespace node_compat {

namespace {

/// The pasteboard every read and write uses, decided once: the one
/// HERMES_NODE_UI_TEST_PASTEBOARD names, else the general pasteboard. The
/// test wrapper (utils/with-display.py) sets a name unique to the run, so
/// that the suite never touches the clipboard of whoever is using the Mac
/// and parallel runs do not share one (the plan's Ruling 87). Traced once,
/// with the name read back from the pasteboard AppKit returned.
NSPasteboard *pasteboard() {
  static NSPasteboard *chosen = nil;
  if (chosen == nil) {
    const char *name = std::getenv("HERMES_NODE_UI_TEST_PASTEBOARD");
    chosen = name != nullptr && name[0] != '\0'
        ? [NSPasteboard pasteboardWithName:@(name)]
        : [NSPasteboard generalPasteboard];
    uiTrace("clipboard: pasteboard '%s'", [chosen.name UTF8String]);
  }
  return chosen;
}

} // namespace

ClipboardResult uiPlatformReadClipboard(int timeoutMs, size_t maxBytes) {
  // AppKit's pasteboard read has no deadline to offer, so this never
  // produces kTimeout itself; the request's own deadline does
  // (applyReadClipboard). Nothing is cached: every answer is this read's.
  (void)timeoutMs;
  ClipboardResult result;
  @autoreleasepool {
    NSPasteboard *board = pasteboard();
    if (![board.types containsObject:NSPasteboardTypeString]) {
      result.status = ClipboardStatus::kUnavailable;
      return result;
    }
    NSString *text = [board stringForType:NSPasteboardTypeString];
    if (text == nil) {
      result.status = ClipboardStatus::kError;
      return result;
    }
    const NSUInteger bytes =
        [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    if (bytes > maxBytes) {
      result.status = ClipboardStatus::kTooLarge;
      return result;
    }
    result.text.assign([text UTF8String], bytes);
    result.status = ClipboardStatus::kOk;
  }
  return result;
}

void uiPlatformWriteClipboard(const std::string &utf8) {
  // Ours rather than sapp_set_clipboard_string(), whose pasteboard is
  // always the general one (see pasteboard()).
  @autoreleasepool {
    NSString *text = [[NSString alloc] initWithBytes:utf8.data()
                                              length:utf8.size()
                                            encoding:NSUTF8StringEncoding];
    NSPasteboard *board = pasteboard();
    [board clearContents];
    if (text != nil)
      [board setString:text forType:NSPasteboardTypeString];
  }
}

std::string uiPlatformDescribeClipboardWrite(size_t bytesWritten) {
  // Read back from the pasteboard: what it holds as a string now, in UTF-8
  // bytes (0 if it holds no string), beside what was written. One line per
  // write, whatever the text: test/ui-abi-sizes.js counts them.
  size_t held = 0;
  @autoreleasepool {
    NSString *text = [pasteboard() stringForType:NSPasteboardTypeString];
    if (text != nil)
      held = [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
  }
  char buffer[96];
  std::snprintf(
      buffer,
      sizeof buffer,
      "pasteboard_bytes=%zu bytes=%zu",
      held,
      bytesWritten);
  return buffer;
}

bool uiPlatformWindowTitle(std::string &out) {
  // Read back from the window AppKit holds, not from the title requested.
  // Only ever called after init_cb, when the window exists.
  NSWindow *window = (__bridge NSWindow *)uiMacWindow();
  if (window == nil)
    return false;
  NSString *title = window.title;
  out = title != nil ? std::string([title UTF8String]) : std::string();
  return true;
}

std::string uiPlatformDescribeIcon() {
  // Sokol puts the icon in the dock tile as an NSImageView holding an
  // NSImage made from it (_sapp_macos_set_icon). Read back from there, as
  // the CGImage's size: the image's one representation reports the tile it
  // was sized to instead -- measured: `256x256` pixels for a 128-point tile
  // at 2x, where the CGImage handed back is the 16x16 Sokol was given.
  @autoreleasepool {
    NSView *view = NSApp.dockTile.contentView;
    if (![view isKindOfClass:[NSImageView class]])
      return "no dock tile image";
    CGImageRef image = [((NSImageView *)view).image CGImageForProposedRect:NULL
                                                                   context:nil
                                                                     hints:nil];
    if (image == nullptr)
      return "dock tile image has no CGImage";
    char buffer[64];
    std::snprintf(
        buffer,
        sizeof buffer,
        "dock tile %zux%zu",
        CGImageGetWidth(image),
        CGImageGetHeight(image));
    return buffer;
  }
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
