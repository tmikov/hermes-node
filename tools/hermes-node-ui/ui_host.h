/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

#include <uv.h>

namespace hermes {
namespace node_compat {

/// The hermes-node-ui host: runs a hermes-node runtime on a thread of its
/// own so that this process's main thread stays free for a future
/// windowing system to use -- opening a window, and pumping its native
/// event loop, generally has to happen on the thread the OS calls "main".
///
/// run() spawns the JS thread and then waits on a condition variable whose
/// predicate is "a window was requested, or the runtime finished" -- never
/// "a window was requested" alone, which would hang forever on a script
/// that never asks for one. On that windowless path a script that sets
/// process.exitCode and returns runs to completion, and this process exits
/// with that code.
///
/// requestWindow() is the other half of the predicate; `createWindow()` in
/// `ui_module.cpp` calls it. run() then calls the window runner main()
/// installed (see setWindowRunner). This library does not link the window
/// code, so a host with no runner installed cannot honour a request: run()
/// reports the error and terminates the process (see the comment there for
/// why that is _exit(), not a normal return) rather than joining a JS
/// thread that, by construction, has nothing left to end it -- a join
/// there would trade the "wait for a window forever" hang for a "wait for
/// the JS thread forever" hang, which is not a fix.
///
/// Threading: the constructor and run() are called from the process's main
/// thread and must not be called from the JS thread. requestWindow() may be
/// called from any thread, including the JS thread (that is the point of
/// it) -- it only ever takes the mutex, sets a flag, and signals.
class UiHost {
 public:
  /// \p config is moved in and run entirely on the spawned JS thread;
  /// nothing here reads it before that thread starts.
  explicit UiHost(HermesNodeConfig config);
  ~UiHost();

  UiHost(const UiHost &) = delete;
  UiHost &operator=(const UiHost &) = delete;

  /// Spawns the JS thread (with an explicit stack size -- see
  /// kJsThreadStackSize in ui_host.cpp) and blocks until the predicate
  /// above is true.
  ///
  /// On the windowless path (the runtime finished, no window was ever
  /// requested) this joins the JS thread and returns its exit code
  /// verbatim: runHermesNode() already carries the process.exitCode
  /// reconciliation described in CLAUDE.md's "Bootstrap Sequence" section,
  /// so nothing here may re-derive or clamp it. On a window request with
  /// no runner installed -- see the class comment -- it does
  /// not return at all: it terminates the process instead. Nor, in
  /// practice, does it return on the path that runs a window: the process
  /// is ended from the JS thread (process.exit(), the fatal path, or the
  /// pre-cleanup hook when the runtime reaches its own end), while this
  /// thread is still in the runner or in the join after it. So this
  /// signature's "returns an exit code" contract describes the windowless
  /// path.
  int run();

  /// Wakes run()'s wait with the window half of the predicate. Safe to
  /// call from any thread, including after the runtime has already
  /// finished (a no-op then: run() has nothing left to wait for).
  void requestWindow();

  /// What run() calls, on this (the platform) thread, when the window half
  /// of its predicate comes back true. Returns a process exit code; on
  /// macOS it will never return at all, because `sapp_run()` there is
  /// `[NSApp run]`.
  using WindowRunner = int (*)();

  /// Install the window runner, `uiRunPlatformLoop()`. Called by main()
  /// before run().
  ///
  /// **A function pointer rather than a direct call**, and that is a build
  /// constraint rather than a taste: `uiRunPlatformLoop()` lives in
  /// `ui_platform.cpp`, which is compiled into the hermes-node-ui executable
  /// and not into `hermesNodeUiHost`. `unittests/UiHostTest.cpp` links that
  /// library, so a direct call from here would leave the test with an
  /// undefined symbol -- and satisfying it would mean dragging sokol_app,
  /// a GL backend and the X11 libraries into a GTest whose whole subject is
  /// a condition-variable predicate.
  ///
  /// Unset is a handled state: run() then reports the window request and
  /// terminates. hermes-node-ui's main() always sets a runner, so only an
  /// embedder that links this library without one -- `UiHostTest` does, to
  /// test that branch -- reaches it.
  void setWindowRunner(WindowRunner runner);

 private:
  static void jsThreadMain(void *arg);

  HermesNodeConfig config_;

  uv_mutex_t mutex_;
  uv_cond_t cond_;
  // Guarded by mutex_.
  bool windowRequested_ = false;
  bool runtimeFinished_ = false;
  int runtimeExitCode_ = 0;

  uv_thread_t jsThread_{};

  /// Null when no runner was installed (see setWindowRunner). Written by main()
  /// before the JS thread starts and read by run() on the same thread, so it
  /// needs no synchronisation of its own.
  WindowRunner windowRunner_ = nullptr;
};

} // namespace node_compat
} // namespace hermes
