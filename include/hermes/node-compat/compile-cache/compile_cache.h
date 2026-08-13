/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// Which entry point produced a cache entry. Part of the cache key: the
/// entry points hash differently shaped strings for the same file, so they
/// must not collide.
enum class CompileCacheKind : uint8_t {
  /// compileFunctionForCJSLoaderCb. The hashed string is the unwrapped
  /// module source; the CJS wrapper is applied natively afterwards.
  kCommonJS = 0,
  /// compileAndRunCallback with enableTS false. The hashed string is
  /// already wrapped, by libjs/loader.js.
  kLoaderWrapped = 1,
  /// compileAndRunCallback with enableTS true. Separate kind because
  /// enableTS changes the compile flags.
  kLoaderWrappedTS = 2,
};

/// CRC-32 (zlib polynomial), used for both cache keys and source hashes.
/// This is what Node's compile cache uses; see src/compile_cache.cc:42.
uint32_t compileCacheCrc32(const void *data, size_t size);

/// Cache key for a module: CRC-32 over the kind byte followed by the
/// filename. Depends only on the path, never on content, so editing a file
/// rewrites its one entry rather than leaving a new one behind.
uint32_t compileCacheKey(std::string_view filename, CompileCacheKind kind);

/// Name of the generation directory, e.g. "0.3.0-x86_64-bc99-3f9c21ab".
/// Readable on purpose: the active generation should be answerable by
/// looking. \p configCrc covers the native CJS wrapper text and the
/// compile-flag bit patterns.
std::string compileCacheGenerationName(
    std::string_view version,
    std::string_view arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc);

/// A mapped cache file.
///
/// Ownership is normally transferred to Hermes by passing `finalizer` and
/// the CacheMapping pointer as the finalize callback and hint of
/// hermes_run_bytecode; Hermes then calls finalizer when the RuntimeModule
/// dies. On paths that never reach Hermes, call destroy() instead.
struct CacheMapping {
  void *base = nullptr;
  size_t length = 0;

  /// hermes_run_bytecode finalize_cb. \p hint is the CacheMapping.
  static void finalizer(const uint8_t *data, size_t size, void *hint);

  /// Unmap and delete this mapping.
  void destroy();
};

/// One cache entry, in flight. Filled by the caller with the identifying
/// fields, then completed by compileCacheReadEntry on a hit.
struct CompileCacheEntry {
  /// Identity, set before lookup.
  uint32_t key = 0;
  uint32_t sourceCrc = 0;
  uint32_t sourceSize = 0;
  std::string cacheFilePath;

  /// Set on a hit. The caller takes ownership of `mapping`.
  CacheMapping *mapping = nullptr;
  const uint8_t *bytecode = nullptr;
  uint32_t bytecodeSize = 0;

  bool hit() const {
    return mapping != nullptr;
  }
};

/// Size of the entry header. A multiple of 8, so a page-aligned mmap base
/// plus this offset satisfies Hermes's BYTECODE_ALIGNMENT (4).
inline constexpr size_t kCompileCacheHeaderSize = 24;

/// Write \p bytecode to \p path with a header describing \p entry. Writes a
/// temp file and renames it, so a concurrent reader never sees a partial
/// entry. Returns false on any failure; failure is not an error, the caller
/// simply goes uncached.
bool compileCacheWriteEntry(
    const std::string &path,
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize);

/// Try to fill \p entry from its cacheFilePath. Returns true and sets
/// mapping/bytecode/bytecodeSize on a hit. Returns false for every kind of
/// miss: absent file, short file, bad magic, wrong header version, changed
/// source size or CRC, or a payload shorter than the header claims.
bool compileCacheReadEntry(CompileCacheEntry &entry);

/// Default cache root: $XDG_CACHE_HOME/hermes-node/compile-cache, falling
/// back to $HOME/.cache/hermes-node/compile-cache. Returns an empty string
/// when neither variable is set, which disables the cache.
std::string compileCacheDefaultRoot();

/// mkdir -p. Returns true if the directory exists afterwards.
bool compileCacheMakeDirs(const std::string &path);

/// Delete generation directories under \p versionedRoot, keeping
/// \p keepName plus the \p keepCount most recently modified others.
///
/// Best effort: failures are ignored. Safe to run while another process is
/// using a directory being removed -- on POSIX, unlinking a mapped file
/// removes only the directory entry, and the inode survives until the last
/// mapping is dropped.
void compileCachePruneGenerations(
    const std::string &versionedRoot,
    const std::string &keepName,
    size_t keepCount);

/// Number of old generations kept when a new one is created. Three rather
/// than one because version strings come from git tags, so two checkouts at
/// different commits produce different generations; keeping only the current
/// one would make alternating between them pay full compile cost each time.
inline constexpr size_t kCompileCacheGenerationsKept = 3;

/// On-disk bytecode cache. One instance per runtime, owned by RuntimeState.
///
/// Every operation is best effort: a failure anywhere degrades to "compile
/// from source" and never surfaces to the program being run.
class CompileCache {
 public:
  CompileCache() = default;
  CompileCache(const CompileCache &) = delete;
  CompileCache &operator=(const CompileCache &) = delete;

  /// Create <root>/v1/<generationName>/, prune old generations, and enable
  /// the cache. Returns false if the directory could not be created, in
  /// which case every later call is a no-op.
  bool enable(const std::string &root, const std::string &generationName);

  bool enabled() const {
    return enabled_;
  }

  /// "<root>/v1/<generationName>". Empty when not enabled.
  const std::string &generationDir() const {
    return generationDir_;
  }

  /// Emit hit/miss tracing to stderr. Driven by
  /// HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE.
  void setTracing(bool on) {
    tracing_ = on;
  }

  /// Fill \p entry's identity from (\p source, \p filename, \p kind) and try
  /// to load it. Returns true on a hit, in which case the caller owns
  /// entry.mapping. Returns false on a miss with the identity fields still
  /// populated, ready to be passed to save().
  bool lookup(
      CompileCacheEntry &entry,
      std::string_view source,
      std::string_view filename,
      CompileCacheKind kind);

  /// Persist a freshly compiled entry. Creates the fanout directory as
  /// needed. Failures are ignored.
  void save(
      const CompileCacheEntry &entry,
      const uint8_t *bytecode,
      size_t bytecodeSize);

  /// Delete an entry whose bytecode failed to run.
  void invalidate(const CompileCacheEntry &entry);

 private:
  void trace(const char *what, std::string_view filename) const;

  bool enabled_ = false;
  bool tracing_ = false;
  std::string generationDir_;
};

} // namespace node_compat
} // namespace hermes
