/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_EMBEDDED_MODULES_H
#define HERMES_NODE_COMPAT_EMBEDDED_MODULES_H

#include <node_api_types.h>

#include <napi/hermes_napi.h>

#include <cstddef>
#include <cstdint>

namespace hermes {
namespace node_compat {

/// Describes a single embedded JS module. Exactly one of \c data and
/// \c creator is set, and which one says how the module was compiled: a
/// bytecode registry fills \c data / \c size, a native one fills \c creator.
/// Both registries define findEmbeddedModule(); a build-native link resolves
/// it from the native archive, which is why this one struct has to describe
/// both (see the design doc, "Two registries, chosen by the linker").
struct EmbeddedModule {
  const char *id;
  const uint8_t *data;
  size_t size;
  SHUnitCreator creator;
  bool isBootstrap;
};

/// Look up an embedded module by ID. Returns nullptr if not found.
const EmbeddedModule *findEmbeddedModule(const char *id);

/// Evaluate an embedded module, by whichever route it was compiled:
/// hermes_init_sh_unit() for a native unit, hermes_run_bytecode() for a
/// bytecode one. Which registry linked decides, not the caller.
/// \p id is the module ID (e.g. "events", "internal/errors", "primordials").
/// \p result receives the evaluation result.
/// Returns napi_ok on success, napi_pending_exception on JS error,
/// or napi_generic_failure if the module is not found.
napi_status runEmbeddedModule(napi_env env, const char *id, napi_value *result);

/// NAPI callback for JS: loadBytecodeModule(id) -> function | undefined.
/// Looks up the module by ID in the embedded registry, evaluates it through
/// runEmbeddedModule() above -- native unit or bytecode, as linked, despite
/// this callback's JS-side name -- and returns the resulting CJS wrapper
/// function.
/// Returns undefined if the module is not found (user script fallback).
napi_value loadBytecodeModuleCallback(napi_env env, napi_callback_info info);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_EMBEDDED_MODULES_H
