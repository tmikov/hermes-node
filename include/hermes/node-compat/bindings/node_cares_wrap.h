/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_CARES_WRAP_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_CARES_WRAP_H

#include <node_api_types.h>

namespace hermes {
namespace node_compat {

/// Call before closing the event loop to prevent GC finalizers from touching
/// libuv handles after the loop is destroyed.
void caresWrapShutdown(napi_env env);

/// Init function for the 'cares_wrap' binding.
napi_value initCaresWrapBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_CARES_WRAP_H
