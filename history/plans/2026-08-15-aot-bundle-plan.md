# AOT Bundle Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package an application's compiled JavaScript into one distributable
file that runs on another machine with no `node_modules` tree and no
compilation at startup.

**Architecture:** A new `lib/bundle/` library owns an `mmap`-friendly container
format with fixed-width module and edge tables. A producer mode
(`--build-bundle`) parses from an entry script with Hermes's own parser,
follows literal `require()` calls, resolves them, compiles each file to
optimized bytecode, and writes the container. A consumer mode (`--bundle`)
maps the container and wraps Node's `Module._load` so a hit returns bundled
exports without touching the filesystem.

**Tech Stack:** C++17, Hermes `JSParser` + `RecursiveVisitor`, Hermes NAPI
compile API (`hermes_compile_to_bytecode` / `hermes_run_bytecode`), zlib CRC32,
CMake + Ninja, GTest, LLVM Lit.

**Spec:** `history/plans/2026-08-15-aot-bundle-design.md`

## Deviation from the spec

The spec's Architecture section says the consumer "installs a resolution table
into the module loader," written as though there were one loader. There are
two. `libjs/loader.js` is the bootstrap loader for embedded builtins only;
user code and everything under `node_modules` goes through Node's own CJS
loader, reached at `libjs/loader.js:238` via `CJSModule._load`.

This plan therefore integrates by wrapping **`Module._load`** in Node's CJS
loader, which covers resolution and file reading in a single hook. The
codebase already wraps `Module._load` (see the `__initCJS` comment at
`libjs/loader.js:257`), so this follows existing precedent rather than
introducing a new mechanism. No other part of the spec changes.

## Global Constraints

- Copyright header on every new file: `Copyright (c) Tzvetan Mikov.` (NOT Meta Platforms).
- Commit messages: ASCII only, no emojis.
- Never `git add hermes`; the submodule pin must not change.
- Do not modify anything under `libjs-node/`. Integration happens by wrapping
  Node's loader from our own code, never by editing Node's copy.
- Build with Clang, never GCC. Primary config is `cmake-build-asan`.
- Before every commit: `./utils/format.sh --check` and
  `cmake --build cmake-build-asan --target check-hermes-node`.
  Use `--check`, never `-f` (which formats the *last commit*, not the worktree).
- Magic string is exactly `HNBUNDLE` (8 bytes, no NUL terminator).
- Bundle format version starts at `1`.
- All multi-byte integers in the container are little-endian `uint32_t`,
  written and read as native types (we do not support cross-endian bundles;
  the generation tag would reject them anyway).
- Payload entries are aligned to 8 bytes. Hermes bytecode requires alignment
  when executed in place.
- New shim files require a CMake reconfigure (shim resolution uses `EXISTS`
  at configure time). No new shims are added by this plan, but any file added
  under `libjs/` must be registered in the embedded-modules list.

## File structure

| File | Responsibility |
| --- | --- |
| `include/hermes/node-compat/bundle/bundle_format.h` | On-disk record structs, magic, version, offsets. No Hermes, no napi. |
| `include/hermes/node-compat/bundle/bundle_reader.h` | Read-only view over a mapped container; edge lookup. |
| `include/hermes/node-compat/bundle/bundle_writer.h` | Accumulate modules and edges, serialize. |
| `lib/bundle/bundle_reader.cpp` | Reader implementation. |
| `lib/bundle/bundle_writer.cpp` | Writer implementation. |
| `lib/bundle/CMakeLists.txt` | `hermesNodeBundle` static lib, links `zlib_a` only. |
| `include/hermes/node-compat/bundle/bundle_build.h` | Producer entry point. |
| `lib/bundle/bundle_build.cpp` | Discovery, resolution, compilation, writing. Links Hermes parser + NAPI compile. |
| `lib/bundle/bundle_run.cpp` | Consumer: native lookup callbacks exposed to JS. |
| `libjs/bundle-loader.js` | Installs the `Module._load` wrapper when bundle mode is active. |
| `unittests/BundleFormatTest.cpp` | Round trip, corruption, lookup. |
| `test/bundle-*.js` | Lit behavior tests. |

The split mirrors `lib/compile-cache/`: `hermesNodeBundle` (format only, no
Hermes headers) is separable and unit-testable with no runtime, while
`hermesNodeBundleRun` carries the Hermes and napi dependencies.

---

### Task 1: Container format and reader/writer round trip

**Files:**
- Create: `include/hermes/node-compat/bundle/bundle_format.h`
- Create: `include/hermes/node-compat/bundle/bundle_writer.h`
- Create: `include/hermes/node-compat/bundle/bundle_reader.h`
- Create: `lib/bundle/bundle_writer.cpp`
- Create: `lib/bundle/bundle_reader.cpp`
- Create: `lib/bundle/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add `add_subdirectory(lib/bundle)` after line 59)
- Test: `unittests/BundleFormatTest.cpp`, `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `BundleWriter::addModule(std::string_view identity, ModuleKind kind, std::string_view payload) -> uint32_t`
  - `BundleWriter::addEdge(uint32_t importer, std::string_view specifier, uint32_t target)`
  - `BundleWriter::setEntry(uint32_t moduleIndex)`
  - `BundleWriter::serialize(uint32_t generationTag) -> std::vector<uint8_t>`
  - `BundleReader::open(const uint8_t *data, size_t size, uint32_t expectedGeneration, std::string *error) -> std::optional<BundleReader>`
  - `BundleReader::lookup(uint32_t importer, std::string_view specifier) -> std::optional<uint32_t>`
  - `BundleReader::identity(uint32_t moduleIndex) -> std::string_view`
  - `BundleReader::payload(uint32_t moduleIndex) -> std::string_view`
  - `BundleReader::kind(uint32_t moduleIndex) -> ModuleKind`
  - `BundleReader::entry() -> uint32_t`
  - `BundleReader::moduleCount() -> uint32_t`

- [ ] **Step 1: Write `bundle_format.h`**

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_FORMAT_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_FORMAT_H

#include <cstddef>
#include <cstdint>

