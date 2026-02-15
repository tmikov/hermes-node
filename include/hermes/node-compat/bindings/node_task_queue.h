/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_TASK_QUEUE_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_TASK_QUEUE_H

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Function type for draining the microtask queue. Called by the
/// runMicrotasks binding function.
using DrainMicrotasksFn = void (*)(void *data);

/// Set the microtask drain function BEFORE the task_queue binding is
/// initialized. This must be called during bootstrap, before any JS code
/// loads internal/process/task_queues.
void setTaskQueueDrainMicrotasks(DrainMicrotasksFn fn, void *data);

/// Initialize the task_queue binding.
///
/// Exports:
///   tickInfo         — Uint32Array(2) shared state for tick scheduling
///   runMicrotasks    — drains the microtask queue (via
///   setTaskQueueDrainMicrotasks) setTickCallback  — registers the JS tick
///   drain function enqueueMicrotask — enqueues a function as a microtask
///   setPromiseRejectCallback — registers the promise rejection handler
///   promiseRejectEvents — object with kPromiseReject* constants
napi_value initTaskQueueBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_TASK_QUEUE_H
