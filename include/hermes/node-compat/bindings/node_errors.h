/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_ERRORS_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_ERRORS_H

#include <node_api_types.h>

namespace hermes {
namespace node_compat {

/// Init function for the 'errors' binding.
/// Follows the napi_addon_register_func signature.
napi_value initErrorsBinding(napi_env env, napi_value exports);

/// Hand \p error, which escaped an asynchronous callback, to JavaScript --
/// the route Node's node::errors::TriggerUncaughtException takes, and the
/// reason `process.on('uncaughtException')` sees anything at all. There must
/// be no exception pending when this is called; get and clear it first, and
/// pass the value here.
///
/// Returns true when a listener took responsibility for the error, in which
/// case the caller should carry on as though the callback had returned
/// normally. A caller that owns a libuv handle has to remember to put it
/// back the way a normal return would have left it -- the timers binding
/// re-arms its shared timer here, without which every timer still pending
/// after a handled throw stops firing.
///
/// **Does not return when nothing handled it.** `process._fatalException`
/// has by then set `process.exitCode` and emitted 'exit', so the only thing
/// left is to report the error and terminate with status 1. Callers
/// therefore need no unhandled path of their own, which is the point: the
/// decision belongs in one place rather than in every callback.
bool triggerUncaughtException(napi_env env, napi_value error);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_ERRORS_H
