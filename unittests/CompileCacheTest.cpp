/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/cache_config.h>
#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <gtest/gtest.h>

using namespace hermes::node_compat;

TEST(CompileCacheTest, Crc32MatchesKnownValue) {
  // The standard CRC-32 of "123456789" is 0xCBF43926.
  EXPECT_EQ(0xCBF43926u, compileCacheCrc32("123456789", 9));
}

TEST(CompileCacheTest, Crc32OfEmptyInputIsZero) {
  EXPECT_EQ(0u, compileCacheCrc32("", 0));
}

TEST(CompileCacheTest, KeyIsStableForSameInputs) {
  EXPECT_EQ(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS),
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, KeyDiffersByPath) {
  EXPECT_NE(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS),
      compileCacheKey("/a/b/d.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, KeyDiffersByKind) {
  // The two entry points hash differently shaped strings for one file, so
  // they must never share a key.
  EXPECT_NE(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS),
      compileCacheKey("/a/b/c.js", CompileCacheKind::kLoaderWrapped));
  EXPECT_NE(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kLoaderWrapped),
      compileCacheKey("/a/b/c.js", CompileCacheKind::kLoaderWrappedTS));
}

TEST(CompileCacheTest, GenerationNameIsReadable) {
  EXPECT_EQ(
      "0.3.0-x86_64-bc99-3f9c21ab",
      compileCacheGenerationName("0.3.0", "x86_64", 99, 0x3f9c21ab));
}

TEST(CompileCacheTest, GenerationNamePadsConfigCrc) {
  EXPECT_EQ(
      "0.3.0-arm64-bc99-0000000f",
      compileCacheGenerationName("0.3.0", "arm64", 99, 0xf));
}

TEST(CompileCacheTest, GenerationNameVariesWithEachComponent) {
  std::string base = compileCacheGenerationName("0.3.0", "x86_64", 99, 1);
  EXPECT_NE(base, compileCacheGenerationName("0.3.1", "x86_64", 99, 1));
  EXPECT_NE(base, compileCacheGenerationName("0.3.0", "arm64", 99, 1));
  EXPECT_NE(base, compileCacheGenerationName("0.3.0", "x86_64", 100, 1));
  EXPECT_NE(base, compileCacheGenerationName("0.3.0", "x86_64", 99, 2));
}

#include "TempTree.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

using hermes::node_compat::test::TempTree;

namespace {

/// A recognisable fake payload. The entry format does not interpret the
/// payload, so real bytecode is not needed to test it.
std::vector<uint8_t> fakePayload(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<uint8_t>(seed + i);
  return v;
}

/// Build an entry describing \p source at \p path, as lookup would.
CompileCacheEntry makeEntry(
    const std::string &path,
    const std::string &source) {
  CompileCacheEntry entry;
  entry.key = compileCacheKey(path, CompileCacheKind::kCommonJS);
  entry.sourceCrc = compileCacheCrc32(source.data(), source.size());
  entry.sourceSize = static_cast<uint32_t>(source.size());
  entry.cacheFilePath = path;
  return entry;
}

} // namespace

TEST(CompileCacheTest, WriteThenReadRoundTrips) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  std::string source = "module.exports = 1;";
  auto payload = fakePayload(1234, 7);

  CompileCacheEntry written = makeEntry(file, source);
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  CompileCacheEntry read = makeEntry(file, source);
  ASSERT_TRUE(compileCacheReadEntry(read));
  ASSERT_TRUE(read.hit());
  EXPECT_EQ(payload.size(), read.bytecodeSize);
  EXPECT_EQ(0, memcmp(payload.data(), read.bytecode, payload.size()));
  read.mapping->destroy();
}

TEST(CompileCacheTest, ReadMissesWhenFileAbsent) {
  TempTree dir;
  CompileCacheEntry entry = makeEntry(dir.path() + "/nope", "x");
  EXPECT_FALSE(compileCacheReadEntry(entry));
  EXPECT_FALSE(entry.hit());
}

TEST(CompileCacheTest, ReadMissesWhenSourceChanged) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  auto payload = fakePayload(64, 1);
  CompileCacheEntry written = makeEntry(file, "var a = 1;");
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  // Same length, different content: only the CRC can tell them apart.
  CompileCacheEntry read = makeEntry(file, "var b = 2;");
  EXPECT_EQ(written.sourceSize, read.sourceSize);
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, ReadMissesWhenSourceSizeChanged) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  auto payload = fakePayload(64, 1);
  CompileCacheEntry written = makeEntry(file, "var a = 1;");
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  CompileCacheEntry read = makeEntry(file, "var a = 1; var b = 2;");
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, ReadMissesOnBadMagic) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  std::string source = "x";
  auto payload = fakePayload(64, 1);
  CompileCacheEntry written = makeEntry(file, source);
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
  f.seekp(0);
  f.write("XXXX", 4);
  f.close();

  CompileCacheEntry read = makeEntry(file, source);
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, ReadMissesOnTruncatedFile) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  std::string source = "x";
  auto payload = fakePayload(4096, 3);
  CompileCacheEntry written = makeEntry(file, source);
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  // Cut the file in half; the header still claims the full payload.
  ASSERT_EQ(0, ::truncate(file.c_str(), 512));

  CompileCacheEntry read = makeEntry(file, source);
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, ReadMissesOnGarbageShorterThanHeader) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  {
    std::ofstream f(file, std::ios::binary);
    f << "junk";
  }
  CompileCacheEntry read = makeEntry(file, "x");
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, WriteLeavesNoTempFileBehind) {
  TempTree dir;
  std::string file = dir.path() + "/entry";
  auto payload = fakePayload(64, 1);
  CompileCacheEntry written = makeEntry(file, "x");
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  // Exactly one file: the entry. The temp file must have been renamed, not
  // left alongside it.
  FILE *pipe = ::popen(("ls -1 " + dir.path() + " | wc -l").c_str(), "r");
  ASSERT_NE(nullptr, pipe);
  char buf[32] = {0};
  ASSERT_NE(nullptr, fgets(buf, sizeof(buf), pipe));
  ::pclose(pipe);
  EXPECT_EQ(1, atoi(buf));
}

