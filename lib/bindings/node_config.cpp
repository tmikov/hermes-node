/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_config.h>
#include <node_api.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

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

static void
setBoolProp(napi_env env, napi_value obj, const char *name, bool value) {
  napi_value val;
  napi_get_boolean(env, value, &val);
  napi_set_named_property(env, obj, name, val);
}

static napi_value getDefaultLocale(napi_env env, napi_callback_info /*info*/) {
  // Return a reasonable default locale. Check LC_ALL, LC_MESSAGES, LANG.
  const char *locale = nullptr;
  locale = getenv("LC_ALL");
  if (!locale || !locale[0])
    locale = getenv("LC_MESSAGES");
  if (!locale || !locale[0])
    locale = getenv("LANG");
  if (!locale || !locale[0])
    locale = "en-US";

  // Strip encoding suffix (e.g., "en_US.UTF-8" -> "en_US")
  // and convert underscore to hyphen for BCP 47 format.
  char buf[64];
  size_t len = 0;
  for (size_t i = 0; locale[i] && locale[i] != '.' && len < sizeof(buf) - 1;
       ++i) {
    buf[len++] = (locale[i] == '_') ? '-' : locale[i];
  }
  buf[len] = '\0';

  napi_value result;
  napi_create_string_utf8(env, buf, len, &result);
  return result;
}

napi_value initConfigBinding(napi_env env, napi_value exports) {
  // Boolean feature flags — most are false for Hermes build.
  setBoolProp(env, exports, "hasOpenSSL", false);
  setBoolProp(env, exports, "openSSLIsBoringSSL", false);
  setBoolProp(env, exports, "fipsMode", false);
  setBoolProp(env, exports, "hasIntl", false);
  setBoolProp(env, exports, "hasSmallICU", false);
  setBoolProp(env, exports, "hasTracing", false);
  setBoolProp(env, exports, "hasNodeOptions", true);
  setBoolProp(env, exports, "hasInspector", false);
  setBoolProp(env, exports, "noBrowserGlobals", false);

#ifdef NDEBUG
  setBoolProp(env, exports, "isDebugBuild", false);
#else
  setBoolProp(env, exports, "isDebugBuild", true);
#endif

  // Pointer size in bits.
  napi_value bitsVal;
  NAPI_CALL(napi_create_int32(
      env, static_cast<int32_t>(8 * sizeof(intptr_t)), &bitsVal));
  NAPI_CALL(napi_set_named_property(env, exports, "bits", bitsVal));

  // getDefaultLocale() function.
  napi_value fn;
  NAPI_CALL(napi_create_function(
      env,
      "getDefaultLocale",
      NAPI_AUTO_LENGTH,
      getDefaultLocale,
      nullptr,
      &fn));
  NAPI_CALL(napi_set_named_property(env, exports, "getDefaultLocale", fn));

  return exports;
}

#undef NAPI_CALL

} // namespace node_compat
} // namespace hermes
