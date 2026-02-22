/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * A minimal test NAPI addon for testing process.dlopen / require('.node').
 * Exports: hello() -> "world", add(a,b) -> a+b
 */

#include <js_native_api.h>
#include <node_api.h>

static napi_value hello(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_create_string_utf8(env, "world", 5, &result);
  return result;
}

static napi_value add(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

  double a, b;
  napi_get_value_double(env, argv[0], &a);
  napi_get_value_double(env, argv[1], &b);

  napi_value result;
  napi_create_double(env, a + b, &result);
  return result;
}

NAPI_MODULE_INIT() {
  napi_property_descriptor descs[] = {
      {"hello", NULL, hello, NULL, NULL, NULL, napi_default, NULL},
      {"add", NULL, add, NULL, NULL, NULL, napi_default, NULL},
  };
  napi_define_properties(env, exports, 2, descs);
  return exports;
}