#include <dirent.h>
#include <sys/time.h>

namespace {

/// Count entries in \p dir, ignoring "." and "..".
size_t countDirEntries(const std::string &dir) {
  DIR *d = ::opendir(dir.c_str());
  if (d == nullptr)
    return 0;
  size_t n = 0;
  while (struct dirent *e = ::readdir(d)) {
    if (::strcmp(e->d_name, ".") != 0 && ::strcmp(e->d_name, "..") != 0)
      ++n;
  }
  ::closedir(d);
  return n;
}

bool fileExists(const std::string &path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string &path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// Saves an environment variable on construction and restores it on
/// destruction. GoogleTest runs every test in one process, so a test that
/// changes the environment must put it back -- otherwise the next test
/// reads the mutated value as if it were the original.
///
/// Declare one before any TempTree it interacts with: destructors run in
/// reverse order, so the TempTree is removed first and the variable is
/// restored afterwards.
class EnvGuard {
 public:
  explicit EnvGuard(const char *name) : name_(name) {
    if (const char *v = ::getenv(name)) {
      had_ = true;
      value_ = v;
    }
  }
  ~EnvGuard() {
    if (had_)
      ::setenv(name_, value_.c_str(), 1);
    else
      ::unsetenv(name_);
  }
  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;

 private:
  const char *name_;
  bool had_ = false;
  std::string value_;
};

/// Create \p name under \p root with an mtime \p ageSeconds in the past, so
/// pruning order is deterministic instead of depending on creation speed.
///
/// Sets the mtime directly rather than shelling out to `touch -d`, whose
/// relative-time syntax ("40 seconds ago") is a GNU extension and is not
/// accepted by the BSD touch on macOS, which CI also builds.
void makeAgedDir(
    const std::string &root,
    const std::string &name,
    int ageSeconds) {
  std::string path = root + "/" + name;
  ASSERT_TRUE(compileCacheMakeDirs(path));
  struct timeval times[2];
  ASSERT_EQ(0, ::gettimeofday(&times[0], nullptr));
  times[0].tv_sec -= ageSeconds;
  times[1] = times[0];
  ASSERT_EQ(0, ::utimes(path.c_str(), times));
}

} // namespace

TEST(CompileCacheTest, MakeDirsCreatesNestedPath) {
  TempTree dir;
  std::string deep = dir.path() + "/a/b/c";
  ASSERT_TRUE(compileCacheMakeDirs(deep));
  EXPECT_TRUE(dirExists(deep));
}

TEST(CompileCacheTest, MakeDirsIsIdempotent) {
  TempTree dir;
  std::string deep = dir.path() + "/a/b/c";
  ASSERT_TRUE(compileCacheMakeDirs(deep));
  EXPECT_TRUE(compileCacheMakeDirs(deep));
}

TEST(CompileCacheTest, DefaultRootHonoursXdgCacheHome) {
  EnvGuard xdgGuard("XDG_CACHE_HOME");
  EnvGuard homeGuard("HOME");
  TempTree dir;
  ::setenv("XDG_CACHE_HOME", dir.path().c_str(), 1);
  EXPECT_EQ(
      dir.path() + "/hermes-node/compile-cache", compileCacheDefaultRoot());
}

TEST(CompileCacheTest, DefaultRootFallsBackToHome) {
  EnvGuard xdgGuard("XDG_CACHE_HOME");
  EnvGuard homeGuard("HOME");
  TempTree dir;
  ::unsetenv("XDG_CACHE_HOME");
  ::setenv("HOME", dir.path().c_str(), 1);
  EXPECT_EQ(
      dir.path() + "/.cache/hermes-node/compile-cache",
      compileCacheDefaultRoot());
}

TEST(CompileCacheTest, DefaultRootIsEmptyWithoutHome) {
  EnvGuard xdgGuard("XDG_CACHE_HOME");
  EnvGuard homeGuard("HOME");
  ::unsetenv("XDG_CACHE_HOME");
  ::unsetenv("HOME");
  EXPECT_TRUE(compileCacheDefaultRoot().empty());
}

TEST(CompileCacheTest, PruneKeepsCurrentPlusThreeMostRecent) {
  TempTree dir;
  makeAgedDir(dir.path(), "gen-current", 0);
  makeAgedDir(dir.path(), "gen-1s", 1);
  makeAgedDir(dir.path(), "gen-10s", 10);
  makeAgedDir(dir.path(), "gen-100s", 100);
  makeAgedDir(dir.path(), "gen-1000s", 1000);
  ASSERT_EQ(5u, countDirEntries(dir.path()));

  compileCachePruneGenerations(dir.path(), "gen-current", 3);

  // keepCount counts the OTHERS kept, not the total: keepName is never
  // pruned, and the 3 most recently modified others survive alongside it,
  // so 4 directories remain.
  EXPECT_EQ(4u, countDirEntries(dir.path()));
  EXPECT_TRUE(dirExists(dir.path() + "/gen-current"));
  EXPECT_TRUE(dirExists(dir.path() + "/gen-1s"));
  EXPECT_TRUE(dirExists(dir.path() + "/gen-10s"));
  EXPECT_TRUE(dirExists(dir.path() + "/gen-100s"));
  EXPECT_FALSE(dirExists(dir.path() + "/gen-1000s"));
}

TEST(CompileCacheTest, PruneDoesNothingWhenUnderLimit) {
  TempTree dir;
  makeAgedDir(dir.path(), "gen-current", 0);
  makeAgedDir(dir.path(), "gen-old", 10);

  compileCachePruneGenerations(dir.path(), "gen-current", 3);

  EXPECT_EQ(2u, countDirEntries(dir.path()));
}

TEST(CompileCacheTest, PruneRemovesGenerationContents) {
  TempTree dir;
  makeAgedDir(dir.path(), "gen-current", 0);
  makeAgedDir(dir.path(), "gen-a", 10);
  makeAgedDir(dir.path(), "gen-b", 20);
  makeAgedDir(dir.path(), "gen-c", 30);

  // Give the oldest a populated fanout directory; pruning must remove it
  // recursively, not fail on a non-empty directory.
  //
  // Populate it BEFORE ageing it. Creating an entry inside a directory
  // updates that directory's own mtime, so ageing first and writing second
  // would reset gen-d to the newest generation and it would survive.
  ASSERT_TRUE(compileCacheMakeDirs(dir.path() + "/gen-d/ab"));
  {
    std::ofstream f(dir.path() + "/gen-d/ab/deadbeef", std::ios::binary);
    f << "payload";
  }
  makeAgedDir(dir.path(), "gen-d", 40);

  compileCachePruneGenerations(dir.path(), "gen-current", 3);

  EXPECT_FALSE(dirExists(dir.path() + "/gen-d"));
}

TEST(CompileCacheTest, PruneToleratesMissingRoot) {
  TempTree dir;
  // Must not crash or create anything.
  compileCachePruneGenerations(dir.path() + "/absent", "gen-current", 3);
  EXPECT_FALSE(dirExists(dir.path() + "/absent"));
}

TEST(CompileCacheTest, DisabledUntilEnabled) {
  CompileCache cache;
  EXPECT_FALSE(cache.enabled());

  CompileCacheEntry entry;
  EXPECT_FALSE(cache.lookup(
      entry, "var a = 1;", "/a/b.js", CompileCacheKind::kCommonJS));
  EXPECT_FALSE(entry.hit());
}

TEST(CompileCacheTest, EnableCreatesVersionedGenerationDir) {
  TempTree dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "0.3.0-x86_64-bc99-3f9c21ab"));
  EXPECT_TRUE(cache.enabled());
  EXPECT_EQ(
      dir.path() + "/v1/0.3.0-x86_64-bc99-3f9c21ab", cache.generationDir());
  EXPECT_TRUE(dirExists(cache.generationDir()));
}

