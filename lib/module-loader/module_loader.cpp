/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/embedded-modules/embedded_modules.h>
#include <hermes/node-compat/module-loader/module_loader.h>

#include "napi/hermes_napi.h"

#include <js_native_api.h>
#include <node_api.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

ModuleLoader::ModuleLoader() = default;

ModuleLoader::~ModuleLoader() = default;

/// Native callback for readFileSync(path) -> string.
/// Used by the JS loader to read user script source files from disk.
static napi_value readFileSyncCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_status status =
      napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (status != napi_ok)
    return nullptr;

  if (argc < 1) {
    napi_throw_type_error(
        env, nullptr, "readFileSync requires a path argument");
    return nullptr;
  }

  // Get the path string.
  char pathBuf[4096];
  size_t pathLen = 0;
  status = napi_get_value_string_utf8(
      env, argv[0], pathBuf, sizeof(pathBuf), &pathLen);
  if (status != napi_ok) {
    napi_throw_type_error(env, nullptr, "readFileSync: path must be a string");
    return nullptr;
  }

  FILE *f = std::fopen(pathBuf, "rb");
  if (!f) {
    std::string msg = "Cannot read file: ";
    msg += pathBuf;
    msg += " (";
    msg += std::strerror(errno);
    msg += ")";
    napi_throw_error(env, "ERR_MODULE_NOT_FOUND", msg.c_str());
    return nullptr;
  }

  // Get file size.
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    napi_throw_error(env, nullptr, "readFileSync: fseek failed");
    return nullptr;
  }
  long size = std::ftell(f);
  if (size < 0) {
    std::fclose(f);
    napi_throw_error(env, nullptr, "readFileSync: ftell failed");
    return nullptr;
  }
  std::rewind(f);

  std::string content(static_cast<size_t>(size), '\0');
  size_t nread = std::fread(&content[0], 1, static_cast<size_t>(size), f);
  std::fclose(f);

  if (nread != static_cast<size_t>(size)) {
    napi_throw_error(env, nullptr, "readFileSync: short read");
    return nullptr;
  }

  napi_value result;
  status =
      napi_create_string_utf8(env, content.c_str(), content.size(), &result);
  if (status != napi_ok)
    return nullptr;

  return result;
}

/// Native callback for __evalTS(source, sourceUrl) -> value.
/// Compiles and runs a source string with TypeScript parsing enabled.
/// Used by the JS loader to execute .ts module source.
static napi_value evalTSCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_status status =
      napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (status != napi_ok)
    return nullptr;

  if (argc < 2) {
    napi_throw_type_error(
        env, nullptr, "__evalTS requires (source, sourceUrl) arguments");
    return nullptr;
  }

  // Get the source string length.
  size_t sourceLen = 0;
  status = napi_get_value_string_utf8(env, argv[0], nullptr, 0, &sourceLen);
  if (status != napi_ok) {
    napi_throw_type_error(env, nullptr, "__evalTS: source must be a string");
    return nullptr;
  }

  // Allocate buffer for source + null terminator.
  // hermes_run_script uses zero-copy when the last byte is '\0'.
  std::string source(sourceLen, '\0');
  size_t copied = 0;
  status = napi_get_value_string_utf8(
      env, argv[0], &source[0], sourceLen + 1, &copied);
  if (status != napi_ok)
    return nullptr;

  // Get the sourceUrl string.
  char urlBuf[4096];
  size_t urlLen = 0;
  status =
      napi_get_value_string_utf8(env, argv[1], urlBuf, sizeof(urlBuf), &urlLen);
  if (status != napi_ok) {
    napi_throw_type_error(env, nullptr, "__evalTS: sourceUrl must be a string");
    return nullptr;
  }

  // Compile and run with TypeScript support enabled.
  // Persistent: CJS modules live for the lifetime of the process.
  hermes_run_script_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.enable_ts = true;
  flags.persistent = true;

  napi_value result;
  status = hermes_run_script(
      env,
      reinterpret_cast<const uint8_t *>(source.c_str()),
      sourceLen + 1, // includes trailing '\0' for zero-copy
      nullptr,
      nullptr,
      urlBuf,
      &flags,
      &result);
  if (status != napi_ok)
    return nullptr;

  return result;
}

napi_status ModuleLoader::init(
    napi_env env,
    napi_value primordials,
    napi_value internalBindingFn) {
  // Execute the loader from embedded bytecode.
  napi_value setupFn;
  napi_status status = runEmbeddedModule(env, "loader", &setupFn);
  if (status != napi_ok) {
    if (status == napi_generic_failure) {
      napi_throw_error(
          env, nullptr, "loader module not found in embedded bytecode");
      return napi_pending_exception;
    }
    return status;
  }

  // Verify it's a function.
  napi_valuetype setupType;
  status = napi_typeof(env, setupFn, &setupType);
  if (status != napi_ok)
    return status;
  if (setupType != napi_function) {
    napi_throw_error(env, nullptr, "loader.js must evaluate to a function");
    return napi_pending_exception;
  }

  // Create the loadBytecodeModule native function.
  napi_value loadBytecodeFn;
  status = napi_create_function(
      env,
      "loadBytecodeModule",
      NAPI_AUTO_LENGTH,
      loadBytecodeModuleCallback,
      nullptr,
      &loadBytecodeFn);
  if (status != napi_ok)
    return status;

  // Create the readFileSync native function (for user scripts).
  napi_value readFileSyncFn;
  status = napi_create_function(
      env,
      "readFileSync",
      NAPI_AUTO_LENGTH,
      readFileSyncCallback,
      nullptr,
      &readFileSyncFn);
  if (status != napi_ok)
    return status;

  // Create the __evalTS native function (for TypeScript modules).
  napi_value evalTSFn;
  status = napi_create_function(
      env, "__evalTS", NAPI_AUTO_LENGTH, evalTSCallback, nullptr, &evalTSFn);
  if (status != napi_ok)
    return status;

  // Call setup(loadBytecodeModule, readFileSync, evalTS, primordials,
  // internalBinding). Returns the require function.
  napi_value global;
  status = napi_get_global(env, &global);
  if (status != napi_ok)
    return status;

  napi_value args[] = {
      loadBytecodeFn, readFileSyncFn, evalTSFn, primordials, internalBindingFn};

  napi_value requireFn;
  status = napi_call_function(env, global, setupFn, 5, args, &requireFn);
  if (status != napi_ok)
    return status;

  // Store the require function as a persistent reference.
  status = napi_create_reference(env, requireFn, 1, &requireFnRef_);
  return status;
}

napi_status
ModuleLoader::require(napi_env env, const char *name, napi_value *result) {
  assert(requireFnRef_ && "init() must be called before require()");
  assert(name && "name must not be null");
  assert(result && "result must not be null");

  napi_value requireFn;
  napi_status status = napi_get_reference_value(env, requireFnRef_, &requireFn);
  if (status != napi_ok)
    return status;

  napi_value nameStr;
  status = napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &nameStr);
  if (status != napi_ok)
    return status;

  napi_value global;
  status = napi_get_global(env, &global);
  if (status != napi_ok)
    return status;

  status = napi_call_function(env, global, requireFn, 1, &nameStr, result);
  return status;
}

void ModuleLoader::detach(napi_env env) {
  if (requireFnRef_) {
    napi_delete_reference(env, requireFnRef_);
    requireFnRef_ = nullptr;
  }
}

} // namespace node_compat
} // namespace hermes
