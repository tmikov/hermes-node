/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_FILE_DIR_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_FILE_DIR_H

#include <node_api_types.h>

struct uv_loop_s;
typedef struct uv_loop_s uv_loop_t;

namespace hermes {
namespace node_compat {

/// Set the event loop for async fs_dir operations.
/// Must be called before initFsDirBinding().
void setFsDirEventLoop(uv_loop_t *loop);

/// Init function for the 'fs_dir' binding.
/// Follows the napi_addon_register_func signature.
napi_value initFsDirBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_FILE_DIR_H