TEST(CompileCacheTest, EnableFailsOnUnwritableRoot) {
  CompileCache cache;
  EXPECT_FALSE(cache.enable("/proc/nonexistent/cache", "gen"));
  EXPECT_FALSE(cache.enabled());
}

TEST(CompileCacheTest, EnableFailsOnEmptyRoot) {
  CompileCache cache;
  EXPECT_FALSE(cache.enable("", "gen"));
  EXPECT_FALSE(cache.enabled());
}

TEST(CompileCacheTest, SaveThenLookupHits) {
  TempTree dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));

  std::string source = "module.exports = 42;";
  auto payload = fakePayload(777, 5);

  CompileCacheEntry miss;
  EXPECT_FALSE(
      cache.lookup(miss, source, "/x/y.js", CompileCacheKind::kCommonJS));
  cache.save(miss, payload.data(), payload.size());

  CompileCacheEntry hit;
  ASSERT_TRUE(
      cache.lookup(hit, source, "/x/y.js", CompileCacheKind::kCommonJS));
  ASSERT_TRUE(hit.hit());
  EXPECT_EQ(payload.size(), hit.bytecodeSize);
  EXPECT_EQ(0, memcmp(payload.data(), hit.bytecode, payload.size()));
  hit.mapping->destroy();
}

