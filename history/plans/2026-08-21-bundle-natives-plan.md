# Native addons in a bundle: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package native `.node` addons into an AOT bundle as records whose
bytes ship as flat sidecar files next to the container, loadable through
`process.dlopen` from inside the closed world.

**Architecture:** Format v4 gains a `kNative` module kind plus a native table
(module index, sidecar name, byte length, SHA-256) parallel to the existing
preload table. The producer copies each addon into the output directory
under a collision-free basename; the run layer exposes the identity-to-
sidecar map once at install time; the JS loader `dlopen`s the sidecar
instead of running a module wrapper. A fourth read-only verb,
`--verify-natives`, checks the recorded hashes on demand.

**Tech Stack:** C++17, CMake/Ninja, GTest, LLVM lit + FileCheck, picohash
(vendored, SHA-256), Node-API.

**Spec:** `history/plans/2026-08-21-bundle-natives-design.md`

## Global Constraints

- New files carry `Copyright (c) Tzvetan Mikov.` (NOT Meta Platforms), MIT
  license header, matching the existing files in the same directory.
- ASCII only in commit messages and source. No emojis. Use `--` for dashes.
- Build and test with `cmake-build-asan` (Debug + Clang + ASAN).
- Before every commit: `./utils/format.sh -f` then
  `cmake --build cmake-build-asan --target check-hermes-node`.
- Never `git add hermes`, never `git submodule update hermes`.
- Never push. Never check in `examples/*/node_modules`.
- `hermesNodeBundle` and `hermesNodeBundleTools` must stay free of a link
  dependency on the Hermes VM (`hermesvm_a`). picohash is VM-free, so it may
  be linked into either.
- Format version is bumped exactly once, in Task 1, to `4`. No later task
  bumps it again.
- Single-source constant: the SHA-256 hex digest is 64 characters; the raw
  digest is 32 bytes (`PICOHASH_SHA256_DIGEST_LENGTH`).

**Run a single lit test** (absolute paths required):

```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/bundle-natives.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param hello_addon=$(pwd)/cmake-build-asan/lib/hello_addon.node \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

(The `hello_addon` path is whatever `$<TARGET_FILE:hello_addon>` resolves to
in your build directory; check `cmake-build-asan/CMakeCache.txt` or just run
the whole suite instead.)

**Run one GTest binary:**

```bash
cmake-build-asan/unittests/BundleFormatTest --gtest_filter='*Native*'
```

---

## File Structure

**Created:**

- `include/hermes/node-compat/bundle/native_digest.h` -- declaration of
  `nativeFileDigest()`, the one SHA-256-of-a-file helper. Its own header
  because both the producer (`bundle_build.cpp`, in
  `hermesNodeBundleBuild`) and the tools (`bundle_tools.cpp`, in
  `hermesNodeBundleTools`) need it and neither may depend on the other.
- `lib/bundle/native_digest.cpp` -- implementation, in `hermesNodeBundle`.
- `test/bundle-natives.js` -- build-and-run behavior.
- `test/bundle-natives-errors.js` -- missing sidecar, unrecorded addon,
  `--extract-module` on a native.
- `test/bundle-verify-natives.js` -- the new verb's three outcomes.
- `examples/hermes-parser-ast/{package.json,ast.js,run.sh,README.md}`.

**Modified:**

- `include/hermes/node-compat/bundle/bundle_format.h` -- `kNative`,
  `BundleNativeRecord`, header fields, version 4.
- `include/hermes/node-compat/bundle/bundle_writer.h` +
  `lib/bundle/bundle_writer.cpp` -- `addNative()`, native table layout.
- `include/hermes/node-compat/bundle/bundle_reader.h` +
  `lib/bundle/bundle_reader.cpp` -- validation, `nativeCount()`,
  `native(i)`, `nativeFor(moduleIndex)`.
- `lib/bundle/bundle_resolve.cpp` -- explicit absolute-specifier branch.
- `lib/bundle/bundle_build.cpp` -- native classification, sidecar naming,
  copying, reporting.
- `lib/bundle/bundle_run.cpp` -- `__bundleNatives`, `__bundleLoad` refusal.
- `lib/bundle/bundle_tools.cpp` +
  `include/hermes/node-compat/bundle/bundle_tools.h` -- `NATIVES` dump
  section, `verifyNatives()`, `--extract-module` refusal.
- `lib/bundle/CMakeLists.txt` -- `native_digest.cpp`, picohash link.
- `lib/runtime/hermes_node_runtime.cpp` -- pass `fs` to
  `installBundleLoader`.
- `libjs/bundle-loader.js` -- the dlopen path, error text, deleted special
  case.
- `tools/hermes-node/hermes-node.cpp` -- `--verify-natives` parsing,
  dispatch, conflict matrix, help text.
- `unittests/{BundleFormatTest,BundleToolsTest,BundleResolveTest,BundleFileSourceTest}.cpp`.
- `test/bundle-{dump,verbose,tool-errors,tool-no-runtime}.js`.
- `CLAUDE.md`, `history/plans/progress-aot-bundle.md`.
- `examples/run-examples.sh` (register the new example).

---

## Task 1: Format v4 -- the native table

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_format.h`
- Modify: `include/hermes/node-compat/bundle/bundle_writer.h`
- Modify: `lib/bundle/bundle_writer.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h`
- Modify: `lib/bundle/bundle_reader.cpp`
- Test: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Produces:
  - `ModuleKind::kNative = 2`
  - `struct BundleNativeRecord { uint32_t moduleIndex, sidecarString, byteLength, hashString; }`
  - `BundleHeader` gains `uint32_t nativeTableOffset; uint32_t nativeCount;`
    placed immediately after `preloadCount` and before `payloadOffset`.
  - `void BundleWriter::addNative(uint32_t moduleIndex, std::string_view sidecar, uint32_t byteLength, std::string_view rawDigest)`
  - `struct BundleReader::NativeView { uint32_t moduleIndex; std::string_view sidecar; uint32_t byteLength; std::string_view digest; }`
  - `uint32_t BundleReader::nativeCount() const`
  - `NativeView BundleReader::native(uint32_t i) const`
  - `std::optional<NativeView> BundleReader::nativeFor(uint32_t moduleIndex) const`

- [ ] **Step 1: Write the failing test**

Append to `unittests/BundleFormatTest.cpp`:

```cpp
TEST(BundleFormatTest, NativeRecordRoundTrips) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  uint32_t addon = writer.addModule(
      "node_modules/a/build/Release/a.node", ModuleKind::kNative,
      kRequirable, "");
  writer.setEntry(entry);
  // A 32-byte digest with a NUL and a high byte, so the string table is
  // proven to carry raw bytes rather than a C string.
  std::string digest(32, '\x00');
  digest[0] = '\xff';
  digest[31] = '\x7f';
  writer.addNative(addon, "a.node", 4096, digest);

  std::vector<uint8_t> bytes = writer.serialize(/*generationTag=*/7);
  ASSERT_FALSE(bytes.empty());

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_EQ(reader->formatVersion(), 4u);
  EXPECT_EQ(reader->kind(addon), ModuleKind::kNative);
  EXPECT_EQ(reader->payload(addon).size(), 0u);

  ASSERT_EQ(reader->nativeCount(), 1u);
  BundleReader::NativeView view = reader->native(0);
  EXPECT_EQ(view.moduleIndex, addon);
  EXPECT_EQ(view.sidecar, "a.node");
  EXPECT_EQ(view.byteLength, 4096u);
  EXPECT_EQ(view.digest, digest);

  auto found = reader->nativeFor(addon);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->sidecar, "a.node");
  EXPECT_FALSE(reader->nativeFor(entry).has_value());
}

TEST(BundleFormatTest, NativeTableIsSortedByModuleIndex) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  uint32_t b = writer.addModule("b.node", ModuleKind::kNative, kRequirable, "");
  uint32_t a = writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  // Added out of module-index order on purpose.
  writer.addNative(b, "b.node", 1, std::string(32, 'b'));
  writer.addNative(a, "a.node", 2, std::string(32, 'a'));

  std::vector<uint8_t> bytes = writer.serialize(7);
  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  ASSERT_EQ(reader->nativeCount(), 2u);
  EXPECT_LT(reader->native(0).moduleIndex, reader->native(1).moduleIndex);
  EXPECT_EQ(reader->nativeFor(a)->sidecar, "a.node");
  EXPECT_EQ(reader->nativeFor(b)->sidecar, "b.node");
}

TEST(BundleFormatTest, NativeTableOutOfRangeModuleIndexIsRejected) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  // A real kNative module, so the container is valid until it is corrupted
  // below. Pointing the native record at `entry` instead would be rejected
  // by the "names a module that is not native" check, and this test would
  // pass without ever exercising the range check it is named for.
  uint32_t addon =
      writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 1, std::string(32, 'a'));
  std::vector<uint8_t> bytes = writer.serialize(7);

  // Corrupt the native record's moduleIndex to one past the last module.
  auto *header = reinterpret_cast<BundleHeader *>(bytes.data());
  auto *record = reinterpret_cast<BundleNativeRecord *>(
      bytes.data() + header->nativeTableOffset);
  record->moduleIndex = header->moduleCount;

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  EXPECT_FALSE(reader.has_value());
  EXPECT_NE(error.find("native"), std::string::npos) << error;
}

TEST(BundleFormatTest, NoNativesCostsNothing) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(7);
  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_EQ(reader->nativeCount(), 0u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BundleFormatTest
cmake-build-asan/unittests/BundleFormatTest --gtest_filter='*Native*'
```

Expected: compile failure -- `kNative`, `addNative`, `nativeCount`,
`BundleNativeRecord` do not exist.

- [ ] **Step 3: Extend the format header**

In `include/hermes/node-compat/bundle/bundle_format.h`:

```cpp
constexpr uint32_t kBundleFormatVersion = 4;

enum class ModuleKind : uint32_t {
  kJavaScript = 0,
  kJSON = 1,
  /// A native addon. Its bytes are NOT in the container: they ship as a
  /// flat sidecar file next to the bundle, because dlopen() takes a path
  /// and there is no portable way to load a shared object from memory.
  /// payloadOffset and payloadSize are zero; everything else about the
  /// addon lives in the native table (BundleNativeRecord below).
  kNative = 2,
};
```

