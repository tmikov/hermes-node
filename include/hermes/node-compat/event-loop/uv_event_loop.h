/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UV_EVENT_LOOP_H
#define HERMES_NODE_COMPAT_UV_EVENT_LOOP_H

#include <napi/hermes_napi.h>
#include <uv.h>

namespace hermes {
namespace node_compat {

/// A libuv-backed implementation of hermes_napi_host.
///
/// This adapter owns a uv_loop_t and provides the operations that
/// Hermes NAPI needs: post_work (background thread work), cancel_work,
/// post_task (main-thread callbacks), and uv_loop access.
///
/// Usage:
///   UvEventLoop loop;
///   loop.init();
///   napi_env env = hermes_napi_create_env(runtime, loop.getHost());
///   // ... register bindings, bootstrap JS ...
///   loop.run();   // blocks until no more active handles/requests
///   loop.close();
class UvEventLoop {
 public:
  UvEventLoop();
  ~UvEventLoop();

  UvEventLoop(const UvEventLoop &) = delete;
  UvEventLoop &operator=(const UvEventLoop &) = delete;

  /// Initialize the libuv loop and internal handles.
  /// Returns 0 on success, a libuv error code on failure.
  int init();

  /// Run the event loop. Blocks until there are no more active handles
  /// or requests (UV_RUN_DEFAULT mode).
  /// Returns 0 on success, non-zero on failure.
  int run();

  /// Run a single iteration of the event loop (UV_RUN_ONCE).
  /// Returns 0 when done, 1 if there are still active handles.
  int runOnce();

  /// Close the event loop. Must be called after run() returns.
  /// All handles must be closed before calling this.
  /// Returns 0 on success, UV_EBUSY if handles are still active.
  int close();

  /// Get the hermes_napi_host pointer to pass to
  /// hermes_napi_create_env(). Only valid after init().
  hermes_napi_host *getHost();

  /// Get the underlying uv_loop_t. Only valid after init().
  uv_loop_t *getLoop();

 private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UV_EVENT_LOOP_H
