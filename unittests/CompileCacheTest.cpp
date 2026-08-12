/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

/// A temporary directory removed on destruction.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/hncc-test-XXXXXX";
    const char *made = ::mkdtemp(tmpl);
    EXPECT_NE(nullptr, made);
    path_ = made ? made : "";
  }
  ~TempDir() {
    if (!path_.empty())
      ::system(("rm -rf " + path_).c_str());
  }
  const std::string &path() const {
    return path_;
  }

 private:
  std::string path_;
};

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
  TempDir dir;
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
  TempDir dir;
  CompileCacheEntry entry = makeEntry(dir.path() + "/nope", "x");
  EXPECT_FALSE(compileCacheReadEntry(entry));
  EXPECT_FALSE(entry.hit());
}

TEST(CompileCacheTest, ReadMissesWhenSourceChanged) {
  TempDir dir;
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
  TempDir dir;
  std::string file = dir.path() + "/entry";
  auto payload = fakePayload(64, 1);
  CompileCacheEntry written = makeEntry(file, "var a = 1;");
  ASSERT_TRUE(
      compileCacheWriteEntry(file, written, payload.data(), payload.size()));

  CompileCacheEntry read = makeEntry(file, "var a = 1; var b = 2;");
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, ReadMissesOnBadMagic) {
  TempDir dir;
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
  TempDir dir;
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
  TempDir dir;
  std::string file = dir.path() + "/entry";
  {
    std::ofstream f(file, std::ios::binary);
    f << "junk";
  }
  CompileCacheEntry read = makeEntry(file, "x");
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, WriteLeavesNoTempFileBehind) {
  TempDir dir;
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

bool dirExists(const std::string &path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// Saves an environment variable on construction and restores it on
/// destruction. GoogleTest runs every test in one process, so a test that
/// changes the environment must put it back -- otherwise the next test
/// reads the mutated value as if it were the original.
///
/// Declare one before any TempDir it interacts with: destructors run in
/// reverse order, so the TempDir is removed first and the variable is
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
  TempDir dir;
  std::string deep = dir.path() + "/a/b/c";
  ASSERT_TRUE(compileCacheMakeDirs(deep));
  EXPECT_TRUE(dirExists(deep));
}

TEST(CompileCacheTest, MakeDirsIsIdempotent) {
  TempDir dir;
  std::string deep = dir.path() + "/a/b/c";
  ASSERT_TRUE(compileCacheMakeDirs(deep));
  EXPECT_TRUE(compileCacheMakeDirs(deep));
}

TEST(CompileCacheTest, DefaultRootHonoursXdgCacheHome) {
  EnvGuard xdgGuard("XDG_CACHE_HOME");
  EnvGuard homeGuard("HOME");
  TempDir dir;
  ::setenv("XDG_CACHE_HOME", dir.path().c_str(), 1);
  EXPECT_EQ(
      dir.path() + "/hermes-node/compile-cache", compileCacheDefaultRoot());
}

TEST(CompileCacheTest, DefaultRootFallsBackToHome) {
  EnvGuard xdgGuard("XDG_CACHE_HOME");
  EnvGuard homeGuard("HOME");
  TempDir dir;
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
  TempDir dir;
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
  TempDir dir;
  makeAgedDir(dir.path(), "gen-current", 0);
  makeAgedDir(dir.path(), "gen-old", 10);

  compileCachePruneGenerations(dir.path(), "gen-current", 3);

  EXPECT_EQ(2u, countDirEntries(dir.path()));
}

TEST(CompileCacheTest, PruneRemovesGenerationContents) {
  TempDir dir;
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
  TempDir dir;
  // Must not crash or create anything.
  compileCachePruneGenerations(dir.path() + "/absent", "gen-current", 3);
  EXPECT_FALSE(dirExists(dir.path() + "/absent"));
}
