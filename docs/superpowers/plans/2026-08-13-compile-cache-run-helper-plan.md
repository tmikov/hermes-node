# Compile-Cache Execution Helper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the two copies of the compile cache's hit/miss/fallback sequence with one helper, and introduce the buffer type that makes memory-mapped zero-copy source expressible later.

**Architecture:** A header-only `SourceBuffer` records whether a NUL follows a source range, so a caller never has to decide whether to add one to a length. A new `hermesNodeCompileCacheRun` target holds `compileCacheRun`, which owns the whole cache-enabled sequence; both entry points call it and keep only their own uncached fallback. Compile and run flags are derived from the cache kind rather than passed alongside it.

**Tech Stack:** C++17, CMake + Ninja, Clang, Hermes NAPI compile API, GTest, LLVM Lit.

**Design doc:** `docs/superpowers/specs/2026-08-13-compile-cache-run-helper-design.md`

## Global Constraints

- Always build with Clang, never GCC. `cmake-build-asan` (Debug + ASAN) is already configured -- do not re-run cmake configure by hand.
- Copyright header on every new file: `Copyright (c) Tzvetan Mikov.` (NOT Meta Platforms), followed by the MIT license paragraph used by every other file in this repository.
- Commit messages: ASCII only, no emojis.
- `git add` your changes first, then run `./utils/format.sh -f`. **Then run `./utils/format.sh --check` and confirm it reports "All files are properly formatted" before committing.** `-f` is fire-and-forget and its own usage text describes it as formatting files in the *last commit*, so it can silently miss an edit made after you ran it. `--check` is a dry run over the whole tree and is what CI runs, so it is the only verification that matches the gate.
- Run `cmake --build cmake-build-asan --target check-hermes-node` before committing. Baseline is 129 unit tests and 153 JS tests passing.
- Never `git add hermes` -- the submodule pin must not change.
- No changes under `libjs/`, `libjs-node/`, or `libjs/shims/`. Files under `test/` and `unittests/` are in scope.
- Unit test target names must end in `Test` (singular). `unittests/lit.cfg` discovers GoogleTest binaries by that suffix; a name ending in `Tests` is silently never collected.
- **This is a behaviour-preserving refactor except where a task says otherwise.** The seven compile-cache lit tests (`compile-cache-enable.js`, `-cjs.js`, `-typescript.ts`, `-corrupt.js`, `-syntax-error.js`, `-ts-throws.js`, `-inspect.js`) must pass unchanged throughout. If one starts failing, the refactor broke something -- do not edit the test.
- `hermesNodeCompileCache` links `zlib_a` and nothing else and includes no Hermes header. Keep it that way; that is what the new second target is for.
- Platform is Linux/macOS. No Windows support.

---

### Task 1: `SourceBuffer`

**Files:**
- Create: `include/hermes/node-compat/compile-cache/source_buffer.h`
- Modify: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Produces: class `SourceBuffer` in namespace `hermes::node_compat`, with `const char *data() const`, `size_t size() const`, `bool isNulTerminated() const`, `size_t readableSize() const`, and a protected constructor `SourceBuffer(const char *data, size_t size, bool nulTerminated)`. Class `BorrowedStringSourceBuffer final : public SourceBuffer` with `explicit BorrowedStringSourceBuffer(const std::string &)`. Tasks 3, 4 and 5 consume both.

- [ ] **Step 1: Write the failing tests**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
```

Expected: build failure -- `source_buffer.h` does not exist.

- [ ] **Step 3: Write the header**

Create `include/hermes/node-compat/compile-cache/source_buffer.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <string>

