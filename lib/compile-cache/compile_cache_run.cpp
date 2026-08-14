/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache_run.h>

#include <napi/hermes_napi.h>
#include <napi/hermes_napi_compile.h>

#include <cassert>
#include <cstdlib>
#include <optional>
#include <string>

namespace hermes {
namespace node_compat {

namespace {

struct KindFlags {
  bool enableTS;
  bool persistent;
};

/// Compile and run flags implied by the calling entry point.
///
/// enable_ts MUST be a function of the kind rather than an independent
/// argument. It is a compile flag: it changes the bytecode produced from
/// identical source text. If a call site could pass kLoaderWrapped with
/// enable_ts true, the same file would hash to the same key under both
/// settings, and a later non-TypeScript lookup could be served
/// TypeScript-compiled bytecode. The kinds exist to keep differently
/// compiled artifacts apart.
///
/// persistent is a RuntimeModuleFlags setting that does not affect the
/// compiled output and so has no bearing on cache identity. It is derived
/// here only because it too is a fixed property of each call site.
KindFlags flagsFor(CompileCacheKind kind) {
  switch (kind) {
    case CompileCacheKind::kCommonJS:
      return {/*enableTS*/ false, /*persistent*/ false};
    case CompileCacheKind::kLoaderWrapped:
      return {/*enableTS*/ false, /*persistent*/ true};
    case CompileCacheKind::kLoaderWrappedTS:
      return {/*enableTS*/ true, /*persistent*/ true};
  }
  // Unreachable for a valid enum value. The switch has no default so that
  // adding a kind produces a -Wswitch diagnostic; this abort is the backstop,
  // because -Werror is off and enable_ts silently defaulting to false would
  // key TypeScript bytecode under a non-TypeScript kind.
  assert(false && "unhandled CompileCacheKind");
  std::abort();
}

} // namespace

napi_status compileCacheRun(
    napi_env env,
    CompileCache *cache,
    bool optimize,
    CompileCacheKind kind,
    const SourceBuffer &source,
    std::string_view wrapPrefix,
    std::string_view wrapSuffix,
    const char *sourceUrl,
    napi_value *result) {
  const KindFlags flags = flagsFor(kind);
  // Use an empty-but-non-null view rather than a default-constructed one:
  // compileCacheKey/trace treat a null data() as "no filename" in ways that
  // are easy to get subtly wrong, so keep data() valid even when empty.
  const std::string_view filename =
      sourceUrl ? std::string_view(sourceUrl) : std::string_view("");

  CompileCacheEntry entry;

  // Hash size(), never readableSize(): the terminator is not part of the
  // source text. Hashing it would key identical text to different entries
  // depending on whether its buffer happened to be terminated.
  if (cache != nullptr &&
      cache->lookup(
          entry,
          std::string_view(source.data(), source.size()),
          filename,
          kind)) {
    hermes_bytecode_flags bcFlags{};
    bcFlags.struct_size = sizeof(bcFlags);
    bcFlags.persistent = flags.persistent;
    if (hermes_run_bytecode(
            env,
            entry.bytecode,
            entry.bytecodeSize,
            CacheMapping::finalizer,
            entry.mapping,
            sourceUrl,
            &bcFlags,
            result) == napi_ok) {
      return napi_ok;
    }
    // A cache file that passed our header checks but that Hermes will not
    // load must not surface as a SyntaxError in valid user code. Clear the
    // pending exception, drop the entry, and fall through to compiling from
    // source. hermes_run_bytecode consumed the mapping even on failure, so
    // the pointers are nulled rather than destroyed.
    napi_value ignored;
    napi_get_and_clear_last_exception(env, &ignored);
    cache->invalidate(entry);
    entry.mapping = nullptr;
    entry.bytecode = nullptr;
    *result = nullptr;
  }

  // Assemble only on a miss, and only when there is a wrapper. With no
  // wrapper the caller's buffer reaches the compiler untouched, which is
  // what keeps the module loader's path copy-free.
  std::string wrapped;
  std::optional<BorrowedStringSourceBuffer> wrappedBuf;
  const SourceBuffer *toCompile = &source;
  if (!wrapPrefix.empty() || !wrapSuffix.empty()) {
    wrapped.reserve(wrapPrefix.size() + source.size() + wrapSuffix.size());
    wrapped.append(wrapPrefix);
    wrapped.append(source.data(), source.size());
    wrapped.append(wrapSuffix);
    // `wrapped` is complete and will not reallocate before the buffer dies.
    wrappedBuf.emplace(wrapped);
    toCompile = &*wrappedBuf;
  }

  hermes_compile_flags cflags{};
  cflags.struct_size = sizeof(cflags);
  cflags.enable_ts = flags.enableTS;
  // Optimizing trades compile time for execution speed, so it only pays when
  // the result is kept. The caller resolves that; the cache's generation name
  // must reflect it, or optimized and unoptimized bytecode compiled from
  // identical source would share an entry.
  cflags.optimize = optimize;
  uint8_t *bytecodeData = nullptr;
  size_t bytecodeSize = 0;
  napi_status compileStatus = hermes_compile_to_bytecode(
      env,
      reinterpret_cast<const uint8_t *>(toCompile->data()),
      toCompile->readableSize(),
      sourceUrl,
      &cflags,
      &bytecodeData,
      &bytecodeSize);
  if (compileStatus != napi_ok) {
    // A real error in the user's own source. Leave it pending.
    return compileStatus;
  }

  if (cache != nullptr)
    cache->save(entry, bytecodeData, bytecodeSize);

  hermes_bytecode_flags bcFlags{};
  bcFlags.struct_size = sizeof(bcFlags);
  bcFlags.persistent = flags.persistent;
  return hermes_run_bytecode(
      env,
      bytecodeData,
      bytecodeSize,
      [](const uint8_t *data, size_t, void *) {
        hermes_free_bytecode(const_cast<uint8_t *>(data));
      },
      nullptr,
      sourceUrl,
      &bcFlags,
      result);
}

} // namespace node_compat
} // namespace hermes
