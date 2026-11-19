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
/// Task 6's scope stops short of any window. What exists here is the shape
/// the window support arrives into: run() spawns the JS thread and then
/// waits on a condition variable whose predicate is "a window was
/// requested, or the runtime finished" -- never "a window was requested"
/// alone, which would hang forever on a script that never asks for one.
/// That windowless path is the one thing this build can actually exercise,
/// and is its own gate: a script that sets process.exitCode and returns
/// runs to completion, and this process exits with that code.
///
/// requestWindow() is the other half of the predicate. Nothing calls it
/// yet -- the NAPI surface a script would call it through (hnui, Task 8+)
/// does not exist in this build -- but the wait predicate already has to
/// be the OR shown above for that call to be safe to add later without
/// touching run() again, so it is written now rather than bolted on when
/// the addon lands. Because nothing yet exists to service a window
/// request, run() cannot honour one in this build: it reports the error
/// and terminates the process (see run()'s own comment for why that is
/// _exit(), not a normal return) rather than joining a JS thread that, by
/// construction, has nothing left to end it -- a join there would trade
/// the "wait for a window forever" hang for a "wait for the JS thread
/// forever" hang, which is not a fix.
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
  /// so nothing here may re-derive or clamp it. On the window-requested
  /// path this build cannot service -- see the class comment -- it does
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

  /// Install the window runner. Called by main() before run(), and only in
  /// a build that has one -- Task 8's `uiRunPlatformLoop()`.
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
  /// Unset is a supported state, not a missing case: a build without window
  /// support (macOS until Task 16) leaves it null, and run() then reports
  /// and terminates exactly as it did before Task 8.
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

  /// Null in a build with no window support. Written by main() before the
  /// JS thread starts and read by run() on the same thread, so it needs no
  /// synchronisation of its own.
  WindowRunner windowRunner_ = nullptr;
};

} // namespace node_compat
} // namespace hermes