namespace hermes {
namespace node_compat {

/// A read-only source range that records whether a NUL byte follows its
/// contents.
///
/// SIZE NEVER COUNTS THE TERMINATOR. size() is the length of the source text
/// alone; when isNulTerminated() is true the NUL lives at data()[size()],
/// one past the last source byte, and is readable. This is deliberately the
/// opposite of the hermes_compile_to_bytecode convention, where the caller
/// folds the terminator into the size it passes. The two are easy to confuse
/// -- which is the whole reason this class exists -- so the difference is
/// stated here and asserted below.
///
/// Hermes compiles zero-copy from a terminated buffer and copies internally
/// otherwise. A memory-mapped file satisfies it for free, since the kernel
/// zero-fills the tail of the final page, except when the file length is an
/// exact multiple of the page size. Neither const std::string & nor
/// std::string_view can express the distinction -- the first demands an
/// owning terminated heap string, the second discards the information.
///
/// Only destruction is virtual, because ownership is the only thing that
/// differs between a mapping, a heap block and a std::string. The accessors
/// read members directly.
class SourceBuffer {
 public:
  virtual ~SourceBuffer() = default;

  SourceBuffer(const SourceBuffer &) = delete;
  SourceBuffer &operator=(const SourceBuffer &) = delete;

  const char *data() const {
    return data_;
  }

  /// Length of the source text, never counting the terminator.
  size_t size() const {
    return size_;
  }

  /// True when data()[size()] is readable and is '\0'.
  bool isNulTerminated() const {
    return nulTerminated_;
  }

  /// Number of bytes readable at data(). Includes the terminator when this
  /// buffer has one, so it is size() + 1 for a terminated buffer and size()
  /// otherwise. APIs that want the terminator counted in the length they are
  /// given take this instead of size().
  size_t readableSize() const {
    return nulTerminated_ ? size_ + 1 : size_;
  }

 protected:
  SourceBuffer(const char *data, size_t size, bool nulTerminated)
      : data_(data), size_(size), nulTerminated_(nulTerminated) {
    assert((data != nullptr || size == 0) && "null data with nonzero size");
    // Reads the byte the caller just promised is readable. A caller that
    // lies fails here, rather than silently extending the compiled text by
    // one byte or reading out of bounds deep inside Hermes.
    assert(
        (!nulTerminated || data[size] == '\0') &&
        "isNulTerminated set but data[size] is not 0");
  }

 private:
  const char *const data_;
  const size_t size_;
  const bool nulTerminated_;
};

/// Borrows a std::string. c_str() guarantees the terminator; the assert in
/// the base checks it rather than assuming it.
class BorrowedStringSourceBuffer final : public SourceBuffer {
 public:
  explicit BorrowedStringSourceBuffer(const std::string &s)
      : SourceBuffer(s.data(), s.size(), true) {}
};

} // namespace node_compat
} // namespace hermes
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest
```

Expected: `[  PASSED  ] 39 tests.` (35 existing plus 4 new.)

- [ ] **Step 5: Format and commit**

```bash
git add include/hermes/node-compat/compile-cache/source_buffer.h unittests/CompileCacheTest.cpp
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git commit -m "Add SourceBuffer, a source range that knows its terminator"
```

---

### Task 2: Widen the storage API to `std::string_view`

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Modify: `lib/compile-cache/compile_cache.cpp`
- Modify: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Produces: `uint32_t compileCacheKey(std::string_view filename, CompileCacheKind kind)`; `std::string compileCacheGenerationName(std::string_view version, std::string_view arch, uint32_t bytecodeVersion, uint32_t configCrc)`; `bool CompileCache::lookup(CompileCacheEntry &entry, std::string_view source, std::string_view filename, CompileCacheKind kind)`. Task 3 consumes `lookup`.

Only the three functions above change. `compileCacheWriteEntry`, `compileCacheMakeDirs` and `compileCachePruneGenerations` keep `const std::string &`, because they need `.c_str()` for a syscall and a view would move the allocation rather than remove it.

- [ ] **Step 1: Write the failing test**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
```

Expected: build failure -- `std::string_view` does not convert to `const std::string &`.

- [ ] **Step 3: Change the declarations**

In `include/hermes/node-compat/compile-cache/compile_cache.h`, add `#include <string_view>` to the includes, then change exactly these three declarations:

```cpp
uint32_t compileCacheKey(std::string_view filename, CompileCacheKind kind);
```

```cpp
std::string compileCacheGenerationName(
    std::string_view version,
    std::string_view arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc);
```

