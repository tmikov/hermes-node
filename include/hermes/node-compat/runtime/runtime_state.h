/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <node_api.h>
#include <uv.h>

#include <unordered_set>

namespace hermes {
namespace node_compat {

/// Per-runtime-instance state. Replaces all file-scope statics in bindings.
/// Stored via napi_set_instance_data, retrieved via getRuntimeState(env).
///
/// Lifetime: allocated by the bootstrap, deleted manually AFTER runtime
/// destruction (not by napi_set_instance_data finalizer) so that GC finalizers
/// can still access it during Runtime::~Runtime().
struct RuntimeState {
  // Shared event loop (replaces 6 separate s_*Loop globals).
  uv_loop_t *loop = nullptr;

  // Microtask drain (replaces s_drainMicrotasksFn/Data).
  void (*drainMicrotasksFn)(void *) = nullptr;
  void *drainMicrotasksData = nullptr;

  // Async break for contextify SIGINT (replaces s_triggerAsyncBreak/Data).
  void (*triggerAsyncBreakFn)(void *) = nullptr;
  void *triggerAsyncBreakData = nullptr;

  // Stream base shared typed array (replaces s_streamBaseState).
  int32_t *streamBaseState = nullptr;

  // Timers state pointer for cleanup (replaces s_timersState).
  // Typed as void* to avoid including timers internals.
  void *timersState = nullptr;

  // c-ares channels set for shutdown (replaces s_channels).
  // Typed as void* to avoid including ChannelWrap.
  std::unordered_set<void *> caresChannels;

  // Constructor refs (per-env, replaces file-scope napi_ref globals).
  napi_ref tcpCtorRef = nullptr;
  napi_ref pipeCtorRef = nullptr;
  napi_ref hashCtorRef = nullptr;
  napi_ref contextifySymbolRef = nullptr;
};

/// Retrieve the per-env RuntimeState. Returns nullptr if not set.
inline RuntimeState *getRuntimeState(napi_env env) {
  void *data = nullptr;
  napi_get_instance_data(env, &data);
  return static_cast<RuntimeState *>(data);
}

} // namespace node_compat
} // namespace hermes
