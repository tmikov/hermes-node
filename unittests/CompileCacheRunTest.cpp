/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache_run.h>

#include "napi/hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/hermes.h"

#include <gtest/gtest.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace hermes::node_compat;

namespace {

/// A temporary directory removed on destruction.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/hnccrun-test-XXXXXX";
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

/// The single entry file under a populated cache root, or "" if there is not
/// exactly one.
std::string soleEntryPath(const std::string &root) {
  std::string found;
  size_t count = 0;
  std::string cmd = "find " + root + " -type f";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (!pipe)
    return "";
  char line[4096];
  while (::fgets(line, sizeof(line), pipe)) {
    size_t n = ::strlen(line);
    if (n && line[n - 1] == '\n')
      line[n - 1] = '\0';
    found = line;
    ++count;
  }
  ::pclose(pipe);
  return count == 1 ? found : "";
}

std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(
      std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeFile(const std::string &path, const std::string &bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

/// Test fixture with a Hermes Runtime and napi_env, mirroring
/// unittests/ModuleLoaderTest.cpp.
class CompileCacheRunTest : public ::testing::Test {
 protected:
  std::unique_ptr<facebook::hermes::HermesRuntime> rt_;
  napi_env env_ = nullptr;
  napi_handle_scope scope_ = nullptr;

  void SetUp() override {
    auto config = hermes::vm::RuntimeConfig::Builder()
                      .withGCConfig(hermes::vm::GCConfig::Builder()
                                        .withInitHeapSize(1 << 20)
                                        .withMaxHeapSize(1 << 24)
                                        .build())
                      .withES6BlockScoping(true)
                      .withEnableAsyncGenerators(true)
                      .build();
    rt_ = facebook::hermes::makeHermesRuntime(config);
    env_ = hermes_napi_create_env(rt_->getVMRuntimeUnsafe());
    ASSERT_EQ(napi_open_handle_scope(env_, &scope_), napi_ok);
  }

  void TearDown() override {
    if (scope_) {
      napi_close_handle_scope(env_, scope_);
      scope_ = nullptr;
    }
    env_ = nullptr;
    rt_.reset();
  }

  /// True if an exception was pending; clears it either way.
  bool clearException() {
    bool pending = false;
    napi_is_exception_pending(env_, &pending);
    if (pending) {
      napi_value exc;
      napi_get_and_clear_last_exception(env_, &exc);
    }
    return pending;
  }

  /// Run \p source with no wrapper under kLoaderWrapped, which is the kind
  /// whose caller hands over already-wrapped text.
  napi_status run(
      CompileCache &cache,
      const std::string &source,
      const char *url,
      napi_value *out) {
    BorrowedStringSourceBuffer buf(source);
    return compileCacheRun(
        env_, cache, CompileCacheKind::kLoaderWrapped, buf, "", "", url, out);
  }

  /// The double value of \p v, or NaN if it is not a number.
  double asNumber(napi_value v) {
    double d = 0;
    if (napi_get_value_double(env_, v, &d) != napi_ok)
      return std::nan("");
    return d;
  }
};

TEST_F(CompileCacheRunTest, MissCompilesPersistsAndRuns) {
  TempDir dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, run(cache, "40 + 2", "/x/y.js", &result));
  EXPECT_DOUBLE_EQ(42.0, asNumber(result));
  EXPECT_FALSE(clearException());

  // Exactly one entry was written.
  EXPECT_FALSE(soleEntryPath(dir.path()).empty());
}

TEST_F(CompileCacheRunTest, HitRunsBytecodeFromTheEntry) {
  // Proving a hit requires observing that the CACHE FILE's bytecode ran,
  // not merely that the call succeeded. Build an entry for "40 + 2", then
  // overwrite its payload with the bytecode for "40 + 3" while leaving the
  // header -- which identifies the source as "40 + 2" -- intact. A lookup
  // for "40 + 2" then validates, maps the swapped payload, and yields 43.
  TempDir dirA;
  TempDir dirB;
  CompileCache cacheA;
  CompileCache cacheB;
  ASSERT_TRUE(cacheA.enable(dirA.path(), "gen"));
  ASSERT_TRUE(cacheB.enable(dirB.path(), "gen"));

  napi_value ignored = nullptr;
  ASSERT_EQ(napi_ok, run(cacheA, "40 + 2", "/x/y.js", &ignored));
  ASSERT_EQ(napi_ok, run(cacheB, "40 + 3", "/x/y.js", &ignored));

  std::string entryA = soleEntryPath(dirA.path());
  std::string entryB = soleEntryPath(dirB.path());
  ASSERT_FALSE(entryA.empty());
  ASSERT_FALSE(entryB.empty());

  std::string bytesA = readFile(entryA);
  std::string bytesB = readFile(entryB);
  // Both sources are the same shape, so their bytecode should be the same
  // length; swapping payloads then needs no header surgery. Assert it rather
  // than assume it, so a Hermes change that breaks the assumption fails here
  // loudly instead of corrupting the entry silently.
  ASSERT_EQ(bytesA.size(), bytesB.size())
      << "test assumes these two sources compile to equal-length bytecode";
  ASSERT_GT(bytesA.size(), kCompileCacheHeaderSize);

  // A's header, B's payload.
  std::string swapped = bytesA.substr(0, kCompileCacheHeaderSize) +
      bytesB.substr(kCompileCacheHeaderSize);
  writeFile(entryA, swapped);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, run(cacheA, "40 + 2", "/x/y.js", &result));
  EXPECT_DOUBLE_EQ(43.0, asNumber(result))
      << "expected the entry's bytecode to run, not a recompile of the source";
  EXPECT_FALSE(clearException());
}

TEST_F(CompileCacheRunTest, CorruptEntryRecompilesLeavingNoPendingException) {
  TempDir dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));

  napi_value ignored = nullptr;
  ASSERT_EQ(napi_ok, run(cache, "40 + 2", "/x/y.js", &ignored));

  // Zero the payload, leaving our own header valid so the failure lands on
  // the Hermes bytecode path rather than being rejected as a bad header.
  std::string entry = soleEntryPath(dir.path());
  ASSERT_FALSE(entry.empty());
  std::string bytes = readFile(entry);
  ASSERT_GT(bytes.size(), kCompileCacheHeaderSize);
  for (size_t i = kCompileCacheHeaderSize; i < bytes.size(); ++i)
    bytes[i] = '\0';
  writeFile(entry, bytes);

  napi_value result = nullptr;
  EXPECT_EQ(napi_ok, run(cache, "40 + 2", "/x/y.js", &result));
  EXPECT_DOUBLE_EQ(42.0, asNumber(result));
  // The swallow must leave nothing pending. A stale pending exception would
  // corrupt the NEXT unrelated napi call, which is invisible end to end.
  EXPECT_FALSE(clearException())
      << "swallowing a cached-bytecode failure left an exception pending";
}

