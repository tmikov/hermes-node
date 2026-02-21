/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_module_wrap.h>
#include <node_api.h>

namespace hermes {
namespace node_compat {

#define NAPI_CALL(call)                                             \
  do {                                                              \
    napi_status status_ = (call);                                   \
    if (status_ != napi_ok) {                                       \
      napi_throw_error(env, nullptr, "NAPI call failed in " #call); \
      return nullptr;                                               \
    }                                                               \
  } while (0)

/// Stub: require() of ES modules is not supported.
static napi_value createRequiredModuleFacadeCb(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_throw_error(
      env, "ERR_REQUIRE_ESM", "require() of ES modules is not supported");
  return nullptr;
}

napi_value initModuleWrapBinding(napi_env env, napi_value exports) {
  // kEvaluated = 4 (V8's Module::Status::kEvaluated).
  // Used by CJS loader to detect circular require() of ESM modules.
  napi_value kEvaluated;
  NAPI_CALL(napi_create_int32(env, 4, &kEvaluated));
  NAPI_CALL(
      napi_set_named_property(env, exports, "kEvaluated", kEvaluated));

  napi_value createRequiredModuleFacade;
  NAPI_CALL(napi_create_function(
      env,
      "createRequiredModuleFacade",
      NAPI_AUTO_LENGTH,
      createRequiredModuleFacadeCb,
      nullptr,
      &createRequiredModuleFacade));
  NAPI_CALL(napi_set_named_property(
      env,
      exports,
      "createRequiredModuleFacade",
      createRequiredModuleFacade));

  return exports;
}

#undef NAPI_CALL

} // namespace node_compat
} // namespace hermes