Add to `BundleHeader`, immediately after `preloadCount` and before
`payloadOffset`:

```cpp
  // The native table: an array of BundleNativeRecord, sorted by
  // moduleIndex, one per kNative module. A section of its own rather than
  // three more fields on every module record: a real bundle has ~1500
  // modules and one or two natives, so the record would carry twelve bytes
  // of zeros per module to describe the exception. This mirrors the
  // preload table's reasoning above.
  uint32_t nativeTableOffset;
  uint32_t nativeCount;
```

And after `BundleEdgeRecord`:

```cpp
/// One per kNative module. `sidecarString` and `hashString` index the
/// string table; the hash entry holds the raw 32-byte SHA-256, not hex, so
/// it may contain NUL bytes -- which the string table's explicit length
/// prefix already allows.
///
/// `byteLength` and the digest are recorded at build time and read by
/// nothing on the run path: hashing at load would mean reading the whole
/// addon on every launch, in an artifact whose reason for existing is
/// startup cost. They exist for --dump and --verify-natives.
struct BundleNativeRecord {
  uint32_t moduleIndex;
  uint32_t sidecarString;
  uint32_t byteLength;
  uint32_t hashString;
};
```

- [ ] **Step 4: Extend the writer**

In `include/hermes/node-compat/bundle/bundle_writer.h`, after `addPreload`:

```cpp
  /// Records the sidecar file that carries module \p moduleIndex's bytes.
  /// \p rawDigest is the raw SHA-256 (32 bytes), not hex. Call order does
  /// not matter: serialize() sorts the table by module index, which is what
  /// BundleReader::nativeFor() binary-searches.
  void addNative(
      uint32_t moduleIndex,
      std::string_view sidecar,
      uint32_t byteLength,
      std::string_view rawDigest);
```

and in the private section:

```cpp
  struct PendingNative {
    uint32_t moduleIndex;
    std::string sidecar;
    uint32_t byteLength;
    std::string digest;
  };
  std::vector<PendingNative> natives_;
```

In `lib/bundle/bundle_writer.cpp`, after `addPreload`:

```cpp
void BundleWriter::addNative(
    uint32_t moduleIndex,
    std::string_view sidecar,
    uint32_t byteLength,
    std::string_view rawDigest) {
  natives_.push_back(PendingNative{
      moduleIndex,
      std::string(sidecar),
      byteLength,
      std::string(rawDigest)});
}
```

In `serialize()`, immediately after the edge-specifier interning loop (and
before the layout block, since interning must not run after the string
table's size is frozen):

```cpp
  // Sort by module index: that is the order BundleReader::nativeFor()
  // binary-searches. The producer adds natives in discovery order, which
  // is not module-index order once an --include seeds one late.
  std::sort(
      natives_.begin(),
      natives_.end(),
      [](const PendingNative &a, const PendingNative &b) {
        return a.moduleIndex < b.moduleIndex;
      });
  std::vector<uint32_t> nativeSidecarStrings(natives_.size());
  std::vector<uint32_t> nativeDigestStrings(natives_.size());
  for (size_t i = 0; i < natives_.size(); ++i) {
    nativeSidecarStrings[i] = internString(natives_[i].sidecar);
    nativeDigestStrings[i] = internString(natives_[i].digest);
  }
```

In the layout block, after `preloadTableSize`:

```cpp
  size_t nativeTableOffset = preloadTableOffset + preloadTableSize;
  size_t nativeTableSize = natives_.size() * sizeof(BundleNativeRecord);
  size_t payloadOffset =
      alignUp(nativeTableOffset + nativeTableSize, kBundlePayloadAlign);
```

(replacing the existing `payloadOffset` computation, which used
`preloadTableOffset + preloadTableSize`).

Header fields, after `header.preloadCount`:

```cpp
  header.nativeTableOffset = static_cast<uint32_t>(nativeTableOffset);
  header.nativeCount = static_cast<uint32_t>(natives_.size());
```

Emission, after the preload loop and before `appendPadding(out,
payloadOffset - out.size())`:

```cpp
  for (size_t i = 0; i < natives_.size(); ++i) {
    BundleNativeRecord record{};
    record.moduleIndex = natives_[i].moduleIndex;
    record.sidecarString = nativeSidecarStrings[i];
    record.byteLength = natives_[i].byteLength;
    record.hashString = nativeDigestStrings[i];
    appendPod(out, record);
  }
```

- [ ] **Step 5: Extend the reader**

In `include/hermes/node-compat/bundle/bundle_reader.h`, after `preload(i)`:

```cpp
  /// One entry of the native table -- see bundle_format.h. `sidecar` and
  /// `digest` are views into the mapped string table; `digest` is the raw
  /// 32-byte SHA-256 and may contain NUL bytes.
  struct NativeView {
    uint32_t moduleIndex;
    std::string_view sidecar;
    uint32_t byteLength;
    std::string_view digest;
  };

  uint32_t nativeCount() const;

  /// Only valid for \p i below nativeCount(), exactly like edge() above.
  NativeView native(uint32_t i) const;

  /// The native table entry for \p moduleIndex, or nullopt when that module
  /// is not a kNative. Binary search: the table is sorted by module index.
  std::optional<NativeView> nativeFor(uint32_t moduleIndex) const;
```

and a private member `const BundleNativeRecord *natives_ = nullptr;`.

In `lib/bundle/bundle_reader.cpp`, inside `openImpl`'s validation chain,
alongside the preload table's checks (mirror their exact shape and error
wording):

```cpp
  // The native table sits between the preload table and the payload.
  if (header->nativeTableOffset % alignof(uint32_t) != 0) {
    *error = "bundle native table is misaligned";
    return std::nullopt;
  }
  size_t nativeTableSize =
      static_cast<size_t>(header->nativeCount) * sizeof(BundleNativeRecord);
  if (header->nativeTableOffset > size ||
      nativeTableSize > size - header->nativeTableOffset) {
    *error = "bundle native table is out of range";
    return std::nullopt;
  }
  const auto *natives = reinterpret_cast<const BundleNativeRecord *>(
      data + header->nativeTableOffset);
  for (uint32_t i = 0; i < header->nativeCount; ++i) {
    if (natives[i].moduleIndex >= header->moduleCount) {
      *error = "bundle native table names a module that does not exist";
      return std::nullopt;
    }
    // Sorted and strictly increasing: nativeFor() binary-searches it, and
    // two rows for one module would make "the" sidecar ambiguous.
    if (i != 0 && natives[i].moduleIndex <= natives[i - 1].moduleIndex) {
      *error = "bundle native table is not sorted by module index";
      return std::nullopt;
    }
    // Every kNative module must have a row, and only a kNative module may.
    // Without this a container could name a JavaScript module as native
    // (loading its bytecode through dlopen) or leave a kNative with no
    // sidecar (a require() that cannot report what file is missing).
    if (modules[natives[i].moduleIndex].kind !=
        static_cast<uint32_t>(ModuleKind::kNative)) {
      *error = "bundle native table names a module that is not native";
      return std::nullopt;
    }
  }
  for (uint32_t i = 0; i < header->moduleCount; ++i) {
    if (modules[i].kind != static_cast<uint32_t>(ModuleKind::kNative))
      continue;
    bool found = false;
    for (uint32_t j = 0; j < header->nativeCount; ++j) {
      if (natives[j].moduleIndex == i) {
        found = true;
        break;
      }
    }
    if (!found) {
      *error = "bundle has a native module with no native table entry";
      return std::nullopt;
    }
  }
```

Validate the two string indices with whatever helper the existing code uses
for `identityString` and `specifierString` (follow that call exactly -- do
not invent a second validation path), then assign `reader.natives_ =
natives;` next to the existing `reader.preloads_` assignment.

Accessors:

```cpp
uint32_t BundleReader::nativeCount() const {
  return header_->nativeCount;
}

BundleReader::NativeView BundleReader::native(uint32_t i) const {
  const BundleNativeRecord &record = natives_[i];
  return NativeView{
      record.moduleIndex,
      stringAt(record.sidecarString),
      record.byteLength,
      stringAt(record.hashString)};
}

std::optional<BundleReader::NativeView> BundleReader::nativeFor(
    uint32_t moduleIndex) const {
  uint32_t lo = 0, hi = header_->nativeCount;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (natives_[mid].moduleIndex < moduleIndex)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < header_->nativeCount && natives_[lo].moduleIndex == moduleIndex)
    return native(lo);
  return std::nullopt;
}
```

Add a `nativeTableSize()` peer for the dump's section-size listing (Task 9
prints it):

```cpp
uint32_t BundleReader::nativeTableSize() const {
  return header_->nativeCount * sizeof(BundleNativeRecord);
}
```

declaring it in the header next to `edgeTableSize()`.

- [ ] **Step 6: Pin that a native is resolvable from the container**

`BundleFileSource::index()` (`lib/bundle/bundle_file_source.cpp:30`) builds
its entry list from every module index, filtering on neither kind nor
`kRequirable`, so a `kNative` is already indexed and `require.resolve` of
one is already answered. That is what the design requires, and it currently
holds by omission. Pin it, in `unittests/BundleFileSourceTest.cpp`:

```cpp
TEST(BundleFileSourceTest, ANativeIsAnOrdinaryIdentity) {
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t addon =
      writer.addModule("build/a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 8, std::string(32, 'z'));
  std::vector<uint8_t> bytes = writer.serialize(7);
  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;

  BundleFileSource src(*reader, "/app");
  // An empty payload must not make it invisible: require.resolve() has to
  // answer the same way require() will.
  EXPECT_TRUE(src.isRegularFile("/app/build/a.node"));
  EXPECT_TRUE(src.isDirectory("/app/build"));
  auto resolved = resolveSpecifier(src, "/app/cli.js", "./build/a.node");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, "/app/build/a.node");
}
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target BundleFormatTest BundleFileSourceTest
cmake-build-asan/unittests/BundleFormatTest
cmake-build-asan/unittests/BundleFileSourceTest
```

Expected: PASS, including the pre-existing cases (a v3 container in a
fixture, if any test carries one, must now report a version mismatch -- fix
the expectation, do not add back-compatibility).

