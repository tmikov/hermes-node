/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Which timestamp eviction orders by.
enum class CacheRecency : uint8_t {
  /// Real LRU. Under relatime -- the common mount default -- atime is
  /// updated whenever it is older than mtime/ctime or more than 24 hours
  /// old, which is ample resolution for a cache measured in weeks.
  kAtime,
  /// Insertion order. For noatime mounts and filesystems where atime means
  /// nothing, where this is at least wrong knowingly.
  kMtime,
};

/// Configuration read from the cache directory. Every field has a default
/// that is used when the file is absent, unreadable or malformed: a broken
/// configuration must degrade to a working cache, never break a program.
struct CacheConfig {
  CacheRecency recency = CacheRecency::kAtime;
  /// Budget over Wasm entries. 0 means evict everything, not "unlimited".
  uint64_t maxWasmBytes = 268435456ull; // 256 MB
};

/// File name, at the cache ROOT -- outside v1/<generation>/, so generation
/// pruning can never delete it.
inline constexpr const char *kCacheConfigFileName = "config";

/// Parse \p text. Unknown keys and unparseable values are ignored in favour
/// of the default.
///
/// \p complaints, when non-null, receives one message per thing that was not
/// understood, each already carrying its line number. They are returned
/// rather than printed so that this stays a pure function and so that a test
/// can assert what it complained about; the caller decides where they go.
CacheConfig cacheConfigParse(
    std::string_view text,
    std::vector<std::string> *complaints = nullptr);

/// Read <root>/config, writing it with the defaults first if it is absent.
/// Never fails: an unwritable directory just means defaults.
///
/// \p complaints is as above, plus one entry if the file exists and could
/// not be read -- silently indistinguishable from absent otherwise, and the
/// same class of surprise.
CacheConfig cacheConfigLoadOrCreate(
    const std::string &root,
    std::vector<std::string> *complaints = nullptr);

} // namespace node_compat
} // namespace hermes
