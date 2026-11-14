/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <node_api.h>

/// At GLOBAL scope, deliberately, and this is not a stylistic choice.
/// `hnui_abi.h` declares `::hnui_window`, and `hnui_window.h` defines it
/// there. Writing `struct hnui_window *` inside the namespace below would
/// declare a SECOND, unrelated `hermes::node_compat::hnui_window` -- an
/// elaborated type specifier declares the name in its enclosing
/// namespace -- and the definition in `ui_module.cpp`, which returns the
/// global one, would then not match this declaration at all.
struct hnui_window;

namespace hermes {
namespace node_compat {

class UiHost;

/// The `hermes-node:ui` module, and the platform-thread half of the window
/// it creates.
///
/// This header exists because the two halves live on different threads and
/// in different files: `main.cpp` registers the module and owns the
/// `UiHost`, `ui_host.cpp` runs the platform thread, and `ui_module.cpp`
/// holds both the NAPI surface the JS thread calls and the `sokol_app`
/// callbacks the platform thread runs. Task 8's brief lists only
/// `ui_module.cpp`; without a header, `ui_host.cpp` could not call into it.
///
/// `initUiModule` is implemented in EVERY build: `ui_module.cpp` where a GPU
/// backend was built (Linux GL today, macOS Metal with Task 16), and
/// `ui_module_nowindow.cpp` where none was, whose `createWindow` rejects
/// with the sentence `UiHost::run()` prints. `main.cpp` registers the module
/// unconditionally, because a `hermes-node:ui` that cannot be required is
/// reported as MODULE_NOT_FOUND -- indistinguishable from a typo.
///
/// Everything else declared here belongs to the window and is defined only
/// under `HERMES_NODE_UI_HAVE_WINDOW`, the compile definition
/// `tools/hermes-node-ui/CMakeLists.txt` sets when a backend exists. A
/// declaration costs nothing where there is no definition, and `main.cpp`
/// calls those two under that same guard.

/// Give the module the host whose `requestWindow()` it calls to wake the
/// platform thread. Called from `main()` before the JS thread starts, which
/// is what makes a process-global safe here: it is written once, before the
/// only other thread that reads it exists.
///
/// A process-global rather than a parameter because
/// `napi_addon_register_func` has no user-data argument -- the same
/// signature `BindingRegistry`'s bindings and `process.dlopen`'s addons use
/// (see `EmbedderModule::init`). Changing that signature to carry one would
/// be a change to the embedder-module contract Task 5 just settled.
void setUiModuleHost(UiHost *host);

/// `EmbedderModule::init` for `hermes-node:ui`. Runs on the JS thread, the
/// first time a script requires the module.
napi_value initUiModule(napi_env env, napi_value exports);

/// The window \p env owns, or null if it has none -- what
/// `hnui_get_window()` answers with.
///
/// The seam exists because the two halves live in different files:
/// `ui_module.cpp` owns the window object and everything in it, and
/// `hnui_api.cpp` implements the public C entry points against it without
/// reaching into that file's statics. Returns null for a runtime that
/// never called `createWindow`, or whose `createWindow` was rejected --
/// an ordinary answer, not an error.
///
/// Unqualified `hnui_window`, which ordinary lookup resolves to the
/// global one forward-declared at the top of this header -- see there for
/// why the `struct` keyword must not appear here.
hnui_window *uiWindowForEnv(napi_env env);

/// Run the window the JS thread asked for, on the process's main thread.
///
/// Called by `UiHost::run()` once its wait predicate has come back with
/// "a window was requested". Performs stage 1 of startup (the context
/// route, which must be decided before `sapp_run()` ranks its framebuffer
/// config and creates its context -- `sokol_app.h:14272-14274`), installs
/// the Sokol logger, and then enters `sapp_run()`.
///
/// \return a process exit code for the paths that can produce one. On Linux
///   `sapp_run()` does return; on macOS it never will (`[NSApp run]`),
///   which is why nothing downstream of this may rely on getting control
///   back.
int uiRunPlatformLoop();

} // namespace node_compat
} // namespace hermes
