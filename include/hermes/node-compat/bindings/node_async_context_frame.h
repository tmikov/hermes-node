/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_ASYNC_CONTEXT_FRAME_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_ASYNC_CONTEXT_FRAME_H

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Initialize the async_context_frame binding (stub).
///
/// Exports:
///   getContinuationPreservedEmbedderData — returns undefined (stub)
///   setContinuationPreservedEmbedderData — no-op (stub)
napi_value initAsyncContextFrameBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_ASYNC_CONTEXT_FRAME_H