TEST(CompileCacheTest, LookupMissesAfterSourceChanges) {
  TempTree dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));
  auto payload = fakePayload(64, 1);

  CompileCacheEntry first;
  cache.lookup(first, "var a = 1;", "/x/y.js", CompileCacheKind::kCommonJS);
  cache.save(first, payload.data(), payload.size());

  CompileCacheEntry second;
  EXPECT_FALSE(cache.lookup(
      second, "var a = 2222;", "/x/y.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, KindsDoNotShareEntries) {
  TempTree dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));
  std::string source = "var a = 1;";
  auto payload = fakePayload(64, 1);

  CompileCacheEntry cjs;
  cache.lookup(cjs, source, "/x/y.js", CompileCacheKind::kCommonJS);
  cache.save(cjs, payload.data(), payload.size());

  // Same file, same source text, different entry point: must not hit.
  CompileCacheEntry wrapped;
  EXPECT_FALSE(cache.lookup(
      wrapped, source, "/x/y.js", CompileCacheKind::kLoaderWrapped));
}

TEST(CompileCacheTest, EntriesGoInFanoutSubdirectories) {
  TempTree dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));
  auto payload = fakePayload(64, 1);

  CompileCacheEntry entry;
  cache.lookup(entry, "var a = 1;", "/x/y.js", CompileCacheKind::kCommonJS);
  cache.save(entry, payload.data(), payload.size());

  // <generationDir>/<2 hex chars>/<8 hex chars>
  std::string rel = entry.cacheFilePath.substr(cache.generationDir().size());
  ASSERT_EQ(12u, rel.size()) << entry.cacheFilePath;
  EXPECT_EQ('/', rel[0]);
  EXPECT_EQ('/', rel[3]);
}

TEST(CompileCacheTest, InvalidateRemovesTheEntry) {
  TempTree dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));
  auto payload = fakePayload(64, 1);

  CompileCacheEntry entry;
  cache.lookup(entry, "var a = 1;", "/x/y.js", CompileCacheKind::kCommonJS);
  cache.save(entry, payload.data(), payload.size());

  CompileCacheEntry hit;
  ASSERT_TRUE(
      cache.lookup(hit, "var a = 1;", "/x/y.js", CompileCacheKind::kCommonJS));
  hit.mapping->destroy();

  cache.invalidate(hit);

  CompileCacheEntry gone;
  EXPECT_FALSE(
      cache.lookup(gone, "var a = 1;", "/x/y.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, EnablePrunesOldGenerations) {
  TempTree dir;
  std::string versioned = dir.path() + "/v1";
  makeAgedDir(versioned, "gen-a", 10);
  makeAgedDir(versioned, "gen-b", 20);
  makeAgedDir(versioned, "gen-c", 30);
  makeAgedDir(versioned, "gen-d", 40);

  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen-new"));

  // gen-new plus the 3 newest others.
  EXPECT_EQ(4u, countDirEntries(versioned));
  EXPECT_TRUE(dirExists(versioned + "/gen-new"));
  EXPECT_FALSE(dirExists(versioned + "/gen-d"));
}

// Two distinct codegen configurations, in the shape Hermes actually hands
// over: an opaque, self-describing byte string rather than a packed word.
static const uint8_t kCfgA[] = "hermes-wasm;bc=100;cg=1;t262=0";
static const uint8_t kCfgB[] = "hermes-wasm;bc=100;cg=1;t262=1";
#define CFG_A kCfgA, sizeof(kCfgA) - 1
#define CFG_B kCfgB, sizeof(kCfgB) - 1

/// CompileCache::lookupWasm() takes the digest its caller derived, because
/// the caller keys a container's baked Wasm table and a --record-wasm file
/// on the same identity and must not derive it twice. These tests still
/// think in terms of (module, codegen configuration), so this derives it for
/// them exactly as the Wasm cache hooks do.
static bool wasmLookup(
    CompileCache &cache,
    CompileCacheEntry &entry,
    const uint8_t *wasm,
    size_t size,
    const uint8_t *codegenConfig,
    size_t codegenConfigSize) {
  return cache.lookupWasm(
      entry,
      compileCacheWasmDigest(codegenConfig, codegenConfigSize, wasm, size),
      wasm,
      size);
}

TEST(CompileCacheTest, WasmDigestIsStableAndLowercaseHex) {
  const uint8_t bytes[] = {1, 2, 3, 4};
  std::string a = compileCacheWasmDigest(CFG_A, bytes, sizeof(bytes));
  std::string b = compileCacheWasmDigest(CFG_A, bytes, sizeof(bytes));
  EXPECT_EQ(a, b);
  EXPECT_EQ(64u, a.size());
  for (char c : a)
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
}

TEST(CompileCacheTest, WasmDigestDiffersByContent) {
  const uint8_t a[] = {1, 2, 3, 4};
  const uint8_t b[] = {1, 2, 3, 5};
  EXPECT_NE(
      compileCacheWasmDigest(CFG_A, a, sizeof(a)),
      compileCacheWasmDigest(CFG_A, b, sizeof(b)));
}

TEST(CompileCacheTest, WasmDigestDiffersByCodegenConfig) {
  // This is the silent failure the whole scheme guards against: the same
  // module compiled under a different configuration must not share an entry.
  const uint8_t bytes[] = {1, 2, 3, 4};
  EXPECT_NE(
      compileCacheWasmDigest(CFG_A, bytes, sizeof(bytes)),
      compileCacheWasmDigest(CFG_B, bytes, sizeof(bytes)));
}

TEST(CompileCacheTest, WasmDigestCannotResplitConfigAndModule) {
  // The config is variable-length now, so hashing config||module alone would
  // be ambiguous: these two are different compiles that a naive
  // concatenation gives one digest to.
  const uint8_t cfg1[] = {'a', 'b'};
  const uint8_t mod1[] = {'c'};
  const uint8_t cfg2[] = {'a'};
  const uint8_t mod2[] = {'b', 'c'};
  EXPECT_NE(
      compileCacheWasmDigest(cfg1, sizeof(cfg1), mod1, sizeof(mod1)),
      compileCacheWasmDigest(cfg2, sizeof(cfg2), mod2, sizeof(mod2)));
}

TEST(CompileCacheTest, WasmDigestOfEmptyInputIsWellDefined) {
  EXPECT_EQ(64u, compileCacheWasmDigest(CFG_A, nullptr, 0).size());
}

TEST(CompileCacheTest, WasmEntryRoundTripsThroughADirectory) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  const uint8_t wasm[] = {0, 97, 115, 109, 1, 0, 0, 0};
  const uint8_t bytecode[] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

  CompileCacheEntry miss;
  EXPECT_FALSE(wasmLookup(cache, miss, wasm, sizeof(wasm), CFG_A));
  cache.saveWasm(miss, bytecode, sizeof(bytecode));

  CompileCacheEntry hit;
  ASSERT_TRUE(wasmLookup(cache, hit, wasm, sizeof(wasm), CFG_A));
  ASSERT_EQ(sizeof(bytecode), hit.bytecodeSize);
  EXPECT_EQ(0, memcmp(bytecode, hit.bytecode, sizeof(bytecode)));
  hit.mapping->destroy();

  // A different codegen config must miss.
  CompileCacheEntry other;
  EXPECT_FALSE(wasmLookup(cache, other, wasm, sizeof(wasm), CFG_B));

  // A one-byte edit must miss.
  uint8_t edited[sizeof(wasm)];
  memcpy(edited, wasm, sizeof(wasm));
  edited[7] = 1;
  CompileCacheEntry edit;
  EXPECT_FALSE(wasmLookup(cache, edit, edited, sizeof(edited), CFG_A));
}

