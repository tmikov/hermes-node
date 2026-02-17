/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_CARES_WRAP_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_CARES_WRAP_H

#include <node_api_types.h>
#include <uv.h>

namespace hermes {
namespace node_compat {

/// Set the event loop for cares_wrap binding (must be called before binding
/// init).
void setCaresWrapEventLoop(uv_loop_t *loop);

/// Call before closing the event loop to prevent GC finalizers from touching
/// libuv handles after the loop is destroyed.
void caresWrapShutdown();

/// Init function for the 'cares_wrap' binding.
napi_value initCaresWrapBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_CARES_WRAP_H
