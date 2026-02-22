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

/// Stub constructor for ModuleWrap — ESM is not supported.
static napi_value moduleWrapConstructorCb(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_throw_error(env, "ERR_REQUIRE_ESM", "ES modules are not supported");
  return nullptr;
}

/// No-op stub — only used in ESM evaluate paths.
static napi_value throwIfPromiseRejectedCb(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// No-op stub — ESM dynamic import callback.
static napi_value setImportModuleDynamicallyCallbackCb(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// No-op stub — ESM import.meta callback.
static napi_value setInitializeImportMetaObjectCallbackCb(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// Helper to set an integer constant on exports.
static napi_status setIntConstant(
    napi_env env,
    napi_value exports,
    const char *name,
    int32_t value) {
  napi_value val;
  napi_status s = napi_create_int32(env, value, &val);
  if (s != napi_ok)
    return s;
  return napi_set_named_property(env, exports, name, val);
}

/// Helper to set a function property on exports.
static napi_status setFunction(
    napi_env env,
    napi_value exports,
    const char *name,
    napi_callback cb) {
  napi_value fn;
  napi_status s =
      napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn);
  if (s != napi_ok)
    return s;
  return napi_set_named_property(env, exports, name, fn);
}

napi_value initModuleWrapBinding(napi_env env, napi_value exports) {
  // V8 Module::Status enum constants.
  NAPI_CALL(setIntConstant(env, exports, "kUninstantiated", 0));
  NAPI_CALL(setIntConstant(env, exports, "kInstantiating", 1));
  NAPI_CALL(setIntConstant(env, exports, "kInstantiated", 2));
  NAPI_CALL(setIntConstant(env, exports, "kEvaluating", 3));
  NAPI_CALL(setIntConstant(env, exports, "kEvaluated", 4));
  NAPI_CALL(setIntConstant(env, exports, "kErrored", 5));

  // Node ModulePhase enum constants.
  NAPI_CALL(setIntConstant(env, exports, "kSourcePhase", 1));
  NAPI_CALL(setIntConstant(env, exports, "kEvaluationPhase", 2));

  // Function stubs.
  NAPI_CALL(setFunction(
      env,
      exports,
      "createRequiredModuleFacade",
      createRequiredModuleFacadeCb));
  NAPI_CALL(setFunction(env, exports, "ModuleWrap", moduleWrapConstructorCb));
  NAPI_CALL(setFunction(
      env, exports, "throwIfPromiseRejected", throwIfPromiseRejectedCb));
  NAPI_CALL(setFunction(
      env,
      exports,
      "setImportModuleDynamicallyCallback",
      setImportModuleDynamicallyCallbackCb));
  NAPI_CALL(setFunction(
      env,
      exports,
      "setInitializeImportMetaObjectCallback",
      setInitializeImportMetaObjectCallbackCb));

  return exports;
}

#undef NAPI_CALL

} // namespace node_compat
} // namespace hermes
