/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_credentials.h>
#include <node_api.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// getTempDir() -> string | undefined
// Checks TMPDIR, TMP, TEMP env vars. Returns undefined if none set.
// ---------------------------------------------------------------------------

static napi_value getTempDir(napi_env env, napi_callback_info /*info*/) {
  const char *dir = nullptr;

  dir = std::getenv("TMPDIR");
  if (!dir || dir[0] == '\0')
    dir = std::getenv("TMP");
  if (!dir || dir[0] == '\0')
    dir = std::getenv("TEMP");

  if (!dir || dir[0] == '\0') {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Strip trailing slash if not root.
  size_t len = std::strlen(dir);
  if (len > 1 && dir[len - 1] == '/') {
    --len;
  }

  napi_value result;
  napi_create_string_utf8(env, dir, len, &result);
  return result;
}

// ---------------------------------------------------------------------------
// safeGetenv(key) -> string
// Simplified: just calls getenv (Node checks secure mode).
// ---------------------------------------------------------------------------

static napi_value safeGetenv(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  char key[256];
  size_t keyLen = 0;
  napi_get_value_string_utf8(env, argv[0], key, sizeof(key), &keyLen);

  const char *val = std::getenv(key);

  napi_value result;
  if (val) {
    napi_create_string_utf8(env, val, NAPI_AUTO_LENGTH, &result);
  } else {
    napi_create_string_utf8(env, "", 0, &result);
  }
  return result;
}

// ---------------------------------------------------------------------------
// getuid() -> number
// ---------------------------------------------------------------------------

#ifndef _WIN32
static napi_value getUid(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_create_uint32(env, getuid(), &result);
  return result;
}

static napi_value getEUid(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_create_uint32(env, geteuid(), &result);
  return result;
}

static napi_value getGid(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_create_uint32(env, getgid(), &result);
  return result;
}

static napi_value getEGid(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_create_uint32(env, getegid(), &result);
  return result;
}

static napi_value getGroups(napi_env env, napi_callback_info /*info*/) {
  int ngroups = getgroups(0, nullptr);
  if (ngroups <= 0) {
    napi_value arr;
    napi_create_array_with_length(env, 0, &arr);
    return arr;
  }

  std::vector<gid_t> groups(static_cast<size_t>(ngroups));
  ngroups = getgroups(ngroups, groups.data());

  napi_value arr;
  napi_create_array_with_length(env, static_cast<size_t>(ngroups), &arr);
  for (int i = 0; i < ngroups; ++i) {
    napi_value val;
    napi_create_uint32(env, groups[static_cast<size_t>(i)], &val);
    napi_set_element(env, arr, static_cast<uint32_t>(i), val);
  }
  return arr;
}
#endif

// ---------------------------------------------------------------------------
// initCredentialsBinding
// ---------------------------------------------------------------------------

napi_value initCredentialsBinding(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
      {"getTempDir",
       nullptr,
       getTempDir,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"safeGetenv",
       nullptr,
       safeGetenv,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
#ifndef _WIN32
      {"getuid",
       nullptr,
       getUid,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"geteuid",
       nullptr,
       getEUid,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getgid",
       nullptr,
       getGid,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getegid",
       nullptr,
       getEGid,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getgroups",
       nullptr,
       getGroups,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
#endif
  };

  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);

#ifndef _WIN32
  // implementsPosixCredentials flag.
  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  napi_set_named_property(env, exports, "implementsPosixCredentials", trueVal);
#endif

  return exports;
}

} // namespace node_compat
} // namespace hermes