namespace hermes {
namespace node_compat {

/// Eight bytes, no NUL. A JavaScript file cannot begin with these.
constexpr char kBundleMagic[8] = {'H', 'N', 'B', 'U', 'N', 'D', 'L', 'E'};

/// Bumped whenever the layout below changes in a way older readers cannot
/// interpret. A mismatch is a hard error; there is no forward compatibility.
constexpr uint32_t kBundleFormatVersion = 1;

/// Every payload entry starts at a multiple of this. Hermes bytecode is
/// executed in place from the mapping and requires alignment.
constexpr size_t kBundlePayloadAlign = 8;

enum class ModuleKind : uint32_t {
  kJavaScript = 0,
  kJSON = 1,
};

/// Fixed-width. Offsets are byte offsets from the start of the file.
struct BundleHeader {
  char magic[8];
  uint32_t formatVersion;
  uint32_t generationTag;
  uint32_t entryModule;
  uint32_t stringsOffset;
  uint32_t stringsSize;
  uint32_t moduleTableOffset;
  uint32_t moduleCount;
  uint32_t edgeTableOffset;
  uint32_t edgeCount;
  uint32_t payloadOffset;
  uint32_t payloadSize;
};

/// One per packaged module. `identityString` indexes the string table.
struct BundleModuleRecord {
  uint32_t identityString;
  uint32_t kind; // ModuleKind
  uint32_t payloadOffset; // from payloadOffset in the header
  uint32_t payloadSize;
};

/// One per resolved (importer, specifier) pair. Sorted by importer index,
/// then by the *bytes* of the specifier -- see bundle_reader.h for why.
struct BundleEdgeRecord {
  uint32_t importer;
  uint32_t specifierString;
  uint32_t target;
};

/// Entries in the string table are stored as uint32 length followed by that
/// many bytes, with no NUL. A string index is the byte offset of its length
/// field from stringsOffset.
struct BundleStringHeader {
  uint32_t length;
};

} // namespace node_compat
} // namespace hermes

#endif
```

- [ ] **Step 2: Write the failing round-trip test**

Create `unittests/BundleFormatTest.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/bundle_writer.h>

#include <gtest/gtest.h>

using namespace hermes::node_compat;

namespace {

constexpr uint32_t kGen = 0xABCD1234;

TEST(BundleFormatTest, RoundTripSingleModule) {
  BundleWriter w;
  uint32_t m = w.addModule("cli.js", ModuleKind::kJavaScript, "BYTECODE");
  w.setEntry(m);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->moduleCount(), 1u);
  EXPECT_EQ(r->entry(), m);
  EXPECT_EQ(r->identity(m), "cli.js");
  EXPECT_EQ(r->payload(m), "BYTECODE");
  EXPECT_EQ(r->kind(m), ModuleKind::kJavaScript);
}

TEST(BundleFormatTest, EdgeLookupHitAndMiss) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, "A");
  uint32_t b = w.addModule("node_modules/b/index.js", ModuleKind::kJavaScript, "B");
  uint32_t c = w.addModule("c.json", ModuleKind::kJSON, "{\"x\":1}");
  w.addEdge(a, "b", b);
  w.addEdge(a, "./c.json", c);
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;

  EXPECT_EQ(r->lookup(a, "b"), std::optional<uint32_t>(b));
  EXPECT_EQ(r->lookup(a, "./c.json"), std::optional<uint32_t>(c));
  // Right specifier, wrong importer.
  EXPECT_FALSE(r->lookup(b, "b").has_value());
  // Unknown specifier.
  EXPECT_FALSE(r->lookup(a, "nope").has_value());
  EXPECT_EQ(r->kind(c), ModuleKind::kJSON);
  EXPECT_EQ(r->payload(c), "{\"x\":1}");
}

// Specifiers that share a prefix must not collide in the binary search.
TEST(BundleFormatTest, PrefixSpecifiersDoNotCollide) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, "A");
  uint32_t p = w.addModule("p.js", ModuleKind::kJavaScript, "P");
  uint32_t pp = w.addModule("pp.js", ModuleKind::kJavaScript, "PP");
  w.addEdge(a, "./p", p);
  w.addEdge(a, "./pp", pp);
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->lookup(a, "./p"), std::optional<uint32_t>(p));
  EXPECT_EQ(r->lookup(a, "./pp"), std::optional<uint32_t>(pp));
}

TEST(BundleFormatTest, StringTableDeduplicates) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, "A");
  uint32_t b = w.addModule("b.js", ModuleKind::kJavaScript, "B");
  uint32_t t = w.addModule("node_modules/path-ish/index.js",
                           ModuleKind::kJavaScript, "T");
  // Same specifier from two importers -- one string, two edges.
  w.addEdge(a, "path-ish", t);
  w.addEdge(b, "path-ish", t);
  w.setEntry(a);
  std::vector<uint8_t> small = w.serialize(kGen);

  BundleWriter w2;
  uint32_t a2 = w2.addModule("a.js", ModuleKind::kJavaScript, "A");
  uint32_t b2 = w2.addModule("b.js", ModuleKind::kJavaScript, "B");
  uint32_t t2 = w2.addModule("node_modules/path-ish/index.js",
                             ModuleKind::kJavaScript, "T");
  w2.addEdge(a2, "path-ish", t2);
  w2.addEdge(b2, "path-ish-other-longer-name", t2);
  w2.setEntry(a2);
  std::vector<uint8_t> big = w2.serialize(kGen);

  // Deduplication is observable: identical specifiers produce a smaller
  // string table than two distinct ones.
  EXPECT_LT(small.size(), big.size());
}

TEST(BundleFormatTest, PayloadEntriesAreAligned) {
  BundleWriter w;
  // Deliberately unaligned sizes.
  w.addModule("a.js", ModuleKind::kJavaScript, "1");
  uint32_t b = w.addModule("b.js", ModuleKind::kJavaScript, "22222");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;
  const char *base = reinterpret_cast<const char *>(bytes.data());
  EXPECT_EQ((r->payload(b).data() - base) % kBundlePayloadAlign, 0u);
}

TEST(BundleFormatTest, RejectsBadMagic) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  bytes[0] = 'X';

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("not a hermes-node bundle"), std::string::npos) << error;
}

TEST(BundleFormatTest, RejectsGenerationMismatch) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen + 1, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("built by a different hermes-node"), std::string::npos)
      << error;
}

TEST(BundleFormatTest, RejectsFormatVersionMismatch) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  // formatVersion sits immediately after the 8 magic bytes.
  uint32_t bogus = kBundleFormatVersion + 99;
  std::memcpy(bytes.data() + 8, &bogus, sizeof(bogus));

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("format version"), std::string::npos) << error;
}

