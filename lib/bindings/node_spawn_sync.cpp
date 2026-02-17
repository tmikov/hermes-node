/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_spawn_sync.h>
#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Stub spawn function that throws "not implemented".
static napi_value spawnStub(napi_env env, napi_callback_info /*info*/) {
  napi_throw_error(env, nullptr, "spawn_sync is not yet implemented");
  return nullptr;
}

napi_value initSpawnSyncBinding(napi_env env, napi_value exports) {
  napi_value spawnFn;
  napi_create_function(
      env, "spawn", NAPI_AUTO_LENGTH, spawnStub, nullptr, &spawnFn);
  napi_set_named_property(env, exports, "spawn", spawnFn);
  return exports;
}

} // namespace node_compat
} // namespace hermes
