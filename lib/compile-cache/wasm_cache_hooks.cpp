/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/compile-cache/wasm_cache_hooks.h"

#include <cstring>

namespace hermes {
namespace node_compat {

// Defined outside the HERMES_ENABLE_WASM branch: it uses nothing of
// Hermes's, and a build without Wasm still links it.
const NativeWasmUnitEntry *findNativeWasmUnit(
    const NativeWasmUnitEntry *table,
    size_t count,
    const uint8_t *digest) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = std::memcmp(table[mid].digest, digest, kNativeDigestBytes);
    if (c == 0)
      return &table[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return nullptr;
}

} // namespace node_compat
} // namespace hermes

// This target defines HERMES_ENABLE_WASM (lib/compile-cache/CMakeLists.txt)
// to mirror the top-level option, which otherwise only reaches Hermes's own
// add_definitions() scope. Guarding here keeps the adapter's Wasm-specific
// logic out of a build that opted out of WebAssembly entirely, even though
// hermes_set_wasm_cache() itself already degrades to a reported failure in
// that configuration.
#ifdef HERMES_ENABLE_WASM

#include <hermes/node-compat/bundle/bundle_run.h>
#include <hermes/node-compat/bundle/native_digest.h>
#include <hermes/node-compat/bundle/wasm_record.h>

#include <napi/hermes_napi.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <new>
#include <string>

namespace hermes {
namespace node_compat {

namespace {

/// What lookup() hands back for store()/discard(). Carries the identity on
/// EVERY path, hit and miss alike: Hermes discards the token only for an
/// ACCEPTED hit. A rejected hit keeps it, compiles the module, and calls
/// store() with the result.
struct StoreToken {
  WasmCacheContext *ctx = nullptr;

  /// The raw SHA-256 every tier keys on, derived once by the lookup that
  /// created this token. It lives here independently of `entry` because the
  /// recorder and the refusal message need it whether or not there is a disk
  /// cache to have filled `entry` in.
  std::array<uint8_t, kNativeDigestBytes> digest{};

  /// The disk tier's identity: where a store would write. Only meaningful
  /// when ctx->cache exists, and left blank when the container answered
  /// first, since a store after a container hit never reaches the disk.
  CompileCacheEntry entry;

  /// Which tier answered the lookup. Hermes passes no origin to store(), so
  /// a store arriving after a container hit is recognizable only from what
  /// the lookup recorded here -- and that is exactly the case where the
  /// container's own bytecode was refused.
  enum class Origin { kMiss, kContainer, kDisk };
  Origin origin = Origin::kMiss;