// Truncation at every byte length must be rejected, never crash. This is the
// test that catches missing bounds checks in the reader.
TEST(BundleFormatTest, RejectsTruncationAtEveryLength) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, "AAAA");
  uint32_t b = w.addModule("b.js", ModuleKind::kJavaScript, "BBBB");
  w.addEdge(a, "./b", b);
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  for (size_t n = 0; n < bytes.size(); ++n) {
    std::string error;
    auto r = BundleReader::open(bytes.data(), n, kGen, &error);
    EXPECT_FALSE(r.has_value()) << "accepted a truncated bundle of " << n
                                << " bytes";
    EXPECT_FALSE(error.empty()) << "no error message at length " << n;
  }
}

TEST(BundleFormatTest, RejectsOutOfRangeEntry) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  uint32_t bogus = 99;
  std::memcpy(bytes.data() + offsetof(BundleHeader, entryModule), &bogus,
              sizeof(bogus));

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
}

TEST(BundleFormatTest, EmptyBundleIsRejected) {
  BundleWriter w;
  std::string error;
  // No modules and no entry: serialize must refuse rather than emit a
  // container whose entry index cannot be valid.
  std::vector<uint8_t> bytes = w.serialize(kGen);
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
}

} // namespace
```

- [ ] **Step 3: Register the test**

Add to `unittests/CMakeLists.txt` after the `CompileCacheRunTest` block:

```cmake
add_node_compat_unittest(BundleFormatTest BundleFormatTest.cpp)
target_link_libraries(BundleFormatTest hermesNodeBundle)
```

- [ ] **Step 4: Run to verify it fails**

Run: `cmake --build cmake-build-asan --target BundleFormatTest`
Expected: FAIL at configure or compile, `hermesNodeBundle` does not exist.

- [ ] **Step 5: Write `bundle_writer.h` and `bundle_reader.h`**

`bundle_writer.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_WRITER_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_WRITER_H

#include <hermes/node-compat/bundle/bundle_format.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Accumulates modules and edges, then serializes one container.
///
/// Deliberately free of Hermes and napi headers so the format can be unit
/// tested with no runtime, exactly as CompileCache is.
class BundleWriter {
 public:
  /// Copies \p payload. Returns the new module's index.
  uint32_t
  addModule(std::string_view identity, ModuleKind kind, std::string_view payload);

  /// Records that \p importer resolved \p specifier to \p target.
  void addEdge(uint32_t importer, std::string_view specifier, uint32_t target);

  void setEntry(uint32_t moduleIndex);

  /// Returns the serialized container. Sorts the edge table by
  /// (importer, specifier bytes) as the reader's binary search requires.
  /// Returns an empty vector if no module was ever added.
  std::vector<uint8_t> serialize(uint32_t generationTag);

 private:
  /// Interns \p s, returning its byte offset within the string table.
  uint32_t internString(std::string_view s);

  struct PendingModule {
    uint32_t identityString;
    ModuleKind kind;
    std::string payload;
  };
  struct PendingEdge {
    uint32_t importer;
    std::string specifier;
    uint32_t target;
  };

  std::vector<PendingModule> modules_;
  std::vector<PendingEdge> edges_;
  std::map<std::string, uint32_t, std::less<>> internTable_;
  std::string stringBytes_;
  uint32_t entry_ = 0;
  bool hasEntry_ = false;
};

} // namespace node_compat
} // namespace hermes

#endif
```

`bundle_reader.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_READER_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_READER_H

#include <hermes/node-compat/bundle/bundle_format.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// A read-only view over a mapped container. Does not own the bytes; the
/// caller keeps the mapping alive for the reader's lifetime.
///
/// open() validates every offset and length in the header against \p size
/// before any accessor can be called, so accessors perform no bounds checks
/// of their own beyond index range.
class BundleReader {
 public:
  /// Returns std::nullopt and sets \p error on bad magic, format version
  /// mismatch, generation mismatch, truncation, or any out-of-range offset.
  static std::optional<BundleReader> open(
      const uint8_t *data,
      size_t size,
      uint32_t expectedGeneration,
      std::string *error);

  /// Binary search for the edge (importer, specifier).
  ///
  /// The edge table is sorted by (importer index, specifier bytes) rather
  /// than by (importer, specifier index). At run time require() supplies
  /// bytes, not an index; sorting by index would force a string-to-index
  /// hash built at load time, which is exactly the startup pass this format
  /// exists to avoid. Comparing bytes costs about a dozen short memcmps per
  /// require and keeps load-time work at zero.
  std::optional<uint32_t> lookup(uint32_t importer, std::string_view specifier)
      const;

  std::string_view identity(uint32_t moduleIndex) const;
  std::string_view payload(uint32_t moduleIndex) const;
  ModuleKind kind(uint32_t moduleIndex) const;
  uint32_t entry() const;
  uint32_t moduleCount() const;

 private:
  BundleReader() = default;
  std::string_view stringAt(uint32_t offset) const;

