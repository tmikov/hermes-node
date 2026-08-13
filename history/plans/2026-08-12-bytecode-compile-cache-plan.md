# Bytecode Compile Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cache compiled Hermes bytecode on disk so a second run of the same program skips parsing and compiling the JavaScript it already compiled once.

**Architecture:** A new native library `lib/compile-cache/` stores one file per (module path, entry-point kind) under a generation directory named for the compile configuration. The CJS loader hook and the module loader's `compileAndRun` consult it: a hit `mmap`s the file and runs it through `hermes_run_bytecode` with no parse; a miss compiles with `hermes_compile_to_bytecode`, writes the entry atomically, and runs the buffer it already has. Nothing is observable from JavaScript.

**Tech Stack:** C++17, CMake + Ninja, Clang, Hermes NAPI compile API (`hermes_compile_to_bytecode` / `hermes_run_bytecode`), zlib CRC32, POSIX `mmap`/`rename`, GTest, LLVM Lit.

**Design doc:** `history/plans/2026-08-12-bytecode-compile-cache-design.md`

## Global Constraints

- Always build with Clang, never GCC.
- Primary configuration is `cmake-build-asan` (Debug + ASAN). Verify there unless a step says otherwise.
- **Exception:** Task 9 measures against `cmake-build-release`. The flow-bundler workload exceeds ten minutes under ASAN.
- Commit messages: ASCII only, no emojis.
- Copyright header on every new file: `Copyright (c) Tzvetan Mikov.` (NOT Meta Platforms), followed by the MIT license paragraph used by every other file in this repository.
- Run `./utils/format.sh -f` before any commit that touches C++.
- Run `cmake --build cmake-build-asan --target check-hermes-node` before any commit unless the task says otherwise.
- The `hermes/` submodule pin does not change. Never `git add hermes`.
- Unit test target names must end in `Test` (singular). `unittests/lit.cfg` discovers GoogleTest binaries by that suffix; a name ending in `Tests` is silently never collected.
- No JavaScript changes anywhere in this plan. `libjs/`, `libjs-node/` and `libjs/shims/` are untouched, and the compile-cache stubs in `lib/bindings/node_modules.cpp:87-124` stay exactly as they are.
- Platform is Linux and macOS. POSIX `mmap`, `munmap`, `rename`, `mkdir` are used directly; no Windows support.
- New library targets are declared with `add_hermes_library` (from `hermes/cmake/modules/Hermes.cmake`), matching every other `lib/<name>/CMakeLists.txt`.

---

### Task 1: Library skeleton, CRC32, key and generation naming

**Files:**
- Create: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Create: `lib/compile-cache/compile_cache.cpp`
- Create: `lib/compile-cache/CMakeLists.txt`
- Modify: `CMakeLists.txt` (one `add_subdirectory` after line 57)
- Create: `unittests/CompileCacheTest.cpp`
- Modify: `unittests/CMakeLists.txt`

**Interfaces:**
- Produces: CMake target `hermesNodeCompileCache`. Enum `CompileCacheKind {kCommonJS = 0, kLoaderWrapped = 1, kLoaderWrappedTS = 2}`. Free functions `uint32_t compileCacheCrc32(const void *data, size_t size)`, `uint32_t compileCacheKey(const std::string &filename, CompileCacheKind kind)`, `std::string compileCacheGenerationName(const std::string &version, const std::string &arch, uint32_t bytecodeVersion, uint32_t configCrc)`. Tasks 2-8 consume all of these.

- [ ] **Step 1: Write the failing test**

Create `unittests/CompileCacheTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
```

Expected: configure or build failure -- there is no `CompileCacheTest` target and no `compile_cache.h`.

- [ ] **Step 3: Write the header**

Create `include/hermes/node-compat/compile-cache/compile_cache.h`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Create `lib/compile-cache/compile_cache.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <zlib.h>

#include <cstdio>

namespace hermes {
namespace node_compat {

uint32_t compileCacheCrc32(const void *data, size_t size) {
  uLong crc = crc32(0L, Z_NULL, 0);
  if (size == 0)
    return static_cast<uint32_t>(crc);
  return static_cast<uint32_t>(
      crc32(crc, static_cast<const Bytef *>(data), static_cast<uInt>(size)));
}

uint32_t compileCacheKey(const std::string &filename, CompileCacheKind kind) {
  auto kindByte = static_cast<uint8_t>(kind);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef *>(&kindByte), 1);
  crc = crc32(
      crc,
      reinterpret_cast<const Bytef *>(filename.data()),
      static_cast<uInt>(filename.size()));
  return static_cast<uint32_t>(crc);
}

std::string compileCacheGenerationName(
    const std::string &version,
    const std::string &arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc) {
  char buf[64];
  std::snprintf(
      buf, sizeof(buf), "-bc%u-%08x", bytecodeVersion, configCrc);
  return version + "-" + arch + buf;
}

} // namespace node_compat
} // namespace hermes
```

- [ ] **Step 5: Write the library CMakeLists**

Create `lib/compile-cache/CMakeLists.txt`:

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeCompileCache STATIC
  compile_cache.cpp
)

