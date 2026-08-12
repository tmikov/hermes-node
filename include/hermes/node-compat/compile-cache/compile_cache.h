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
uint32_t compileCacheKey(const std::string &filename, CompileCacheKind kind);

/// Name of the generation directory, e.g. "0.3.0-x86_64-bc99-3f9c21ab".
/// Readable on purpose: the active generation should be answerable by
/// looking. \p configCrc covers the native CJS wrapper text and the
/// compile-flag bit patterns.
std::string compileCacheGenerationName(
    const std::string &version,
    const std::string &arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc);

} // namespace node_compat
} // namespace hermes
