/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDING_REGISTRY_H
#define HERMES_NODE_COMPAT_BINDING_REGISTRY_H

#include <node_api_types.h>

#include <string>
#include <unordered_map>

namespace hermes {
namespace node_compat {

/// Registry that maps binding names to NAPI module init functions, providing
/// lazy initialization and caching of binding objects.
///
/// Usage:
///   BindingRegistry registry;
///   registry.registerBinding("constants", InitConstants);
///   registry.registerBinding("fs", InitFs);
///
///   // Later, after napi_env is available:
///   registry.attach(env);
///
///   // Create the internalBinding JS function:
///   napi_value fn = registry.createInternalBindingFunction(env);
///   // Set it as a global or pass to module wrappers.
///
///   // Before destroying the env:
///   registry.detach(env);
///
/// The `internalBinding("name")` JS function returned by
/// createInternalBindingFunction will:
///   1. Look up the name in the registry.
///   2. On first access, call the init function to create the binding object.
///   3. Cache the result (via napi_ref) so subsequent calls return the same
///      object.
///   4. Throw an error if the name is not registered.
class BindingRegistry {
 public:
  BindingRegistry();
  ~BindingRegistry();

  BindingRegistry(const BindingRegistry &) = delete;
  BindingRegistry &operator=(const BindingRegistry &) = delete;

  /// Register a binding init function under the given name.
  /// Must be called before attach(). The init function follows the
  /// napi_addon_register_func signature: it receives (env, exports) and
  /// returns the populated exports object.
  void registerBinding(
      const std::string &name,
      napi_addon_register_func initFunc);

  /// Attach to a napi_env. Must be called after the env is created and
  /// before createInternalBindingFunction() or getBinding().
  void attach(napi_env env);

  /// Detach from the napi_env, releasing all cached binding references.
  /// Must be called before destroying the env.
  void detach(napi_env env);

  /// Get a cached binding object by name, initializing it if needed.
  /// Returns napi_ok on success, with *result set to the binding object.
  /// Returns napi_pending_exception if the init function throws.
  /// Throws a JS error (and returns napi_pending_exception) if the name
  /// is not registered.
  napi_status getBinding(napi_env env, const char *name, napi_value *result);

  /// Create the `internalBinding` JS function that can be called from JS.
  /// The returned function takes a single string argument (the binding name)
  /// and returns the corresponding binding object.
  napi_status createInternalBindingFunction(napi_env env, napi_value *result);

 private:
  struct Entry {
    napi_addon_register_func initFunc;
    napi_ref cachedRef = nullptr;
  };

  std::unordered_map<std::string, Entry> bindings_;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDING_REGISTRY_H