target_include_directories(hermesNodeCompileCache
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# This library talks to the filesystem through raw POSIX calls, not libuv,
# and never includes a Hermes header. zlib, for CRC32, is its only
# dependency -- keep it that way.
target_link_libraries(hermesNodeCompileCache
  PRIVATE
    zlib_a
)
```

- [ ] **Step 6: Register the library and the test**

In the top-level `CMakeLists.txt`, add after the `add_subdirectory(lib/module-loader)` line:

```cmake
add_subdirectory(lib/compile-cache)
```

In `unittests/CMakeLists.txt`, add after the `ModuleLoaderTest` block:

```cmake
add_node_compat_unittest(CompileCacheTest CompileCacheTest.cpp)
target_link_libraries(CompileCacheTest hermesNodeCompileCache)
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest
```

Expected: `[  PASSED  ] 8 tests.`

- [ ] **Step 8: Format and commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/compile-cache lib/compile-cache CMakeLists.txt unittests/CompileCacheTest.cpp unittests/CMakeLists.txt
git commit -m "Add compile cache library skeleton with key derivation"
```

---

### Task 2: Entry file format -- write, read, validate

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Modify: `lib/compile-cache/compile_cache.cpp`
- Modify: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Consumes: `compileCacheCrc32` from Task 1.
- Produces: `struct CacheMapping {void *base; size_t length; static void finalizer(const uint8_t *, size_t, void *); void destroy();}`; `struct CompileCacheEntry` with fields `key`, `sourceCrc`, `sourceSize`, `cacheFilePath`, `mapping`, `bytecode`, `bytecodeSize` and method `bool hit() const`; `bool compileCacheWriteEntry(const std::string &path, const CompileCacheEntry &entry, const uint8_t *bytecode, size_t bytecodeSize)`; `bool compileCacheReadEntry(CompileCacheEntry &entry)`. Tasks 4, 6 and 7 consume all of these.

- [ ] **Step 1: Write the failing tests**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
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
  const std::string &path() const { return path_; }

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
CompileCacheEntry makeEntry(const std::string &path, const std::string &source) {
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
  ASSERT_TRUE(compileCacheWriteEntry(
      file, written, payload.data(), payload.size()));

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
  ASSERT_TRUE(compileCacheWriteEntry(
      file, written, payload.data(), payload.size()));

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
  ASSERT_TRUE(compileCacheWriteEntry(
      file, written, payload.data(), payload.size()));

  CompileCacheEntry read = makeEntry(file, "var a = 1; var b = 2;");
  EXPECT_FALSE(compileCacheReadEntry(read));
}

TEST(CompileCacheTest, ReadMissesOnBadMagic) {
  TempDir dir;
  std::string file = dir.path() + "/entry";
  std::string source = "x";
  auto payload = fakePayload(64, 1);
  CompileCacheEntry written = makeEntry(file, source);
  ASSERT_TRUE(compileCacheWriteEntry(
      file, written, payload.data(), payload.size()));

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
  ASSERT_TRUE(compileCacheWriteEntry(
      file, written, payload.data(), payload.size()));

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
  ASSERT_TRUE(compileCacheWriteEntry(
      file, written, payload.data(), payload.size()));

  // Exactly one file: the entry. The temp file must have been renamed, not
  // left alongside it.
  FILE *pipe = ::popen(("ls -1 " + dir.path() + " | wc -l").c_str(), "r");
  ASSERT_NE(nullptr, pipe);
  char buf[32] = {0};
  ASSERT_NE(nullptr, fgets(buf, sizeof(buf), pipe));
  ::pclose(pipe);
  EXPECT_EQ(1, atoi(buf));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
```

Expected: build failure -- `compileCacheWriteEntry`, `compileCacheReadEntry`, `CacheMapping` and `CompileCacheEntry` do not exist.

- [ ] **Step 3: Add the types to the header**

Append inside the namespace in `include/hermes/node-compat/compile-cache/compile_cache.h`, before the closing braces:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Add to the includes at the top of `lib/compile-cache/compile_cache.cpp`:

```cpp
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
```

Append inside the namespace in `lib/compile-cache/compile_cache.cpp`:

```cpp
namespace {

/// 'HNCC' -- Hermes Node Compile Cache.
constexpr uint32_t kMagic = 0x4343484e;
/// Bumped when the header layout below changes.
constexpr uint32_t kHeaderVersion = 1;

/// Entry header, 24 bytes. Field order matches the design document.
struct EntryHeader {
  uint32_t magic;
  uint32_t headerVersion;
  uint32_t sourceCrc;
  uint32_t sourceSize;
  uint32_t bytecodeSize;
  uint32_t reserved;
};

static_assert(
    sizeof(EntryHeader) == kCompileCacheHeaderSize,
    "EntryHeader must match kCompileCacheHeaderSize");

/// Write exactly \p size bytes, retrying short writes. Returns false on error.
bool writeAll(int fd, const void *data, size_t size) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (size > 0) {
    ssize_t n = ::write(fd, p, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

/// Read exactly \p size bytes, retrying short reads. Returns false on error
/// or premature end of file.
bool readAll(int fd, void *data, size_t size) {
  auto *p = static_cast<uint8_t *>(data);
  while (size > 0) {
    ssize_t n = ::read(fd, p, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

} // namespace

void CacheMapping::destroy() {
  if (base != nullptr)
    ::munmap(base, length);
  delete this;
}

void CacheMapping::finalizer(const uint8_t *, size_t, void *hint) {
  static_cast<CacheMapping *>(hint)->destroy();
}

bool compileCacheWriteEntry(
    const std::string &path,
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize) {
  if (bytecode == nullptr || bytecodeSize == 0)
    return false;
  if (bytecodeSize > UINT32_MAX)
    return false;

  // Unique temp name so concurrent writers never collide.
  std::string tmp = path + "." + std::to_string(::getpid()) + ".tmp";

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;

  EntryHeader header{};
  header.magic = kMagic;
  header.headerVersion = kHeaderVersion;
  header.sourceCrc = entry.sourceCrc;
  header.sourceSize = entry.sourceSize;
  header.bytecodeSize = static_cast<uint32_t>(bytecodeSize);
  header.reserved = 0;

  bool ok = writeAll(fd, &header, sizeof(header)) &&
      writeAll(fd, bytecode, bytecodeSize);
  ::close(fd);

  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool compileCacheReadEntry(CompileCacheEntry &entry) {
  int fd = ::open(entry.cacheFilePath.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  EntryHeader header{};
  if (!readAll(fd, &header, sizeof(header))) {
    ::close(fd);
    return false;
  }

  // Cheapest checks first: a changed file usually changes length, so the
  // size check rejects most edits before the CRC matters.
  if (header.magic != kMagic || header.headerVersion != kHeaderVersion ||
      header.sourceSize != entry.sourceSize ||
      header.sourceCrc != entry.sourceCrc || header.bytecodeSize == 0) {
    ::close(fd);
    return false;
  }

  size_t mapSize = sizeof(EntryHeader) + header.bytecodeSize;

  // Refuse a file shorter than its header claims. Atomic writes make this
  // unreachable for entries we wrote, but the directory is user-writable.
  struct stat st {};
  if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < mapSize) {
    ::close(fd);
    return false;
  }

  void *base = ::mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd); // The mapping keeps the inode alive on its own.
  if (base == MAP_FAILED)
    return false;

  auto *mapping = new CacheMapping();
  mapping->base = base;
  mapping->length = mapSize;

  entry.mapping = mapping;
  entry.bytecode = static_cast<const uint8_t *>(base) + sizeof(EntryHeader);
  entry.bytecodeSize = header.bytecodeSize;
  return true;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest
```

Expected: `[  PASSED  ] 16 tests.`

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/compile-cache lib/compile-cache unittests/CompileCacheTest.cpp
git commit -m "Add compile cache entry format with atomic writes"
```

---

### Task 3: Cache root, generation directory, pruning

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Modify: `lib/compile-cache/compile_cache.cpp`
- Modify: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Consumes: `compileCacheGenerationName` from Task 1.
- Produces: `std::string compileCacheDefaultRoot()`; `bool compileCacheMakeDirs(const std::string &path)`; `void compileCachePruneGenerations(const std::string &versionedRoot, const std::string &keepName, size_t keepCount)`. Task 4 consumes all three.

- [ ] **Step 1: Write the failing tests**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
```

Expected: build failure -- the three new functions do not exist.

- [ ] **Step 3: Add the declarations to the header**

Append inside the namespace in `include/hermes/node-compat/compile-cache/compile_cache.h`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Add to the includes at the top of `lib/compile-cache/compile_cache.cpp`:

```cpp
#include <dirent.h>

#include <algorithm>
#include <cstdlib>
#include <vector>
```

Append inside the namespace in `lib/compile-cache/compile_cache.cpp`:

```cpp
namespace {

/// Recursively delete \p path. Best effort.
void removeTree(const std::string &path) {
  DIR *d = ::opendir(path.c_str());
  if (d != nullptr) {
    while (struct dirent *e = ::readdir(d)) {
      if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
        continue;
      std::string child = path + "/" + e->d_name;
      struct stat st {};
      if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        removeTree(child);
      else
        ::unlink(child.c_str());
    }
    ::closedir(d);
  }
  ::rmdir(path.c_str());
}

} // namespace

std::string compileCacheDefaultRoot() {
  if (const char *xdg = ::getenv("XDG_CACHE_HOME")) {
    if (xdg[0] != '\0')
      return std::string(xdg) + "/hermes-node/compile-cache";
  }
  if (const char *home = ::getenv("HOME")) {
    if (home[0] != '\0')
      return std::string(home) + "/.cache/hermes-node/compile-cache";
  }
  return std::string();
}

bool compileCacheMakeDirs(const std::string &path) {
  if (path.empty())
    return false;
  // Create each component in turn, tolerating EEXIST so concurrent
  // processes racing to create the same directory both succeed.
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/')
      continue;
    std::string component = path.substr(0, i);
    if (::mkdir(component.c_str(), 0755) != 0 && errno != EEXIST)
      return false;
  }
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void compileCachePruneGenerations(
    const std::string &versionedRoot,
    const std::string &keepName,
    size_t keepCount) {
  DIR *d = ::opendir(versionedRoot.c_str());
  if (d == nullptr)
    return;

  std::vector<std::pair<time_t, std::string>> others;
  while (struct dirent *e = ::readdir(d)) {
    if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
      continue;
    if (keepName == e->d_name)
      continue;
    std::string child = versionedRoot + "/" + e->d_name;
    struct stat st {};
    if (::lstat(child.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      continue;
    others.emplace_back(st.st_mtime, child);
  }
  ::closedir(d);

  if (others.size() <= keepCount)
    return;

  // Most recently modified first; everything past keepCount goes.
  std::sort(others.begin(), others.end(), [](const auto &a, const auto &b) {
    return a.first > b.first;
  });
  for (size_t i = keepCount; i < others.size(); ++i)
    removeTree(others[i].second);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest
```

Expected: `[  PASSED  ] 25 tests.`

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/compile-cache lib/compile-cache unittests/CompileCacheTest.cpp
git commit -m "Add compile cache root resolution and generation pruning"
```

---

### Task 4: The CompileCache class

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Modify: `lib/compile-cache/compile_cache.cpp`
- Modify: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: `class CompileCache` with `bool enable(const std::string &root, const std::string &generationName)`, `bool enabled() const`, `const std::string &generationDir() const`, `bool lookup(CompileCacheEntry &entry, const std::string &source, const std::string &filename, CompileCacheKind kind)`, `void save(const CompileCacheEntry &entry, const uint8_t *bytecode, size_t bytecodeSize)`, `void invalidate(const CompileCacheEntry &entry)`, `void setTracing(bool)`. Tasks 5, 6 and 7 consume all of these.

Note: there is deliberately **no in-memory entry store**. Write-through means a second require of the same path in one process already takes the disk hit, and an in-memory layer would hand the same mapping to `hermes_run_bytecode` twice, double-`munmap`ing it via `CacheMapping::finalizer`.

- [ ] **Step 1: Write the failing tests**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
TEST(CompileCacheTest, DisabledUntilEnabled) {
  CompileCache cache;
  EXPECT_FALSE(cache.enabled());

  CompileCacheEntry entry;
  EXPECT_FALSE(
      cache.lookup(entry, "var a = 1;", "/a/b.js",
                   CompileCacheKind::kCommonJS));
  EXPECT_FALSE(entry.hit());
}

TEST(CompileCacheTest, EnableCreatesVersionedGenerationDir) {
  TempDir dir;
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
  TempDir dir;
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
  TempDir dir;
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
  TempDir dir;
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
  TempDir dir;
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
  TempDir dir;
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
  EXPECT_FALSE(cache.lookup(
      gone, "var a = 1;", "/x/y.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, EnablePrunesOldGenerations) {
  TempDir dir;
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
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
```

Expected: build failure -- `CompileCache` does not exist.

- [ ] **Step 3: Add the class to the header**

Append inside the namespace in `include/hermes/node-compat/compile-cache/compile_cache.h`:

```cpp
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
      const std::string &source,
      const std::string &filename,
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
  void trace(const char *what, const std::string &filename) const;

  bool enabled_ = false;
  bool tracing_ = false;
  std::string generationDir_;
};
```

- [ ] **Step 4: Write the implementation**

Append inside the namespace in `lib/compile-cache/compile_cache.cpp`:

```cpp
bool CompileCache::enable(
    const std::string &root,
    const std::string &generationName) {
  if (root.empty() || generationName.empty())
    return false;

  // v1/ is a structure escape hatch: a fundamentally different layout later
  // becomes v2/ and can coexist with this one.
  std::string versionedRoot = root + "/v1";
  std::string generationDir = versionedRoot + "/" + generationName;

  bool existed = false;
  {
    struct stat st {};
    existed = ::stat(generationDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }

  if (!compileCacheMakeDirs(generationDir))
    return false;

  // Prune only when a generation was actually created, so a normal startup
  // never scans the cache root.
  if (!existed) {
    compileCachePruneGenerations(
        versionedRoot, generationName, kCompileCacheGenerationsKept);
  }

  generationDir_ = generationDir;
  enabled_ = true;
  return true;
}

void CompileCache::trace(const char *what, const std::string &filename) const {
  if (tracing_)
    std::fprintf(stderr, "[compile cache] %s %s\n", what, filename.c_str());
}

bool CompileCache::lookup(
    CompileCacheEntry &entry,
    const std::string &source,
    const std::string &filename,
    CompileCacheKind kind) {
  if (!enabled_)
    return false;

  entry.key = compileCacheKey(filename, kind);
  entry.sourceCrc = compileCacheCrc32(source.data(), source.size());
  entry.sourceSize = static_cast<uint32_t>(source.size());

  char rel[16];
  std::snprintf(
      rel, sizeof(rel), "/%02x/%08x", (entry.key >> 24) & 0xff, entry.key);
  entry.cacheFilePath = generationDir_ + rel;

  if (compileCacheReadEntry(entry)) {
    trace("hit ", filename);
    return true;
  }
  trace("miss", filename);
  return false;
}

void CompileCache::save(
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize) {
  if (!enabled_ || entry.cacheFilePath.empty())
    return;

  // Create the fanout directory. Cheap enough to attempt every time; mkdir
  // on an existing directory is a single failed syscall.
  size_t slash = entry.cacheFilePath.rfind('/');
  if (slash != std::string::npos)
    compileCacheMakeDirs(entry.cacheFilePath.substr(0, slash));

  if (!compileCacheWriteEntry(
          entry.cacheFilePath, entry, bytecode, bytecodeSize))
    trace("save-failed", entry.cacheFilePath);
}

void CompileCache::invalidate(const CompileCacheEntry &entry) {
  if (!enabled_ || entry.cacheFilePath.empty())
    return;
  ::unlink(entry.cacheFilePath.c_str());
  trace("invalidated", entry.cacheFilePath);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest
```

Expected: `[  PASSED  ] 35 tests.`

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/compile-cache lib/compile-cache unittests/CompileCacheTest.cpp
git commit -m "Add CompileCache class tying key, entry and generation together"
```

---

### Task 5: Runtime wiring -- config, flags, enable at bootstrap, test isolation

**Files:**
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h` (config struct, after the `inspectOpen` field around line 52)
- Modify: `include/hermes/node-compat/runtime/runtime_state.h`
- Modify: `lib/runtime/hermes_node_runtime.cpp` (around the `new RuntimeState()` at line 487)
- Modify: `lib/runtime/CMakeLists.txt`
- Modify: `tools/hermes-node/hermes-node.cpp` (usage text near line 26, argument parsing near line 120)
- Modify: `test/lit.cfg`
- Create: `test/compile-cache-enable.js`

**Interfaces:**
- Consumes: `CompileCache`, `compileCacheDefaultRoot`, `compileCacheGenerationName`, `compileCacheCrc32` from Tasks 1-4.
- Produces: `HermesNodeConfig::compileCacheDir` (`std::string`) and `HermesNodeConfig::disableCompileCache` (`bool`); `RuntimeState::compileCache` (a `CompileCache *`, owned by `RuntimeState`, deleted with it). Tasks 6 and 7 read `getRuntimeState(env)->compileCache`.

- [ ] **Step 1: Isolate the existing test suite first**

This must land before the cache is ever enabled by default, or the whole
suite starts writing to the user's real `~/.cache`.

In `test/lit.cfg`, add after the `config.test_exec_root` assignment:

```python
# The compile cache is on by default. Off for the suite as a whole so tests
# never share state through the user's real cache directory and ordering does
# not become a variable; the compile-cache tests opt back in explicitly with
# their own directory, through the %hermes-node-cc substitution below.
#
# lit's ShTest replaces the environment rather than merging with the ambient
# one, which is why PATH and HOME have to be passed through explicitly.
config.environment['HERMES_NODE_DISABLE_COMPILE_CACHE'] = '1'
config.environment['PATH'] = os.environ.get('PATH', '')
config.environment['HOME'] = os.environ.get('HOME', '')
```

Then add the opt-in substitution. It MUST be appended **before** `%hermes-node`,
because lit applies substitutions in list order and `%hermes-node` is a prefix
of `%hermes-node-cc` -- registering the short name first would rewrite
`%hermes-node-cc` into `<path-to-binary>-cc`. The existing "longer names first"
comment in the file is about exactly this hazard.

```python
# Tests that exercise the compile cache run it through this substitution,
# which clears the suite-wide disable above. Anything using plain
# %hermes-node still runs with the cache off.
config.substitutions.append(
    ('%hermes-node-cc',
     'env -u HERMES_NODE_DISABLE_COMPILE_CACHE ' + hermes_node))
config.substitutions.append(('%hermes-node', hermes_node))
```

The existing `config.substitutions.append(('%hermes-node', hermes_node))` line
is replaced by the pair above, not duplicated.

- [ ] **Step 2: Write the failing test**

Create `test/compile-cache-enable.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The cache directory is created when the cache is enabled, and not created
// when it is disabled. Detection is through the filesystem because the cache
// is deliberately not observable from JavaScript.

// RUN: rm -rf %t.cache %t.off %t.env
// RUN: %hermes-node-cc --compile-cache=%t.cache %s | %FileCheck %s
// RUN: test -d %t.cache/v1
// RUN: %hermes-node-cc --no-compile-cache --compile-cache=%t.off %s | %FileCheck %s
// RUN: test ! -d %t.off
// RUN: env HERMES_NODE_DISABLE_COMPILE_CACHE=1 %hermes-node --compile-cache=%t.off %s | %FileCheck %s
// RUN: test ! -d %t.off
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t.env %hermes-node %s | %FileCheck %s
// RUN: test -d %t.env/v1

'use strict';

// The JS API must keep reporting that there is no caching, whatever the
// native cache is doing.
const mod = require('module');
if (mod.enableCompileCache().status !== 0) throw new Error('status changed');
if (mod.getCompileCacheDir() !== undefined) throw new Error('dir exposed');

console.log('PASS');
// CHECK: PASS
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/compile-cache-enable.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: FAIL -- `--compile-cache` is an unrecognized argument.

- [ ] **Step 4: Add the config fields**

In `include/hermes/node-compat/runtime/hermes_node_runtime.h`, add after the `inspectOpen` field:

```cpp
  /// Compile cache root directory. Empty = use the default XDG location.
  std::string compileCacheDir;

  /// Disable the on-disk compile cache entirely.
  bool disableCompileCache = false;
```

In `include/hermes/node-compat/runtime/runtime_state.h`, add the include and the field:

```cpp
#include <hermes/node-compat/compile-cache/compile_cache.h>
```

```cpp
  // On-disk bytecode cache. Owned here; deleted with RuntimeState. Null when
  // the cache could not be enabled or was disabled.
  CompileCache *compileCache = nullptr;
```

- [ ] **Step 5: Parse the flags**

In `tools/hermes-node/hermes-node.cpp`, add to the usage text after the `--inspect-brk` line:

```cpp
      "  --compile-cache=<dir>          Bytecode cache directory\n"
      "  --no-compile-cache             Disable the bytecode cache\n"
```

Add to the argument parsing chain, after the `--inspect-open` branch:

```cpp
    } else if (std::strncmp(argv[i], "--compile-cache=", 16) == 0) {
      config.compileCacheDir = argv[i] + 16;
    } else if (std::strcmp(argv[i], "--no-compile-cache") == 0) {
      config.disableCompileCache = true;
```

- [ ] **Step 6: Enable the cache during bootstrap**

In `lib/runtime/hermes_node_runtime.cpp`, add the includes near the other
`hermes/node-compat` includes:

```cpp
#include <hermes/node-compat/compile-cache/compile_cache.h>
#include <hermes/node-compat/bindings/node_contextify.h>
#include <hermes/node-compat/version.h>
#include <hermes/BCGen/HBC/BytecodeVersion.h>
```

Add this helper above the function containing `new RuntimeState()`:

```cpp
namespace {

/// Build and enable the compile cache for this runtime, or return nullptr.
///
/// Disabled under --inspect / --inspect-brk: cached bytecode is compiled by
/// hermes_compile_to_bytecode, which has no debug flag and always produces
/// DebugInfoSetting::THROWING, while napi_run_script compiles at
/// DebugInfoSetting::ALL under HERMES_ENABLE_DEBUGGER. Bypassing here rather
/// than at each compile site keeps debugger behaviour unchanged by
/// construction.
CompileCache *createCompileCache(const HermesNodeConfig &config) {
  if (config.disableCompileCache || config.inspect || config.inspectBrk)
    return nullptr;
  if (const char *off = ::getenv("HERMES_NODE_DISABLE_COMPILE_CACHE")) {
    if (off[0] != '\0' && std::strcmp(off, "0") != 0)
      return nullptr;
  }

  std::string root = config.compileCacheDir;
  if (root.empty()) {
    if (const char *fromEnv = ::getenv("HERMES_NODE_COMPILE_CACHE"))
      root = fromEnv;
  }
  if (root.empty())
    root = compileCacheDefaultRoot();
  if (root.empty())
    return nullptr;

  // Everything that invalidates every entry at once goes in the generation
  // name: the wrapper text applied natively, and the compile flags.
  uint32_t configCrc = compileCacheCrc32(
      kCJSWrapperPrefix, sizeof(kCJSWrapperPrefix) - 1);

  auto cache = std::make_unique<CompileCache>();
  if (!cache->enable(
          root,
          compileCacheGenerationName(
              HERMES_NODE_VERSION_STRING,
              HERMES_NODE_CACHE_ARCH,
              hermes::hbc::BYTECODE_VERSION,
              configCrc))) {
    return nullptr;
  }

  if (const char *dbg = ::getenv("HERMES_NODE_DEBUG_NATIVE"))
    cache->setTracing(std::strstr(dbg, "COMPILE_CACHE") != nullptr);

  return cache.release();
}

} // namespace
```

Immediately after `runtimeState->inspectorBridgeContext = config.inspectorBridgeContext;`, add:

```cpp
  runtimeState->compileCache = createCompileCache(config);
```

At line 1388, replace `delete runtimeState;` with:

```cpp
  delete runtimeState->compileCache;
  delete runtimeState;
```

- [ ] **Step 7: Link and define the arch macro**

In `lib/runtime/CMakeLists.txt`, add `hermesNodeCompileCache` to the
`target_link_libraries` call, and add:

```cmake
target_compile_definitions(hermesNodeRuntime PRIVATE
  HERMES_NODE_CACHE_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)
```

`HERMES_NODE_VERSION_STRING` comes from the generated header
`hermes/node-compat/version.h`, which this target did not previously consume.
Add its directory to `target_include_directories` and depend on the generator,
mirroring what `tools/hermes-node/CMakeLists.txt` already does:

```cmake
    ${CMAKE_BINARY_DIR}/generated
```
```cmake
add_dependencies(hermesNodeRuntime hermes-node-version)
```

- [ ] **Step 8: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target hermes-node
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/compile-cache-enable.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: `PASS: hermes-node :: compile-cache-enable.js`

- [ ] **Step 9: Verify nothing writes to the real cache**

```bash
rm -rf ~/.cache/hermes-node
cmake --build cmake-build-asan --target check-hermes-node
test ! -d ~/.cache/hermes-node && echo "ISOLATION OK"
```

Expected: the full suite passes and prints `ISOLATION OK`.

- [ ] **Step 10: Format and commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/runtime lib/runtime tools/hermes-node/hermes-node.cpp test/lit.cfg test/compile-cache-enable.js
git commit -m "Enable the compile cache at bootstrap behind flags and env vars"
```

---

### Task 6: Consult the cache from the CJS loader hook

**Files:**
- Modify: `lib/bindings/node_contextify.cpp:594-676` (`compileFunctionForCJSLoaderCb`)
- Modify: `lib/bindings/CMakeLists.txt`
- Create: `test/compile-cache-cjs.js`

**Interfaces:**
- Consumes: `getRuntimeState(env)->compileCache` from Task 5; `CompileCache::lookup/save/invalidate`, `CacheMapping::finalizer`, `CompileCacheKind::kCommonJS` from Task 4.
- Produces: nothing new; this is the behavioural payoff.

- [ ] **Step 1: Write the failing test**

Create `test/compile-cache-cjs.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A cold run populates the cache and a warm run produces identical output.
// Editing the required file invalidates its entry.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'module.exports = function () { return "first"; };' > %t.dir/dep.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js > %t.cold.txt
// RUN: %FileCheck --check-prefix=FIRST %s < %t.cold.txt
// RUN: find %t.cache -type f | wc -l | %FileCheck --check-prefix=POPULATED %s
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js > %t.warm.txt
// RUN: diff %t.cold.txt %t.warm.txt
// RUN: echo 'module.exports = function () { return "second"; };' > %t.dir/dep.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js | %FileCheck --check-prefix=SECOND %s

'use strict';

const dep = require(process.argv[2]);
console.log('VALUE ' + dep());
console.log('PASS');

// FIRST: VALUE first
// FIRST: PASS
// SECOND: VALUE second
// SECOND: PASS
// POPULATED-NOT: {{^0$}}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/compile-cache-cjs.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: FAIL at the `POPULATED` check -- the cache directory has no entry
files, because nothing consults the cache yet.

- [ ] **Step 3: Add includes and link the library**

At the top of `lib/bindings/node_contextify.cpp`, add:

```cpp
#include <hermes/node-compat/compile-cache/compile_cache.h>
```

In `lib/bindings/CMakeLists.txt`, add `hermesNodeCompileCache` to the
`PRIVATE` list of `target_link_libraries(hermesNodeBindings ...)`.

- [ ] **Step 4: Consult the cache**

In `lib/bindings/node_contextify.cpp`, replace lines 626-655 of
`compileFunctionForCJSLoaderCb` -- everything from the comment
`// Wrap the source in a function with CJS parameters` down to and including
the `if (runStatus != napi_ok) { ... return nullptr; }` block that follows
`napi_run_script`. Leave the `result` object construction after it untouched;
it already reads the `fn` this block produces.

```cpp
  CompileCache *cache = nullptr;
  if (auto *state = getRuntimeState(env))
    cache = state->compileCache;

  napi_value fn = nullptr;
  CompileCacheEntry entry;

  // The hashed string is the raw content: the wrapper below is a constant
  // folded into the generation name, so it need not be hashed per entry.
  bool hit = cache != nullptr &&
      cache->lookup(entry, content, filename, CompileCacheKind::kCommonJS);

  if (hit) {
    hermes_bytecode_flags bcFlags{};
    bcFlags.struct_size = sizeof(bcFlags);
    napi_status runStatus = hermes_run_bytecode(
        env,
        entry.bytecode,
        entry.bytecodeSize,
        CacheMapping::finalizer,
        entry.mapping,
        filename.empty() ? nullptr : filename.c_str(),
        &bcFlags,
        &fn);
    if (runStatus != napi_ok) {
      // A cache file that passed our header checks but that Hermes will not
      // load must not surface as a SyntaxError in valid user code. Clear the
      // pending exception, drop the entry, and compile from source. The
      // mapping was consumed by hermes_run_bytecode.
      napi_value ignored;
      napi_get_and_clear_last_exception(env, &ignored);
      cache->invalidate(entry);
      entry.mapping = nullptr;
      entry.bytecode = nullptr;
      hit = false;
      fn = nullptr;
    }
  }

  if (!hit) {
    // Wrap the source in a function with CJS parameters, matching Node's
    // GetCJSParameters: exports, require, module, __filename, __dirname.
    // No newline after the opening `{` so user line N maps to wrapped line N
    // (the debugger reports line numbers based on the wrapped source).
    std::string wrappedSource;
    wrappedSource.reserve(content.size() + filename.size() + 128);
    wrappedSource += kCJSWrapperPrefix;
    wrappedSource += content;
    wrappedSource += "\n})";
    if (!filename.empty()) {
      wrappedSource += "\n//# sourceURL=";
      wrappedSource += filename;
      wrappedSource += "\n";
    }

    if (cache != nullptr) {
      // Compile to a buffer we can both persist and run.
      hermes_compile_flags cflags{};
      cflags.struct_size = sizeof(cflags);
      uint8_t *bytecodeData = nullptr;
      size_t bytecodeSize = 0;
      napi_status compileStatus = hermes_compile_to_bytecode(
          env,
          reinterpret_cast<const uint8_t *>(wrappedSource.c_str()),
          // +1 for the terminating '\0' that c_str() guarantees, so
          // hermes_compile_to_bytecode can wrap the buffer zero-copy
          // instead of making an internal null-terminated copy.
          wrappedSource.size() + 1,
          filename.empty() ? nullptr : filename.c_str(),
          &cflags,
          &bytecodeData,
          &bytecodeSize);
      if (compileStatus != napi_ok) {
        // A real SyntaxError in the user's source. Propagate it.
        napi_close_handle_scope(env, scope);
        return nullptr;
      }

      cache->save(entry, bytecodeData, bytecodeSize);

      hermes_bytecode_flags bcFlags{};
      bcFlags.struct_size = sizeof(bcFlags);
      napi_status runStatus = hermes_run_bytecode(
          env,
          bytecodeData,
          bytecodeSize,
          [](const uint8_t *data, size_t, void *) {
            hermes_free_bytecode(const_cast<uint8_t *>(data));
          },
          nullptr,
          filename.empty() ? nullptr : filename.c_str(),
          &bcFlags,
          &fn);
      if (runStatus != napi_ok) {
        napi_close_handle_scope(env, scope);
        return nullptr;
      }
    } else {
      // No cache: the original path, unchanged.
      napi_value sourceStr;
      napi_create_string_utf8(
          env, wrappedSource.c_str(), wrappedSource.size(), &sourceStr);
      napi_status runStatus = napi_run_script(env, sourceStr, &fn);
      if (runStatus != napi_ok) {
        napi_close_handle_scope(env, scope);
        return nullptr;
      }
    }
  }
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target hermes-node
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/compile-cache-cjs.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: `PASS: hermes-node :: compile-cache-cjs.js`

- [ ] **Step 6: Verify the cache is actually being read**

```bash
rm -rf /tmp/hncc-manual && mkdir -p /tmp/hncc-manual
echo 'console.log("hello");' > /tmp/hncc-manual/x.js
./cmake-build-asan/bin/hermes-node --compile-cache=/tmp/hncc-manual/cc \
  /tmp/hncc-manual/x.js
HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE ./cmake-build-asan/bin/hermes-node \
  --compile-cache=/tmp/hncc-manual/cc /tmp/hncc-manual/x.js 2>&1 | \
  grep -c "compile cache. hit"
```

Expected: a nonzero count -- the second run reports hits.

- [ ] **Step 7: Format and commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add lib/bindings/node_contextify.cpp lib/bindings/CMakeLists.txt test/compile-cache-cjs.js
git commit -m "Consult the compile cache from the CJS loader hook"
```

---

### Task 7: Consult the cache from the module loader

**Files:**
- Modify: `lib/module-loader/module_loader.cpp:102-178` (`compileAndRunCallback`)
- Modify: `lib/module-loader/CMakeLists.txt`
- Create: `test/compile-cache-typescript.ts`

**Interfaces:**
- Consumes: everything Task 6 consumes, plus `CompileCacheKind::kLoaderWrapped` and `kLoaderWrappedTS`.
- Produces: nothing new.

Note: `compileAndRunCallback` serves two call sites in `libjs/loader.js` -- the
bootstrap loader's disk fallback (`:64`) and the `.ts` extension handler
(`:327`). Both pass an already-wrapped source, which is why the hashed string
differs in shape from the CJS hook's and needs its own kinds.

- [ ] **Step 1: Write the failing test**

Create `test/compile-cache-typescript.ts`:

```ts
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// TypeScript files compiled through the module loader are cached too, and a
// warm run produces identical output.

// RUN: rm -rf %t.cache
// RUN: %hermes-node-cc --compile-cache=%t.cache %s > %t.cold.txt
// RUN: %FileCheck %s < %t.cold.txt
// The TypeScript path goes through compileAndRunCallback, so the cold run
// must leave an entry behind. This is what fails before the change: output
// equality alone holds either way.
// RUN: find %t.cache -type f | wc -l | %FileCheck --check-prefix=POPULATED %s
// RUN: %hermes-node-cc --compile-cache=%t.cache %s > %t.warm.txt
// RUN: diff %t.cold.txt %t.warm.txt

function greet(name: string): string {
  return 'hello ' + name;
}

console.log(greet('world'));
console.log('PASS');

// CHECK: hello world
// CHECK: PASS
// POPULATED-NOT: {{^0$}}
```

Do not touch the `config.excludes` list in `test/lit.cfg`. It names
`test-typescript-module.ts` specifically; `compile-cache-typescript.ts` is a
different filename and is collected normally.

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/compile-cache-typescript.ts \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: FAIL at the `POPULATED` check -- the cold run leaves the cache
directory empty, because `compileAndRunCallback` does not consult the cache
yet. The `hello world` / `PASS` checks and the `diff` already pass; the
cache-population check is what makes this a failing test.

- [ ] **Step 3: Add includes and link the library**

At the top of `lib/module-loader/module_loader.cpp`, add:

```cpp
#include <hermes/node-compat/compile-cache/compile_cache.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <napi/hermes_napi_compile.h>
```

In `lib/module-loader/CMakeLists.txt`, add the whole call -- this target does
not currently have one. `uv_a` is needed because `runtime_state.h`, pulled in
for `RuntimeState::compileCache`, includes `<uv.h>` directly:

```cmake
target_link_libraries(hermesNodeModuleLoader
  PRIVATE
    hermesNodeCompileCache
    # runtime_state.h (pulled in for RuntimeState::compileCache) includes
    # uv.h directly.
    uv_a
)
```

- [ ] **Step 4: Consult the cache**

In `lib/module-loader/module_loader.cpp`, replace the block from the
`hermes_run_script_flags flags{};` declaration through the `return result;`
at the end of `compileAndRunCallback` with:

```cpp
  CompileCacheKind kind = enableTS ? CompileCacheKind::kLoaderWrappedTS
                                   : CompileCacheKind::kLoaderWrapped;

  CompileCache *cache = nullptr;
  if (auto *state = getRuntimeState(env))
    cache = state->compileCache;

  napi_value result = nullptr;
  CompileCacheEntry entry;
  std::string filename(urlBuf, urlLen);

  if (cache != nullptr &&
      cache->lookup(entry, source, filename, kind)) {
    hermes_bytecode_flags bcFlags{};
    bcFlags.struct_size = sizeof(bcFlags);
    // Persistent: modules loaded this way live for the process lifetime.
    bcFlags.persistent = true;
    if (hermes_run_bytecode(
            env,
            entry.bytecode,
            entry.bytecodeSize,
            CacheMapping::finalizer,
            entry.mapping,
            urlBuf,
            &bcFlags,
            &result) == napi_ok) {
      return result;
    }
    // Unloadable cache file: clear the exception it raised, drop the entry,
    // and fall through to compiling from source. The mapping was consumed.
    napi_value ignored;
    napi_get_and_clear_last_exception(env, &ignored);
    cache->invalidate(entry);
    entry.mapping = nullptr;
    result = nullptr;
  }

  if (cache != nullptr) {
    hermes_compile_flags cflags{};
    cflags.struct_size = sizeof(cflags);
    cflags.enable_ts = enableTS;
    uint8_t *bytecodeData = nullptr;
    size_t bytecodeSize = 0;
    if (hermes_compile_to_bytecode(
            env,
            reinterpret_cast<const uint8_t *>(source.c_str()),
            sourceLen + 1, // includes the trailing '\0' for zero-copy
            urlBuf,
            &cflags,
            &bytecodeData,
            &bytecodeSize) != napi_ok) {
      return nullptr; // Real compile error; the exception is pending.
    }

    cache->save(entry, bytecodeData, bytecodeSize);

    hermes_bytecode_flags bcFlags{};
    bcFlags.struct_size = sizeof(bcFlags);
    bcFlags.persistent = true;
    if (hermes_run_bytecode(
            env,
            bytecodeData,
            bytecodeSize,
            [](const uint8_t *data, size_t, void *) {
              hermes_free_bytecode(const_cast<uint8_t *>(data));
            },
            nullptr,
            urlBuf,
            &bcFlags,
            &result) != napi_ok) {
      return nullptr;
    }
    return result;
  }

  // No cache: the original path, unchanged.
  hermes_run_script_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.enable_ts = enableTS;
  flags.persistent = true;

  if (hermes_run_script(
          env,
          reinterpret_cast<const uint8_t *>(source.c_str()),
          sourceLen + 1, // includes trailing '\0' for zero-copy
          nullptr,
          nullptr,
          urlBuf,
          &flags,
          &result) != napi_ok) {
    return nullptr;
  }

  return result;
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target hermes-node
for t in compile-cache-typescript.ts compile-cache-cjs.js compile-cache-enable.js; do
  python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/$t \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test
done
```

Expected: three `PASS:` lines.

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add lib/module-loader test/compile-cache-typescript.ts
git commit -m "Consult the compile cache from the module loader"
```

---

### Task 8: Robustness tests

**Files:**
- Create: `test/compile-cache-corrupt.js`
- Create: `test/compile-cache-syntax-error.js`
- Create: `test/compile-cache-inspect.js`

**Interfaces:**
- Consumes: the behaviour built in Tasks 5-7. Produces nothing.

- [ ] **Step 1: Write the corrupt-entry test**

Create `test/compile-cache-corrupt.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A cache file whose header is intact but whose bytecode Hermes cannot load
// must not surface as an error. The run recompiles from source and succeeds.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'module.exports = 7;' > %t.dir/dep.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js | %FileCheck %s
// Zero 64 payload bytes in place, starting right after the 24-byte header.
// That leaves our header (and so our own validation) intact and destroys the
// Hermes bytecode magic, which is what forces the failure onto the
// cached-bytecode path rather than a header rejection. conv=notrunc keeps
// the file length unchanged so the fstat size check still passes.
//
// dd is used rather than head/tail with `stat -c%s`: that spelling of stat
// is a GNU extension and CI also builds macOS, where it is `stat -f%z`.
// RUN: for f in $(find %t.cache -type f); do \
// RUN:   dd if=/dev/zero of="$f" bs=1 seek=24 count=64 conv=notrunc 2>/dev/null; \
// RUN: done
// RUN: %hermes-node-cc --compile-cache=%t.cache %s %t.dir/dep.js | %FileCheck %s

'use strict';

console.log('VALUE ' + require(process.argv[2]));
console.log('PASS');

// CHECK: VALUE 7
// CHECK: PASS
```

- [ ] **Step 2: Write the syntax-error parity test**

Create `test/compile-cache-syntax-error.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A genuine SyntaxError in the user's source is still reported, and reports
// the same file both cold and warm. Only failures on the cached-bytecode path
// are swallowed.

// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'function ( { oops' > %t.dir/bad.js
// RUN: %not %hermes-node-cc --compile-cache=%t.cache %s %t.dir/bad.js > %t.cold.txt 2>&1
// RUN: %FileCheck %s < %t.cold.txt
// The identical failure must be reported on a warm run. This is the property
// the cache could actually break: a real compile error must never be cached,
// never swallowed, and never replaced by a stale success.
// RUN: %not %hermes-node-cc --compile-cache=%t.cache %s %t.dir/bad.js > %t.warm.txt 2>&1
// RUN: diff %t.cold.txt %t.warm.txt

'use strict';

require(process.argv[2]);
console.log('SHOULD NOT REACH');

// Hermes formats parse errors as "<line>:<col>:<message>" and does not
// include the filename (SimpleDiagHandler::getErrorString in the hermes
// submodule), so this asserts on the diagnostic text. The column is past the
// end of the one-line source because the CJS wrapper prefix is prepended
// before compiling.
// CHECK: SyntaxError: {{[0-9]+}}:{{[0-9]+}}:{{.*}}'identifier' expected after 'function'
// CHECK-NOT: SHOULD NOT REACH
```

- [ ] **Step 3: Write the inspect-bypass test**

Create `test/compile-cache-inspect.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The cache is not enabled under --inspect, so that debugging keeps the
// napi_run_script path, which compiles with full debug info.

// The -cc substitution matters here: with the suite-wide disable still in
// effect this test would pass for the wrong reason, asserting nothing.
// RUN: rm -rf %t.cache
// RUN: %hermes-node-cc --inspect --compile-cache=%t.cache %s | %FileCheck %s
// RUN: test ! -d %t.cache

'use strict';

console.log('PASS');
// CHECK: PASS
```

- [ ] **Step 4: Run all three**

```bash
cmake --build cmake-build-asan --target hermes-node
for t in compile-cache-corrupt.js compile-cache-syntax-error.js compile-cache-inspect.js; do
  python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/$t \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test
done
```

Expected: three `PASS:` lines.

`--inspect` (unlike `--inspect-brk`) does not wait for a client: it prints the
"Debugger listening" banner, runs the script and exits 0. Verified before this
plan was written. It does bind port 9229, so `compile-cache-inspect.js` is the
only test that may use `--inspect`; adding a second one would make the two
collide under lit's parallel execution.

- [ ] **Step 5: Format and commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
git add test/compile-cache-corrupt.js test/compile-cache-syntax-error.js test/compile-cache-inspect.js
git commit -m "Add compile cache robustness tests"
```

---

### Task 9: Measure and document

**Files:**
- Create: `history/plans/progress-compile-cache.md`
- Modify: `CLAUDE.md` (add a Compile Cache section)

**Interfaces:**
- Consumes: the finished feature. Produces documentation only.

- [ ] **Step 1: Build Release**

```bash
cmake --build cmake-build-release --target hermes-node hermes-parser-napi
```

- [ ] **Step 2: Measure cold and warm on the flow bundler**

```bash
cd examples/flow-bundler
export HERMES_PARSER_NATIVE_ADDON=$(cd ../.. && pwd)/cmake-build-release/external/hermes-parser-native/hermes-parser.node
rm -rf /tmp/hncc-bench out
echo "--- cold"
time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/hncc-bench \
  -r ./babel-register.js ./bundler/buildBundleCLI.js -c ./build.config.js > /dev/null
echo "--- warm"
rm -rf out
time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/hncc-bench \
  -r ./babel-register.js ./bundler/buildBundleCLI.js -c ./build.config.js > /dev/null
echo "--- cache size"
du -sh /tmp/hncc-bench
find /tmp/hncc-bench -type f | wc -l
cd ../..
```

Record the three numbers. The design predicts warm lands around 3.2-3.5 s
against a ~6.3 s cold run, and a cache roughly the size of the sources
(~15-20 MB across ~2841 entries). **If warm is not materially faster than
cold, stop and investigate before writing this up** -- run the warm case again
under `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` and check that it reports hits
rather than misses.

- [ ] **Step 3: Verify the example still produces correct output**

```bash
./examples/flow-bundler/run.sh cmake-build-release
```

Expected: `PASS: 6 bundles match expected/`

- [ ] **Step 4: Write the progress file**

Create `history/plans/progress-compile-cache.md` recording: which plan it
tracks (`2026-08-12-bytecode-compile-cache-plan.md`), that all nine tasks are
complete, and the measured cold/warm/size numbers from Step 2 as actual
figures rather than the predictions.

- [ ] **Step 5: Document the feature in CLAUDE.md**

Add a section after "Native Addons":

```markdown
## Compile Cache

Compiled bytecode for user and `node_modules` JavaScript is cached on disk,
on by default. Built-in JS is unaffected (already embedded as bytecode).

- Root: `$XDG_CACHE_HOME/hermes-node/compile-cache`, else
  `~/.cache/hermes-node/compile-cache`. Layout `v1/<generation>/<ab>/<key>`.
- Controls: `--compile-cache=<dir>`, `--no-compile-cache`,
  `HERMES_NODE_COMPILE_CACHE`, `HERMES_NODE_DISABLE_COMPILE_CACHE`,
  `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`. No `NODE_*` variables.
- Disabled under `--inspect` / `--inspect-brk`: cache entries are compiled at
  `DebugInfoSetting::THROWING`, the debugger needs `ALL`.
- Not observable from JavaScript. `module.enableCompileCache()` still reports
  `FAILED` and `getCompileCacheDir()` still returns `undefined`, deliberately.
- `test/lit.cfg` sets `HERMES_NODE_DISABLE_COMPILE_CACHE=1` for the whole
  suite; compile-cache tests opt in with their own directory under `%t`.
- Implementation: `lib/compile-cache/`, consulted from
  `node_contextify.cpp` (`compileFunctionForCJSLoaderCb`) and
  `module_loader.cpp` (`compileAndRunCallback`).
```

- [ ] **Step 6: Commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
git add history/plans/progress-compile-cache.md CLAUDE.md
git commit -m "Document the compile cache and record measurements"
```

---

## Verification checklist

After Task 9, all of the following must hold:

- [ ] `cmake --build cmake-build-asan --target check-hermes-node` passes.
- [ ] `rm -rf ~/.cache/hermes-node && cmake --build cmake-build-asan --target check-hermes-node` leaves `~/.cache/hermes-node` absent.
- [ ] `./examples/flow-bundler/run.sh cmake-build-release` prints `PASS: 6 bundles match expected/`.
- [ ] A warm flow-bundler run is measurably faster than a cold one, with the figure recorded in the progress file.
- [ ] `git diff --stat` over the whole branch shows no changes under `libjs/`, `libjs-node/`, or `hermes/`.
