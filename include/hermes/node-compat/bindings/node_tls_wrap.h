/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_TLS_WRAP_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_TLS_WRAP_H

#include <node_api_types.h>

namespace hermes {
namespace node_compat {

/// Client TLS wrap. OpenSSL SSL + BIO pair over an existing TCP handle.
/// JS surface is tls.connect() / tls.TLSSocket, enough for https.get.
napi_value initTlsWrapBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_TLS_WRAP_H
