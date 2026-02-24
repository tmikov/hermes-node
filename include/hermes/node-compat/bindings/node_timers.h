/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_TIMERS_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_TIMERS_H

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Initialize the timers binding.
///
/// Exports:
///   setupTimers(processImmediate, processTimers) -- store JS callbacks
///   getLibuvNow()           -- current libuv time (ms, relative to start)
///   scheduleTimer(duration) -- start/restart the uv_timer_t
///   toggleTimerRef(ref)     -- ref/unref the timer handle
///   toggleImmediateRef(ref) -- start/stop the immediate idle handle
///   immediateInfo           -- Uint32Array(3): [kCount, kRefCount,
///   kHasOutstanding] timeoutInfo             -- Int32Array(1): [refed timeout
///   count]
napi_value initTimersBinding(napi_env env, napi_value exports);

/// Close the timer libuv handles. Must be called before the event loop is
/// destroyed. After calling this, run uv_run(UV_RUN_NOWAIT) to process close
/// callbacks.
void closeTimersHandles(napi_env env);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_TIMERS_H