  /// --record-wasm only. The module bytes are valid only during lookup, and
  /// a hit's bytecode may be unmapped by its finalizer before accepted runs
  /// (the order is finalizer, accepted, discard), so both are copied here,
  /// at lookup, and committed only once Hermes has taken the module: at
  /// store (a compile succeeded) or accepted (the returned bytecode was
  /// loaded). A bare discard commits nothing. Without a recorder nothing is
  /// copied, and a hit costs what it always did.
  bool recording = false;
  std::string sourceDigest; // sha256Digest(module bytes), raw
  std::string source;
  std::string hitBytecode;
};

void wasmTrace(
    const WasmCacheContext *ctx,
    const char *what,
    const uint8_t *rawDigest) {
  if (!ctx->tracing)
    return;
  // Same prefix and same hex spelling as CompileCache's own tracing, so the
  // container tier's lines and the disk tier's read as one story.
  std::string digestHex = nativeDigestToHex(std::string_view(
      reinterpret_cast<const char *>(rawDigest), kNativeDigestBytes));
  std::fprintf(stderr, "[compile cache] %s %s\n", what, digestHex.c_str());
}

/// The compile cache's digest is hex, because it is a file name; the
/// container's Wasm table and the record file key on the raw bytes. Decoded
/// rather than hashed a second time, so the two spellings cannot describe
/// different things.
std::array<uint8_t, kNativeDigestBytes> rawDigestFromHex(
    const std::string &hex) {
  auto value = [](char c) -> uint8_t {
    return static_cast<uint8_t>(c >= 'a' ? c - 'a' + 10 : c - '0');
  };
  std::array<uint8_t, kNativeDigestBytes> raw{};
  // compileCacheWasmDigest() always returns exactly 2 * kNativeDigestBytes
  // lowercase hex characters -- both are SHA-256 widths. Asserted rather
  // than trusted silently: a width that diverges without this would yield
  // a zero-tailed digest, and every lookup against it would miss forever
  // with nothing printed. The std::min below is the memory-safety guard
  // and stays regardless -- it bounds a release build where this assert is
  // compiled out.
  assert(hex.size() == 2 * kNativeDigestBytes);
  size_t n = std::min(hex.size() / 2, raw.size());
  for (size_t i = 0; i < n; ++i)
    raw[i] =
        static_cast<uint8_t>((value(hex[2 * i]) << 4) | value(hex[2 * i + 1]));
  return raw;
}

SHUnitCreator cacheLookupNative(void *ctx, const uint8_t *wasm, size_t size) {
  auto *c = static_cast<WasmCacheContext *>(ctx);
  std::string digest = sha256Digest(wasm, size);
  const NativeWasmUnitEntry *e = findNativeWasmUnit(
      c->nativeWasmUnits,
      c->nativeWasmUnitCount,
      reinterpret_cast<const uint8_t *>(digest.data()));
  c->nativeAnswered[digest] = e != nullptr;
  // "hit" means the table offered a unit. Hermes may still refuse it, in
  // which case the bytecode lookup that follows says so.
  wasmTrace(
      c,
      e ? "wasm native hit" : "wasm native miss",
      reinterpret_cast<const uint8_t *>(digest.data()));
  return e ? reinterpret_cast<SHUnitCreator>(const_cast<void *>(e->creator))
           : nullptr;
}

/// Called at the top of the bytecode-tier lookup, before anything there can
/// fail: reaching that lookup means the native tier did not serve this
/// module.
void warnNativeFallback(WasmCacheContext *c, const uint8_t *wasm, size_t size) {
  if (!c->warnOnNativeFallback)
    return;
  std::string digest = sha256Digest(wasm, size);
  if (!c->nativeWarned.insert(digest).second)
    return;
  auto it = c->nativeAnswered.find(digest);
  bool offered = it != c->nativeAnswered.end() && it->second;
  std::string hex = nativeDigestToHex(digest).substr(0, 16);
  if (offered)
    std::fprintf(
        stderr,
        "warning: WebAssembly module %s: its native unit is unavailable or "
        "incompatible with this run (e.g. -test262); falling back to "
        "bytecode\n",
        hex.c_str());
  else
    std::fprintf(
        stderr,
        "warning: WebAssembly module %s was not compiled natively (it is not "
        "in the recording); falling back to bytecode\n",
        hex.c_str());
}

/// Commit this token's module to the --record-wasm writer, if there is one:
/// the bytecode Hermes ended up with (\p bytes) and the module bytes copied
/// at lookup, replacing whatever was committed for this digest before.
/// \p traceEvent names where the bytecode came from, and is used for the
/// trace line only.
///
/// Called from exactly two places -- cacheStore (a compile succeeded) and
/// cacheAccepted (Hermes loaded the bytecode a hit returned) -- so nothing
/// is recorded for a module Hermes did not end up with.
///
/// A failed write is reported ONCE and fails the run (see recordingFailed):
/// the compile cache next door swallows every failure because it has a right
/// answer to fall back on, and a recording does not -- a record file that
/// silently did not get written produces a container that silently compiles
/// at every launch.
void commitRecord(
    const StoreToken *token,
    const char *traceEvent,
    const uint8_t *bytes,
    size_t size) {
  if (!token->recording)
    return;
  WasmCacheContext *ctx = token->ctx;

  wasmTrace(ctx, traceEvent, token->digest.data());
  WasmRecordCommit commit{
      token->digest.data(),
      bytes,
      size,
      reinterpret_cast<const uint8_t *>(token->sourceDigest.data()),
      reinterpret_cast<const uint8_t *>(token->source.data()),
      token->source.size()};
  if (ctx->recorder->record(commit))
    return;

  // Once per run, not once per failure: a full disk fails every record, and
  // a flooded stderr buries the one line that matters.
  if (ctx->recordingFailed)
    return;
  ctx->recordingFailed = true;
  std::fprintf(stderr, "Error: --record-wasm: the recording is incomplete\n");
  // The writer's own line follows, naming the temp file and the errno text.
  std::fputs(ctx->recorder->lastError().c_str(), stderr);
}

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
  auto *cacheCtx = static_cast<WasmCacheContext *>(ctx);
  warnNativeFallback(cacheCtx, wasm, wasmSize);
  // nothrow: this whole library is built -fno-exceptions, so a plain new
  // that cannot allocate calls std::terminate -- killing a program over an
  // optional cache, which is exactly what this feature promises never to do.
  // Returning null here is a miss, and Hermes compiles the module normally.
  auto *token = new (std::nothrow) StoreToken();
  *storeToken = token;
  if (token == nullptr)
    return false;
  token->ctx = cacheCtx;

