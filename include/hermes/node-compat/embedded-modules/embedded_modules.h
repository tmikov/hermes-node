/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_EMBEDDED_MODULES_H
#define HERMES_NODE_COMPAT_EMBEDDED_MODULES_H

#include <node_api_types.h>

#include <cstddef>
#include <cstdint>

namespace hermes {
namespace node_compat {

/// Describes a single embedded JS module compiled to Hermes bytecode.
struct EmbeddedModule {
  const char *id;
  const uint8_t *data;
  size_t size;
  bool isBootstrap;
};

/// Look up an embedded module by ID. Returns nullptr if not found.
const EmbeddedModule *findEmbeddedModule(const char *id);

/// Run an embedded module's bytecode via hermes_run_bytecode().
/// \p id is the module ID (e.g. "events", "internal/errors", "primordials").
/// \p result receives the evaluation result.
/// Returns napi_ok on success, napi_pending_exception on JS error,
/// or napi_generic_failure if the module is not found.
napi_status runEmbeddedModule(napi_env env, const char *id, napi_value *result);

/// NAPI callback for JS: loadBytecodeModule(id) -> function | undefined.
/// Looks up the module by ID in the embedded registry, runs the bytecode,
/// and returns the resulting CJS wrapper function.
/// Returns undefined if the module is not found (user script fallback).
napi_value loadBytecodeModuleCallback(napi_env env, napi_callback_info info);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_EMBEDDED_MODULES_H