```cpp
  bool lookup(
      CompileCacheEntry &entry,
      std::string_view source,
      std::string_view filename,
      CompileCacheKind kind);
```

- [ ] **Step 4: Change the definitions**

In `lib/compile-cache/compile_cache.cpp`, change the three matching signatures the same way. Two bodies need edits beyond the signature.

`compileCacheGenerationName` currently concatenates with `operator+` on `std::string`; build it explicitly instead:

```cpp
std::string compileCacheGenerationName(
    std::string_view version,
    std::string_view arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "-bc%u-%08x", bytecodeVersion, configCrc);

  std::string result;
  result.reserve(version.size() + arch.size() + sizeof(buf));
  result.append(version);
  result += '-';
  result.append(arch);
  result += buf;
  return result;
}
```

`CompileCache::lookup` builds `entry.cacheFilePath` from `generationDir_` and a formatted suffix; that code is unchanged. Its uses of `source.data()`, `source.size()` and `filename` already work on a view.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest
```

Expected: `[  PASSED  ] 41 tests.` All previously existing tests still pass unchanged, because `std::string` converts to `std::string_view` implicitly.

- [ ] **Step 6: Format and commit**

```bash
git add include/hermes/node-compat/compile-cache lib/compile-cache/compile_cache.cpp unittests/CompileCacheTest.cpp
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git commit -m "Widen compile cache hashed-value parameters to string_view"
```

---

### Task 3: The `compileCacheRun` helper

**Files:**
- Create: `include/hermes/node-compat/compile-cache/compile_cache_run.h`
- Create: `lib/compile-cache/compile_cache_run.cpp`
- Modify: `lib/compile-cache/CMakeLists.txt`
- Create: `unittests/CompileCacheRunTest.cpp`
- Modify: `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SourceBuffer` and `BorrowedStringSourceBuffer` from Task 1; `CompileCache::lookup` taking views from Task 2; existing `CompileCache::save`, `CompileCache::invalidate`, `CacheMapping::finalizer`, `CompileCacheEntry`, `CompileCacheKind`.
- Produces: CMake target `hermesNodeCompileCacheRun`, and `napi_status compileCacheRun(napi_env env, CompileCache &cache, CompileCacheKind kind, const SourceBuffer &source, std::string_view wrapPrefix, std::string_view wrapSuffix, const char *sourceUrl, napi_value *result)`. Tasks 4 and 5 consume it.

- [ ] **Step 1: Write the failing tests**

Create `unittests/CompileCacheRunTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheRunTest
```

Expected: configure or build failure -- there is no `CompileCacheRunTest` target and no `compile_cache_run.h`.

- [ ] **Step 3: Write the header**

Create `include/hermes/node-compat/compile-cache/compile_cache_run.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/compile-cache/compile_cache.h>
#include <hermes/node-compat/compile-cache/source_buffer.h>

#include <node_api.h>

#include <string_view>

namespace hermes {
namespace node_compat {

/// Produce a runnable value for a module's source, serving it from \p cache
/// when a valid entry exists and compiling, persisting and running it
/// otherwise.
///
/// What gets compiled is always what gets hashed, plus the caller's wrapper:
/// \p source is hashed and forms the body, and \p wrapPrefix / \p wrapSuffix
/// are concatenated around it before compiling. That invariant is what lets
/// the wrapper text live in the generation directory name instead of being
/// hashed per entry. Taking a finished string instead would let a caller key
/// an entry by text that did not produce it.
///
/// Compile and run flags are derived from \p kind rather than passed
/// alongside it; see flagsFor() in compile_cache_run.cpp for why that is a
/// correctness requirement for enable_ts.
///
/// Never opens or closes a handle scope; the caller owns scoping.
///
/// Returns napi_ok with \p result set on success. Returns a non-ok status
/// with the originating exception still pending when the user's own source
/// fails to compile, or when freshly compiled bytecode fails to run -- the
/// caller propagates both. A failure to run CACHED bytecode is never
/// reported: the exception is cleared, the entry deleted, and the source
/// recompiled, because a corrupt cache file must not surface as a
/// SyntaxError in valid user code.
napi_status compileCacheRun(
    napi_env env,
    CompileCache &cache,
    CompileCacheKind kind,
    const SourceBuffer &source,
    std::string_view wrapPrefix,
    std::string_view wrapSuffix,
    const char *sourceUrl,
    napi_value *result);

} // namespace node_compat
} // namespace hermes
```

- [ ] **Step 4: Write the implementation**

Create `lib/compile-cache/compile_cache_run.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache_run.h>

