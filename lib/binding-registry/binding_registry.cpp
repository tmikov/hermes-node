/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/binding-registry/binding_registry.h>

#include <js_native_api.h>
#include <node_api.h>

#include <cassert>
#include <cstring>

namespace hermes {
namespace node_compat {

BindingRegistry::BindingRegistry() = default;

BindingRegistry::~BindingRegistry() = default;

void BindingRegistry::registerBinding(
    const std::string &name,
    napi_addon_register_func initFunc) {
  assert(initFunc && "initFunc must not be null");
  bindings_[name] = Entry{initFunc, nullptr};
}

void BindingRegistry::attach(napi_env /*env*/) {
  // Currently a no-op. Cached refs are created lazily in getBinding().
}

void BindingRegistry::detach(napi_env env) {
  // Delete all cached references.
  for (auto &pair : bindings_) {
    if (pair.second.cachedRef) {
      napi_delete_reference(env, pair.second.cachedRef);
      pair.second.cachedRef = nullptr;
    }
  }
}

napi_status BindingRegistry::getBinding(
    napi_env env,
    const char *name,
    napi_value *result) {
  assert(name && "name must not be null");
  assert(result && "result must not be null");

  auto it = bindings_.find(name);
  if (it == bindings_.end()) {
    // Throw a JS error: "No such binding: <name>"
    std::string msg = "No such binding: ";
    msg += name;
    napi_throw_error(env, "ERR_INTERNAL_BINDING", msg.c_str());
    return napi_pending_exception;
  }

  Entry &entry = it->second;

  // Return cached binding if available.
  if (entry.cachedRef) {
    return napi_get_reference_value(env, entry.cachedRef, result);
  }

  // Create a new exports object and call the init function.
  napi_value exports;
  napi_status status = napi_create_object(env, &exports);
  if (status != napi_ok)
    return status;

  napi_value initResult = entry.initFunc(env, exports);

  // Check if the init function threw an exception.
  bool hasPending = false;
  napi_is_exception_pending(env, &hasPending);
  if (hasPending)
    return napi_pending_exception;

  // Use the return value from initFunc (it may return a different object
  // than exports, following the NAPI module convention).
  if (initResult)
    exports = initResult;

  // Cache the result as a strong reference.
  status = napi_create_reference(env, exports, 1, &entry.cachedRef);
  if (status != napi_ok)
    return status;

  *result = exports;
  return napi_ok;
}

/// The C callback for the internalBinding JS function.
/// The BindingRegistry pointer is passed as the `data` parameter.
static napi_value internalBindingCallback(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  void *data = nullptr;

  napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  if (status != napi_ok)
    return nullptr;

  if (argc < 1) {
    napi_throw_type_error(
        env,
        "ERR_INVALID_ARG_TYPE",
        "internalBinding requires a string argument");
    return nullptr;
  }

  // Get the binding name as a UTF-8 string.
  char nameBuf[256];
  size_t nameLen = 0;
  status =
      napi_get_value_string_utf8(env, argv[0], nameBuf, sizeof(nameBuf), &nameLen);
  if (status != napi_ok) {
    napi_throw_type_error(
        env,
        "ERR_INVALID_ARG_TYPE",
        "internalBinding requires a string argument");
    return nullptr;
  }

  auto *registry = static_cast<BindingRegistry *>(data);
  napi_value result = nullptr;
  registry->getBinding(env, nameBuf, &result);
  return result;
}

napi_status BindingRegistry::createInternalBindingFunction(
    napi_env env,
    napi_value *result) {
  return napi_create_function(
      env,
      "internalBinding",
      NAPI_AUTO_LENGTH,
      internalBindingCallback,
      this, // pass registry as data
      result);
}

} // namespace node_compat
} // namespace hermes
