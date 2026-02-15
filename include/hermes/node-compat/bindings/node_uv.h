/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_UV_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_UV_H

#include <node_api_types.h>

namespace hermes {
namespace node_compat {

/// Init function for the 'uv' binding.
/// Exports UV_* error code constants, errname(), getErrorMap(),
/// getErrorMessage().
napi_value initUvBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_UV_H