- [ ] **Step 8: Run the whole suite**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
```

Expected: PASS. Existing bundle lit tests rebuild their containers from
source every run, so the version bump does not strand them.

- [ ] **Step 9: Commit**

```bash
git add include/hermes/node-compat/bundle/bundle_format.h \
        include/hermes/node-compat/bundle/bundle_writer.h \
        include/hermes/node-compat/bundle/bundle_reader.h \
        lib/bundle/bundle_writer.cpp lib/bundle/bundle_reader.cpp \
        unittests/BundleFormatTest.cpp unittests/BundleFileSourceTest.cpp \
        history/plans/2026-08-21-bundle-natives-design.md
git commit -m "bundle: format v4, a native table"
```

---

## Task 2: `nativeFileDigest()` -- SHA-256 of a file

**Files:**
- Create: `include/hermes/node-compat/bundle/native_digest.h`
- Create: `lib/bundle/native_digest.cpp`
- Modify: `lib/bundle/CMakeLists.txt`
- Test: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `struct NativeDigest { uint32_t byteLength; std::string raw; }` -- `raw`
    is 32 bytes.
  - `std::optional<NativeDigest> nativeFileDigest(const std::string &path, std::string *error)`
  - `std::string nativeDigestToHex(std::string_view raw)` -- 64 lowercase
    hex characters.

- [ ] **Step 1: Write the failing test**

Append to `unittests/BundleFormatTest.cpp` (it already has a temp-file
helper pattern via `unittests/TempTree.h` -- use it; if it does not fit,
write the file with `std::ofstream` into `std::filesystem::temp_directory_path()`
and remove it at the end of the test):

```cpp
TEST(BundleFormatTest, NativeFileDigestMatchesKnownVector) {
  // SHA-256("abc"), the standard test vector.
  const char kAbcHex[] =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  TempTree tree;
  std::string path = tree.write("blob.bin", "abc");

  std::string error;
  auto digest = nativeFileDigest(path, &error);
  ASSERT_TRUE(digest.has_value()) << error;
  EXPECT_EQ(digest->byteLength, 3u);
  EXPECT_EQ(digest->raw.size(), 32u);
  EXPECT_EQ(nativeDigestToHex(digest->raw), kAbcHex);
}

TEST(BundleFormatTest, NativeFileDigestOfEmptyFile) {
  const char kEmptyHex[] =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  TempTree tree;
  std::string path = tree.write("empty.bin", "");
  std::string error;
  auto digest = nativeFileDigest(path, &error);
  ASSERT_TRUE(digest.has_value()) << error;
  EXPECT_EQ(digest->byteLength, 0u);
  EXPECT_EQ(nativeDigestToHex(digest->raw), kEmptyHex);
}

TEST(BundleFormatTest, NativeFileDigestReportsAMissingFile) {
  std::string error;
  auto digest = nativeFileDigest("/nonexistent/nope.node", &error);
  EXPECT_FALSE(digest.has_value());
  EXPECT_FALSE(error.empty());
}
```

If `TempTree` has no `write(name, contents)` returning an absolute path,
add one there rather than duplicating file-writing code in the test.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BundleFormatTest
```

Expected: compile failure -- `native_digest.h` does not exist.

- [ ] **Step 3: Write the header**

`include/hermes/node-compat/bundle/native_digest.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_NATIVE_DIGEST_H
#define HERMES_NODE_COMPAT_BUNDLE_NATIVE_DIGEST_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// A native addon's length and content hash, as recorded in the container's
/// native table.
struct NativeDigest {
  uint32_t byteLength = 0;
  /// The raw SHA-256: exactly 32 bytes, which may include NULs.
  std::string raw;
};

/// SHA-256 and byte length of the file at \p path, streamed rather than
/// read whole: an addon can be tens of megabytes and nothing here needs the
/// bytes themselves.
///
/// SHA-256 rather than the CRC32 used for the generation tag and the
/// compile cache, because --verify-natives is offered as a manual security
/// check and a CRC can be forged to any value. It is still an audit and not
/// an enforcement -- see the design doc -- but the hash itself should not
/// be the weak part.
///
/// Returns nullopt with \p error set when the file cannot be opened or
/// read, or when it is larger than a uint32_t can express (the container
/// field is 32-bit, and a 4 GiB shared object is a mistake worth naming
/// rather than truncating).
std::optional<NativeDigest> nativeFileDigest(
    const std::string &path,
    std::string *error);

/// \p raw (32 bytes) as 64 lowercase hex characters.
std::string nativeDigestToHex(std::string_view raw);

} // namespace node_compat
} // namespace hermes

#endif
```

- [ ] **Step 4: Write the implementation**

`lib/bundle/native_digest.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/native_digest.h>

#include <picohash_wrapper.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace hermes {
namespace node_compat {

std::optional<NativeDigest> nativeFileDigest(
    const std::string &path,
    std::string *error) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    *error = "cannot open " + path + ": " + std::strerror(errno);
    return std::nullopt;
  }

  picohash_ctx_t ctx;
  ph_init_sha256(&ctx);

  NativeDigest result;
  uint64_t total = 0;
  char buffer[64 * 1024];
  while (true) {
    size_t n = std::fread(buffer, 1, sizeof(buffer), f);
    if (n == 0)
      break;
    ph_update(&ctx, buffer, n);
    total += n;
    if (total > std::numeric_limits<uint32_t>::max()) {
      std::fclose(f);
      *error = path + " is larger than 4 GiB";
      return std::nullopt;
    }
  }
  bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    *error = "cannot read " + path;
    return std::nullopt;
  }

  result.byteLength = static_cast<uint32_t>(total);
  result.raw.resize(PICOHASH_SHA256_DIGEST_LENGTH);
  ph_final(&ctx, result.raw.data());
  return result;
}

std::string nativeDigestToHex(std::string_view raw) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

} // namespace node_compat
} // namespace hermes
```

Check the exact picohash wrapper function names in
`external/picohash/picohash_wrapper.h` before writing this -- the crypto
binding calls `ph_init_sha256`; confirm the update/final spellings there and
use whatever it actually declares.

- [ ] **Step 5: Wire the build**

In `lib/bundle/CMakeLists.txt`, add `native_digest.cpp` to the
`hermesNodeBundle` source list, with a comment in the style of its
neighbours:

```cmake
# native_digest.cpp is picohash and stdio, and nothing more, so it does not
# cost this target its VM-free property. It lives here rather than in the
# build or tools layer because both need it: the producer records a digest
# and --verify-natives checks one, and the two must compute it identically.
```

and add `picohash` to that target's `target_link_libraries(... PRIVATE ...)`
list.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target BundleFormatTest
cmake-build-asan/unittests/BundleFormatTest --gtest_filter='*Digest*'
```

Expected: PASS, three tests.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle/native_digest.h \
        lib/bundle/native_digest.cpp lib/bundle/CMakeLists.txt \
        unittests/BundleFormatTest.cpp unittests/TempTree.h
git commit -m "bundle: add nativeFileDigest, one SHA-256 for producer and tools"
```

---

## Task 3: An explicit absolute-specifier branch in `resolveSpecifier`

**Files:**
- Modify: `lib/bundle/bundle_resolve.cpp:328-350`
- Test: `unittests/BundleResolveTest.cpp`, `unittests/BundleFileSourceTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: no signature change. `resolveSpecifier(src, fromFile, "/abs/x.js")`
  resolves against `src` directly instead of taking the `node_modules` walk.

**Why this task exists:** an absolute request already resolves correctly, but
only because `joinNormalized(dir / "node_modules", specifier)` calls
`fs::path::operator/`, which discards the left operand when the right one is
absolute. Nothing states that and nothing tests it, and the loader path for
a bundled native (`require(path.join(__dirname, 'x.node'))`) depends on it.

- [ ] **Step 1: Write the failing test**

Append to `unittests/BundleResolveTest.cpp`, following the file's existing
fixture style:

```cpp
TEST(BundleResolveTest, AbsoluteSpecifierResolvesDirectly) {
  TempTree tree;
  tree.write("app/cli.js", "");
  tree.write("app/deep/thing.js", "");
  DiskFileSource disk;

  std::string from = tree.path("app/cli.js");
  std::string target = tree.path("app/deep/thing.js");
  auto resolved = resolveSpecifier(disk, from, target);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, target);
}

TEST(BundleResolveTest, AbsoluteSpecifierDoesNotWalkNodeModules) {
  // A file that exists ONLY under node_modules must not be found by an
  // absolute request naming a path where it does not exist. The walk would
  // have produced <dir>/node_modules + the absolute path, which
  // fs::path::operator/ collapses to the absolute path -- the right answer
  // by accident. Pin the intent, so a future joinNormalized cannot silently
  // change it.
  TempTree tree;
  tree.write("app/cli.js", "");
  tree.write("app/node_modules/x/index.js", "");
  DiskFileSource disk;

  auto resolved =
      resolveSpecifier(disk, tree.path("app/cli.js"), tree.path("app/x"));
  EXPECT_FALSE(resolved.has_value());
}

