/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Tests for the `cache` subcommand's engine: scanning a cache directory,
/// and the prune and clean operations.
///
/// Like BundleToolsTest, these run with no runtime at all -- the tools live
/// in the VM-free half of the compile-cache library on purpose, so a command
/// that describes a directory cannot fail for reasons belonging to a runtime
/// it never needed.
//===----------------------------------------------------------------------===//

#include "hermes/node-compat/compile-cache/cache_tools.h"

#include "hermes/node-compat/compile-cache/compile_cache.h"

#include "TempTree.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "gtest/gtest.h"

using namespace hermes::node_compat;
using hermes::node_compat::test::TempTree;

namespace {

/// Write \p bytes bytes to \p path, creating parent directories.
void writeFile(const std::string &path, size_t bytes, char fill = 'x') {
  size_t slash = path.rfind('/');
  ASSERT_TRUE(slash != std::string::npos) << path;
  ASSERT_TRUE(compileCacheMakeDirs(path.substr(0, slash)));
  std::ofstream f(path, std::ios::binary);
  ASSERT_TRUE(f.good()) << path;
  std::string data(bytes, fill);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool exists(const std::string &path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

/// A 64-hex-digit digest built from one repeated character, so entries in a
/// test are distinguishable and obviously synthetic.
std::string digestOf(char c) {
  return std::string(64, c);
}

/// Populate a cache with a current generation and a stale one.
struct Fixture {
  TempTree tree;
  std::string root;
  std::string current = "gen-current";
  std::string stale = "gen-stale";

  Fixture() : root(tree.path()) {
    // Current: one 400-byte Wasm entry and one 100-byte JavaScript entry.
    writeFile(wasmPath(current, 'a'), 400);
    writeFile(jsPath(current, "1234abcd"), 100);
    // Stale: one 500-byte Wasm entry.
    writeFile(wasmPath(stale, 'b'), 500);
  }

  std::string generationDir(const std::string &gen) const {
    return root + "/v1/" + gen;
  }
  std::string wasmPath(const std::string &gen, char c) const {
    std::string d = digestOf(c);
    return generationDir(gen) + "/" + d.substr(0, 2) + "/w" + d;
  }
  std::string jsPath(const std::string &gen, const std::string &key) const {
    return generationDir(gen) + "/" + key.substr(0, 2) + "/" + key;
  }
};

} // namespace

TEST(CacheToolsTest, ScanSeparatesKindsAndMarksTheCurrentGeneration) {
  Fixture f;
  CacheInfo info = cacheToolsScan(f.root, f.current);

  ASSERT_TRUE(info.exists);
  ASSERT_EQ(2u, info.generations.size());
  // Current sorts first, so the reader is not made to hunt for the one that
  // matters among the retained ones.
  EXPECT_EQ(f.current, info.generations[0].name);
  EXPECT_TRUE(info.generations[0].current);
  EXPECT_FALSE(info.generations[1].current);

  // Kinds are counted apart because only one of them is bounded by the
  // budget.
  EXPECT_EQ(1u, info.generations[0].wasmEntries);
  EXPECT_EQ(400u, info.generations[0].wasmBytes);
  EXPECT_EQ(1u, info.generations[0].jsEntries);
  EXPECT_EQ(100u, info.generations[0].jsBytes);
  EXPECT_EQ(1u, info.generations[1].wasmEntries);
  EXPECT_EQ(500u, info.generations[1].wasmBytes);
}

TEST(CacheToolsTest, ScanCountsAbandonedTempFilesApart) {
  Fixture f;
  writeFile(f.wasmPath(f.current, 'a') + ".999.0.tmp", 70);
  CacheInfo info = cacheToolsScan(f.root, f.current);

  ASSERT_EQ(2u, info.generations.size());
  // Not as a Wasm entry, and not as a JavaScript one either: a temp file is
  // neither, and lumping it into either total would misreport the budget.
  EXPECT_EQ(1u, info.generations[0].wasmEntries);
  EXPECT_EQ(1u, info.generations[0].jsEntries);
  EXPECT_EQ(1u, info.generations[0].tempFiles);
  EXPECT_EQ(70u, info.generations[0].tempBytes);
}

TEST(CacheToolsTest, ScanOfAnAbsentRootIsNotAnError) {
  TempTree tree;
  CacheInfo info = cacheToolsScan(tree.path() + "/never-created", "gen");
  EXPECT_FALSE(info.exists);
  EXPECT_TRUE(info.generations.empty());
}

TEST(CacheToolsTest, ScanReportsConfigComplaints) {
  Fixture f;
  {
    std::ofstream cfg(f.root + "/config", std::ios::binary);
    cfg << "recency: banana\n";
  }
  CacheInfo info = cacheToolsScan(f.root, f.current);
  EXPECT_TRUE(info.hasConfigFile);
  ASSERT_EQ(1u, info.configComplaints.size());
  EXPECT_NE(std::string::npos, info.configComplaints[0].find("banana"));
}

TEST(CacheToolsTest, PruneDropsStaleGenerationsAndLeavesTheCurrentOne) {
  Fixture f;
  CacheConfig config; // default budget: nothing is over it
  CacheChangeReport report = cacheToolsPrune(f.root, f.current, config);

  EXPECT_EQ(1u, report.generationsRemoved);
  EXPECT_EQ(500u, report.generationBytesRemoved);
  EXPECT_FALSE(exists(f.generationDir(f.stale)));
  EXPECT_TRUE(exists(f.wasmPath(f.current, 'a')));
  // Nothing was over budget, so nothing was evicted -- dropping the
  // unreachable generation first is what makes that the common case.
  EXPECT_EQ(0u, report.wasmEntriesEvicted);
}

TEST(CacheToolsTest, PruneAppliesTheBudgetAfterDroppingGenerations) {
  Fixture f;
  // Two more entries in the current generation, so that dropping the stale
  // one is not enough on its own.
  writeFile(f.wasmPath(f.current, 'c'), 400);
  writeFile(f.wasmPath(f.current, 'd'), 400);

  CacheConfig config;
  config.maxWasmBytes = 500; // 1200 bytes of current-generation entries
  CacheChangeReport report = cacheToolsPrune(f.root, f.current, config);

  EXPECT_EQ(1u, report.generationsRemoved);
  EXPECT_GT(report.wasmEntriesEvicted, 0u);

  CacheInfo after = cacheToolsScan(f.root, f.current);
  uint64_t wasmBytes = 0;
  for (const CacheGenerationInfo &gen : after.generations)
    wasmBytes += gen.wasmBytes;
  EXPECT_LE(wasmBytes, config.maxWasmBytes);
}

TEST(CacheToolsTest, PruneOfAnAbsentRootReportsNothing) {
  TempTree tree;
  CacheChangeReport report =
      cacheToolsPrune(tree.path() + "/never-created", "gen", CacheConfig{});
  EXPECT_EQ(0u, report.generationsRemoved);
  EXPECT_EQ(0u, report.wasmEntriesEvicted);
}

TEST(CacheToolsTest, CleanCurrentGenerationLeavesTheRestAlone) {
  Fixture f;
  {
    std::ofstream cfg(f.root + "/config", std::ios::binary);
    cfg << "recency: mtime\n";
  }
  CacheChangeReport report =
      cacheToolsClean(f.root, f.current, CacheCleanScope::kCurrentGeneration);

  EXPECT_EQ(1u, report.generationsRemoved);
  EXPECT_FALSE(report.removedEverything);
  EXPECT_FALSE(exists(f.generationDir(f.current)));
  EXPECT_TRUE(exists(f.generationDir(f.stale)));
  EXPECT_TRUE(exists(f.root + "/config"));
}

TEST(CacheToolsTest, CleanAllRemovesTheRootAndSaysSo) {
  Fixture f;
  {
    std::ofstream cfg(f.root + "/config", std::ios::binary);
    cfg << "recency: mtime\n";
  }
  CacheChangeReport report =
      cacheToolsClean(f.root, f.current, CacheCleanScope::kAll);

  // removedEverything rather than a generation count: a cache holding only a
  // config file has nothing to count and was still deleted, and reporting
  // "nothing to clean" after deleting it is a lie the user would notice.
  EXPECT_TRUE(report.removedEverything);
  EXPECT_GT(report.generationBytesRemoved, 0u);
  EXPECT_FALSE(exists(f.root));
}

TEST(CacheToolsTest, CleanOfAnAbsentRootReportsNothing) {
  TempTree tree;
  CacheChangeReport report = cacheToolsClean(
      tree.path() + "/never-created", "gen", CacheCleanScope::kAll);
  EXPECT_FALSE(report.removedEverything);
  EXPECT_EQ(0u, report.generationsRemoved);
}

TEST(CacheToolsTest, FormatBytesReadsAsASize) {
  // Whole bytes below a kilobyte: "1.0 B" reads worse than "999 B".
  EXPECT_EQ("0 B", cacheToolsFormatBytes(0));
  EXPECT_EQ("999 B", cacheToolsFormatBytes(999));
  EXPECT_EQ("1.0 KB", cacheToolsFormatBytes(1024));
  EXPECT_EQ("256.0 MB", cacheToolsFormatBytes(268435456));
  EXPECT_EQ("1.0 GB", cacheToolsFormatBytes(1073741824));
}