#include <napi/hermes_napi.h>
#include <napi/hermes_napi_compile.h>

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
  // adding a kind is a compile diagnostic until its flags are stated.
  return {false, false};
}

} // namespace

napi_status compileCacheRun(
    napi_env env,
    CompileCache &cache,
    CompileCacheKind kind,
    const SourceBuffer &source,
    std::string_view wrapPrefix,
    std::string_view wrapSuffix,
    const char *sourceUrl,
    napi_value *result) {
  const KindFlags flags = flagsFor(kind);
  const std::string_view filename =
      sourceUrl ? std::string_view(sourceUrl) : std::string_view();

  CompileCacheEntry entry;

  // Hash size(), never readableSize(): the terminator is not part of the
  // source text. Hashing it would key identical text to different entries
  // depending on whether its buffer happened to be terminated.
  if (cache.lookup(
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
    cache.invalidate(entry);
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

  cache.save(entry, bytecodeData, bytecodeSize);

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
```

- [ ] **Step 5: Add the CMake target**

Append to `lib/compile-cache/CMakeLists.txt`:

```cmake
# Execution layer: binds the storage layer above to Hermes. Kept separate so
# hermesNodeCompileCache stays free of Hermes and napi, which is what lets
# CompileCacheTest exercise the storage code with no runtime.
add_hermes_library(hermesNodeCompileCacheRun STATIC
  compile_cache_run.cpp
)

target_include_directories(hermesNodeCompileCacheRun
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # compile_cache_run.h includes <node_api.h>, so consumers of the header
    # need this path too -- it cannot be PRIVATE.
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
)

target_link_libraries(hermesNodeCompileCacheRun
  PUBLIC
    hermesNodeCompileCache
    # hermes_compile_to_bytecode / hermes_free_bytecode live here. PRIVATE
    # would also link correctly -- CMake propagates a PRIVATE dependency of a
    # static library through INTERFACE_LINK_LIBRARIES wrapped in
    # $<LINK_ONLY:>, precisely so transitive linking works without usage
    # requirements leaking. PUBLIC is chosen only to match how lib/runtime
    # links the same target, and keeps consumers' link lines explicit.
    hermesNapiCompile
)
```

Note that `hermesNapi` is deliberately NOT linked here even though the code
calls `napi_get_and_clear_last_exception` and `hermes_run_bytecode`. That
library is whole-archive-linked at each leaf instead -- by
`tools/hermes-node/CMakeLists.txt` and by every unit test target -- and
`hermesNodeModuleLoader` already follows the same convention.

- [ ] **Step 6: Register the test**

In `unittests/CMakeLists.txt`, add after the `CompileCacheTest` block:

```cmake
add_node_compat_unittest(CompileCacheRunTest CompileCacheRunTest.cpp)
target_link_libraries(CompileCacheRunTest
  hermesNodeCompileCacheRun
  hermesNapi
  hermesvm_a
)
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target CompileCacheRunTest
./cmake-build-asan/unittests/CompileCacheRunTest
```

Expected: `[  PASSED  ] 4 tests.`

If `HitRunsBytecodeFromTheEntry` fails on its `ASSERT_EQ(bytesA.size(), bytesB.size())`, the two sources no longer compile to equal-length bytecode. Do not weaken the assertion: change the two sources to another pair of the same shape (for example `"10 + 2"` and `"10 + 3"`) and re-check, keeping the expected results distinct.

- [ ] **Step 8: Format and commit**

```bash
git add include/hermes/node-compat/compile-cache lib/compile-cache unittests/CompileCacheRunTest.cpp unittests/CMakeLists.txt
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git commit -m "Add compileCacheRun, one implementation of the cache sequence"
```

---

### Task 4: Rewrite the CommonJS loader hook onto the helper

**Files:**
- Modify: `lib/bindings/node_contextify.cpp:627-735`
- Modify: `lib/bindings/CMakeLists.txt`

**Interfaces:**
- Consumes: `compileCacheRun` and `BorrowedStringSourceBuffer` from Tasks 1 and 3.
- Produces: nothing new. Behaviour must be identical.

- [ ] **Step 1: Switch the library dependency**

In `lib/bindings/CMakeLists.txt`, replace `hermesNodeCompileCache` with `hermesNodeCompileCacheRun` in the `PRIVATE` list of `target_link_libraries(hermesNodeBindings ...)`. The run target links the storage target publicly, so the types remain available.

- [ ] **Step 2: Add the include**

In `lib/bindings/node_contextify.cpp`, replace

```cpp
#include <hermes/node-compat/compile-cache/compile_cache.h>
```

with

```cpp
#include <hermes/node-compat/compile-cache/compile_cache_run.h>
```

- [ ] **Step 3: Replace the body**

Replace everything in `compileFunctionForCJSLoaderCb` from the line

```cpp
  CompileCache *cache = nullptr;
```

down to the closing brace of the `if (!hit) { ... }` block -- that is, the whole cache-and-compile section, ending immediately before the `// Build result:` comment -- with:

```cpp
  CompileCache *cache = nullptr;
  if (auto *state = getRuntimeState(env))
    cache = state->compileCache;

  // Wrap the source in a function with CJS parameters, matching Node's
  // GetCJSParameters: exports, require, module, __filename, __dirname.
  // No newline after the opening `{` so user line N maps to wrapped line N
  // (the debugger reports line numbers based on the wrapped source).
  //
  // The prefix and suffix are handed to the helper rather than concatenated
  // here, so the assembly happens only when the source is actually compiled.
  std::string wrapSuffix = "\n})";
  if (!filename.empty()) {
    wrapSuffix += "\n//# sourceURL=";
    wrapSuffix += filename;
    wrapSuffix += "\n";
  }

  const char *sourceUrl = filename.empty() ? nullptr : filename.c_str();
  napi_value fn = nullptr;

  if (cache != nullptr) {
    BorrowedStringSourceBuffer sourceBuf(content);
    if (compileCacheRun(
            env,
            *cache,
            CompileCacheKind::kCommonJS,
            sourceBuf,
            kCJSWrapperPrefix,
            wrapSuffix,
            sourceUrl,
            &fn) != napi_ok) {
      napi_close_handle_scope(env, scope);
      return nullptr;
    }
  } else {
    // No cache: compile and run through napi_run_script, as before. This
    // path is deliberately not shared -- napi_run_script compiles with
    // debug info under HERMES_ENABLE_DEBUGGER, which is why the cache is
    // disabled under --inspect.
    std::string wrappedSource;
    wrappedSource.reserve(
        sizeof(kCJSWrapperPrefix) + content.size() + wrapSuffix.size());
    wrappedSource += kCJSWrapperPrefix;
    wrappedSource += content;
    wrappedSource += wrapSuffix;

    napi_value sourceStr;
    napi_create_string_utf8(
        env, wrappedSource.c_str(), wrappedSource.size(), &sourceStr);
    if (napi_run_script(env, sourceStr, &fn) != napi_ok) {
      napi_close_handle_scope(env, scope);
      return nullptr;
    }
  }
```

Note the suffix now carries the `sourceURL` directive that the old code appended after `"\n})"`; the assembled text is byte-identical to before.

- [ ] **Step 4: Verify the lit tests still pass**

```bash
cmake --build cmake-build-asan --target hermes-node
for t in compile-cache-cjs.js compile-cache-corrupt.js compile-cache-syntax-error.js; do
  python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/$t \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test
done
```

Expected: three `PASS:` lines, with no edits to any test.

- [ ] **Step 5: Verify warm hits still happen**

```bash
rm -rf /tmp/hncc-t4 && mkdir -p /tmp/hncc-t4
echo 'console.log("hello");' > /tmp/hncc-t4/x.js
./cmake-build-asan/bin/hermes-node --compile-cache=/tmp/hncc-t4/cc /tmp/hncc-t4/x.js
HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE ./cmake-build-asan/bin/hermes-node \
  --compile-cache=/tmp/hncc-t4/cc /tmp/hncc-t4/x.js 2>&1 | grep -c "compile cache\] hit"
```

Expected: a nonzero count.

- [ ] **Step 6: Format and commit**

```bash
git add lib/bindings/node_contextify.cpp lib/bindings/CMakeLists.txt
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git commit -m "Move the CJS loader hook onto compileCacheRun"
```

---

### Task 5: Rewrite the module loader onto the helper

**Files:**
- Modify: `lib/module-loader/module_loader.cpp:161-254`
- Modify: `lib/module-loader/CMakeLists.txt`

**Interfaces:**
- Consumes: `compileCacheRun` and `BorrowedStringSourceBuffer` from Tasks 1 and 3.
- Produces: nothing new. Behaviour must be identical.

- [ ] **Step 1: Switch the library dependency**

In `lib/module-loader/CMakeLists.txt`, replace `hermesNodeCompileCache` with `hermesNodeCompileCacheRun` in the `PRIVATE` list. Keep `uv_a`; `runtime_state.h` still includes `uv.h`.

- [ ] **Step 2: Add the include**

In `lib/module-loader/module_loader.cpp`, replace

```cpp
#include <hermes/node-compat/compile-cache/compile_cache.h>
```

with

```cpp
#include <hermes/node-compat/compile-cache/compile_cache_run.h>
```

- [ ] **Step 3: Replace the body**

Replace everything in `compileAndRunCallback` from

```cpp
  CompileCacheKind kind = enableTS ? CompileCacheKind::kLoaderWrappedTS
```

to the end of the function with:

```cpp
  CompileCacheKind kind = enableTS ? CompileCacheKind::kLoaderWrappedTS
                                   : CompileCacheKind::kLoaderWrapped;

  CompileCache *cache = nullptr;
  if (auto *state = getRuntimeState(env))
    cache = state->compileCache;

  // The caller has already wrapped this source in JavaScript, so there is
  // no wrapper to add here and the buffer reaches the compiler untouched.
  BorrowedStringSourceBuffer sourceBuf(source);
  napi_value result = nullptr;

  if (cache != nullptr) {
    if (compileCacheRun(
            env, *cache, kind, sourceBuf, "", "", urlBuf, &result) != napi_ok)
      return nullptr;
    return result;
  }

  // No cache: compile and run through hermes_run_script, as before.
  hermes_run_script_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.enable_ts = enableTS;
  // Persistent: modules loaded this way live for the process lifetime.
  flags.persistent = true;

  if (hermes_run_script(
          env,
          reinterpret_cast<const uint8_t *>(sourceBuf.data()),
          sourceBuf.readableSize(),
          nullptr,
          nullptr,
          urlBuf,
          &flags,
          &result) != napi_ok) {
    return nullptr;
  }

  return result;
```

The bare `sourceLen + 1` is gone: the uncached path takes its length from the buffer too, so the convention lives in one place.

- [ ] **Step 4: Verify the lit tests still pass**

```bash
cmake --build cmake-build-asan --target hermes-node
for t in compile-cache-typescript.ts compile-cache-ts-throws.js compile-cache-cjs.js; do
  python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/$t \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test
done
```

Expected: three `PASS:` lines, with no edits to any test.

- [ ] **Step 5: Format and commit**

```bash
git add lib/module-loader
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git commit -m "Move the module loader onto compileCacheRun"
```

---

### Task 6: Fix the sourceURL truncation

**Files:**
- Modify: `lib/module-loader/module_loader.cpp` (the `urlBuf` read, around line 137)

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new.

**This task is a behaviour change, not part of the refactor.** `compileAndRunCallback` reads the sourceURL into `char urlBuf[4096]` via `napi_get_value_string_utf8`, which silently truncates anything longer, and the truncated value becomes the cache key's filename. Two paths sharing a 4095-byte prefix would key to one entry and evict each other on every run.

**Honest note on testing:** no automated test can reach this today. `PATH_MAX` is 4096 on Linux, so a real filesystem path cannot exceed the buffer, and nothing currently passes a synthetic URL such as a data URL. The fix is defensive: it removes a silent-truncation footgun ahead of any caller that can produce a long URL. Do not invent a test that cannot fail; the verification is that every existing test still passes.

- [ ] **Step 1: Replace the fixed buffer**

Replace the block that reads the sourceUrl into `urlBuf`:

```cpp
  // Get the sourceUrl string. Sized to the actual length rather than a fixed
  // buffer: napi_get_value_string_utf8 silently truncates when the value does
  // not fit, and the result becomes the cache key's filename, so a truncated
  // URL would let two long paths share one entry.
  size_t urlLen = 0;
  status = napi_get_value_string_utf8(env, argv[1], nullptr, 0, &urlLen);
  if (status != napi_ok) {
    napi_throw_type_error(
        env, nullptr, "compileAndRun: sourceUrl must be a string");
    return nullptr;
  }
  std::string url(urlLen, '\0');
  status =
      napi_get_value_string_utf8(env, argv[1], &url[0], urlLen + 1, nullptr);
  if (status != napi_ok)
    return nullptr;
```

- [ ] **Step 2: Update the uses**

Every later use of `urlBuf` in this function becomes `url.c_str()` -- after Task 5 those are the `compileCacheRun` call and the `hermes_run_script` call. Grep to confirm none survive, including the declaration:

```bash
grep -n "urlBuf" lib/module-loader/module_loader.cpp
```

Expected after editing: no matches.

- [ ] **Step 3: Verify nothing regressed**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```

Expected: 129 unit tests and 153 JS tests passing, plus the 4 from `CompileCacheRunTest` and the 6 added to `CompileCacheTest` in Tasks 1 and 2 -- so 139 unit tests.

- [ ] **Step 4: Format and commit**

```bash
git add lib/module-loader/module_loader.cpp
./utils/format.sh -f
git commit -m "Size the sourceURL buffer to the value instead of truncating at 4096"
```

---

## Verification checklist

After Task 6, all of the following must hold:

- [ ] `cmake --build cmake-build-asan --target check-hermes-node` passes: 139 unit tests, 153 JS tests.
- [ ] No file under `test/` was edited. The seven compile-cache lit tests pass unchanged; that is the evidence this was behaviour-preserving.
- [ ] No bare terminator arithmetic remains on a call into Hermes: every length passed to `hermes_compile_to_bytecode` or `hermes_run_script` comes from `SourceBuffer::readableSize()`. Check with
      `grep -n "readableSize\|size() + 1\|Len + 1" lib/module-loader/module_loader.cpp lib/bindings/node_contextify.cpp`.
      Two `+ 1`s legitimately survive, both on `napi_get_value_string_utf8`
      calls -- one reading the source, one reading the sourceURL after
      Task 6. That is napi's own buffer-size convention, where the argument
      is the capacity including room for the terminator, and it has nothing
      to do with the Hermes convention this change centralises. Do not
      "fix" either.
- [ ] `lib/compile-cache/CMakeLists.txt` still links `zlib_a` and nothing else into `hermesNodeCompileCache`, and `CompileCacheTest` still links only that target.
- [ ] `rm -rf ~/.cache/hermes-node && cmake --build cmake-build-asan --target check-hermes-node && test ! -d ~/.cache/hermes-node`.
- [ ] `./examples/flow-bundler/run.sh cmake-build-release` prints `PASS: 6 bundles match expected/` after `cmake --build cmake-build-release --target hermes-node`.
- [ ] `git diff --stat` over the branch shows no changes under `libjs/`, `libjs-node/`, or `hermes/`.
