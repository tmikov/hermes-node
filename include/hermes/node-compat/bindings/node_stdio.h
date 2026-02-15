/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_STDIO_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_STDIO_H

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Initialize the stdio binding. Provides:
///   writeString(fd, string) — synchronous write of a string to a file
///   descriptor writeBuffer(fd, buffer) — synchronous write of a Uint8Array to
///   a file descriptor getHandleType(fd) — returns handle type string ("TTY",
///   "FILE", "PIPE", etc.)
napi_value initStdioBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_STDIO_H
