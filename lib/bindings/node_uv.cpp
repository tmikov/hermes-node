/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_uv.h>
#include <node_api.h>
#include <uv.h>

#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// errname(err) — returns the error name string for a UV error code
// ---------------------------------------------------------------------------

static napi_value uvErrName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t err = 0;
  napi_get_value_int32(env, argv[0], &err);

  char name[64];
  uv_err_name_r(err, name, sizeof(name));

  napi_value result;
  napi_create_string_utf8(env, name, strlen(name), &result);
  return result;
}

// ---------------------------------------------------------------------------
// getErrorMessage(err) — returns the error message for a UV error code
// ---------------------------------------------------------------------------

static napi_value uvGetErrorMessage(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t err = 0;
  napi_get_value_int32(env, argv[0], &err);

  char message[64];
  uv_strerror_r(err, message, sizeof(message));

  napi_value result;
  napi_create_string_utf8(env, message, strlen(message), &result);
  return result;
}

// ---------------------------------------------------------------------------
// getErrorMap() — returns a Map of UV error code -> [name, message]
// ---------------------------------------------------------------------------

struct UVError {
  int value;
  const char *name;
  const char *message;
};

static const UVError uv_errors_map[] = {
#define V(name, message) {UV_##name, #name, message},
    UV_ERRNO_MAP(V)
#undef V
};

static napi_value uvGetErrorMap(napi_env env, napi_callback_info) {
  // We can't create a Map directly via NAPI, so create an array of
  // [key, value] pairs and use JS Map constructor.
  // Alternative: just build a plain object. But Node returns a Map.
  // Actually, let's just use napi to build a JS object with numeric keys,
  // then the JS side can convert if needed. However, Node returns a real Map.
  //
  // Since NAPI doesn't have Map creation, we'll create a plain object with
  // numeric-string keys, each mapping to a [name, message] array.
  // The main consumer (internal/errors.js) does `uvBinding.getErrorMap()` and
  // iterates it. We'll return an array of [code, [name, message]] entries
  // that the JS side can convert.
  //
  // Actually, let's look at how it's used. internal/errors.js line ~620:
  //   for (const [code, [name]] of uvBinding.getErrorMap()) ...
  // So it expects an iterable of [code, [name, msg]]. A Map works, but
  // so does an array of pairs.

  size_t count = sizeof(uv_errors_map) / sizeof(uv_errors_map[0]);

  // Build array of [code, [name, message]] entries.
  napi_value entries;
  napi_create_array_with_length(env, count, &entries);

  for (size_t i = 0; i < count; ++i) {
    const auto &e = uv_errors_map[i];

    napi_value nameStr, msgStr;
    napi_create_string_utf8(env, e.name, NAPI_AUTO_LENGTH, &nameStr);
    napi_create_string_utf8(env, e.message, NAPI_AUTO_LENGTH, &msgStr);

    napi_value pair;
    napi_create_array_with_length(env, 2, &pair);
    napi_set_element(env, pair, 0, nameStr);
    napi_set_element(env, pair, 1, msgStr);

    napi_value code;
    napi_create_int32(env, e.value, &code);

    napi_value entry;
    napi_create_array_with_length(env, 2, &entry);
    napi_set_element(env, entry, 0, code);
    napi_set_element(env, entry, 1, pair);

    napi_set_element(env, entries, static_cast<uint32_t>(i), entry);
  }

  // Wrap as a Map: new Map(entries)
  napi_value global, mapCtor, mapObj;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Map", &mapCtor);
  napi_value mapArgs[] = {entries};
  napi_new_instance(env, mapCtor, 1, mapArgs, &mapObj);

  return mapObj;
}

// ---------------------------------------------------------------------------
// initUvBinding — exports UV_* error constants and utility functions
// ---------------------------------------------------------------------------

napi_value initUvBinding(napi_env env, napi_value exports) {
  // Export all UV_* error constants with "UV_" prefix.
  size_t count = sizeof(uv_errors_map) / sizeof(uv_errors_map[0]);
  for (size_t i = 0; i < count; ++i) {
    const auto &e = uv_errors_map[i];
    std::string prefixed = std::string("UV_") + e.name;
    napi_value val;
    napi_create_int32(env, e.value, &val);
    napi_set_named_property(env, exports, prefixed.c_str(), val);
  }

  // Export functions.
  napi_value fn;

  napi_create_function(
      env, "errname", NAPI_AUTO_LENGTH, uvErrName, nullptr, &fn);
  napi_set_named_property(env, exports, "errname", fn);

  napi_create_function(
      env, "getErrorMap", NAPI_AUTO_LENGTH, uvGetErrorMap, nullptr, &fn);
  napi_set_named_property(env, exports, "getErrorMap", fn);

  napi_create_function(
      env,
      "getErrorMessage",
      NAPI_AUTO_LENGTH,
      uvGetErrorMessage,
      nullptr,
      &fn);
  napi_set_named_property(env, exports, "getErrorMessage", fn);

  return exports;
}

} // namespace node_compat
} // namespace hermes