  // Derived once, for every tier: the disk cache's file name, the
  // container's table key and the record file's key are all this digest.
  std::string digest =
      compileCacheWasmDigest(codegenConfig, codegenConfigSize, wasm, wasmSize);
  token->digest = rawDigestFromHex(digest);

  // Copied now because the module bytes are valid only during this call.
  // Committed later, at store or accepted, if Hermes takes the module.
  if (cacheCtx->recorder != nullptr) {
    token->recording = true;
    token->sourceDigest = sha256Digest(wasm, wasmSize);
    token->source.assign(reinterpret_cast<const char *>(wasm), wasmSize);
  }

  // Container tier first. A miss here falls through to exactly the path this
  // run would have taken without a container, so the two tiers cooperate.
  if (cacheCtx->container) {
    const uint8_t *baked = nullptr;
    size_t bakedSize = 0;
    if (bundleWasmLookup(token->digest.data(), &baked, &bakedSize)) {
      wasmTrace(cacheCtx, "wasm container hit", token->digest.data());
      token->origin = StoreToken::Origin::kContainer;
      if (token->recording)
        token->hitBytecode.assign(
            reinterpret_cast<const char *>(baked), bakedSize);
      *hbc = baked;
      *hbcSize = bakedSize;
      // No finalizer: these bytes are part of the container's mapping, which
      // is never unmapped, so there is nothing to release.
      *finalizeCb = nullptr;
      *finalizeHint = nullptr;
      return true;
    }
    wasmTrace(cacheCtx, "wasm container miss", token->digest.data());
  }

  // Disk tier. lookupWasm() traces its own hit and miss.
  if (cacheCtx->cache == nullptr)
    return false;
  if (!cacheCtx->cache->lookupWasm(token->entry, digest, wasm, wasmSize))
    return false;

  token->origin = StoreToken::Origin::kDisk;
  // Copied before the mapping is handed off: Hermes runs its finalizer
  // before it reports acceptance.
  if (token->recording)
    token->hitBytecode.assign(
        reinterpret_cast<const char *>(token->entry.bytecode),
        token->entry.bytecodeSize);

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