TEST(CompileCacheTest, EvictionSpansEveryGeneration) {
  // The budget names the cache, not one directory inside it. Older
  // generations are kept (kCompileCacheGenerationsKept) and each held its own
  // budget's worth back when it was current, so a sweep confined to the
  // current one leaves on-disk use at a multiple of the configured number --
  // 4x at the default, about 1 GB against a 256 MB setting.
  TempTree tree;
  std::string versionedRoot = tree.path() + "/v1";

  // A stale generation with one entry in it, written directly: nothing reads
  // an old generation, so there is no API that puts an entry there.
  std::string staleFan = versionedRoot + "/old-generation/ab";
  ASSERT_TRUE(compileCacheMakeDirs(staleFan));
  std::string stalePath = staleFan + "/w" + std::string(64, 'a');
  {
    std::ofstream f(stalePath, std::ios::binary);
    std::vector<uint8_t> bytes(200, 0xcd);
    f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  ASSERT_TRUE(fileExists(stalePath));

  // The current generation, one entry, written through the cache.
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "current-generation"));
  CacheConfig config;
  config.maxWasmBytes = 150; // smaller than either entry alone
  config.recency = CacheRecency::kMtime;
  cache.setConfig(config);

  uint8_t wasm[] = {0, 97, 115, 109, 1, 0, 0, 9};
  CompileCacheEntry e;
  EXPECT_FALSE(wasmLookup(cache, e, wasm, sizeof(wasm), CFG_A));
  std::vector<uint8_t> payload(100, 0xab);
  cache.saveWasm(e, payload.data(), payload.size());

  compileCacheEvictWasm(versionedRoot, config);

  // The stale generation's entry is gone: it is unreachable -- the running
  // binary only ever looks in its own generation -- so it is the first thing
  // that should go, and sorting by timestamp gets that for free without a
  // rule about generations.
  EXPECT_FALSE(fileExists(stalePath));
}

TEST(CompileCacheTest, EvictionReapsStaleTempFilesButNotFreshOnes) {
  // An interrupted write leaves "w<digest>.<pid>.<n>.tmp" behind. The sweep
  // must not delete one a live process is still writing -- that is why
  // isWasmEntryName refuses them -- but nothing else ever collected them
  // either, so they leaked a full-size entry per crash and did not count
  // against the budget. Reap by age: an hour is far beyond the milliseconds
  // a real write takes.
  TempTree tree;
  std::string versionedRoot = tree.path() + "/v1";
  std::string fan = versionedRoot + "/g/ab";
  ASSERT_TRUE(compileCacheMakeDirs(fan));

  std::string digest(64, 'b');
  std::string stale = fan + "/w" + digest + ".1234.0.tmp";
  std::string fresh = fan + "/w" + digest + ".5678.0.tmp";
  for (const std::string &p : {stale, fresh}) {
    std::ofstream f(p, std::ios::binary);
    f << "partial";
  }

  // Age the stale one two hours into the past.
  struct stat st {};
  ASSERT_EQ(0, ::stat(stale.c_str(), &st));
  struct timespec times[2];
  times[0].tv_sec = st.st_atime - 7200;
  times[0].tv_nsec = 0;
  times[1].tv_sec = st.st_mtime - 7200;
  times[1].tv_nsec = 0;
  ASSERT_EQ(0, ::utimensat(AT_FDCWD, stale.c_str(), times, 0));

  CacheConfig config;
  config.maxWasmBytes = 0; // evict everything, so nothing else is in play
  compileCacheEvictWasm(versionedRoot, config);

  EXPECT_FALSE(fileExists(stale)) << "a stale temp file must be reaped";
  EXPECT_TRUE(fileExists(fresh))
      << "a temp file young enough to belong to a live writer must survive";
}