  const uint8_t *data_ = nullptr;
  const BundleHeader *header_ = nullptr;
  const BundleModuleRecord *modules_ = nullptr;
  const BundleEdgeRecord *edges_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif
```

- [ ] **Step 6: Implement `bundle_writer.cpp` and `bundle_reader.cpp`**

Writer requirements the tests pin down:
- `internString` returns an existing offset for an identical string (dedup).
- `serialize` returns `{}` when `modules_` is empty.
- `serialize` sorts `edges_` by `(importer, specifier)` using
  `std::tuple<uint32_t, std::string_view>` comparison, so byte order matches
  the reader's `memcmp` order.
- Each payload is padded to `kBundlePayloadAlign` *before* the next one starts,
  and `payloadOffset` in the header is itself aligned.
- Section order in the file: header, strings, module table, edge table, payload.

Reader requirements the tests pin down:
- Check `size >= sizeof(BundleHeader)` before dereferencing the header.
- Check magic, then `formatVersion`, then `generationTag`, in that order, so
  the error message is the most specific one that applies.
- Validate every `(offset, size)` pair: `offset + size` must not overflow and
  must be `<= size`.
- Validate `entryModule < moduleCount`.
- Validate every module record's `payloadOffset + payloadSize <= payloadSize`
  of the header, and every string index within the string table.
- Validate every edge record's `importer` and `target` are `< moduleCount`.

Error strings must contain the substrings the tests assert on:
`"not a hermes-node bundle"`, `"format version"`,
`"built by a different hermes-node"`.

- [ ] **Step 7: Write `lib/bundle/CMakeLists.txt`**

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Format layer: no napi, and no link dependency on the Hermes VM. This is
# what lets BundleFormatTest exercise the container with no runtime. Task 2
# adds one header-only Hermes include here for BYTECODE_VERSION; that stays
# compatible with the above, but do not let a VM link dependency in.
add_hermes_library(hermesNodeBundle STATIC
  bundle_writer.cpp
  bundle_reader.cpp
)

target_include_directories(hermesNodeBundle
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
```

Add `add_subdirectory(lib/bundle)` to the top-level `CMakeLists.txt`
immediately after the existing `add_subdirectory(lib/compile-cache)` line.

- [ ] **Step 8: Run the tests**

Run: `cmake --build cmake-build-asan --target BundleFormatTest && ./cmake-build-asan/unittests/BundleFormatTest`
Expected: all tests PASS.

- [ ] **Step 9: Format check and commit**

```bash
./utils/format.sh --check
git add include/hermes/node-compat/bundle lib/bundle unittests/BundleFormatTest.cpp unittests/CMakeLists.txt CMakeLists.txt
git commit -m "Add AOT bundle container format with reader and writer"
```

---

### Task 2: Generation tag

**Files:**
- Create: `include/hermes/node-compat/bundle/bundle_generation.h`
- Create: `lib/bundle/bundle_generation.cpp`
- Modify: `lib/bundle/CMakeLists.txt`
- Test: `unittests/BundleFormatTest.cpp` (append)

**Interfaces:**
- Consumes: nothing from Task 1 except the library target.
- Produces: `uint32_t bundleGenerationTag()` -- the value both producer and
  consumer pass to `serialize` / `open`.

- [ ] **Step 1: Write the failing test**

Append to `unittests/BundleFormatTest.cpp`:

```cpp
TEST(BundleGenerationTest, IsStableWithinOneBuild) {
  EXPECT_EQ(bundleGenerationTag(), bundleGenerationTag());
}

TEST(BundleGenerationTest, IsNonZero) {
  // A zero tag would make an all-zero header look plausible.
  EXPECT_NE(bundleGenerationTag(), 0u);
}
```

Add `#include <hermes/node-compat/bundle/bundle_generation.h>` at the top.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build cmake-build-asan --target BundleFormatTest`
Expected: FAIL, no such header.

- [ ] **Step 3: Implement**

`bundle_generation.h` declares `uint32_t bundleGenerationTag();`.

`bundle_generation.cpp` computes a CRC32 over, in order:
`HERMES_NODE_VERSION_STRING`, `HERMES_NODE_CACHE_ARCH`, the decimal text of
`hermes::hbc::BYTECODE_VERSION`, and the single byte `'O'` (the producer
always optimizes, per the spec; the byte is present so a future
non-optimizing producer gets a distinct tag rather than a silent collision).
If the result is 0, return 1 -- the test above requires non-zero.

This file *does* include a Hermes header for `BYTECODE_VERSION`, so it goes
in a separate translation unit and `lib/bundle/CMakeLists.txt` gains:

```cmake
target_sources(hermesNodeBundle PRIVATE bundle_generation.cpp)
target_include_directories(hermesNodeBundle PRIVATE
  ${PROJECT_SOURCE_DIR}/hermes/include
)
target_link_libraries(hermesNodeBundle PRIVATE zlib_a)
```

Note: this makes `hermesNodeBundle` depend on Hermes headers. That is
acceptable because it is headers only, not a link dependency on the VM, and
`BundleFormatTest` still runs with no runtime.

- [ ] **Step 4: Run the tests**

Run: `./cmake-build-asan/unittests/BundleFormatTest`
Expected: PASS.

- [ ] **Step 5: Format check and commit**

```bash
./utils/format.sh --check
git add include/hermes/node-compat/bundle lib/bundle unittests/BundleFormatTest.cpp
git commit -m "Add bundle generation tag"
```

---

### Task 3: Static discovery -- find literal require() calls

**Files:**
- Create: `include/hermes/node-compat/bundle/require_scanner.h`
- Create: `lib/bundle/require_scanner.cpp`
- Modify: `lib/bundle/CMakeLists.txt` (new `hermesNodeBundleBuild` target)
- Test: `unittests/RequireScannerTest.cpp`, `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `bool scanRequires(std::string_view source, bool enableTS, std::vector<std::string> *out, std::string *error)`
    -- appends every literal `require()` argument, in source order, with
    duplicates removed. Returns false and sets `error` on a parse error.

- [ ] **Step 1: Write the failing test**

Create `unittests/RequireScannerTest.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/require_scanner.h>

#include <gtest/gtest.h>

using namespace hermes::node_compat;

namespace {

std::vector<std::string> scan(const char *src, bool ts = false) {
  std::vector<std::string> out;
  std::string error;
  EXPECT_TRUE(scanRequires(src, ts, &out, &error)) << error;
  return out;
}

TEST(RequireScannerTest, FindsTopLevelRequires) {
  EXPECT_EQ(scan("const a = require('a');\nconst b = require(\"b\");"),
            (std::vector<std::string>{"a", "b"}));
}

TEST(RequireScannerTest, FindsNestedRequires) {
  EXPECT_EQ(scan("function f() { if (x) { return require('deep'); } }"),
            (std::vector<std::string>{"deep"}));
}

TEST(RequireScannerTest, DeduplicatesPreservingOrder) {
  EXPECT_EQ(scan("require('b'); require('a'); require('b');"),
            (std::vector<std::string>{"b", "a"}));
}

TEST(RequireScannerTest, IgnoresNonLiteralArguments) {
  EXPECT_EQ(scan("require(name); require('ok'); require(`t${x}`);"),
            (std::vector<std::string>{"ok"}));
}

TEST(RequireScannerTest, IgnoresRequireResolveAndMemberCalls) {
  // require.resolve() is not a module load; obj.require() is not our require.
  EXPECT_EQ(scan("require.resolve('x'); obj.require('y'); require('z');"),
            (std::vector<std::string>{"z"}));
}

TEST(RequireScannerTest, IgnoresRequireWithNoArguments) {
  EXPECT_EQ(scan("require(); require('a');"),
            (std::vector<std::string>{"a"}));
}

TEST(RequireScannerTest, AcceptsTemplateLiteralWithNoSubstitutions) {
  // `foo` is a literal string; treating it as one avoids a silent miss.
  EXPECT_EQ(scan("require(`foo`);"), (std::vector<std::string>{"foo"}));
}

TEST(RequireScannerTest, ParsesTypeScriptWhenEnabled) {
  EXPECT_EQ(scan("const x: string = require('ts-dep');", /*ts*/ true),
            (std::vector<std::string>{"ts-dep"}));
}

TEST(RequireScannerTest, ReportsParseError) {
  std::vector<std::string> out;
  std::string error;
  EXPECT_FALSE(scanRequires("function (", false, &out, &error));
  EXPECT_FALSE(error.empty());
}

} // namespace
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build cmake-build-asan --target RequireScannerTest`
Expected: FAIL, no such header.

- [ ] **Step 3: Implement `require_scanner.cpp`**

Use Hermes's own parser. The shape:

```cpp
#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Parser/JSParser.h"

namespace {
class RequireVisitor {
 public:
  explicit RequireVisitor(std::vector<std::string> *out) : out_(out) {}

  bool incRecursionDepth(hermes::ESTree::Node *) { return true; }
  void decRecursionDepth() {}

  void visit(hermes::ESTree::Node *node) {
    hermes::ESTree::visitESTreeChildren(*this, node);
  }

  void visit(hermes::ESTree::CallExpressionNode *node) {
    collect(node);
    hermes::ESTree::visitESTreeChildren(*this, node);
  }

 private:
  void collect(hermes::ESTree::CallExpressionNode *node);
  std::vector<std::string> *out_;
};
} // namespace
```

`collect` accepts the call only when **all** of these hold, which is what the
tests above pin down:
- the callee is an `IdentifierNode` whose name is exactly `require` (this
  rejects both `require.resolve(...)`, whose callee is a `MemberExpression`,
  and `obj.require(...)` for the same reason);
- there is at least one argument;
- the first argument is a `StringLiteralNode`, or a `TemplateLiteralNode` with
  exactly one quasi and no expressions.

Append the string to `*out_` if not already present (linear scan is fine;
per-file specifier counts are small).

Driver:

```cpp
bool scanRequires(std::string_view source, bool enableTS,
                  std::vector<std::string> *out, std::string *error) {
  auto context = std::make_shared<hermes::Context>();
  if (enableTS)
    context->setParseTS(true);
  // Collect diagnostics into `error` rather than printing to stderr.
  // ... install a SourceErrorManager handler ...
  hermes::parser::JSParser parser(*context, llvh::StringRef(source.data(), source.size()));
  auto program = parser.parse();
  if (!program) { /* set *error */ return false; }
  RequireVisitor v(out);
  hermes::ESTree::visitESTreeNode(v, *program);
  return true;
}
```

Add to `lib/bundle/CMakeLists.txt` a second target that carries the Hermes
dependency, keeping `hermesNodeBundle` free of it:

```cmake
add_hermes_library(hermesNodeBundleBuild STATIC
  require_scanner.cpp
)
target_include_directories(hermesNodeBundleBuild
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/hermes/include
)
target_link_libraries(hermesNodeBundleBuild
  PUBLIC hermesNodeBundle
  PRIVATE hermesvm_a
)
```

Register the test in `unittests/CMakeLists.txt`:

```cmake
add_node_compat_unittest(RequireScannerTest RequireScannerTest.cpp)
target_link_libraries(RequireScannerTest hermesNodeBundleBuild)
```

The GC define is subdirectory-scoped and does not propagate; if the target
fails to link, add
`target_compile_definitions(hermesNodeBundleBuild PRIVATE HERMESVM_GC_${HERMESVM_GCKIND})`
as the other libraries in this tree do.

- [ ] **Step 4: Run the tests**

Run: `cmake --build cmake-build-asan --target RequireScannerTest && ./cmake-build-asan/unittests/RequireScannerTest`
Expected: all PASS.

- [ ] **Step 5: Format check and commit**

```bash
./utils/format.sh --check
git add include/hermes/node-compat/bundle lib/bundle unittests/RequireScannerTest.cpp unittests/CMakeLists.txt
git commit -m "Add literal require() scanner using the Hermes parser"
```

---

### Task 4: Resolution and the build root

**Files:**
- Create: `include/hermes/node-compat/bundle/bundle_resolve.h`
- Create: `lib/bundle/bundle_resolve.cpp`
- Modify: `lib/bundle/CMakeLists.txt`
- Test: `unittests/BundleResolveTest.cpp`, `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `std::optional<std::string> resolveSpecifier(std::string_view fromFile, std::string_view specifier)`
    -- absolute path, or nullopt if it does not resolve on disk.
  - `bool isBuiltinSpecifier(std::string_view specifier)` -- true for `fs`,
    `node:fs`, and the rest of the embedded builtin list.
  - `std::string commonAncestor(const std::vector<std::string> &absPaths)`

- [ ] **Step 1: Write the failing test**

Create `unittests/BundleResolveTest.cpp` with a fixture that builds a real
temporary tree (the resolver stats the filesystem, so a fake would not
exercise it):

```
<tmp>/app/cli.js
<tmp>/app/lib/util.js
<tmp>/app/lib/index.js
<tmp>/app/data.json
<tmp>/app/node_modules/dep/package.json   {"main":"main.js"}
<tmp>/app/node_modules/dep/main.js
<tmp>/app/node_modules/noMain/index.js
```

Cases:
- `resolveSpecifier("<tmp>/app/cli.js", "./lib/util")` -> `<tmp>/app/lib/util.js`
- `resolveSpecifier("<tmp>/app/cli.js", "./lib")` -> `<tmp>/app/lib/index.js`
- `resolveSpecifier("<tmp>/app/cli.js", "./data.json")` -> `<tmp>/app/data.json`
- `resolveSpecifier("<tmp>/app/cli.js", "dep")` -> `<tmp>/app/node_modules/dep/main.js`
- `resolveSpecifier("<tmp>/app/cli.js", "noMain")` -> `<tmp>/app/node_modules/noMain/index.js`
- `resolveSpecifier("<tmp>/app/cli.js", "missing")` -> nullopt
- `isBuiltinSpecifier("fs")` and `isBuiltinSpecifier("node:fs")` -> true
- `isBuiltinSpecifier("dep")` -> false
- `commonAncestor({"/a/b/c.js", "/a/b/d/e.js"})` -> `/a/b`
- `commonAncestor({"/a/b/c.js"})` -> `/a/b`
- `commonAncestor({"/a/x.js", "/b/y.js"})` -> `/`

Use an `EnvGuard`-style RAII temp directory that removes itself, matching the
pattern already used in `unittests/CompileCacheTest.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build cmake-build-asan --target BundleResolveTest`
Expected: FAIL, no such header.

- [ ] **Step 3: Implement**

`resolveSpecifier` implements the subset of Node resolution the producer
needs, in this order:

1. If the specifier starts with `./` or `../`, join it against
   `dirname(fromFile)`; otherwise walk `node_modules` upward from
   `dirname(fromFile)` to the filesystem root, trying
   `<dir>/node_modules/<specifier>` at each level.
2. For the resulting base path, try in order: the exact path; `+ ".js"`;
   `+ ".ts"`; `+ ".json"`; then as a directory: `package.json`'s `main` field
   resolved recursively through the same extension list; `index.js`;
   `index.ts`; `index.json`.
3. Return the first that exists as a regular file, made absolute and
   normalized.

`package.json` parsing needs only the `main` string. Do not pull in a JSON
library; scan for `"main"` and read the following string value, and treat a
malformed or absent `main` as not present. The `exports` field is
deliberately not supported in v1 -- record that in a comment, since it is the
most likely source of a future resolution mismatch with Node.

`isBuiltinSpecifier` strips a leading `node:` and checks against the same
list `libjs/shims/internal/bootstrap/realm.js` uses (31 names).

`commonAncestor` operates on the `dirname` of each path, splits on `/`, and
takes the longest shared prefix. With one input it returns that file's
directory. With no shared prefix it returns `/`.

- [ ] **Step 4: Run the tests**

Run: `./cmake-build-asan/unittests/BundleResolveTest`
Expected: all PASS.

- [ ] **Step 5: Format check and commit**

```bash
./utils/format.sh --check
git add include/hermes/node-compat/bundle lib/bundle unittests/BundleResolveTest.cpp unittests/CMakeLists.txt
git commit -m "Add bundle specifier resolution and build root computation"
```

---

### Task 5: The producer -- walk, compile, write

**Files:**
- Create: `include/hermes/node-compat/bundle/bundle_build.h`
- Create: `lib/bundle/bundle_build.cpp`
- Modify: `lib/bundle/CMakeLists.txt`
- Test: `test/bundle-build.js` (lit)

**Interfaces:**
- Consumes: `scanRequires`, `resolveSpecifier`, `isBuiltinSpecifier`,
  `commonAncestor`, `BundleWriter`, `bundleGenerationTag`.
- Produces:
  - `int buildBundle(napi_env env, const std::string &entryPath, const std::string &outPath)`
    -- returns 0 on success, non-zero on failure, printing diagnostics to stderr.

- [ ] **Step 1: Write the failing lit test**

Create `test/bundle-build.js`:

```js
// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib %t.tree/node_modules/dep
// RUN: echo "const d = require('dep'); const u = require('./lib/util'); const c = require('./cfg.json'); console.log(d.v + u.v + c.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 100 }' > %t.tree/cfg.json
// RUN: echo '{ "main": "main.js" }' > %t.tree/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 10 };" > %t.tree/node_modules/dep/main.js
// The producer prints the computed build root, and the container starts with
// the magic. Both are asserted -- checking only that a file appeared would
// pass on an empty file.
// RUN: %hermes-node --build-bundle=%t.bundle %t.tree/cli.js | %FileCheck --check-prefix=ROOT %s
// ROOT: bundle root: {{.*}}.tree
// RUN: head -c 8 %t.bundle | %FileCheck --check-prefix=MAGIC %s
// MAGIC: HNBUNDLE

// This file is a lit driver only; the RUN lines above are the test.
```

- [ ] **Step 2: Run to verify it fails**

Run the single-test command from `CLAUDE.md` with this file.
Expected: FAIL, `unknown option '--build-bundle'`.

- [ ] **Step 3: Implement the producer**

`buildBundle` performs:

1. Make `entryPath` absolute and verify it is a regular file; error out
   naming the path if not.
2. Worklist starting from the entry. For each path not yet visited:
   - read the file;
   - if the extension is `.json`, record it as a `kJSON` module with the file
     text as payload and do not scan it;
   - otherwise call `scanRequires(source, endsWith(path, ".ts"), ...)`;
     a parse failure is a hard error naming the file and the parser message;
   - for each specifier: skip it when `isBuiltinSpecifier(specifier)`;
     otherwise `resolveSpecifier(path, specifier)`. A nullopt result is a
     **hard error**: print `error: cannot resolve '<specifier>' from <path>`
     and return non-zero. A resolution whose extension is not `.js`, `.ts`,
     or `.json` prints `warning: skipping <path> (<ext> is not packageable)`,
     records no edge, and continues -- the runtime fallback handles it.
   - record the edge and push the target onto the worklist.
3. Compute the build root with `commonAncestor` over every visited path, and
   print `bundle root: <root>` so the artifact's placement requirement is
   visible.
4. For each visited JavaScript file, compile with `hermes_compile_to_bytecode`
   using the same wrapper the loader applies
   (`(function(exports, require, module, __filename, __dirname) {` ... `\n})`)
   and `optimize = true`. A compile error is a hard error naming the file.
5. `addModule` for every file with its identity = path relative to the root,
   `addEdge` for every recorded edge, `setEntry` to the entry's index.
6. `serialize(bundleGenerationTag())` and write to `outPath` via a temp file
   plus `rename`, so a failed build never leaves a partial bundle in place.

Note the ordering constraint: module indices must be assigned before edges
can reference them, so run the whole walk first and add modules and edges in
two passes.

`lib/bundle/CMakeLists.txt` gains `bundle_build.cpp` on
`hermesNodeBundleBuild` and links `hermesNapiCompile` PUBLIC, matching how
`hermesNodeCompileCacheRun` does it.

- [ ] **Step 4: Wire the CLI flag**

In `tools/hermes-node/hermes-node.cpp`, add to the argument loop, next to
the existing `--compile-cache=` branch:

```cpp
} else if (std::strncmp(argv[i], "--build-bundle=", 15) == 0) {
  config.buildBundlePath = argv[i] + 15;
```

Add `std::string buildBundlePath;` to `HermesNodeConfig` and, in
`runHermesNode`, take the producer path instead of running the script when it
is non-empty. Add to `printUsage`:

```
  --build-bundle=<file>          Compile the script and its requires into <file>
```

- [ ] **Step 5: Run the test**

Expected: PASS, and `%t.bundle` exists.

- [ ] **Step 6: Format check and commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle tools/hermes-node test/bundle-build.js include/hermes/node-compat/runtime
git commit -m "Add --build-bundle producer mode"
```

---

### Task 6: The consumer -- load, validate, and run

**Files:**
- Create: `include/hermes/node-compat/bundle/bundle_run.h`
- Create: `lib/bundle/bundle_run.cpp`
- Create: `libjs/bundle-loader.js`
- Modify: `lib/embedded-modules/CMakeLists.txt` (register the new JS file)
- Modify: `tools/hermes-node/hermes-node.cpp`, `lib/runtime/hermes_node_runtime.cpp`
- Test: `test/bundle-run.js` (lit)

**Interfaces:**
- Consumes: `BundleReader`, `bundleGenerationTag`.
- Produces:
  - native `__bundleLookup(importerIdentity, specifier) -> identity|undefined`
  - native `__bundleLoad(identity) -> function|string` (wrapper function for
    JS, raw text for JSON)
  - native `__bundleEntry() -> identity`
  - native `__bundleRoot() -> string`

- [ ] **Step 1: Write the failing lit test**

Create `test/bundle-run.js`. The critical case is running with the source
tree **deleted**:

```js
// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib %t.tree/node_modules/dep
// RUN: echo "const d = require('dep'); const u = require('./lib/util'); const c = require('./cfg.json'); console.log('SUM', d.v + u.v + c.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 100 }' > %t.tree/cfg.json
// RUN: echo '{ "main": "main.js" }' > %t.tree/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 10 };" > %t.tree/node_modules/dep/main.js
// RUN: %hermes-node --build-bundle=%t.tree/app.bundle %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.bundle | %FileCheck %s

// Delete every source file, keeping only the bundle, and run again. This is
// the test that actually demonstrates self-sufficiency.
// RUN: find %t.tree -name '*.js' -delete && find %t.tree -name '*.json' -delete
// RUN: %hermes-node --bundle=%t.tree/app.bundle | %FileCheck %s

// CHECK: SUM 111
```

- [ ] **Step 2: Run to verify it fails**

Expected: FAIL, `unknown option '--bundle'`.

- [ ] **Step 3: Implement the native side**

`bundle_run.cpp` owns a process-global `BundleReader` plus its mapping,
opened once at startup:

- `openBundle(const std::string &path, std::string *error)` -- `open()`,
  `fstat`, `mmap` read-only, then `BundleReader::open(..., bundleGenerationTag(), error)`.
  A failure here is fatal: print `error: <message>` and exit non-zero. Never
  fall back to disk on a bad bundle, per the spec.
- The root is `dirname(realpath(path))`.
- Identity-keyed lookup: the JS side works in identities, so
  `__bundleLookup` maps `identity -> module index` (a `std::unordered_map`
  built once at open; the *edge* table stays index-keyed and is what the
  lookup consults), then calls `BundleReader::lookup`.
- `__bundleLoad` for `kJavaScript` calls `hermes_run_bytecode` on the payload
  and returns the resulting wrapper function; for `kJSON` it returns the text
  and lets JS call `JSON.parse`.

- [ ] **Step 4: Implement `libjs/bundle-loader.js`**

Installed during bootstrap only when a bundle is active. It wraps
`Module._load`, following the precedent noted at `libjs/loader.js:257`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Wraps Node's CJS Module._load so a bundled (importer, specifier) pair is
// served from the container without touching the filesystem. Anything that
// misses falls through to the original _load, which resolves and compiles
// from disk exactly as it does without a bundle.

(function() {
  'use strict';
  return function installBundleLoader(Module, bundle, path) {
    var cache = Object.create(null);
    var root = bundle.root();

    function identityToPath(identity) {
      return root + '/' + identity;
    }

    var originalLoad = Module._load;
    Module._load = function(request, parent, isMain) {
      var importer = parent && parent.__bundleIdentity;
      if (importer === undefined && !isMain) {
        return originalLoad.call(this, request, parent, isMain);
      }
      var target = bundle.lookup(importer, request);
      if (target === undefined) {
        if (process.env.HERMES_NODE_DEBUG_NATIVE &&
            process.env.HERMES_NODE_DEBUG_NATIVE.indexOf('BUNDLE') >= 0) {
          console.error('[bundle] miss: ' + request + ' from ' +
                        (importer === undefined ? '<entry>' : importer));
        }
        return originalLoad.call(this, request, parent, isMain);
      }
      if (cache[target]) return cache[target].exports;

      var filename = identityToPath(target);
      var mod = {
        id: filename,
        exports: {},
        loaded: false,
        filename: filename,
        __bundleIdentity: target,
      };
      cache[target] = mod;

      var payload = bundle.load(target);
      if (typeof payload === 'string') {
        mod.exports = JSON.parse(payload);
      } else {
        var dirname = filename.substring(0, filename.lastIndexOf('/'));
        var req = function(name) { return Module._load(name, mod, false); };
        req.resolve = function(name) { return name; };
        payload(mod.exports, req, mod, filename, dirname);
      }
      mod.loaded = true;
      return mod.exports;
    };
  };
})();
```

Note the cache is populated **before** the module body runs, which is what
makes circular requires terminate -- the same reason `libjs/loader.js:64`
does it.

Register the file in `lib/embedded-modules/CMakeLists.txt` alongside the
other `libjs/` entries, and reconfigure CMake afterward.

- [ ] **Step 5: Wire the CLI flag and bootstrap**

Add `--bundle=<file>` parsing to `hermes-node.cpp` setting
`config.bundlePath`, and reject it together with the inspector, immediately
after the existing `--optimize=on` check and in the same shape:

```cpp
if (!config.bundlePath.empty() && (config.inspect || config.inspectBrk)) {
  std::fprintf(
      stderr,
      "Error: --bundle cannot be combined with --inspect or --inspect-brk.\n"
      "Bundled code is compiled without the full debug info the debugger "
      "needs to set breakpoints.\n");
  return 1;
}
```

In `runHermesNode`, when `bundlePath` is set: open the bundle before the user
script step, install the loader, and run the entry identity instead of
`__loadUserScript`. `process.argv[1]` is the bundle path as given.

Add to `printUsage`:

```
  --bundle=<file>                Run an application from a bundle file
```

- [ ] **Step 6: Run the test**

Expected: PASS both before and after the tree is deleted.

- [ ] **Step 7: Format check and commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle libjs/bundle-loader.js lib/embedded-modules tools/hermes-node lib/runtime test/bundle-run.js
git commit -m "Add --bundle consumer mode"
```

---

### Task 7: Error paths and the fallback

**Files:**
- Test: `test/bundle-errors.js`, `test/bundle-fallback.js` (lit)
- Modify: whichever of `lib/bundle/*.cpp` the tests find wanting.

- [ ] **Step 1: Write `test/bundle-errors.js`**

```js
// Unresolvable specifier is a hard build error.
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "require('nope-not-here');" > %t.tree/cli.js
// RUN: %not %hermes-node --build-bundle=%t.b %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=UNRESOLVED %s
// UNRESOLVED: cannot resolve 'nope-not-here'

// A corrupt container is a hard error, not a fallback.
// RUN: echo "console.log('ok');" > %t.tree/ok.js
// RUN: %hermes-node --build-bundle=%t.good %t.tree/ok.js
// RUN: cp %t.good %t.bad && printf 'X' | dd of=%t.bad bs=1 seek=0 count=1 conv=notrunc status=none
// RUN: %not %hermes-node --bundle=%t.bad 2>&1 | %FileCheck --check-prefix=CORRUPT %s
// CORRUPT: not a hermes-node bundle

// Truncation is a hard error too.
// RUN: head -c 40 %t.good > %t.trunc
// RUN: %not %hermes-node --bundle=%t.trunc 2>&1 | %FileCheck --check-prefix=TRUNC %s
// TRUNC: error:

// The inspector is refused.
// RUN: %not %hermes-node --inspect --bundle=%t.good 2>&1 | %FileCheck --check-prefix=INSPECT %s
// INSPECT: cannot be combined with --inspect
```

`dd conv=notrunc` and `head -c` are used rather than GNU-only `truncate`,
matching the portability rule established in `test/compile-cache-corrupt.js`.

- [ ] **Step 2: Write `test/bundle-fallback.js`**

A module reached only by a computed `require` is invisible to static
discovery, must still run, and must be logged:

```js
// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "const n = 'dyn' + ''; const m = require('./' + n); console.log('GOT', m.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 7 };" > %t.tree/dyn.js
// RUN: %hermes-node --build-bundle=%t.tree/app.bundle %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.bundle | %FileCheck %s
// CHECK: GOT 7

// With the tree present, the miss falls back to disk and says so.
// RUN: env HERMES_NODE_DEBUG_NATIVE=BUNDLE %hermes-node --bundle=%t.tree/app.bundle 2>&1 | %FileCheck --check-prefix=MISS %s
// MISS: [bundle] miss: ./dyn
```

- [ ] **Step 3: Run both, fix what fails, re-run**

Expected: PASS. If the fallback path throws instead of falling through, the
bug is in `bundle-loader.js`'s miss branch, not in the tests.

- [ ] **Step 4: Format check and commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add test/bundle-errors.js test/bundle-fallback.js lib/bundle libjs/bundle-loader.js
git commit -m "Add bundle error-path and fallback tests"
```

---

### Task 8: Example, docs, and progress file

**Files:**
- Create: `history/plans/progress-aot-bundle.md`
- Modify: `README.md`, `CLAUDE.md`
- Test: `test/bundle-yargs.js` (lit), gated on the example's `node_modules`

- [ ] **Step 1: Bundle a real package end to end**

`examples/yargs-cli` has 50 files. Confirm by hand:

```bash
cd examples/yargs-cli && npm install
../../cmake-build-release/bin/hermes-node --build-bundle=/tmp/yargs.bundle ./cli.js
mv node_modules /tmp/stashed-nm
../../cmake-build-release/bin/hermes-node --bundle=/tmp/yargs.bundle -- --help
mv /tmp/stashed-nm node_modules
```

Expected: the same output with `node_modules` absent as with it present. If
resolution fails on a package that uses `exports` rather than `main`, that is
the known v1 limitation from Task 4 -- record it in the progress file rather
than expanding scope.

- [ ] **Step 2: Add the lit test**

`test/bundle-yargs.js` guarded so it skips when the example has not been
installed, following how `check-hermes-node-examples` stays out of the
offline default suite:

```js
// REQUIRES: examples-installed
// RUN: %hermes-node --build-bundle=%t.bundle %source_dir/examples/yargs-cli/cli.js
// RUN: %hermes-node --bundle=%t.bundle -- --help | %FileCheck %s
// CHECK: Usage
```

Add an `examples-installed` feature to `test/lit.cfg`, set when
`examples/yargs-cli/node_modules` exists.

- [ ] **Step 3: Write the progress file**

Create `history/plans/progress-aot-bundle.md` naming this plan, with a table
of the eight tasks and their status, matching the format of
`history/plans/progress-compile-cache.md`.

- [ ] **Step 4: Document**

`CLAUDE.md` gains an "AOT Bundles" section covering: the two flags, that the
bundle must sit at the printed root, that `.node` and non-JS/JSON assets stay
on disk, that the inspector is refused, and that `exports`-based resolution
is unsupported in v1.

`README.md` gains a short section after "Compile cache" describing
`--build-bundle` and `--bundle`, the deleted-tree property, and the same
limitations. Keep it to the register of the surrounding text -- no em dashes,
no smart quotes.

- [ ] **Step 5: Full suite, format check, commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add history/plans/progress-aot-bundle.md test/bundle-yargs.js test/lit.cfg README.md CLAUDE.md
git commit -m "Document AOT bundle mode and add the yargs end-to-end test"
```

---

## Self-review notes

**Spec coverage.** Container format -> Task 1. Generation tag -> Task 2.
Static discovery -> Task 3. Identity, root, resolution -> Task 4. Producer,
JSON, TypeScript -> Task 5. Consumer, builtin precedence, `__filename` /
`__dirname` -> Task 6. Every row of the spec's error-policy table -> Tasks 5
(build errors) and 7 (runtime errors). Testing section -> Tasks 1, 6, 7, 8.

**Known gaps, deliberate.** `package.json` `exports` is unsupported in v1
(Task 4); it is the most likely cause of a resolution mismatch with Node and
is called out in the docs rather than silently omitted. The `--bundle-root`
override named as a mitigation in the spec's Risks section is not built --
the producer prints the root instead, which is the cheaper half of that
mitigation.

**Not yet verified.** The exact `RecursiveVisitor` member signatures in
Task 3 (`incRecursionDepth` / `decRecursionDepth`) are written from the
header's shape and may need adjustment against
`hermes/include/hermes/AST/RecursiveVisitor.h` at implementation time. The
Task 3 implementer should read that header first and adapt; the tests define
the contract regardless.
