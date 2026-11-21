/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_host.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace hermes {
namespace node_compat {

namespace {

// 8 MiB: the common default main-thread stack size on Linux and macOS.
// uv_thread_create_ex is used specifically so the JS thread gets an
// explicit size rather than whatever a platform's thread-creation default
// happens to be -- the JS thread now runs everything a plain hermes-node
// process used to run directly on its one and only thread, deeply
// recursive JS included, and a smaller platform default (some musl or
// embedded libc builds default well under 1 MiB) would silently shrink
// how much recursion a script gets compared to running it under
// hermes-node itself.
constexpr size_t kJsThreadStackSize = 8u * 1024 * 1024;

} // namespace

UiHost::UiHost(HermesNodeConfig config) : config_(std::move(config)) {
  // Checked unconditionally, not with assert(): an assert compiles out
  // under NDEBUG, which is exactly a Release build, and the failure mode
  // on the other side of a swallowed init failure is silent -- every other
  // method here locks/waits/signals an uninitialised mutex or condition
  // variable from two threads, which is undefined behaviour with no
  // diagnostic pointing back at this constructor. This codebase removes
  // that shape of bug on sight (see CLAUDE.md's Bootstrap Sequence
  // section); a constructor cannot return an error code the way
  // uv_thread_create_ex's caller does in run() below, so the equivalent
  // here is to report and abort rather than continue construction.
  int rc = uv_mutex_init(&mutex_);
  if (rc != 0) {
    std::fprintf(stderr, "Error: uv_mutex_init failed: %s\n", uv_strerror(rc));
    std::abort();
  }
  rc = uv_cond_init(&cond_);
  if (rc != 0) {
    std::fprintf(stderr, "Error: uv_cond_init failed: %s\n", uv_strerror(rc));
    uv_mutex_destroy(&mutex_);
    std::abort();
  }
}

UiHost::~UiHost() {
  uv_cond_destroy(&cond_);
  uv_mutex_destroy(&mutex_);
}

void UiHost::jsThreadMain(void *arg) {
  UiHost *self = static_cast<UiHost *>(arg);
  // Thread-safe by runHermesNode's own contract (hermes_node_runtime.h):
  // its own runtime, its own event loop, its own bindings state, and
  // self->config_ is touched by nothing else for the lifetime of this
  // call -- run() only reads the three fields below, all guarded by
  // mutex_, and only after this function has finished with config_.
  int exitCode = runHermesNode(self->config_);

  uv_mutex_lock(&self->mutex_);
  self->runtimeExitCode_ = exitCode;
  self->runtimeFinished_ = true;
  uv_mutex_unlock(&self->mutex_);
  // Signalled unconditionally, whether or not a window was ever requested.
  // This is the half of the predicate that makes the windowless path
  // terminate: a wait that only woke for requestWindow() would block here
  // forever on a script that never calls it, which is exactly the hang
  // test/ui-windowless.js exists to rule out.
  uv_cond_signal(&self->cond_);
}

void UiHost::setWindowRunner(WindowRunner runner) {
  windowRunner_ = runner;
}

void UiHost::requestWindow() {
  uv_mutex_lock(&mutex_);
  windowRequested_ = true;
  uv_mutex_unlock(&mutex_);
  uv_cond_signal(&cond_);
}

int UiHost::run() {
  // Zero-initialised, not just the two fields set below: libuv's own
  // comment on uv_thread_options_s says "more fields may be added at any
  // time", and an uninitialised field a future libuv reads is exactly the
  // kind of bug that looks fine on the platform it was developed on.
  uv_thread_options_t options = {};
  options.flags = UV_THREAD_HAS_STACK_SIZE;
  options.stack_size = kJsThreadStackSize;

  int rc = uv_thread_create_ex(&jsThread_, &options, jsThreadMain, this);
  if (rc != 0) {
    std::fprintf(
        stderr, "Error: failed to start the JS thread: %s\n", uv_strerror(rc));
    return 1;
  }

  uv_mutex_lock(&mutex_);
  // A while loop, not an if: uv_cond_wait's contract allows it to return
  // with neither half of the predicate true (a spurious wakeup), and
  // re-checking after every wake is what turns that possibility into a
  // non-issue rather than a rare hang. An `if` here would be exactly the
  // kind of bug CLAUDE.md's "Bootstrap Sequence" history is full of: one
  // that mostly does not show, because a spurious wakeup is rare, and then
  // shows up once, under load, on somebody else's machine.
  while (!windowRequested_ && !runtimeFinished_) {
    uv_cond_wait(&cond_, &mutex_);
  }
  const bool windowRequested = windowRequested_;
  const bool runtimeFinished = runtimeFinished_;
  const int runtimeExitCode = runtimeExitCode_;
  uv_mutex_unlock(&mutex_);

  if (windowRequested && !runtimeFinished) {
    if (windowRunner_ != nullptr) {
      // The platform thread now belongs to the windowing system. On Linux
      // this returns when the window closes; on macOS it never returns.
      //
      // Its exit code is deliberately NOT what this function reports. Two
      // reasons, and they point the same way: the program's own
      // process.exitCode reconciliation lives in runHermesNode() (CLAUDE.md,
      // Bootstrap Sequence) and must not be re-derived here, and every path
      // that ends a UI process normally goes through process.exit() on the
      // JS thread, which _exit()s before this line is reached. So the
      // runner's value matters only on the paths where the JS thread is
      // still running and will answer for itself.
      windowRunner_();

      // The runner returns only when startup failed before sapp_run() (the
      // failure is posted to the JS thread as a rejected createWindow):
      // nothing calls sapp_quit(), every quit request is cancelled, and on
      // macOS sapp_run() never returns. Either way the process does not end
      // here. Every path that ends a windowed process ends it from the JS
      // thread with _exit() -- process.exit(), the fatal path, or, when the
      // runtime reaches its own end with a window requested, the
      // pre-cleanup hook uiBeforeRuntimeCleanup() -- so this join is what
      // keeps the process alive until the JS thread does, and in practice
      // it never completes.
      uv_thread_join(&jsThread_);
      uv_mutex_lock(&mutex_);
      const int exitCode = runtimeExitCode_;
      uv_mutex_unlock(&mutex_);
      return exitCode;
    }

    // No window runner was installed. hermes-node-ui's main() always
    // installs one, so this is reached only by an embedder of this library
    // that did not (UiHostTest, deliberately). Reported rather than falling
    // through to the windowless join below, which would report the
    // runtime's eventual exit code as the answer to a window request
    // nothing ever serviced.
    //
    // This does NOT join jsThread_ first. A joinable JS thread here is, by
    // definition, one still waiting for a window this build cannot create
    // -- nothing will ever make it finish, so joining would swap the
    // "waited forever for a window" hang the OR predicate above exists to
    // avoid for a "waited forever for the JS thread" hang one call later,
    // which is not a fix. _exit() (not a normal return, and not
    // std::exit(): no atexit handlers or static destructors may run while
    // that thread might still be touching config_, mutex_ or cond_) is the
    // same tool this codebase already uses for exactly this situation --
    // see CLAUDE.md's Bootstrap Sequence section on why processExit() and
    // triggerUncaughtException() both call _exit() rather than returning:
    // a live Hermes runtime on another thread cannot be safely unwound
    // through normal C++ teardown.
    std::fprintf(
        stderr,
        "Error: a window was requested, but hermes-node-ui has no window "
        "support yet.\n");
    std::fflush(stderr);
    // Each _exit() caller in this tree restores the terminal itself, and
    // this is one of them. stdin.setRawMode(true) reaches uv_tty_set_mode(),
    // which clears ECHO and ICANON; leaving that set hands the user back a
    // shell that no longer echoes what they type, with nothing on screen
    // saying why. atexit() is not available to us -- the _exit() callers
    // use _exit() deliberately, to keep ASAN from reporting the live Hermes
    // runtime as thousands of leaks -- so each caller restores for itself.
    // See lib/process/node_process.cpp and lib/bindings/node_errors.cpp.
    uv_tty_reset_mode();
    // NOT flushed the way those two callers flush. flushPendingWrites()
    // pumps the loop the JS thread owns and is still live in on this branch,
    // so calling it from the main thread would be a data race, not a fix.
    // Queued stdio writes are therefore lost here. Acceptable only because
    // hermes-node-ui never reaches this branch: its main() installs a
    // runner.
    _exit(1);
  }

  // The windowless path: the runtime already finished (or, in the
  // both-true race, finished at essentially the same moment a window was
  // requested -- nothing left to service either way). The join cannot
  // block meaningfully here since runtimeFinished_ was observed true under
  // the same mutex jsThreadMain sets it under, strictly before signalling.
  uv_thread_join(&jsThread_);
  return runtimeExitCode;
}

} // namespace node_compat
} // namespace hermes
