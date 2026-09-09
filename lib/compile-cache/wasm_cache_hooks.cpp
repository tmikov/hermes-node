/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/compile-cache/wasm_cache_hooks.h"

// This target defines HERMES_ENABLE_WASM (lib/compile-cache/CMakeLists.txt)
// to mirror the top-level option, which otherwise only reaches Hermes's own
// add_definitions() scope. Guarding here keeps the adapter's Wasm-specific
// logic out of a build that opted out of WebAssembly entirely, even though
// hermes_set_wasm_cache() itself already degrades to a reported failure in
// that configuration.
#ifdef HERMES_ENABLE_WASM

#include <napi/hermes_napi.h>

#include <new>

namespace hermes {
namespace node_compat {

namespace {

/// What lookup() hands back for store()/discard(). Carries the identity
/// lookup already derived, so store never recomputes the digest -- two
/// derivations from the same inputs can drift, and the failure would be an
/// entry the next lookup never finds.
struct StoreToken {
  CompileCache *cache;
  CompileCacheEntry entry;
};

bool cacheLookup(
    void *ctx,
    const uint8_t *wasm,
    size_t wasmSize,
    const uint8_t *codegenConfig,
    size_t codegenConfigSize,
    const uint8_t **hbc,
    size_t *hbcSize,
    void (**finalizeCb)(const uint8_t *, size_t, void *),
    void **finalizeHint,
    void **storeToken) {
  auto *cache = static_cast<CompileCache *>(ctx);
  // nothrow: this whole library is built -fno-exceptions, so a plain new
  // that cannot allocate calls std::terminate -- killing a program over an
  // optional cache, which is exactly what this feature promises never to do.
  // Returning null here is a miss, and Hermes compiles the module normally.
  auto *token = new (std::nothrow) StoreToken{cache, CompileCacheEntry{}};
  *storeToken = token;
  if (token == nullptr)
    return false;

  if (!cache->lookupWasm(
          token->entry, wasm, wasmSize, codegenConfig, codegenConfigSize))
    return false;

  // Hand the mapping to Hermes along with the finalizer that releases it.
  // Unlike hermes_run_bytecode, which transfers the mapping to the bytecode
  // provider and frees it when that dies, the Wasm hit path copies these
  // bytes and calls the finalizer straight afterwards -- so this mapping is
  // released almost immediately, and nothing downstream still points into
  // it. Do not add anything here that assumes the mapping outlives the
  // lookup.
  *hbc = token->entry.bytecode;
  *hbcSize = token->entry.bytecodeSize;
  *finalizeCb = &CacheMapping::finalizer;
  *finalizeHint = token->entry.mapping;
  token->entry.mapping = nullptr; // ownership transferred
  return true;
}

void cacheStore(
    void *ctx,
    void *storeToken,
    const uint8_t *hbc,
    size_t hbcSize) {
  (void)ctx;
  auto *token = static_cast<StoreToken *>(storeToken);
  // Null is not a protocol violation: a lookup that could not allocate its
  // token hands back null, and Hermes still completes it.
  if (token == nullptr)
    return;
  token->cache->saveWasm(token->entry, hbc, hbcSize);
  delete token;
}

void cacheDiscard(void *ctx, void *storeToken) {
  (void)ctx;
  auto *token = static_cast<StoreToken *>(storeToken);
  if (token == nullptr)
    return;
  if (token->entry.mapping)
    token->entry.mapping->destroy();
  delete token;
}

} // namespace

void installWasmCacheHooks(napi_env env, CompileCache *cache) {
  if (cache == nullptr || !cache->enabled())
    return;

  hermes_wasm_cache_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.ctx = cache;
  callbacks.lookup = &cacheLookup;
  callbacks.store = &cacheStore;
  callbacks.discard = &cacheDiscard;
  // Best effort: a non-napi_ok return (e.g. Hermes built without Wasm
  // support) means the cache is simply never consulted. Nothing here may
  // surface a failure to the running program.
  (void)hermes_set_wasm_cache(env, &callbacks);
}

} // namespace node_compat
} // namespace hermes

#else // !HERMES_ENABLE_WASM

namespace hermes {
namespace node_compat {

void installWasmCacheHooks(napi_env, CompileCache *) {
  // Nothing to install: this build has no Wasm frontend to cache for.
}

} // namespace node_compat
} // namespace hermes

#endif // HERMES_ENABLE_WASM