TEST(CompileCacheTest, EvictionKeepsTheBudget) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  CacheConfig config;
  config.maxWasmBytes = 300; // room for two of the entries below, not three
  config.recency = CacheRecency::kMtime;
  cache.setConfig(config);

  // Three entries of ~100 bytes of payload each, written oldest first.
  std::vector<uint8_t> payload(100, 0xab);
  for (int i = 0; i < 3; ++i) {
    uint8_t wasm[] = {0, 97, 115, 109, 1, 0, 0, static_cast<uint8_t>(i)};
    CompileCacheEntry e;
    EXPECT_FALSE(wasmLookup(cache, e, wasm, sizeof(wasm), CFG_A));
    cache.saveWasm(e, payload.data(), payload.size());
    // Distinct timestamps, since the sweep orders by them.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  }

  compileCacheEvictWasm(cache.versionedRoot(), config);

  // The oldest is gone; the newest survives.
  uint8_t oldest[] = {0, 97, 115, 109, 1, 0, 0, 0};
  uint8_t newest[] = {0, 97, 115, 109, 1, 0, 0, 2};
  CompileCacheEntry a, b;
  EXPECT_FALSE(wasmLookup(cache, a, oldest, sizeof(oldest), CFG_A));
  EXPECT_TRUE(wasmLookup(cache, b, newest, sizeof(newest), CFG_A));
  if (b.mapping)
    b.mapping->destroy();
}

TEST(CompileCacheTest, EvictionLeavesJavaScriptEntriesAlone) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  CompileCacheEntry js;
  std::string source = "1 + 1";
  EXPECT_FALSE(
      cache.lookup(js, source, "/tmp/x.js", CompileCacheKind::kCommonJS));
  const uint8_t bytecode[] = {1, 2, 3, 4};
  cache.save(js, bytecode, sizeof(bytecode));

  CacheConfig config;
  config.maxWasmBytes = 0; // evict every Wasm entry
  compileCacheEvictWasm(cache.versionedRoot(), config);

  CompileCacheEntry again;
  EXPECT_TRUE(
      cache.lookup(again, source, "/tmp/x.js", CompileCacheKind::kCommonJS));
  if (again.mapping)
    again.mapping->destroy();
}

TEST(CompileCacheTest, EvictionIgnoresLeftoverTempFiles) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  // A leftover temp file from an interrupted saveWasm(): its name starts
  // with 'w', like a real entry, but is not exactly "w" + 64 hex digits, so
  // it must not be mistaken for one.
  std::string digest(64, '0');
  for (size_t i = 0; i < digest.size(); ++i)
    digest[i] = "0123456789abcdef"[i % 16];
  std::string fanDir = cache.generationDir() + "/" + digest.substr(0, 2);
  ASSERT_TRUE(compileCacheMakeDirs(fanDir));
  std::string tempPath = fanDir + "/w" + digest + ".1234.0.tmp";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out << "leftover";
  }

  CacheConfig config;
  config.maxWasmBytes = 0; // would evict every real Wasm entry
  compileCacheEvictWasm(cache.versionedRoot(), config);

  struct stat st {};
  EXPECT_EQ(0, ::stat(tempPath.c_str(), &st))
      << "a leftover temp file must not be swept";
}

TEST(CompileCacheTest, EvictionWithZeroBudgetRemovesARealWasmEntry) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  const uint8_t wasm[] = {0, 97, 115, 109, 1, 0, 0, 0};
  const uint8_t bytecode[] = {9, 8, 7, 6};

  CompileCacheEntry miss;
  EXPECT_FALSE(wasmLookup(cache, miss, wasm, sizeof(wasm), CFG_A));
  cache.saveWasm(miss, bytecode, sizeof(bytecode));

  CompileCacheEntry hit;
  ASSERT_TRUE(wasmLookup(cache, hit, wasm, sizeof(wasm), CFG_A));
  if (hit.mapping)
    hit.mapping->destroy();

  CacheConfig config;
  config.maxWasmBytes = 0; // 0 means evict everything, not "no limit"
  compileCacheEvictWasm(cache.versionedRoot(), config);

  CompileCacheEntry again;
  EXPECT_FALSE(wasmLookup(cache, again, wasm, sizeof(wasm), CFG_A));
}

#include <hermes/node-compat/compile-cache/source_buffer.h>

namespace {

/// Minimal concrete SourceBuffer for tests. The base constructor is
/// protected, so an unterminated buffer needs a subclass to build one.
class TestSourceBuffer final : public SourceBuffer {
 public:
  TestSourceBuffer(const char *data, size_t size, bool nulTerminated)
      : SourceBuffer(data, size, nulTerminated) {}
};

} // namespace