TEST(BundleResolveTest, AbsoluteSpecifierAgreesAcrossBackends) {
  TempTree tree;
  tree.write("app/cli.js", "");
  tree.write("app/deep/thing.js", "");
  DiskFileSource disk;
  std::string target = tree.path("app/deep/thing.js");
  auto onDisk = resolveSpecifier(disk, tree.path("app/cli.js"), target);
  ASSERT_TRUE(onDisk.has_value());

  // Same question against a container holding the same two identities,
  // rooted at the same directory. Build it the way the other agreement
  // cases in this file do.
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.addModule(
      "deep/thing.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(7);
  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  BundleFileSource container(*reader, tree.path("app"));

  auto inBundle =
      resolveSpecifier(container, tree.path("app/cli.js"), target);
  ASSERT_TRUE(inBundle.has_value());
  EXPECT_EQ(*inBundle, *onDisk);
}
```

Adapt `TempTree`'s API to whatever the file already uses (`tree.path(...)`,
`tree.write(...)`); do not introduce a second helper.

- [ ] **Step 2: Run the tests to verify their status**

```bash
cmake --build cmake-build-asan --target BundleResolveTest
cmake-build-asan/unittests/BundleResolveTest --gtest_filter='*Absolute*'
```

Expected: the first and third PASS already (that is the accident this task
documents), the second may pass or fail depending on the tree. That is fine
-- this task is a refactor with characterization tests, so record which
passed before the change and require the same set after.

- [ ] **Step 3: Make the branch explicit**

In `lib/bundle/bundle_resolve.cpp`, inside the three-argument
`resolveSpecifier`, immediately after the `isRelative` block:

```cpp
  // An absolute specifier names its target outright, the way Node does it:
  // Module._findPath overrides the lookup paths to [''] for an absolute
  // request (`const absoluteRequest = path.isAbsolute(request); if
  // (absoluteRequest) { paths = ['']; }`, loader.js:699-701) and probes the
  // path itself. Module._resolveLookupPaths has no absolute case at all --
  // it hands back the ordinary node_modules list, which _findPath then
  // throws away. That is the same shape as the accident described below.
  // This is reached constantly in a
  // bundle -- require(path.join(__dirname, 'build', 'Release', 'x.node'))
  // is how bindings, node-gyp-build and hermes-parser's own addon loader
  // all ask -- so it must not be incidental.
  //
  // Before this branch existed the bare walk below produced the same
  // answer, but only because joinNormalized() ends in
  // fs::path::operator/, which DISCARDS its left operand when the right
  // one is absolute. That is correct behaviour resting on a detail of a
  // helper that has nothing to do with resolution; stating it here is what
  // keeps it true.
  if (!specifier.empty() && specifier[0] == '/') {
    return resolveBase(src, joinNormalized(fs::path("/"), specifier), 0);
  }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target BundleResolveTest BundleFileSourceTest
cmake-build-asan/unittests/BundleResolveTest
cmake-build-asan/unittests/BundleFileSourceTest
```

Expected: PASS, with every previously-passing case still passing.

- [ ] **Step 5: Commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add lib/bundle/bundle_resolve.cpp unittests/BundleResolveTest.cpp
git commit -m "bundle: resolve an absolute specifier on purpose, not by accident"
```

---

## Task 4: The producer packages natives and copies sidecars

**Files:**
- Modify: `lib/bundle/bundle_build.cpp`
- Test: `test/bundle-natives.js` (created here, extended in Task 6)

**Interfaces:**
- Consumes: `ModuleKind::kNative`, `BundleWriter::addNative` (Task 1);
  `nativeFileDigest`, `nativeDigestToHex` (Task 2).
- Produces: a container whose `.node` targets are `kNative` modules, and
  sidecar files in the output directory.

- [ ] **Step 1: Write the failing test**

Create `test/bundle-natives.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A bundled program that requires a native addon: the addon is packaged as
// a kNative record and its bytes ship as a flat sidecar next to the
// container.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.dir/main.js
// RUN: %hermes-node --build-bundle=%t.dir/app.hbb %t.dir/main.js 2>&1 | %FileCheck --check-prefix=BUILD %s
// RUN: ls %t.dir/hello_addon.node
// RUN: %hermes-node --bundle=%t.dir/app.hbb | %FileCheck %s

// BUILD: bundle root: {{.*}}
// BUILD: native: hello_addon.node (from hello_addon.node)
// BUILD: note: this bundle requires 1 native addon alongside it; ship them together.

// CHECK: PASS
```

Create `test/fixtures/bundle-natives/main.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

const addon = require('./hello_addon.node');
if (addon.hello() !== 'world') throw new Error('hello: ' + addon.hello());
if (addon.add(2, 3) !== 5) throw new Error('add: ' + addon.add(2, 3));
console.log('PASS');
```

Write ONLY the build half now -- the lines above, minus the final
`RUN: %hermes-node --bundle=...` and its `// CHECK: PASS`. Task 6 adds the
run half. Every `RUN:` line in the file must pass at the end of the task
that adds it; no `XFAIL`.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target hermes-node hello_addon
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/bundle-natives.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param hello_addon=$(pwd)/cmake-build-asan/lib/hello_addon.node \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: FAIL. The producer prints
`warning: skipping .../hello_addon.node (.node is not packageable)` and no
`native:` line.

Note that here the output directory and the source directory are the same,
so `ls %t.dir/hello_addon.node` cannot distinguish a copy from the file the
test already put there. That is fine for this task -- the `BUILD:` lines are
what it proves. Task 6 adds a case building into an empty directory, where
only a real copy can satisfy the run.

- [ ] **Step 3: Classify `.node` as packageable**

In `lib/bundle/bundle_build.cpp`, extend the `Packageability` enum with
`kNative`, and in `classifyFile`:

```cpp
  if (ext == ".node")
    return Packageability::kNative;
```

placed before the final `return Packageability::kSkip;`. Update
`classifyFile`'s doc comment: the paragraph that currently says
"`.node` addons ... skipped for the reason the spec gives" is now wrong and
must say that a `.node` is packaged as a `kNative` record whose bytes ship
alongside.

- [ ] **Step 4: Carry the addon through the walk**

Add to `FileInfo`:

```cpp
  /// For kNative only: the absolute path of the addon on the build
  /// machine, its recorded length and digest, and the flat sidecar name it
  /// will be copied to. Empty for every other kind.
  std::string sidecarName;
  NativeDigest digest;
```

In the worklist loop, before the `readFile` call (a `.node` must not be read
into memory as a module payload -- it can be tens of megabytes and nothing
needs its bytes):

```cpp
    if (classifyFile(path) == Packageability::kNative) {
      FileInfo info;
      info.kind = ModuleKind::kNative;
      std::string digestError;
      std::optional<NativeDigest> digest =
          nativeFileDigest(path, &digestError);
      if (!digest) {
        std::fprintf(stderr, "error: %s\n", digestError.c_str());
        return 1;
      }
      info.digest = std::move(*digest);
      files.emplace(path, std::move(info));
      continue;
    }
```

A `kNative` is a graph leaf: it is never scanned, never compiled, and
contributes no edges. Make sure the compile loop (step 4 of `buildBundle`)
skips it -- it already skips anything whose kind is not `kJavaScript`.

- [ ] **Step 5: Accept a `.node` from the edge filter, `--include` and `--preload`**

Three sites currently treat `Packageability::kSkip` as "leave it out". Each
must now distinguish `kNative`:

In the pass-A edge filter, replace
`if (classifyFile(*resolved) == Packageability::kSkip)` with a check that
only skips for `kSkip`; `kNative` falls through to `keep.emplace_back(...)`
like a JavaScript target.

In the `--include` loop and the `--preload` loop, the existing
`if (classifyFile(*resolved) == Packageability::kSkip)` hard error stays as
it is -- it already rejects only `kSkip`, so a `.node` now passes it. Verify
by reading, and add a one-line comment at the `--include` site:

```cpp
    // A .node is Packageability::kNative, not kSkip, so it passes here:
    // --include=<addon>.node is exactly how an addon reached by a computed
    // path (bindings, node-gyp-build) gets into the container.
```

The entry check (`classifyFile(absEntry) != Packageability::kJavaScript`)
needs no change: a `.node` entry stays an error, and its message already
says the entry must be a CommonJS JavaScript or TypeScript file.

- [ ] **Step 6: Assign sidecar names and copy the files**

After the root is computed and printed (step 3 of `buildBundle`), and before
the compile loop, add:

```cpp
  // Step 3b: assign each native a flat sidecar name and copy it next to
  // the bundle. Flat, not the identity's own subtree: the whole point of
  // shipping a bundle is a small countable set of files, and "bundle plus
  // tree" is not a better distribution unit than a tree (see the design
  // doc). One consequence worth knowing: every native lands in one
  // directory, so an addon whose RPATH is $ORIGIN-relative finds a sibling
  // .so the user drops beside the bundle.
  std::unordered_map<std::string, uint32_t> sidecarUse;
  std::vector<std::pair<std::string, std::string>> nativeCopies; // (src, dst)
  for (size_t i = 0; i < paths.size(); ++i) {
    FileInfo &info = files.at(paths[i]);
    if (info.kind != ModuleKind::kNative)
      continue;
    std::string base = fs::path(paths[i]).filename().string();
    std::string name = base;
    if (sidecarUse.count(name) != 0) {
      // Two identities with the same basename. Disambiguate with a short
      // hash of the identity, which is stable across builds and does not
      // depend on discovery order. The map from identity to sidecar name
      // is recorded in the container, so this rule can change later
      // without invalidating a bundle that already exists.
      std::string identity =
          fs::path(paths[i]).lexically_relative(rootPath).generic_string();
      char suffix[16];
      std::snprintf(
          suffix,
          sizeof(suffix),
          "-%08x",
          static_cast<unsigned>(
              crc32(0L, reinterpret_cast<const Bytef *>(identity.data()),
                    static_cast<uInt>(identity.size()))));
      fs::path p(base);
      name = p.stem().string() + suffix + p.extension().string();
    }
    sidecarUse[name] = 1;
    info.sidecarName = name;
    nativeCopies.emplace_back(paths[i], (fs::path(outPath).parent_path() / name).string());
  }
```

`fs::path(p).lexically_relative(rootPath).generic_string()` is exactly the
expression this file already uses to turn an absolute path into an identity
(`bundle_build.cpp:1046`); use it rather than inventing a helper, and note
that it needs `rootPath`, so this loop must run after the root is computed.
`crc32` needs `<zlib.h>`; the target already links `zlib_a` for
`bundle_generation.cpp`.

Then perform the copies. Use the same temp-file-then-rename guarantee the
container write uses (`atomic_write.h`) so a failure never leaves a
half-written `.so` next to a good bundle:

```cpp
  for (const auto &[src, dst] : nativeCopies) {
    // Refuse to write over the container itself: an identity whose
    // basename collides with the bundle's own name would otherwise have
    // the build destroy its own output. Same (st_dev, st_ino) test
    // --extract-module --out uses.
    if (isSameFile(dst, outPath)) {
      std::fprintf(
          stderr,
          "error: native addon %s would be written over the bundle %s\n",
          src.c_str(),
          outPath.c_str());
      return 1;
    }
    std::string contents;
    if (!readFile(src, &contents)) {
      std::fprintf(stderr, "error: cannot read %s\n", src.c_str());
      return 1;
    }
    // writeFileAtomically(outPath, ..., err) -- see
    // include/hermes/node-compat/bundle/atomic_write.h:38 for its exact
    // parameter list; it reports through an std::ostream, not a
    // std::string*, so pass std::cerr.
    if (!writeFileAtomically(dst, contents, std::cerr)) {
      std::fprintf(stderr, "error: cannot write %s\n", dst.c_str());
      return 1;
    }
    // The copy must also be executable-loadable; preserve the source's
    // mode bits so a 0644 copy of a 0755 addon does not surprise anyone
    // reading `ls -l` (dlopen itself needs only read permission).
    std::error_code modeEc;
    fs::permissions(dst, fs::status(src).permissions(), modeEc);
  }
```

`isSameFile` currently lives as a file-static in `bundle_tools.cpp:155`.
Move it into `atomic_write.h`/`.cpp` (declared there, defined once) so the
producer and `--extract-module` share one copy of the (st_dev, st_ino)
test rather than each carrying its own -- two copies of a refusal rule is
how the two come to disagree.

**Root participation, which falls out and must not be broken.** A native
goes into `paths` through the ordinary worklist, so `commonAncestor(paths)`
already accounts for it -- an `--include`d addon can widen the bundle root
and shift every other identity, which is what the design requires. Do not
add natives to `files` without adding them to `paths`.

- [ ] **Step 7: Record the natives in the container**

In the pass that adds modules to the `BundleWriter`, a `kNative` is added
with an empty payload and `kRequirable` set, exactly like any other module
-- the existing `addModule(identity, info.kind, info.flags, info.payload)`
call already does the right thing because `info.payload` is empty. After
the module indices exist, add:

```cpp
  for (size_t i = 0; i < paths.size(); ++i) {
    const FileInfo &info = files.at(paths[i]);
    if (info.kind != ModuleKind::kNative)
      continue;
    writer.addNative(
        static_cast<uint32_t>(i),
        info.sidecarName,
        info.digest.byteLength,
        info.digest.raw);
  }
```

placed next to the `addPreload` loop, since module indices are the same
discovery-order indices both use.

- [ ] **Step 8: Report**

Print, for each copied native, after the `bundle root:` line, to **stdout**
(it is part of the artifact description, like `bundle root:`):

```cpp
  for (size_t i = 0; i < paths.size(); ++i) {
    const FileInfo &info = files.at(paths[i]);
    if (info.kind != ModuleKind::kNative)
      continue;
    std::printf(
        "native: %s (from %s)\n",
        info.sidecarName.c_str(),
        stripRoot(paths[i], root).c_str());
  }
  if (!nativeCopies.empty()) {
    std::printf(
        "note: this bundle requires %zu native addon%s alongside it; ship "
        "them together.\n",
        nativeCopies.size(),
        nativeCopies.size() == 1 ? "" : "s");
  }
```

Extend `BuildSummary` with `uint32_t nativeModules = 0;` and
`uint64_t nativeBytes = 0;`, fill them from the same loop, and add a summary
line to `BuildReporter` in the style of its existing ones -- kept separate
from the container's own byte totals, because those bytes are not in the
file:

```
  natives: 1 file, 1.2 MB alongside (not in the container)
```

Add a `BuildReporter::nativeCopied(identity, sidecar, src, dst, digest)`
method printing, under `--verbose` only:

```
  native node_modules/a/build/Release/a.node -> a.node
    from /abs/src/node_modules/a/build/Release/a.node
    to   /abs/out/a.node
    1234567 bytes sha256:ba7816bf...
```

and, when a disambiguating suffix was applied, an explicit line saying so
rather than leaving a hash in a filename to be noticed:

```
    name collides with a.node from node_modules/b/a.node; using a-3f2a9c11.node
```

- [ ] **Step 9: Run the test to verify it passes**

Run the lit command from Step 2.

Expected: PASS.

- [ ] **Step 10: Run the whole suite**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
```

Expected: PASS. `test/bundle-build.js` and `test/bundle-verbose.js` may
carry a CHECK asserting the old "skipping .node" warning -- update those
expectations here, in this commit, since this is the change that made them
false.

- [ ] **Step 11: Commit**

```bash
git add lib/bundle/bundle_build.cpp test/bundle-natives.js \
        test/fixtures/bundle-natives/main.js test/bundle-build.js \
        test/bundle-verbose.js
git commit -m "bundle: package native addons and copy them beside the container"
```

---

## Task 5: The run layer exposes the natives

**Files:**
- Modify: `lib/bundle/bundle_run.cpp`
- Test: covered by Task 6's lit test; no GTest (this file needs a runtime).

**Interfaces:**
- Consumes: `BundleReader::nativeCount/native/nativeFor` (Task 1).
- Produces:
  - `__bundleNatives()` -> array of `{identity, sidecar}` objects, in module
    order. Also on the `bundle` object as `natives`.
  - `__bundleLoad(identity)` throws for a `kNative` identity.

- [ ] **Step 1: Write the failing test**

Add to `test/bundle-natives.js` (built in Task 4), after the build half:

```js
// __bundleLoad must refuse a native: its bytes are not in the container,
// and a loader that tried would be running a shared object as bytecode.
// RUN: cp %source_dir/test/fixtures/bundle-natives/load-refused.js %t.dir/refused.js
// RUN: %hermes-node --build-bundle=%t.dir/refused.hbb %t.dir/refused.js > /dev/null
// RUN: %hermes-node --bundle=%t.dir/refused.hbb | %FileCheck --check-prefix=REFUSED %s
// REFUSED: PASS refused
```

`test/fixtures/bundle-natives/load-refused.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Reach the native directly, bypassing the loader, and confirm it refuses.
const natives = globalThis.__bundleNatives();
if (natives.length !== 1) throw new Error('natives: ' + natives.length);
if (natives[0].sidecar !== 'hello_addon.node') {
  throw new Error('sidecar: ' + natives[0].sidecar);
}
let threw = false;
try {
  globalThis.__bundleLoad(natives[0].identity);
} catch (e) {
  threw = true;
}
if (!threw) throw new Error('__bundleLoad did not refuse a native');
console.log('PASS refused');
```

This fixture requires the same `hello_addon.node` sitting beside it; add the
`cp` to the RUN lines the same way the main case does.

- [ ] **Step 2: Run the test to verify it fails**

Run the lit command from Task 4 Step 2.

Expected: FAIL -- `__bundleNatives is not a function`.

- [ ] **Step 3: Refuse a native in `__bundleLoad`**

In `lib/bundle/bundle_run.cpp`'s `bundleLoadCallback`, next to the existing
`isRequirable` refusal:

```cpp
  // A native addon's bytes are not in the container -- they ship as a
  // sidecar file and are loaded by dlopen, never by this loader. Refusing
  // here is the same guard the isRequirable check above is: two load paths
  // that must not be able to blunder into each other.
  if (state.reader->kind(*index) == ModuleKind::kNative) {
    napi_throw_error(
        env,
        nullptr,
        "__bundleLoad: that module is a native addon; it is loaded with "
        "process.dlopen from its sidecar file");
    return nullptr;
  }
```

- [ ] **Step 4: Add `__bundleNatives`**

Alongside `bundlePreloadsCallback`, following its shape exactly:

```cpp
/// __bundleNatives() -> [{identity, sidecar}, ...], in module order.
///
/// Returned as one array rather than queried per module because the loader
/// builds a lookup from it once, at install time. A per-require native call
/// would sit on the hot path of an artifact whose reason for existing is
/// startup cost -- the same reasoning that moved the BUNDLE debug gate off
/// that path.
napi_value bundleNativesCallback(napi_env env, napi_callback_info info) {
  BundleState &state = bundleState();
  napi_value result;
  if (napi_create_array_with_length(env, state.reader->nativeCount(),
                                    &result) != napi_ok) {
    return nullptr;
  }
  for (uint32_t i = 0; i < state.reader->nativeCount(); ++i) {
    BundleReader::NativeView view = state.reader->native(i);
    std::string_view identity = state.reader->identity(view.moduleIndex);
    napi_value entry;
    if (napi_create_object(env, &entry) != napi_ok)
      return nullptr;
    napi_value identityVal;
    if (napi_create_string_utf8(env, identity.data(), identity.size(),
                                &identityVal) != napi_ok) {
      return nullptr;
    }
    napi_value sidecarVal;
    if (napi_create_string_utf8(env, view.sidecar.data(), view.sidecar.size(),
                                &sidecarVal) != napi_ok) {
      return nullptr;
    }
    if (napi_set_named_property(env, entry, "identity", identityVal) !=
            napi_ok ||
        napi_set_named_property(env, entry, "sidecar", sidecarVal) != napi_ok ||
        napi_set_element(env, result, i, entry) != napi_ok) {
      return nullptr;
    }
  }
  return result;
}
```

Register it next to the others in `installBundleGlobals`:

```cpp
  defineBundleNative(
      env, global, bundle, "__bundleNatives", "natives",
      bundleNativesCallback);
```

Use the exact helper name and argument order the neighbouring registrations
use. Update `bundle_run.h`'s doc comment, which currently says "the five
bundle natives" and lists them -- it is now six, and `natives` joins the
`{lookup, resolve, load, entry, root}` object.

- [ ] **Step 5: Run the test to verify it passes**

Expected: the `REFUSED` case passes. The main case still fails at the run
step -- Task 6.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add lib/bundle/bundle_run.cpp include/hermes/node-compat/bundle/bundle_run.h \
        test/bundle-natives.js test/fixtures/bundle-natives/load-refused.js
git commit -m "bundle: expose the native table to the loader, refuse it in __bundleLoad"
```

---

## Task 6: The loader dlopens a bundled native

**Files:**
- Modify: `libjs/bundle-loader.js`
- Modify: `lib/runtime/hermes_node_runtime.cpp:602-610`
- Test: `test/bundle-natives.js` (run half)

**Interfaces:**
- Consumes: `bundle.natives()` (Task 5), sidecar files beside the container
  (Task 4).
- Produces: `require('./x.node')` inside a bundle returns the addon's
  exports. `installBundleLoader(Module, bundle, path, fs)` -- a fourth
  parameter.

- [ ] **Step 1: Write the failing test**

Add the run half to `test/bundle-natives.js`:

```js
// RUN: %hermes-node --bundle=%t.dir/app.hbb | %FileCheck %s
// CHECK: PASS
```

Also add a case proving the sidecar is what actually loads -- build into a
*different* directory, so the copy is the only file that can satisfy it:

```js
// The sidecar, not the original, is what runs: build into a directory that
// holds no addon of its own and confirm the program still works.
// RUN: rm -rf %t.out && mkdir -p %t.out
// RUN: %hermes-node --build-bundle=%t.out/app.hbb %t.dir/main.js > /dev/null
// RUN: ls %t.out/hello_addon.node
// RUN: %hermes-node --bundle=%t.out/app.hbb | %FileCheck %s
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL with the current
`Cannot find module './hello_addon.node' ... Native addons are not supported
in a bundle yet.`

- [ ] **Step 3: Pass `fs` into the loader**

In `lib/runtime/hermes_node_runtime.cpp`, where `installBundleLoader` is
called (around line 602), obtain the `fs` builtin the same way `pathModule`
is obtained a few lines above, and extend the call:

```cpp
  // installBundleLoader(Module, bundle, path, fs) -> run()
  napi_value installArgs[4] = {moduleCtor, bundleObject, pathModule, fsModule};
  napi_value runFn;
  if (napi_call_function(env, global, installFn, 4, installArgs, &runFn) !=
      napi_ok) {
```

`fs` is needed for exactly one thing: deciding whether a missing sidecar is
a missing file (our own MODULE_NOT_FOUND, naming the file to ship) or a real
dlopen failure (ERR_DLOPEN_FAILED, naming the loader error). Passing it
explicitly rather than reaching through `globalThis.require` keeps the
loader's dependencies visible in its signature.

- [ ] **Step 4: Build the sidecar map once, at install time**

In `libjs/bundle-loader.js`, change the signature and add the map near the
top of `installBundleLoader`, beside where `root` is obtained:

```js
  return function installBundleLoader(Module, bundle, path, fs) {
```

```js
    // identity -> sidecar filename, for the container's native addons.
    // Built once, here, rather than asked per require(): a native call on
    // the hot path of every require would cost every module in the bundle
    // to describe the one or two that are addons -- the same reasoning
    // that took the HERMES_NODE_DEBUG_NATIVE read off that path.
    //
    // A null-prototype object so a module named 'constructor.node' (or
    // anything else that collides with Object.prototype) cannot be
    // mistaken for a native.
    var nativeSidecars = Object.create(null);
    var natives = bundle.natives();
    for (var ni = 0; ni < natives.length; ni++)
      nativeSidecars[natives[ni].identity] = natives[ni].sidecar;
```

- [ ] **Step 5: Load a native in `loadIdentity`**

Inside `loadIdentity`, in the `try` block, replace the two-way dispatch on
`bundle.load(target)` with a three-way one. The native branch comes first
because it must not call `bundle.load` at all:

```js
      var threw = true;
      try {
        var sidecar = nativeSidecars[target];
        if (sidecar !== undefined) {
          // The addon's bytes are not in the container -- dlopen takes a
          // path, and there is no portable way to load a shared object
          // from memory -- so they ship as a flat file beside the bundle.
          // mod.filename stays the identity path, like every other bundled
          // module (whose file is not on disk either); the real path is
          // what dlopen is given and what its errors name.
          var addonPath = path.join(root, sidecar);
          if (!fs.existsSync(addonPath)) throw missingSidecar(target, sidecar);
          // Its own outcome name, so HERMES_NODE_DEBUG_NATIVE=BUNDLE
          // distinguishes "resolved to an addon and dlopen'd it" from an
          // ordinary container hit. This is a LOAD, where the other
          // logOutcome calls are resolutions, which is exactly why it is
          // worth telling apart.
          logOutcome('native', sidecar, target, target);
          process.dlopen(mod, addonPath);
        } else {
          var payload = bundle.load(target);
          if (typeof payload === 'string') {
            mod.exports = JSON.parse(payload);
          } else {
            payload(mod.exports, makeRequire(mod, target), mod, filename,
              dirname);
          }
        }
        threw = false;
      } finally {
```

and add, next to `notInBundle`:

```js
    // The addon is recorded in the container but its file is not beside
    // the bundle. MODULE_NOT_FOUND rather than ERR_DLOPEN_FAILED: the
    // practical meaning is "this addon is unavailable", and the code that
    // exists in the world to handle that -- an optional-dependency probe,
    // a napi-rs try/catch chain -- branches on MODULE_NOT_FOUND. Precision
    // that breaks a fallback is worth less than the fallback.
    function missingSidecar(identity, sidecar) {
      var err = new Error(
        "Cannot find module '" + identity + "'\n" +
        '  This bundle records a native addon, but its file is not beside ' +
        'the bundle.\n' +
        '  Expected: ' + path.join(root, sidecar) + '\n' +
        '  Native addons ship alongside the container; copy it there.');
      err.code = 'MODULE_NOT_FOUND';
      return err;
    }
```

- [ ] **Step 6: Run the test to verify it passes**

Expected: PASS, both the same-directory and the separate-output-directory
cases.

- [ ] **Step 7: Run the whole suite**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
```

Note: adding a shim or changing `libjs/*.js` sometimes needs a CMake
reconfigure before the embedded bytecode is rebuilt. If the loader change
appears not to take effect, reconfigure:

```bash
cmake -S . -B cmake-build-asan
```

- [ ] **Step 8: Commit**

```bash
git add libjs/bundle-loader.js lib/runtime/hermes_node_runtime.cpp \
        test/bundle-natives.js
git commit -m "bundle: dlopen a bundled native addon from its sidecar"
```

---

## Task 7: The error paths

**Files:**
- Modify: `libjs/bundle-loader.js`
- Create: `test/bundle-natives-errors.js`

**Interfaces:**
- Consumes: Task 6's loader.
- Produces: the `isAddonRequest` special case is deleted; an unrecorded
  `.node` gets the ordinary `--include` suggestion.

- [ ] **Step 1: Write the failing test**

Create `test/bundle-natives-errors.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// What a bundled program sees when a native addon is missing, and when one
// was never packaged at all.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.dir/main.js
// RUN: %hermes-node --build-bundle=%t.dir/app.hbb %t.dir/main.js > /dev/null

// The recorded addon, deleted after the build: the message names the file
// to ship and the code stays MODULE_NOT_FOUND so a probe still works.
// RUN: rm %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb 2>&1 | %FileCheck --check-prefix=MISSING %s
// MISSING: Cannot find module 'hello_addon.node'
// MISSING: its file is not beside the bundle
// MISSING: Expected: {{.*}}/hello_addon.node
// MISSING: copy it there

// An addon nothing packaged: the ordinary not-in-the-bundle error, with the
// --include suggestion that now actually works.
// RUN: rm -rf %t.two && mkdir -p %t.two
// RUN: cp %source_dir/test/fixtures/bundle-natives/computed.js %t.two/main.js
// RUN: cp %hello_addon %t.two/hello_addon.node
// RUN: %hermes-node --build-bundle=%t.two/app.hbb %t.two/main.js > /dev/null 2>&1
// RUN: %not %hermes-node --bundle=%t.two/app.hbb 2>&1 | %FileCheck --check-prefix=UNRECORDED %s
// UNRECORDED: Cannot find module './hello_addon.node'
// UNRECORDED: Not in the bundle. Add it with:
// UNRECORDED: --include=./hello_addon.node
// UNRECORDED-NOT: Native addons are not supported

// And the suggestion the previous case printed actually resolves.
// RUN: %hermes-node --build-bundle=%t.two/app.hbb --include=./hello_addon.node %t.two/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.two/app.hbb | %FileCheck --check-prefix=INCLUDED %s
// INCLUDED: PASS

// --preload of an addon needs no special case: the preload table holds
// module indices and running one means requiring it, which for a native
// means dlopen. Pinned here so nobody later "fixes" it with a refusal.
// RUN: rm -rf %t.pre && mkdir -p %t.pre
// RUN: cp %hello_addon %t.pre/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/quiet.js %t.pre/main.js
// RUN: %hermes-node --build-bundle=%t.pre/app.hbb --preload=./hello_addon.node %t.pre/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.pre/app.hbb --dump | %FileCheck --check-prefix=PRELOADED %s
// PRELOADED: PRELOADS
// PRELOADED: hello_addon.node
// RUN: %hermes-node --bundle=%t.pre/app.hbb | %FileCheck --check-prefix=INCLUDED %s
```

`test/fixtures/bundle-natives/quiet.js` -- an entry that does not itself
require the addon, so the preload table is the only thing that loads it:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

console.log('PASS');
```

`test/fixtures/bundle-natives/computed.js` -- a computed require, so static
discovery cannot see it:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

const name = './hello_addon' + '.node';
const addon = require(name);
if (addon.hello() !== 'world') throw new Error('hello: ' + addon.hello());
console.log('PASS');
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: the UNRECORDED case fails -- the current loader prints
"Native addons are not supported in a bundle yet."

- [ ] **Step 3: Delete the special case**

In `libjs/bundle-loader.js`, remove `isAddonRequest()` and the branch in
`notInBundle()` that uses it, so a `.node` miss takes the ordinary path with
`includeSuggestion()`. Update the comment block at the top of the file that
describes what happens to a `.node` -- it currently says the producer skips
them deliberately, which is no longer true.

- [ ] **Step 4: Run the test to verify it passes**

Expected: PASS, all four cases.

- [ ] **Step 5: Commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add libjs/bundle-loader.js test/bundle-natives-errors.js \
        test/fixtures/bundle-natives/computed.js
git commit -m "bundle: a missing addon is an ordinary miss with a usable --include"
```

---

## Task 8: `--verify-natives`

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_tools.h`
- Modify: `lib/bundle/bundle_tools.cpp`
- Modify: `lib/bundle/CMakeLists.txt` (picohash reaches
  `hermesNodeBundleTools` transitively through `hermesNodeBundle`; confirm,
  and link it explicitly if not)
- Modify: `tools/hermes-node/hermes-node.cpp`
- Test: `unittests/BundleToolsTest.cpp`, `test/bundle-verify-natives.js`,
  `test/bundle-tool-errors.js`, `test/bundle-tool-no-runtime.js`

**Interfaces:**
- Consumes: `BundleReader::native/nativeCount` (Task 1), `nativeFileDigest`
  (Task 2).
- Produces:
  `int verifyNatives(const std::string &bundlePath, bool verbose, std::ostream &out, std::ostream &err)`
  -- 0 when every native verifies, 1 otherwise (including a container that
  cannot be opened).

- [ ] **Step 1: Write the failing GTest**

Append to `unittests/BundleToolsTest.cpp`:

```cpp
TEST(BundleToolsTest, VerifyNativesReportsOkMissingAndMismatch) {
  TempTree tree;
  std::string addon = tree.write("ok.node", "addon-bytes");
  std::string error;
  auto digest = nativeFileDigest(addon, &error);
  ASSERT_TRUE(digest.has_value()) << error;

  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t okIdx =
      writer.addModule("ok.node", ModuleKind::kNative, kRequirable, "");
  uint32_t goneIdx =
      writer.addModule("gone.node", ModuleKind::kNative, kRequirable, "");
  uint32_t badIdx =
      writer.addModule("bad.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(okIdx, "ok.node", digest->byteLength, digest->raw);
  writer.addNative(goneIdx, "gone.node", 7, std::string(32, '\x01'));
  writer.addNative(badIdx, "bad.node", 11, std::string(32, '\x02'));
  std::vector<uint8_t> bytes = writer.serialize(bundleGenerationTag());

  std::string bundlePath = tree.path("app.hbb");
  tree.writeBytes("app.hbb", bytes);
  tree.write("bad.node", "different!!");  // same length, different bytes

  std::ostringstream out, err;
  int rc = verifyNatives(bundlePath, /*verbose=*/false, out, err);
  EXPECT_EQ(rc, 1);
  std::string text = out.str();
  EXPECT_NE(text.find("OK       ok.node"), std::string::npos) << text;
  EXPECT_NE(text.find("MISSING  gone.node"), std::string::npos) << text;
  EXPECT_NE(text.find("MISMATCH bad.node"), std::string::npos) << text;
  EXPECT_NE(err.str().find("2 of 3"), std::string::npos) << err.str();
}

TEST(BundleToolsTest, VerifyNativesSucceedsWithNoNatives) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(entry);
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream out, err;
  EXPECT_EQ(verifyNatives(tree.path("app.hbb"), false, out, err), 0);
}
```

Add `TempTree::writeBytes` if it does not exist.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BundleToolsTest
```

Expected: compile failure -- `verifyNatives` does not exist.

- [ ] **Step 3: Implement `verifyNatives`**

Declare in `bundle_tools.h`, in the style of `dumpBundle`:

```cpp
/// Checks each native addon the container at \p bundlePath records against
/// the file of that name in the container's own directory, printing one
/// line per addon to \p out and a summary to \p err.
///
/// Opened in inspection mode, like dumpBundle(): a container this binary
/// would refuse to run is still one whose sidecars are worth checking.
///
/// This is an AUDIT, NOT AN ENFORCEMENT. It reports what the files are at
/// the moment it runs; the program dlopens them later, and nothing here
/// closes the gap between the two. A cryptographic hash is used anyway --
/// SHA-256, not the CRC32 the generation tag uses -- because a CRC can be
/// forged to any value, and a check offered as a security step should not
/// have that as its weakest part.
///
/// Returns 0 when every recorded native is present and matches, and 1
/// otherwise -- so it can gate a deployment -- or when the container itself
/// cannot be read.
int verifyNatives(
    const std::string &bundlePath,
    bool verbose,
    std::ostream &out,
    std::ostream &err);
```

Implement in `bundle_tools.cpp`, reusing the mapping + `openForInspection`
preamble the other two verbs share (factor it into a helper if the file does
not already have one -- do not paste a third copy):

- Sidecar directory is `fs::path(bundlePath).parent_path()`, the same rule
  the consumer uses.
- For each `native(i)`: if `nativeFileDigest` fails because the file is
  absent, print `MISSING`; if length or digest differ, print `MISMATCH`;
  else `OK`.
- Column format exactly as the design doc shows:
  `%-8s %-24s (%s)` with status, sidecar, identity.
- `--verbose` adds two indented lines per non-OK entry: `expected` and
  `actual` length and hex digest (`nativeDigestToHex`). Print them for `OK`
  entries too under `--verbose` -- a verification whose passing case shows
  nothing is hard to trust.
- Summary on `err`:
  `error: N of M native addons failed verification` when N > 0; nothing on
  success beyond the per-file lines.

- [ ] **Step 4: Wire the CLI**

In `tools/hermes-node/hermes-node.cpp`:

`ToolOptions` gains, with a comment matching its neighbours:

```cpp
  /// --verify-natives: check the sidecar files of the container named by
  /// --bundle against the lengths and hashes it recorded at build time. A
  /// bool, not an optional<string>, because it takes no value.
  bool verifyNatives = false;
```

Parsing, next to `--dump`:

```cpp
    } else if (std::strcmp(argv[i], "--verify-natives") == 0) {
      tools.verifyNatives = true;
```

`runToolVerb` gains a branch:

```cpp
  if (tools.verifyNatives) {
    exitCode = hermes::node_compat::verifyNatives(
        config.bundlePath, config.verbose, std::cout, std::cerr);
    return true;
  }
```

`checkToolOptions` gains, each message naming both flags:

```cpp
  if (tools.verifyNatives && tools.dump) {
    std::fprintf(
        stderr, "Error: --verify-natives cannot be combined with --dump.\n");
    return false;
  }
  if (tools.verifyNatives && tools.extractModule.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verify-natives cannot be combined with --extract-module.\n");
    return false;
  }
  if (tools.verifyNatives && tools.dumpBytecode.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verify-natives cannot be combined with --dump-bytecode.\n");
    return false;
  }
```

placed with the other two-verbs-at-once rows; extend the `verb` selection
chain with `else if (tools.verifyNatives) verb = "--verify-natives";`; add
the container requirement:

```cpp
  if (tools.verifyNatives && config.bundlePath.empty()) {
    std::fprintf(stderr, "Error: --verify-natives requires --bundle=<file>.\n");
    return false;
  }
```

and widen the `--verbose` rule from three consumers to four, updating both
the condition and the message:

```cpp
  if (config.verbose && config.buildBundlePath.empty() && !tools.dump &&
      !tools.verifyNatives && !tools.dumpBytecode.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verbose requires --build-bundle, --dump, --verify-natives "
        "or --dump-bytecode.\n");
    return false;
  }
```

The `--inspect` refusal already keys off `verb`, so it covers the new one
once the chain above includes it -- read it and confirm rather than
assuming.

Add a help line beside `--extract-module`'s:

```
  --verify-natives               With --bundle, check the native addons
                                 shipped beside it (audit, not enforcement)
```

- [ ] **Step 5: Write the lit test**

Create `test/bundle-verify-natives.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.dir/main.js
// RUN: %hermes-node --build-bundle=%t.dir/app.hbb %t.dir/main.js > /dev/null

// RUN: %hermes-node --bundle=%t.dir/app.hbb --verify-natives | %FileCheck --check-prefix=OK %s
// OK: OK {{ *}}hello_addon.node

// A changed addon is reported and the exit code is non-zero.
// RUN: printf 'x' >> %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=BAD %s
// BAD: MISMATCH {{ *}}hello_addon.node
// BAD: error: 1 of 1 native addon

// A deleted addon likewise.
// RUN: rm %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=GONE %s
// GONE: MISSING {{ *}}hello_addon.node
```

Add to `test/bundle-tool-errors.js` the four new matrix rows (two verbs,
missing `--bundle`, `--verbose` message text) following that file's existing
pattern exactly.

Add to `test/bundle-tool-no-runtime.js` a case proving `--verify-natives`
creates no compile-cache directory, mirroring the `--dump` case already
there.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/bundle/bundle_tools.h \
        lib/bundle/bundle_tools.cpp lib/bundle/CMakeLists.txt \
        tools/hermes-node/hermes-node.cpp unittests/BundleToolsTest.cpp \
        test/bundle-verify-natives.js test/bundle-tool-errors.js \
        test/bundle-tool-no-runtime.js
git commit -m "bundle: add --verify-natives, an audit of the sidecars"
```

---

## Task 9: `--dump` describes the natives

**Files:**
- Modify: `lib/bundle/bundle_tools.cpp`
- Test: `unittests/BundleToolsTest.cpp`, `test/bundle-dump.js`,
  `test/bundle-natives-errors.js`

**Interfaces:**
- Consumes: Task 1's reader accessors, Task 2's hex helper.
- Produces: a `NATIVES` section in the dump; `--extract-module` on a native
  is an error.

- [ ] **Step 1: Write the failing test**

Append to `unittests/BundleToolsTest.cpp`:

```cpp
TEST(BundleToolsTest, DumpPrintsNativesSection) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t addon = writer.addModule(
      "node_modules/a/build/Release/a.node", ModuleKind::kNative,
      kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 4096, std::string(32, '\xab'));
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream out, err;
  ASSERT_EQ(
      dumpBundle(tree.path("app.hbb"), bundleGenerationTag(), false, out, err),
      0)
      << err.str();
  std::string text = out.str();
  EXPECT_NE(text.find("NATIVES"), std::string::npos) << text;
  EXPECT_NE(text.find("a.node"), std::string::npos) << text;
  EXPECT_NE(text.find("4096"), std::string::npos) << text;
  EXPECT_NE(text.find("abababab"), std::string::npos) << text;
}

TEST(BundleToolsTest, ExtractModuleRefusesANative) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t addon =
      writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 4096, std::string(32, '\xab'));
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream err;
  EXPECT_EQ(
      extractModule(
          tree.path("app.hbb"), "a.node", tree.path("out.bin"), err),
      1);
  EXPECT_NE(err.str().find("native addon"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("alongside"), std::string::npos) << err.str();
  EXPECT_FALSE(std::filesystem::exists(tree.path("out.bin")));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Expected: FAIL -- no `NATIVES` in the dump, and `extractModule` writes a
zero-byte file instead of refusing.

- [ ] **Step 3: Add the `NATIVES` section**

In `dumpBundle`, after the `PRELOADS` section (and printed only when
`nativeCount() != 0`, matching how `PRELOADS` handles an empty table --
check and match it):

```
NATIVES
  [2] node_modules/a/build/Release/a.node
      sidecar a.node  4096 bytes  sha256:abababababababab
```

with `--verbose` printing the full 64-character digest instead of the
16-character prefix, alongside the in/out edge counts modules already get.
Add `nativeTableSize()` to the section-size listing at the end of the dump,
beside the edge and preload table sizes.

- [ ] **Step 4: Refuse `--extract-module` on a native**

In `extractModule`, after the identity is found and before anything is
written:

```cpp
  if (reader->kind(*index) == ModuleKind::kNative) {
    std::optional<BundleReader::NativeView> view = reader->nativeFor(*index);
    err << "error: '" << identity
        << "' is a native addon; its bytes are not in the container.\n"
        << "It ships alongside the bundle as "
        << (view ? view->sidecar : std::string_view("<unknown>")) << ".\n";
    return 1;
  }
```

- [ ] **Step 5: Extend the lit tests**

Add to `test/bundle-dump.js` a case building a bundle with the addon and
checking the `NATIVES` section (reuse the `%hello_addon` substitution).

Add to `test/bundle-natives-errors.js`:

```js
// --extract-module on a native says where the bytes actually are.
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --extract-module=hello_addon.node --out=%t.dir/x.bin 2>&1 | %FileCheck --check-prefix=EXTRACT %s
// EXTRACT: is a native addon
// EXTRACT: ships alongside the bundle as hello_addon.node
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
```

- [ ] **Step 7: Commit**

```bash
git add lib/bundle/bundle_tools.cpp unittests/BundleToolsTest.cpp \
        test/bundle-dump.js test/bundle-natives-errors.js
git commit -m "bundle: --dump describes natives, --extract-module refuses one"
```

---

## Task 10: The `hermes-parser-ast` example

**Files:**
- Create: `examples/hermes-parser-ast/{package.json,ast.js,run.sh,README.md}`
- Modify: `examples/run-examples.sh`

**Interfaces:**
- Consumes: everything above.
- Produces: the end-to-end proof, and the case that exercises a computed
  absolute require of a `.node`.

This example is gated on a network `npm install`, so it belongs to
`check-hermes-node-examples`, not `check-hermes-node`. Run it against a
Release build.

- [ ] **Step 1: Write the example's entry point**

`examples/hermes-parser-ast/ast.js`, modelled on
`examples/babel-parser/ast.js` (read that file first and match its shape,
its argument handling and its output format):

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Parses the file named on the command line with the Hermes parser -- a
// native addon -- and writes its ESTree AST to stdout.
//
// The pairing with examples/babel-parser/ast.js is the point: the same job
// with a pure-JavaScript parser bundles to one file, and this one bundles
// to a file plus a shared object.

'use strict';

const fs = require('fs');
const { parse } = require('hermes-parser');

const file = process.argv[2];
if (!file) {
  console.error('usage: ast <file.js>');
  process.exit(1);
}

const source = fs.readFileSync(file, 'utf8');
const ast = parse(source, { babel: false });
console.log(JSON.stringify(ast, null, 2));
```

- [ ] **Step 2: Write `package.json`**

```json
{
  "name": "hermes-parser-ast-example",
  "version": "1.0.0",
  "private": true,
  "description": "Dumps a JavaScript AST using the Hermes native parser addon",
  "type": "commonjs",
  "dependencies": {
    "hermes-estree": "0.37.0",
    "hermes-parser": "file:../../external/hermes-parser-native/package"
  }
}
```

Do NOT commit `node_modules` or add it to git.

- [ ] **Step 3: Write `run.sh`**

Model it on `examples/flow-bundler/run.sh` and
`examples/babel-parser/run.sh`. It must:

1. Locate the build directory the same way the sibling examples do.
2. Inside `# --- BEGIN vendored native parser addon ---` /
   `# --- END vendored native parser addon ---` markers (the deletion recipe
   in `external/hermes-parser-native/README.md` refers to these -- keep the
   wording identical to `flow-bundler/run.sh`'s), copy the freshly built
   addon into the package's `prebuilds/<platform>-<arch>/` directory:

```sh
ADDON="$BUILD_DIR/external/hermes-parser-native/hermes-parser.node"
if [ ! -f "$ADDON" ]; then
  echo "ERROR: missing $ADDON -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node hermes-parser-napi" 1>&2
  exit 1
fi
# Unlike flow-bundler, this example does NOT export
# HERMES_PARSER_NATIVE_ADDON: that override is an absolute path outside the
# bundle root, which a closed world correctly refuses. Copying the addon
# into the package's own prebuilds/ directory is what puts it under the
# root, where --include can name it and the container can record it. It
# also makes the run architecture-correct on a machine the committed
# linux-x64 prebuilt does not fit.
TARGET_DIR="$HERE/node_modules/hermes-parser/prebuilds/$(node_platform_arch)"
mkdir -p "$TARGET_DIR"
cp "$ADDON" "$TARGET_DIR/hermes-parser.node"
```

   deriving `<platform>-<arch>` from `hermes-node` itself
   (`hermes-node -e 'console.log(process.platform + "-" + process.arch)'`)
   rather than from the host `node`, so the copy matches the runtime that
   will load it.

3. Run unbundled: `"$HERMES_NODE" "$HERE/ast.js" "$HERE/sample.js"` and
   assert node types drawn from `sample.js` appear in the output.
4. Assert the no-argument case exits non-zero and prints the usage line --
   a usage error dressed as success passes any check that only reads stdout.
5. Build the bundle. The addon is reached by a computed require, so it must
   be named:

```sh
"$HERMES_NODE" --build-bundle="$OUT/ast.hbb" \
  --include=./node_modules/hermes-parser/prebuilds/$PLATFORM_ARCH/hermes-parser.node \
  "$HERE/ast.js"
```

6. Assert `ast.hbb` and `hermes-parser.node` both exist in `$OUT`, and that
   `$OUT` contains no `node_modules`.
7. Run bundled: `"$HERMES_NODE" --bundle="$OUT/ast.hbb" "$HERE/sample.js"`
   and assert the output is byte-identical to the unbundled run. Same AST or
   the example proves nothing:

```sh
diff <(...unbundled...) <(...bundled...) || { echo "ASTs differ"; exit 1; }
```

8. Run `--verify-natives` against the built bundle and require exit 0.

Add `examples/hermes-parser-ast/sample.js` -- a small file with a couple of
distinctive node types (a class with a private field, an optional chain) so
the AST assertion is specific to this input rather than "some JSON
appeared".

- [ ] **Step 4: Run it**

```bash
cd examples/hermes-parser-ast && npm install && cd -
cmake --build cmake-build-release --target hermes-node hermes-parser-napi
BUILD_DIR=cmake-build-release ./examples/hermes-parser-ast/run.sh
```

Expected: both runs print the same AST; the bundled directory holds two
files and no `node_modules`.

- [ ] **Step 5: Register it**

Add the example to `examples/run-examples.sh` following the existing
entries.

- [ ] **Step 6: Write the README**

`examples/hermes-parser-ast/README.md`, modelled on
`examples/babel-parser/README.md`: what it does, the two commands, the
quoted build output (re-measured, not invented -- run it and paste), the
size of the bundle and of the sidecar, and a short section naming the two
things this example demonstrates that its babel sibling cannot: a computed
absolute require of a `.node` needing `--include`, and an artifact that is
two files rather than one.

- [ ] **Step 7: Commit**

```bash
git add examples/hermes-parser-ast examples/run-examples.sh
git commit -m "examples: dump an AST with the native Hermes parser, bundled"
```

---

## Task 11: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `history/plans/progress-aot-bundle.md`

- [ ] **Step 1: Update `CLAUDE.md`**

In the AOT Bundles section:

- The bullet listing what is packaged currently says `.node` addons warn and
  are skipped. Replace with the native rule: packaged as a `kNative` record
  whose bytes ship as a flat sidecar beside the container, named by
  basename with a hash suffix on collision, recorded in the container.
- The closed-world bullet's addon sentence ("A `.node` addon gets its own
  text...") is now false -- a `.node` miss is an ordinary miss with an
  `--include` suggestion.
- Add the format v4 line to the format bullet.
- Add `--verify-natives` to the Bundle tooling subsection, with the
  audit-not-enforcement caveat and the SHA-256-not-CRC32 reason, and widen
  the `--verbose` consumer count from three to four in the
  `checkToolOptions` paragraph.
- Add the new tests to the test list.
- Add the stat-first limitation (`node-gyp-build`, `node-pre-gyp`) with the
  `bufferutil` instance, and the escape hatch.

- [ ] **Step 2: Update the progress file**

Append a dated section to `history/plans/progress-aot-bundle.md` covering:
which plan it tracks, what shipped, the two findings from the design round
(the `MODULE_NOT_FOUND` continuity and the accidental absolute-path
resolution now made explicit), the measured example numbers, and the
limitations recorded rather than fixed.

- [ ] **Step 3: Measure the bufferutil fallback, do not assert it**

The design says `examples/bufferutil-addon` silently falls back to its pure
JavaScript `./fallback` in a bundle, because `node-gyp-build` stats before
it requires. That is a prediction until it is run. Build and run it:

```bash
cd examples/bufferutil-addon && npm install && cd -
cmake-build-asan/bin/hermes-node --build-bundle=/tmp/bu.hbb \
  examples/bufferutil-addon/mask.js
cmake-build-asan/bin/hermes-node --bundle=/tmp/bu.hbb
```

Record what actually happens in the progress file -- whether it falls back,
throws, or something else -- and write the observed behavior into
`CLAUDE.md`'s limitation bullet. If it does NOT fall back, say so and fix
the design doc's claim rather than the observation.

- [ ] **Step 4: Verify the claims**

Every file path, line number and flag name written in Step 1 must be
checked against the code as it now stands. Do not restate a measurement
from this plan -- re-run the example and paste what it prints.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md history/plans/progress-aot-bundle.md
git commit -m "docs: record native addons in a bundle"
```

---

## Self-review notes for the executor

Three things this plan deliberately does NOT do, so an executor does not
add them:

1. **No `fs` virtualization.** `existsSync` must not learn to answer for
   recorded native identities. It is additive later and deliberately
   deferred; adding it now makes `existsSync` and `readFileSync` disagree
   about the same path.
2. **No run-time hash verification.** The digest is recorded and checked
   only by `--verify-natives`. Hashing on the load path would read the whole
   addon on every launch.
3. **No refusal of `--preload=<x>.node`.** It works through the existing
   preload machinery; writing a special case to forbid something harmless
   costs code and buys nothing.
