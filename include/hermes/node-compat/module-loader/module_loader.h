/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_MODULE_LOADER_H
#define HERMES_NODE_COMPAT_MODULE_LOADER_H

#include <node_api_types.h>

namespace hermes {
namespace node_compat {

/// Internal module loader for Node.js compatibility.
///
/// Loads internal modules from pre-compiled bytecode embedded in the binary.
/// User scripts are still loaded from disk via readFileSync.
///
/// Usage:
///   ModuleLoader loader;
///   loader.init(env, primordials, internalBindingFn);
///   // Now you can require modules:
///   napi_value exports;
///   loader.require(env, "path", &exports);
///
/// The loader handles:
///   - Module resolution: internal modules from embedded bytecode
///   - Module caching: each module loaded only once
///   - Circular dependencies: module.exports set before execution
///   - User scripts: loaded from disk via readFileSync fallback
class ModuleLoader {
 public:
  ModuleLoader();
  ~ModuleLoader();

  ModuleLoader(const ModuleLoader &) = delete;
  ModuleLoader &operator=(const ModuleLoader &) = delete;

  /// Initialize the loader. Must be called after the napi_env is created.
  ///
  /// \param env The NAPI environment.
  /// \param primordials The primordials object (from primordials.js).
  /// \param internalBindingFn The internalBinding JS function.
  /// \return napi_ok on success.
  napi_status
  init(napi_env env, napi_value primordials, napi_value internalBindingFn);

  /// Require a module by name. Returns the module's exports object.
  /// The name follows Node's internal convention:
  ///   - "path" -> embedded bytecode for path module
  ///   - "internal/errors" -> embedded bytecode for internal/errors module
  ///
  /// \param env The NAPI environment.
  /// \param name Module name (e.g. "path", "internal/errors").
  /// \param result Receives the module's exports.
  /// \return napi_ok on success, napi_pending_exception on failure.
  napi_status require(napi_env env, const char *name, napi_value *result);

  /// Detach from the napi_env, releasing all JS references.
  /// Must be called before destroying the env.
  void detach(napi_env env);

 private:
  /// Reference to the JS-side require function created during init().
  napi_ref requireFnRef_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_MODULE_LOADER_H
