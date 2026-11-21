/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_MAC_APP_H
#define HERMES_NODE_COMPAT_UI_MAC_APP_H

namespace hermes {
namespace node_compat {

/// macOS only (`mac_app.mm`). **Main thread only, every function** -- the
/// platform thread, which on macOS is the process main thread and is
/// AppKit's. A wrong-thread AppKit call usually works, which is why the rule
/// is stated here rather than left to be discovered.
///
/// Plain C++, naming no Objective-C type, so `platform_services_mac.mm` and
/// any later C++ caller can include it.

/// Make `NSApp` an instance of `HnuiApplication`, our `NSApplication`
/// subclass, before `sapp_run()`: Sokol's own `[NSApplication
/// sharedApplication]` then returns that instance (Mac probe P1). Idempotent.
///
/// Under `HERMES_NODE_UI_TEST_NO_ACTIVATE` (set by utils/with-display.py)
/// the instance refuses `activateIgnoringOtherApps:` and turns every
/// `setActivationPolicy:` into the Accessory policy, so a test window never
/// takes the keyboard from whoever is using the Mac and puts no icon in the
/// Dock. Traced, after reading the class and the mode back from the
/// installed instance, as `app: HnuiApplication installed (activate=<0|1>)`
/// -- which says the overrides are armed, not what they achieved. The
/// policy's effect is traced separately, by uiPlatformAfterInit().
void uiMacInstallApplication();

/// Whether the installed `HnuiApplication`'s two overrides are armed -- the
/// mode read back from the instance. False before
/// uiMacInstallApplication().
bool uiMacRefusesActivation();

/// The `NSWindow` Sokol created (`sapp_macos_get_window()`), or null before
/// Sokol has created it -- which a termination request can arrive before.
/// `void *` so this header stays C++.
void *uiMacWindow();

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UI_MAC_APP_H
