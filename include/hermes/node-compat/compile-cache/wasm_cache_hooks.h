/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Install \p cache as the Wasm bytecode cache for \p env, so that Hermes
/// consults it before compiling a module and persists the result afterwards.
///
/// A null \p cache installs nothing, which is how --no-compile-cache and
/// --inspect end up uncached: both already make createCompileCache return
/// null. Installing is also a no-op when Hermes itself was built without
/// WebAssembly (hermes_set_wasm_cache reports failure in that configuration,
/// which this function swallows -- everything about this cache is best
/// effort).
///
/// \p cache must outlive \p env.
void installWasmCacheHooks(napi_env env, CompileCache *cache);

} // namespace node_compat
} // namespace hermes
