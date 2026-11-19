/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/compositor/channels.h>

#include <node_api.h>

/// At GLOBAL scope, deliberately, and this is not a stylistic choice.
/// `hnui_abi.h` declares `::hnui_window`, and `hnui_window.h` defines it
/// there. Writing `struct hnui_window *` inside the namespace below would
/// declare a SECOND, unrelated `hermes::node_compat::hnui_window` -- an
/// elaborated type specifier declares the name in its enclosing
/// namespace -- and the definition in `ui_module.cpp`, which returns the
/// global one, would then not match this declaration at all.
struct hnui_window;

/// Global scope too: sokol_app.h declares `typedef struct sapp_event {...}
/// sapp_event;`, and this is that struct's tag. Forward-declared rather
/// than included so that this header stays free of sokol_app.h.
struct sapp_event;

namespace hermes {
namespace node_compat {

class UiHost;
struct PlatformSource;

/// node_process.h's enumeration, declared opaquely rather than included:
/// that header pulls in `uv.h`, and this one is also compiled by the
/// no-window stub on a platform whose hand-compile check passes no libuv
/// include path. An opaque `enum class` declaration has the same underlying
/// type (`int`) as the definition, so the two agree by the language's rule.
enum class ExitReason;

/// The `hermes-node:ui` module, and the platform-thread half of the window
/// it creates.
///
/// This header exists because the two halves live on different threads and
/// in different files: `main.cpp` registers the module and owns the
/// `UiHost`, `ui_host.cpp` runs the platform thread, `ui_module.cpp` holds
/// the NAPI surface the JS thread calls, and `ui_platform.cpp` the
/// `sokol_app` callbacks the platform thread runs (what the two share is
/// declared in `ui_internal.h`). Task 8's brief lists only
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

/// Why the window is entering CLOSING. Traced as `close`, `exit`, `fatal`
/// and `return` respectively.
enum class UiCloseReason {
  /// The window itself: `win.close()`, or a `close` nobody cancelled.
  kWindowClose,
  /// `process.exit()`.
  kExitCall,
  /// An uncaught exception, through `process._fatalException`.
  kFatalException,
  /// `runHermesNode` reaching the end of its run.
  kRuntimeReturn,
};

/// Enter CLOSING (spec: Lifecycle). JS thread only. Idempotent: a second
/// call returns at once.
///
/// From here on no frame callback, event, readiness or capture result
/// reaches the program, and `requestAnimationFrame` (both forms) registers
/// nothing. The window stays on screen, presenting its last published
/// surface, until the process `_exit()`s. Called with no window ever
/// requested it records the state and does nothing else, so that a
/// `createWindow` called from an `'exit'` listener throws rather than
/// opening a window into a process on its way out.
void uiEnterClosing(UiCloseReason reason);

/// True from uiEnterClosing() on. JS thread only.
bool uiClosing();

/// The window closing itself: the default action of a `close` nobody
/// cancelled, and `win.close()`. Enters CLOSING (`kWindowClose`), then calls
/// the current value of `process.exit` with `process` as its receiver and
/// no argument, so `process.exitCode` is the status. Does not return when
/// that exits -- the ordinary case. If a program has replaced
/// `process.exit` with something that returns, this returns too, with the
/// window left in CLOSING and nothing to recover it (traced). JS thread
/// only.
///
/// \return false with a JavaScript exception pending if the call threw, or
///   if `process.exit` is not a function; the caller routes it.
bool uiCloseWindow(hnui_window *w);

/// `ProcessExitHooks::beforeExitListeners`, which `main()` installs: enters
/// CLOSING with the reason \p reason maps to. Ignores an env that is not the
/// window's (when a window was requested). JS thread.
void uiBeforeExitListeners(napi_env env, ExitReason reason);

/// `ProcessExitHooks::beforeRuntimeCleanup`, which `main()` installs.
/// Returns when no window was ever requested (the windowless path ends
/// through `UiHost::run()` as before) or when \p env is not the window's.
/// Otherwise does not return: `std::fflush(nullptr)`, `uv_tty_reset_mode()`,
/// `_exit(exitCode)`. JS thread.
void uiBeforeRuntimeCleanup(napi_env env, int exitCode);

/// Whether `createWindow` has been called. JS thread. What
/// `ui.writeClipboard` checks: with no window ever requested nothing would
/// drain the command queue, so an accepted call would silently do nothing.
bool uiWindowRequested();

/// Queue \p command for the platform thread, which applies it on its next
/// frame (before that frame's injected events). The one entry both the
/// JavaScript surface (`ui_events.cpp`) and the C entry points
/// (`hnui_api.cpp`) push through, so a command means the same thing from
/// either.
///
/// JS thread. Safe at any time, including before the window is ready: the
/// queue is a process-lifetime object that exists from before `createWindow`
/// is called, and a command pushed early is simply applied by the first
/// frame that runs. Not safe to call from the platform thread (it would
/// answer itself next frame, which is harmless but not what the queue is
/// for).
///
/// False, with nothing queued, once the window is CLOSING: the refusal is
/// traced (`command refused (closing): <name>`) and never thrown. Callers
/// that only issue a command ignore the result; the two clipboard reads use
/// it to fail at once instead of waiting for an answer that cannot come.
bool uiPushCommand(Command command);

/// The deadline a clipboard read gets when nobody names one: a JavaScript
/// `readClipboard()`, and the read behind a `paste` event.
constexpr int kClipboardReadDefaultTimeoutMs = 1000;

/// The registry of synchronous clipboard reads (`hnui_read_clipboard_sync`).
/// The JS thread creates a slot and blocks on it; the platform thread
/// answers into it directly when it applies the `kReadClipboard` command
/// that names the slot's id. Any thread; the registry carries its own lock.
ClipboardRequests &uiClipboardReads();

/// Start a new surface generation if \p state's framebuffer size differs
/// from the current generation's and both dimensions are positive; do
/// nothing otherwise. A failure to allocate ends the graphics session
/// (`terminateGraphicsSession`).
///
/// **JS thread only**, and called by `ui_events.cpp` for every delivered
/// event after the mirror is updated and before any listener runs, so a
/// `resize` listener -- native or JS -- already sees a current generation of
/// the new size, and the next `hnui_frame_target` reports it.
void uiFollowFramebufferSize(const WindowState &state);

/// The one entry both real and injected platform events go through.
/// Platform thread only.
///
/// Converts \p event against \p source (`sapp_event_convert.h`), stores
/// the result's window snapshot back as the platform thread's tracked
/// state, tells the tick machine about a minimize or a restore, pushes the
/// event onto the input queue, and posts the JS-thread dispatch -- at most
/// one post outstanding. A `QUIT_REQUESTED` is cancelled first, always --
/// in CLOSING too -- and forwarded as `close`: the platform thread never
/// lets Sokol act on a quit, because `sapp_run()` returning would close the
/// X display the JS thread's GL context was created on, under that thread,
/// and on macOS its quit path exits 0 from the main thread. Whether the
/// window then closes is decided on the JS thread, by the `close`
/// listeners (`uiCloseWindow`), and carried out there.
///
/// `onEvent` calls it with a source built over the real platform;
/// `onFrame` calls it for each injected event (`event_injector.h`) with
/// the injected clipboard, paths and window substituted where present.
void uiHandlePlatformEvent(
    const sapp_event &event,
    const PlatformSource &source);

} // namespace node_compat
} // namespace hermes
