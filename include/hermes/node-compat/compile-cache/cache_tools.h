/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/compile-cache/cache_config.h>

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// What one generation directory holds. Counts and bytes are split by kind
/// because the budget only governs one of them: a JavaScript entry is keyed
/// by path and rewritten in place, so a project's entries converge on a
/// fixed set, while content-keyed Wasm entries accumulate one per distinct
/// module ever seen.
struct CacheGenerationInfo {
  std::string name;
  /// True for the generation this binary would use. Every other generation
  /// is unreachable: a running binary only ever looks in its own.
  bool current = false;
  uint64_t jsEntries = 0;
  uint64_t jsBytes = 0;
  uint64_t wasmEntries = 0;
  uint64_t wasmBytes = 0;
  /// Abandoned "w<digest>.<pid>.<n>.tmp" files from interrupted writes.
  /// Counted here and nowhere else -- they do not count against the budget,
  /// so this is the only place their cost is visible.
  uint64_t tempFiles = 0;
  uint64_t tempBytes = 0;
};

/// A whole cache directory, as `cache info` reports it.
struct CacheInfo {
  std::string root;
  /// False when the root does not exist. Not an error: a cache that has
  /// never been written is a normal state, not a failure to report.
  bool exists = false;
  /// True when <root>/config is present. When false, `config` below holds
  /// the defaults that would be written on first use.
  bool hasConfigFile = false;
  CacheConfig config{};
  /// Complaints from parsing the config file, if any. Same messages the
  /// runtime prints on every run.
  std::vector<std::string> configComplaints;
  std::vector<CacheGenerationInfo> generations;
};

/// Scan \p root and describe what is there. Never fails: an unreadable
/// directory reports what it could see, in keeping with the cache's own
/// best-effort contract.
///
/// \p currentGeneration marks one generation as the one this binary would
/// use. Pass an empty string when that is not known or does not matter, and
/// no generation is marked.
CacheInfo cacheToolsScan(
    const std::string &root,
    const std::string &currentGeneration);

/// Print \p info to \p out in the shape `cache info` reports. The current
/// generation is always listed, empty or not; \p verbose additionally lists
/// stale generations that hold nothing.
void cacheToolsPrintInfo(
    const CacheInfo &info,
    bool verbose,
    std::ostream &out);

/// What a prune or clean actually did, so the caller can report it and a
/// test can assert it without reading the filesystem back.
struct CacheChangeReport {
  /// The whole cache directory went, configuration file included. Tracked
  /// separately from the counts below because it is true even when there was
  /// nothing to count: a cache holding only a config file is still something
  /// that was there and now is not, and reporting "nothing to clean" after
  /// deleting it is a lie a user would notice.
  bool removedEverything = false;
  uint64_t generationsRemoved = 0;
  uint64_t generationBytesRemoved = 0;
  uint64_t wasmEntriesEvicted = 0;
  uint64_t wasmBytesEvicted = 0;
  uint64_t tempFilesReaped = 0;
  uint64_t tempBytesReaped = 0;
};

/// Drop every generation except \p currentGeneration, then apply
/// \p config.maxWasmBytes to what remains, then reap abandoned temp files.
///
/// In that order on purpose: a stale generation is unreachable, so its bytes
/// are the cheapest to give up and freeing them first may leave the budget
/// already satisfied, with no live entry evicted at all.
///
/// Best effort throughout, like everything else about this cache: what
/// cannot be removed is left, and the report says what actually happened
/// rather than what was intended.
CacheChangeReport cacheToolsPrune(
    const std::string &root,
    const std::string &currentGeneration,
    const CacheConfig &config);

/// Scope for cacheToolsClean.
enum class CacheCleanScope {
  /// Everything under the root, including the config file.
  kAll,
  /// Only \p currentGeneration, leaving other generations and the config.
  kCurrentGeneration,
};

/// Delete cache contents. No confirmation and no dry run: a compile cache is
/// reconstructible by definition, and prompting in a tool that runs in
/// scripts costs more than the mistake it would prevent.
CacheChangeReport cacheToolsClean(
    const std::string &root,
    const std::string &currentGeneration,
    CacheCleanScope scope);

/// Print \p report to \p out, naming only what actually changed. A run that
/// removed nothing says so in one line rather than printing zeroes.
void cacheToolsPrintChange(
    const char *what,
    const CacheChangeReport &report,
    std::ostream &out);

/// Render \p bytes as a human-readable size. Sizes here span a config file
/// and a gigabyte of bytecode, and raw byte counts at that range are read
/// wrongly more often than they are read at all.
std::string cacheToolsFormatBytes(uint64_t bytes);

} // namespace node_compat
} // namespace hermes
