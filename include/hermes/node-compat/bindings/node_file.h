/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_FILE_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_FILE_H

#include <node_api_types.h>

namespace hermes {
namespace node_compat {

/// Init function for the 'fs' binding.
/// Follows the napi_addon_register_func signature.
napi_value initFsBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_FILE_H