TEST(CompileCacheTest, SourceBufferFromStringIsTerminated) {
  std::string s = "var a = 1;";
  BorrowedStringSourceBuffer buf(s);
  EXPECT_EQ(s.data(), buf.data());
  EXPECT_EQ(s.size(), buf.size());
  EXPECT_TRUE(buf.isNulTerminated());
  // size() never counts the terminator; readableSize() does when there is one.
  EXPECT_EQ(s.size() + 1, buf.readableSize());
}

TEST(CompileCacheTest, SourceBufferUnterminatedReadableSizeIsSize) {
  // A range covering only part of a larger string: no terminator follows the
  // last byte, so readableSize() must not claim one.
  std::string backing = "abcdefgh";
  TestSourceBuffer buf(backing.data(), 4, false);
  EXPECT_FALSE(buf.isNulTerminated());
  EXPECT_EQ(4u, buf.size());
  EXPECT_EQ(4u, buf.readableSize());
}

TEST(CompileCacheTest, SourceBufferAcceptsEmpty) {
  TestSourceBuffer buf(nullptr, 0, false);
  EXPECT_EQ(0u, buf.size());
  EXPECT_EQ(0u, buf.readableSize());
}

TEST(CompileCacheTest, SourceBufferEmptyStringIsTerminated) {
  std::string empty;
  BorrowedStringSourceBuffer buf(empty);
  EXPECT_EQ(0u, buf.size());
  EXPECT_TRUE(buf.isNulTerminated());
  EXPECT_EQ(1u, buf.readableSize());
}

