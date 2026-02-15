/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_MODULE_LOADER_H
#define HERMES_NODE_COMPAT_MODULE_LOADER_H

#include <node_api_types.h>

#include <string>

namespace hermes {
namespace node_compat {

/// Internal module loader for Node.js compatibility.
///
/// Loads Node's lib/*.js files from disk and provides them to each other
/// via a CJS-style require(). Each module is wrapped in the standard Node
/// module wrapper:
///
///   (function(exports, require, module, __filename, __dirname) {
///     // ... module source ...
///   });
///
/// The wrapper receives primordials and internalBinding via closure from
/// the loader setup.
///
/// Usage:
///   ModuleLoader loader;
///   loader.setLibJsPath("/path/to/libjs");
///   loader.setLibJsNodePath("/path/to/libjs-node");
///   loader.init(env, primordials, internalBindingFn);
///   // Now you can require modules:
///   napi_value exports;
///   loader.require(env, "path", &exports);
///
/// The loader handles:
///   - Module resolution: 'internal/foo' -> libjs-node/internal/foo.js
///   - Module caching: each module loaded only once
///   - Circular dependencies: module.exports set before execution
///   - Shim overrides: libjs/shims/internal/foo.js overrides
///     libjs-node/internal/foo.js
class ModuleLoader {
 public:
  ModuleLoader();
  ~ModuleLoader();

  ModuleLoader(const ModuleLoader &) = delete;
  ModuleLoader &operator=(const ModuleLoader &) = delete;

  /// Set the path to libjs/ directory (our JS files: loader, shims).
  void setLibJsPath(const std::string &path);

  /// Set the path to libjs-node/ directory (vendored Node.js JS files).
  void setLibJsNodePath(const std::string &path);

  /// Initialize the loader. Must be called after setting paths and after
  /// the napi_env is created.
  ///
  /// \param env The NAPI environment.
  /// \param primordials The primordials object (from primordials.js).
  /// \param internalBindingFn The internalBinding JS function.
  /// \return napi_ok on success.
  napi_status init(
      napi_env env,
      napi_value primordials,
      napi_value internalBindingFn);

  /// Require a module by name. Returns the module's exports object.
  /// The name follows Node's internal convention:
  ///   - "path" -> libjs-node/path.js
  ///   - "internal/errors" -> libjs-node/internal/errors.js
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
  std::string libJsPath_;
  std::string libJsNodePath_;

  /// Reference to the JS-side require function created during init().
  napi_ref requireFnRef_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_MODULE_LOADER_H
