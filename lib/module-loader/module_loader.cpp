/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/module-loader/module_loader.h>

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

void ModuleLoader::setLibJsPath(const std::string &path) {
  libJsPath_ = path;
  // Ensure trailing slash.
  if (!libJsPath_.empty() && libJsPath_.back() != '/')
    libJsPath_ += '/';
}

void ModuleLoader::setLibJsNodePath(const std::string &path) {
  libJsNodePath_ = path;
  // Ensure trailing slash.
  if (!libJsNodePath_.empty() && libJsNodePath_.back() != '/')
    libJsNodePath_ += '/';
}

/// Read an entire file into a string. Returns true on success.
static bool readFile(const std::string &path, std::string &out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;

  // Get file size.
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  long size = std::ftell(f);
  if (size < 0) {
    std::fclose(f);
    return false;
  }
  std::rewind(f);

  out.resize(static_cast<size_t>(size));
  size_t read = std::fread(&out[0], 1, static_cast<size_t>(size), f);
  std::fclose(f);

  if (read != static_cast<size_t>(size)) {
    out.clear();
    return false;
  }
  return true;
}

/// Native callback for readFileSync(path) -> string.
/// Used by the JS loader to read module source files from disk.
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

  std::string content;
  if (!readFile(pathBuf, content)) {
    std::string msg = "Cannot read file: ";
    msg += pathBuf;
    msg += " (";
    msg += std::strerror(errno);
    msg += ")";
    napi_throw_error(env, "ERR_MODULE_NOT_FOUND", msg.c_str());
    return nullptr;
  }

  napi_value result;
  status =
      napi_create_string_utf8(env, content.c_str(), content.size(), &result);
  if (status != napi_ok)
    return nullptr;

  return result;
}

napi_status ModuleLoader::init(
    napi_env env,
    napi_value primordials,
    napi_value internalBindingFn) {
  assert(!libJsPath_.empty() && "libJsPath must be set before init");
  assert(!libJsNodePath_.empty() && "libJsNodePath must be set before init");

  // Read the loader JS source.
  std::string loaderPath = libJsPath_ + "loader.js";
  std::string loaderSrc;
  if (!readFile(loaderPath, loaderSrc)) {
    std::string msg = "Cannot read loader: ";
    msg += loaderPath;
    napi_throw_error(env, "ERR_MODULE_NOT_FOUND", msg.c_str());
    return napi_pending_exception;
  }

  // Wrap the loader in an IIFE that returns the setup function.
  // The loader.js file should export a setup function by assigning to
  // the last expression value (IIFE returning setup).
  // We add sourceURL for better error messages.
  std::string wrappedSrc = loaderSrc;
  wrappedSrc += "\n//# sourceURL=";
  wrappedSrc += loaderPath;
  wrappedSrc += "\n";

  // Evaluate the loader to get the setup function.
  napi_value scriptStr;
  napi_status status = napi_create_string_utf8(
      env, wrappedSrc.c_str(), wrappedSrc.size(), &scriptStr);
  if (status != napi_ok)
    return status;

  napi_value setupFn;
  status = napi_run_script(env, scriptStr, &setupFn);
  if (status != napi_ok)
    return status;

  // Verify it's a function.
  napi_valuetype setupType;
  status = napi_typeof(env, setupFn, &setupType);
  if (status != napi_ok)
    return status;
  if (setupType != napi_function) {
    napi_throw_error(env, nullptr, "loader.js must evaluate to a function");
    return napi_pending_exception;
  }

  // Create the readFileSync native function.
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

  // Create path strings for the JS loader.
  napi_value libJsPathVal;
  status = napi_create_string_utf8(
      env, libJsPath_.c_str(), libJsPath_.size(), &libJsPathVal);
  if (status != napi_ok)
    return status;

  napi_value libJsNodePathVal;
  status = napi_create_string_utf8(
      env, libJsNodePath_.c_str(), libJsNodePath_.size(), &libJsNodePathVal);
  if (status != napi_ok)
    return status;

  // Call setup(readFileSync, libJsPath, libJsNodePath, primordials,
  //            internalBinding)
  // Returns the require function.
  napi_value global;
  status = napi_get_global(env, &global);
  if (status != napi_ok)
    return status;

  napi_value args[] = {
      readFileSyncFn,
      libJsPathVal,
      libJsNodePathVal,
      primordials,
      internalBindingFn};

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