TEST(CompileCacheTest, KeyAcceptsANonOwningView) {
  // The widening is real only if a view that is not backed by a std::string
  // produces the same key. A char array with no terminator inside the range
  // would not compile against a const std::string & parameter.
  const char raw[] = "/a/b/c.jsXXXX";
  std::string_view view(raw, 9); // "/a/b/c.js"
  EXPECT_EQ(
      compileCacheKey(std::string("/a/b/c.js"), CompileCacheKind::kCommonJS),
      compileCacheKey(view, CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, KeyStaysKindDistinctForAnEmptyFilename) {
  // zlib's crc32 returns 0 outright for a NULL buffer rather than leaving
  // the accumulated value alone, so an empty filename must not be allowed
  // to discard the kind byte and collapse every kind onto one key.
  uint32_t cjs =
      compileCacheKey(std::string_view(), CompileCacheKind::kCommonJS);
  uint32_t wrapped =
      compileCacheKey(std::string_view(), CompileCacheKind::kLoaderWrapped);
  uint32_t wrappedTs =
      compileCacheKey(std::string_view(), CompileCacheKind::kLoaderWrappedTS);
  EXPECT_NE(cjs, wrapped);
  EXPECT_NE(wrapped, wrappedTs);
  EXPECT_NE(cjs, wrappedTs);
  EXPECT_NE(0u, cjs);
  // An empty std::string and a default-constructed view must agree.
  EXPECT_EQ(cjs, compileCacheKey(std::string(), CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, GenerationNameAcceptsNonOwningViews) {
  const char rawVersion[] = "0.3.0ZZZ";
  const char rawArch[] = "x86_64ZZZ";
  EXPECT_EQ(
      "0.3.0-x86_64-bc99-3f9c21ab",
      compileCacheGenerationName(
          std::string_view(rawVersion, 5),
          std::string_view(rawArch, 6),
          99,
          0x3f9c21ab));
}

TEST(CacheConfigTest, DefaultsAreAtimeAnd256MB) {
  CacheConfig c = cacheConfigParse("");
  EXPECT_EQ(CacheRecency::kAtime, c.recency);
  EXPECT_EQ(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ParsesBothKeys) {
  CacheConfig c = cacheConfigParse(
      "# a comment\n"
      "recency: mtime\n"
      "max_wasm_bytes: 1024\n");
  EXPECT_EQ(CacheRecency::kMtime, c.recency);
  EXPECT_EQ(1024ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ToleratesWhitespaceAndBlankLines) {
  CacheConfig c = cacheConfigParse("\n   recency:   mtime   \n\n");
  EXPECT_EQ(CacheRecency::kMtime, c.recency);
}

TEST(CacheConfigTest, UnknownKeyIsIgnored) {
  CacheConfig c = cacheConfigParse("nonsense: yes\nrecency: mtime\n");
  EXPECT_EQ(CacheRecency::kMtime, c.recency);
}

TEST(CacheConfigTest, BadValueFallsBackToTheDefault) {
  CacheConfig c =
      cacheConfigParse("recency: yesterday\nmax_wasm_bytes: lots\n");
  EXPECT_EQ(CacheRecency::kAtime, c.recency);
  EXPECT_EQ(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ZeroBudgetIsHonoured) {
  // 0 means "evict everything", not "no limit"; it must not be mistaken for
  // an unset value.
  CacheConfig c = cacheConfigParse("max_wasm_bytes: 0\n");
  EXPECT_EQ(0ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, NegativeBudgetFallsBackToTheDefault) {
  // strtoull accepts a leading '-' and negates the magnitude; a byte count
  // must not silently become 2^64-1.
  CacheConfig c = cacheConfigParse("max_wasm_bytes: -1\n");
  EXPECT_EQ(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, WhitespaceHiddenSignFallsBackToTheDefault) {
  // trim() removes spaces and tabs; strtoull skips a wider set of its own,
  // so a vertical tab or form feed hides a sign from a first-character
  // check and the negation yields 2^64-1. Both must reach the default.
  EXPECT_EQ(
      268435456ull, cacheConfigParse("max_wasm_bytes: \v-1\n").maxWasmBytes);
  EXPECT_EQ(
      268435456ull, cacheConfigParse("max_wasm_bytes: \f-1\n").maxWasmBytes);
}

TEST(CacheConfigTest, NonDigitsFallBackToTheDefault) {
  // A byte count is digits and nothing else.
  EXPECT_EQ(
      268435456ull, cacheConfigParse("max_wasm_bytes: 0x400\n").maxWasmBytes);
  EXPECT_EQ(
      268435456ull, cacheConfigParse("max_wasm_bytes: 1024x\n").maxWasmBytes);
  EXPECT_EQ(
      268435456ull, cacheConfigParse("max_wasm_bytes: \v1024\n").maxWasmBytes);
  // And a plain one still parses.
  EXPECT_EQ(1024ull, cacheConfigParse("max_wasm_bytes: 1024\n").maxWasmBytes);
}

TEST(CacheConfigTest, PlusSignedBudgetFallsBackToTheDefault) {
  CacheConfig c = cacheConfigParse("max_wasm_bytes: +1024\n");
  EXPECT_EQ(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ATruncatedDefaultFileIsNotEquivalentToTheDefaults) {
  // Why cacheConfigLoadOrCreate publishes atomically rather than writing the
  // default text in place: a PREFIX of that text is well-formed. Truncated
  // mid-number it yields a two-byte budget, which evicts everything on the
  // next store -- so a reader catching a partial write, or a short write
  // left on disk, would not simply see the defaults.
  CacheConfig c = cacheConfigParse("recency: atime\nmax_wasm_bytes: 2");
  EXPECT_EQ(2ull, c.maxWasmBytes);
  EXPECT_NE(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ComplainsAboutEveryLineItDidNotUnderstand) {
  std::vector<std::string> complaints;
  CacheConfig c = cacheConfigParse(
      "# a comment\n"
      "\n"
      "recency: banana\n"
      "max_wasm_bytes: 500MB\n"
      "nonsense\n"
      "foo: bar\n",
      &complaints);

  // Every bad line still falls back, which is the contract the warnings
  // exist to make visible rather than replace.
  EXPECT_EQ(CacheRecency::kAtime, c.recency);
  EXPECT_EQ(268435456ull, c.maxWasmBytes);

  ASSERT_EQ(4u, complaints.size());
  EXPECT_EQ(
      "line 3: recency must be 'atime' or 'mtime', found 'banana'; "
      "using 'atime'",
      complaints[0]);
  EXPECT_EQ(
      "line 4: max_wasm_bytes must be a decimal byte count, found '500MB'; "
      "using 268435456",
      complaints[1]);
  EXPECT_EQ(
      "line 5: expected '<key>: <value>', found 'nonsense'", complaints[2]);
  EXPECT_EQ("line 6: unknown key 'foo'; ignored", complaints[3]);
}

TEST(CacheConfigTest, SaysNothingAboutAWellFormedFile) {
  // Comments, blank lines and whitespace-only lines are not complaints; a
  // warning printed on every run of a correct configuration would train the
  // reader to ignore the ones that matter.
  std::vector<std::string> complaints;
  cacheConfigParse(
      "# hermes-node compile cache configuration.\n"
      "\n"
      "   \n"
      "recency: mtime\n"
      "max_wasm_bytes: 1024\n",
      &complaints);
  EXPECT_TRUE(complaints.empty())
      << complaints.size()
      << " complaint(s), first: " << (complaints.empty() ? "" : complaints[0]);
}

TEST(CacheConfigTest, ComplaintRendersAnInvisibleCharacterVisibly) {
  // "\v-1" hides its sign behind a vertical tab. A complaint that echoed the
  // value raw would look like it was objecting to "-1" for no reason.
  std::vector<std::string> complaints;
  cacheConfigParse("max_wasm_bytes: \v-1\n", &complaints);
  ASSERT_EQ(1u, complaints.size());
  EXPECT_NE(std::string::npos, complaints[0].find("'\\x0b-1'"));
}

TEST(CacheConfigTest, LoadOrCreateWritesDefaultsThenReads) {
  TempTree dir;

  // First call: directory is empty, so defaults should be written and
  // returned.
  CacheConfig first = cacheConfigLoadOrCreate(dir.path());
  EXPECT_EQ(CacheRecency::kAtime, first.recency);
  EXPECT_EQ(268435456ull, first.maxWasmBytes);

  // Verify the config file was created.
  std::string configPath = dir.path() + "/config";
  {
    std::ifstream f(configPath, std::ios::binary);
    EXPECT_TRUE(f.good()) << "Config file should exist";
  }

  // Write a non-default config to the file.
  {
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    out << "recency: mtime\n"
        << "max_wasm_bytes: 512\n";
  }

  // Second call: should read back the new values.
  CacheConfig second = cacheConfigLoadOrCreate(dir.path());
  EXPECT_EQ(CacheRecency::kMtime, second.recency);
  EXPECT_EQ(512ull, second.maxWasmBytes);
}
