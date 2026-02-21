/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/embedded-modules/embedded_modules.h>

#include "napi/hermes_napi.h"

#include <cstdio>

namespace hermes {
namespace node_compat {

napi_status
runEmbeddedModule(napi_env env, const char *id, napi_value *result) {
  const EmbeddedModule *mod = findEmbeddedModule(id);
  if (!mod) {
    return napi_generic_failure;
  }

  // Static data embedded in the binary -- no finalize callback needed.
  // Persistent: internal modules live for the lifetime of the process.
  hermes_bytecode_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.persistent = true;
  return hermes_run_bytecode(
      env, mod->data, mod->size, nullptr, nullptr, id, &flags, result);
}

napi_value loadBytecodeModuleCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_status status =
      napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (status != napi_ok || argc < 1) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Get the module ID string.
  char idBuf[256];
  size_t idLen = 0;
  status =
      napi_get_value_string_utf8(env, argv[0], idBuf, sizeof(idBuf), &idLen);
  if (status != napi_ok) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Look up the module.
  const EmbeddedModule *mod = findEmbeddedModule(idBuf);
  if (!mod) {
    // Not an embedded module -- return undefined so JS falls back to disk.
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Run the bytecode. For CJS modules this returns the wrapper function.
  // For bootstrap modules it returns the evaluation result.
  // Persistent: internal modules live for the lifetime of the process.
  hermes_bytecode_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.persistent = true;
  napi_value result;
  status = hermes_run_bytecode(
      env, mod->data, mod->size, nullptr, nullptr, idBuf, &flags, &result);
  if (status != napi_ok) {
    // Exception is pending; return nullptr to propagate it.
    return nullptr;
  }

  return result;
}

} // namespace node_compat
} // namespace hermes
