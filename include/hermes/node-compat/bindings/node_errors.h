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

/// The shape every native callback that calls into JavaScript needs: if an
/// exception is pending, take it and hand it to triggerUncaughtException.
///
/// Returns true when there was nothing pending, or when a listener took
/// responsibility -- either way the caller carries on. As above, it **does
/// not return** when nothing handled the error, so no caller needs an
/// unhandled path.
///
/// Carrying on is the caller's problem and not the same everywhere. A
/// callback that owns a libuv request must still free it; one that owns a
/// handle must leave it as a normal return would have. Getting that wrong
/// is how a handled throw turns into a leak or a stalled handle -- the
/// timers binding re-arms its shared timer for exactly this reason.
///
/// What this replaces, at around ten sites, was `napi_get_and_clear_last_
/// exception` and nothing else: the error was dropped on the floor, mostly
/// without even being printed. That exited 0 where Node exits 1, and where
/// the callback owned a live handle it was worse than that -- the handle
/// stayed refed and the program hung instead of failing.
bool handleCallbackException(napi_env env);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_ERRORS_H