  // Reaching store() after a container hit means one thing: Hermes refused
  // the bytecode this container carries and compiled the module instead. A
  // broken artifact is not a slow artifact -- BundleReader::open() already
  // treats a structurally invalid container as fatal, and bundle_run.cpp's
  // fatalBadPayload() is the same rule for a JavaScript module -- so this
  // does not save, does not record, and does not carry on with what Hermes
  // compiled. The trace goes out first, so a trace being read explains the
  // exit instead of stopping mid-story.
  //
  // A refused DISK entry lands here too, with origin kDisk, and falls back
  // silently: that cache is best effort and has a right answer behind it.
  // The container is the artifact.
  if (token->origin == StoreToken::Origin::kContainer) {
    wasmTrace(token->ctx, "wasm container refused", token->digest.data());
    bundleFatalWasmRefused(token->digest.data());
  }

  wasmTrace(token->ctx, "wasm store", token->digest.data());
  // enabled(), not merely non-null, matching the install rule below: a
  // cache that failed to claim its directory answers save() as a no-op
  // (CompileCache::save() checks enabled_ itself), but saveWasm() also
  // runs the once-per-process eviction sweep unconditionally, and that
  // sweep must not run against a root the cache never claimed.
  if (token->ctx->cache != nullptr && token->ctx->cache->enabled())
    token->ctx->cache->saveWasm(token->entry, hbc, hbcSize);
  // Commit what was compiled. A rejected hit reaches here too and commits
  // nothing of its own (accepted never runs for it), so the recording ends
  // up holding the fresh bytecode and never the bad entry.
  commitRecord(token, "wasm record compile", hbc, hbcSize);
  delete token;
}

/// Hermes loaded the bytecode a hit returned. The only point at which a hit
/// is known good, so it is where a hit is recorded. Runs after the hit's
/// finalizer, hence the copy made at lookup. The token is not freed here:
/// discard follows and does that.
void cacheAccepted(void *ctx, void *storeToken) {
  (void)ctx;
  auto *token = static_cast<StoreToken *>(storeToken);
  if (token == nullptr || !token->recording)
    return;
  commitRecord(
      token,
      token->origin == StoreToken::Origin::kContainer ? "wasm record container"
                                                      : "wasm record disk",
      reinterpret_cast<const uint8_t *>(token->hitBytecode.data()),
      token->hitBytecode.size());
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

void installWasmCacheHooks(
    napi_env env,
    WasmCacheContext *ctx,
    bool bundleMode) {
  ctx->container = bundleMode;
  // enabled(), not merely non-null: a cache that failed to claim its
  // directory answers every lookup with a miss, and installing for it would
  // buy the serialize-and-store cost with nothing able to use it.
  const bool cacheCanAnswer = ctx->cache != nullptr && ctx->cache->enabled();
  const bool nativeTier =
      ctx->nativeWasmUnitCount != 0 || ctx->warnOnNativeFallback;
  if (!cacheCanAnswer && ctx->recorder == nullptr && !bundleMode && !nativeTier)
    return;

  hermes_wasm_cache_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.ctx = ctx;
  callbacks.lookup = &cacheLookup;
  callbacks.store = &cacheStore;
  callbacks.accepted = &cacheAccepted;
  callbacks.discard = &cacheDiscard;
  // Only an artifact with a table or a warning policy pays the per-compile
  // SHA-256 the native lookup costs.
  callbacks.lookup_native = nativeTier ? &cacheLookupNative : nullptr;
  // Best effort: a non-napi_ok return (e.g. Hermes built without Wasm
  // support) means the hooks are simply never consulted. Nothing here may
  // surface a failure to the running program.
  (void)hermes_set_wasm_cache(env, &callbacks);
}

} // namespace node_compat
} // namespace hermes

#else // !HERMES_ENABLE_WASM

namespace hermes {
namespace node_compat {

void installWasmCacheHooks(napi_env, WasmCacheContext *, bool) {
  // Nothing to install: this build has no Wasm frontend to cache for, so no
  // callback exists to read the context and nothing here needs to fill it in.
}

} // namespace node_compat
} // namespace hermes

#endif // HERMES_ENABLE_WASM