TEST_F(CompileCacheRunTest, WrapperIsNotPartOfTheCacheKey) {
  // The helper hashes the source alone and compiles source+wrapper. That is
  // what lets the wrapper text live in the generation directory name rather
  // than being hashed per entry, so two calls differing only in wrapper must
  // reuse one entry rather than creating two.
  TempDir dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));

  std::string source = "40 + 2";
  BorrowedStringSourceBuffer buf(source);
  napi_value result = nullptr;

  ASSERT_EQ(
      napi_ok,
      compileCacheRun(
          env_,
          cache,
          CompileCacheKind::kLoaderWrapped,
          buf,
          "(",
          ")",
          "/x/y.js",
          &result));
  ASSERT_EQ(
      napi_ok,
      compileCacheRun(
          env_,
          cache,
          CompileCacheKind::kLoaderWrapped,
          buf,
          "((",
          "))",
          "/x/y.js",
          &result));

  // One entry, not two: the differing wrappers did not change the key.
  EXPECT_FALSE(soleEntryPath(dir.path()).empty());
}

TEST_F(CompileCacheRunTest, RealSyntaxErrorPropagatesWithExceptionPending) {
  TempDir dir;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(dir.path(), "gen"));

  napi_value result = nullptr;
  EXPECT_NE(napi_ok, run(cache, "function ( { oops", "/x/bad.js", &result));

  bool pending = false;
  ASSERT_EQ(napi_ok, napi_is_exception_pending(env_, &pending));
  EXPECT_TRUE(pending) << "a genuine compile error must not be swallowed";

  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));
  napi_value nameVal = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, exc, "name", &nameVal));
  char name[64] = {0};
  size_t nameLen = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, nameVal, name, sizeof(name), &nameLen));
  EXPECT_STREQ("SyntaxError", name);
}
