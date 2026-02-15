/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_trace_events.h>
#include <node_api.h>

#include <cstring>

namespace hermes {
namespace node_compat {

/// getCategoryEnabledBuffer(category) -- returns a Uint8Array(1) that is
/// always 0 (tracing disabled).
static napi_value getCategoryEnabledBuffer(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value ab;
  void *data = nullptr;
  napi_create_arraybuffer(env, 1, &data, &ab);
  std::memset(data, 0, 1);

  napi_value result;
  napi_create_typedarray(env, napi_uint8_array, 1, ab, 0, &result);
  return result;
}

/// trace(phase, category, name, id, data) -- no-op.
static napi_value trace(napi_env env, napi_callback_info /*info*/) {
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

/// setTraceCategoryStateUpdateHandler(handler) -- no-op.
static napi_value setTraceCategoryStateUpdateHandler(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value initTraceEventsBinding(napi_env env, napi_value exports) {
  auto setFn = [&](const char *name, napi_callback cb) {
    napi_value fn;
    napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn);
    napi_set_named_property(env, exports, name, fn);
  };

  setFn("getCategoryEnabledBuffer", getCategoryEnabledBuffer);
  setFn("trace", trace);
  setFn(
      "setTraceCategoryStateUpdateHandler", setTraceCategoryStateUpdateHandler);

  return exports;
}

} // namespace node_compat
} // namespace hermes
