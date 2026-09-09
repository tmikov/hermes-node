/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/compile-cache/cache_tools.h"

#include "hermes/node-compat/compile-cache/compile_cache.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace hermes {
namespace node_compat {

namespace {

/// v1/ under the cache root, where generations live. The layout constant is
/// duplicated from CompileCache::enable deliberately: these tools describe a
/// directory they did not create, possibly written by another build, so they
/// read the layout rather than being handed it.
std::string versionedRootOf(const std::string &root) {
  return root + "/v1";
}

bool isDirectory(const std::string &path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// Total bytes of every regular file under \p path, following no symlinks.
uint64_t treeBytes(const std::string &path) {
  uint64_t total = 0;
  DIR *d = ::opendir(path.c_str());
  if (d == nullptr)
    return 0;
  while (struct dirent *e = ::readdir(d)) {
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
      continue;
    std::string child = path + "/" + e->d_name;
    struct stat st {};
    if (::lstat(child.c_str(), &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      total += treeBytes(child);
    else if (S_ISREG(st.st_mode))
      total += static_cast<uint64_t>(st.st_size);
  }
  ::closedir(d);
  return total;
}

/// Classify and total one generation directory's contents.
CacheGenerationInfo scanGeneration(
    const std::string &generationDir,
    const std::string &name) {
  CacheGenerationInfo info;
  info.name = name;

  DIR *root = ::opendir(generationDir.c_str());
  if (root == nullptr)
    return info;
  while (struct dirent *fan = ::readdir(root)) {
    if (fan->d_name[0] == '.')
      continue;
    std::string fanDir = generationDir + "/" + fan->d_name;
    DIR *sub = ::opendir(fanDir.c_str());
    if (sub == nullptr)
      continue;
    while (struct dirent *ent = ::readdir(sub)) {
      if (ent->d_name[0] == '.')
        continue;
      std::string path = fanDir + "/" + ent->d_name;
      struct stat st {};
      if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        continue;
      uint64_t size = static_cast<uint64_t>(st.st_size);
      // The same predicates the sweep uses, not a second opinion about what
      // an entry looks like.
      if (compileCacheIsWasmEntryName(ent->d_name)) {
        ++info.wasmEntries;
        info.wasmBytes += size;
      } else if (compileCacheIsWasmEntryTempName(ent->d_name)) {
        ++info.tempFiles;
        info.tempBytes += size;
      } else {
        ++info.jsEntries;
        info.jsBytes += size;
      }
    }
    ::closedir(sub);
  }
  ::closedir(root);
  return info;
}

} // namespace

std::string cacheToolsFormatBytes(uint64_t bytes) {
  static const char *kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
    value /= 1024.0;
    ++unit;
  }
  char buf[64];
  // Whole bytes read better than "1.0 B"; everything above gets one decimal.
  if (unit == 0)
    std::snprintf(buf, sizeof(buf), "%" PRIu64 " B", bytes);
  else
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  return std::string(buf);
}

CacheInfo cacheToolsScan(
    const std::string &root,
    const std::string &currentGeneration) {
  CacheInfo info;
  info.root = root;
  if (root.empty() || !isDirectory(root))
    return info;
  info.exists = true;

  std::string configPath = root + "/" + kCacheConfigFileName;
  {
    std::ifstream in(configPath, std::ios::binary);
    if (in) {
      info.hasConfigFile = true;
      std::ostringstream ss;
      ss << in.rdbuf();
      info.config = cacheConfigParse(ss.str(), &info.configComplaints);
    }
  }

  std::string versioned = versionedRootOf(root);
  DIR *d = ::opendir(versioned.c_str());
  if (d == nullptr)
    return info;
  while (struct dirent *e = ::readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    std::string generationDir = versioned + "/" + e->d_name;
    if (!isDirectory(generationDir))
      continue;
    CacheGenerationInfo gen = scanGeneration(generationDir, e->d_name);
    gen.current = !currentGeneration.empty() && currentGeneration == e->d_name;
    info.generations.push_back(std::move(gen));
  }
  ::closedir(d);

  // Current first, then by name, so repeated runs print the same order and
  // the one that matters is not buried among the retained ones.
  std::sort(
      info.generations.begin(),
      info.generations.end(),
      [](const CacheGenerationInfo &a, const CacheGenerationInfo &b) {
        if (a.current != b.current)
          return a.current;
        return a.name < b.name;
      });
  return info;
}

void cacheToolsPrintInfo(
    const CacheInfo &info,
    bool verbose,
    std::ostream &out) {
  out << "compile cache: " << info.root << "\n";
  if (!info.exists) {
    // Not an error, and worth saying plainly: "nothing here yet" is a normal
    // answer that a table of zeroes would obscure.
    out << "  (does not exist)\n";
    return;
  }

  out << "  config:       "
      << (info.hasConfigFile ? "config" : "(defaults, no config file)") << "\n";
  out << "    recency:        "
      << (info.config.recency == CacheRecency::kAtime ? "atime" : "mtime")
      << "\n";
  out << "    max_wasm_bytes: " << info.config.maxWasmBytes << " ("
      << cacheToolsFormatBytes(info.config.maxWasmBytes) << ")\n";
  for (const std::string &complaint : info.configComplaints)
    out << "    warning: " << complaint << "\n";

  uint64_t totalJsEntries = 0, totalJsBytes = 0;
  uint64_t totalWasmEntries = 0, totalWasmBytes = 0;
  uint64_t totalTempFiles = 0, totalTempBytes = 0;

  out << "  generations:  " << info.generations.size() << "\n";
  for (const CacheGenerationInfo &gen : info.generations) {
    totalJsEntries += gen.jsEntries;
    totalJsBytes += gen.jsBytes;
    totalWasmEntries += gen.wasmEntries;
    totalWasmBytes += gen.wasmBytes;
    totalTempFiles += gen.tempFiles;
    totalTempBytes += gen.tempBytes;

    // An empty STALE generation is noise and is hidden without --verbose.
    // The current one is always shown, empty or not: "which generation am I
    // on?" is a question this command exists to answer, and a fresh cache --
    // the case where someone is most likely to ask -- is exactly when it
    // would otherwise print nothing.
    bool empty =
        gen.jsEntries == 0 && gen.wasmEntries == 0 && gen.tempFiles == 0;
    if (empty && !gen.current && !verbose)
      continue;
    out << "    " << gen.name << (gen.current ? "  (current)" : "  (stale)")
        << "\n";
    out << "      js:   " << gen.jsEntries << " entries, "
        << cacheToolsFormatBytes(gen.jsBytes) << "\n";
    out << "      wasm: " << gen.wasmEntries << " entries, "
        << cacheToolsFormatBytes(gen.wasmBytes) << "\n";
    if (gen.tempFiles != 0)
      out << "      temp: " << gen.tempFiles << " abandoned, "
          << cacheToolsFormatBytes(gen.tempBytes) << "\n";
  }

  out << "  total:        "
      << cacheToolsFormatBytes(totalJsBytes + totalWasmBytes + totalTempBytes)
      << "\n";
  out << "    js:   " << totalJsEntries << " entries, "
      << cacheToolsFormatBytes(totalJsBytes) << "\n";
  // Only the Wasm total is measured against the budget, because only Wasm
  // entries are bounded by it.
  out << "    wasm: " << totalWasmEntries << " entries, "
      << cacheToolsFormatBytes(totalWasmBytes) << " of "
      << cacheToolsFormatBytes(info.config.maxWasmBytes);
  if (totalWasmBytes > info.config.maxWasmBytes)
    out << "  (over budget; run 'cache prune')";
  out << "\n";
  if (totalTempFiles != 0)
    out << "    temp: " << totalTempFiles << " abandoned, "
        << cacheToolsFormatBytes(totalTempBytes)
        << "  (not counted against the budget; run 'cache prune')\n";
}

CacheChangeReport cacheToolsPrune(
    const std::string &root,
    const std::string &currentGeneration,
    const CacheConfig &config) {
  CacheChangeReport report;
  if (root.empty() || !isDirectory(root))
    return report;

  std::string versioned = versionedRootOf(root);

  // Stale generations first. They are unreachable -- a running binary only
  // looks in its own -- so they are the cheapest bytes in the cache to give
  // up, and freeing them may satisfy the budget with no live entry touched.
  DIR *d = ::opendir(versioned.c_str());
  if (d != nullptr) {
    std::vector<std::string> stale;
    while (struct dirent *e = ::readdir(d)) {
      if (e->d_name[0] == '.')
        continue;
      if (!currentGeneration.empty() && currentGeneration == e->d_name)
        continue;
      std::string generationDir = versioned + "/" + e->d_name;
      if (isDirectory(generationDir))
        stale.push_back(std::move(generationDir));
    }
    ::closedir(d);
    for (const std::string &generationDir : stale) {
      report.generationBytesRemoved += treeBytes(generationDir);
      compileCacheRemoveTree(generationDir);
      ++report.generationsRemoved;
    }
  }

  // Then the budget, and the temp reaping the sweep does along the way.
  // Measured by scanning before and after rather than by instrumenting the
  // sweep: the sweep is on the run path and exists to be fast, not to
  // narrate, and a tool that runs once can afford to look twice.
  CacheInfo before = cacheToolsScan(root, currentGeneration);
  compileCacheEvictWasm(versioned, config);
  CacheInfo after = cacheToolsScan(root, currentGeneration);

  auto totals = [](const CacheInfo &info,
                   uint64_t &entries,
                   uint64_t &bytes,
                   uint64_t &tempFiles,
                   uint64_t &tempBytes) {
    entries = bytes = tempFiles = tempBytes = 0;
    for (const CacheGenerationInfo &gen : info.generations) {
      entries += gen.wasmEntries;
      bytes += gen.wasmBytes;
      tempFiles += gen.tempFiles;
      tempBytes += gen.tempBytes;
    }
  };
  uint64_t beforeEntries, beforeBytes, beforeTempFiles, beforeTempBytes;
  uint64_t afterEntries, afterBytes, afterTempFiles, afterTempBytes;
  totals(before, beforeEntries, beforeBytes, beforeTempFiles, beforeTempBytes);
  totals(after, afterEntries, afterBytes, afterTempFiles, afterTempBytes);

  report.wasmEntriesEvicted = beforeEntries - afterEntries;
  report.wasmBytesEvicted = beforeBytes - afterBytes;
  report.tempFilesReaped = beforeTempFiles - afterTempFiles;
  report.tempBytesReaped = beforeTempBytes - afterTempBytes;
  return report;
}

CacheChangeReport cacheToolsClean(
    const std::string &root,
    const std::string &currentGeneration,
    CacheCleanScope scope) {
  CacheChangeReport report;
  if (root.empty() || !isDirectory(root))
    return report;

  if (scope == CacheCleanScope::kCurrentGeneration) {
    if (currentGeneration.empty())
      return report;
    std::string generationDir = versionedRootOf(root) + "/" + currentGeneration;
    if (!isDirectory(generationDir))
      return report;
    report.generationBytesRemoved = treeBytes(generationDir);
    compileCacheRemoveTree(generationDir);
    report.generationsRemoved = 1;
    return report;
  }

  // Everything, config file included: "clean" that left configuration behind
  // would be a surprise the next run inherits.
  report.generationBytesRemoved = treeBytes(root);
  DIR *d = ::opendir(versionedRootOf(root).c_str());
  if (d != nullptr) {
    while (struct dirent *e = ::readdir(d)) {
      if (e->d_name[0] == '.')
        continue;
      if (isDirectory(versionedRootOf(root) + "/" + e->d_name))
        ++report.generationsRemoved;
    }
    ::closedir(d);
  }
  compileCacheRemoveTree(root);
  report.removedEverything = true;
  return report;
}

void cacheToolsPrintChange(
    const char *what,
    const CacheChangeReport &report,
    std::ostream &out) {
  if (report.removedEverything) {
    out << "  removed the cache, "
        << cacheToolsFormatBytes(report.generationBytesRemoved) << "\n";
    return;
  }
  bool anything = report.generationsRemoved != 0 ||
      report.wasmEntriesEvicted != 0 || report.tempFilesReaped != 0;
  if (!anything) {
    out << "  nothing to " << what << "\n";
    return;
  }
  if (report.generationsRemoved != 0)
    out << "  removed " << report.generationsRemoved << " generation"
        << (report.generationsRemoved == 1 ? "" : "s") << ", "
        << cacheToolsFormatBytes(report.generationBytesRemoved) << "\n";
  if (report.wasmEntriesEvicted != 0)
    out << "  evicted " << report.wasmEntriesEvicted << " wasm entr"
        << (report.wasmEntriesEvicted == 1 ? "y" : "ies") << " over budget, "
        << cacheToolsFormatBytes(report.wasmBytesEvicted) << "\n";
  if (report.tempFilesReaped != 0)
    out << "  reaped " << report.tempFilesReaped << " abandoned temp file"
        << (report.tempFilesReaped == 1 ? "" : "s") << ", "
        << cacheToolsFormatBytes(report.tempBytesReaped) << "\n";
}

} // namespace node_compat
} // namespace hermes
