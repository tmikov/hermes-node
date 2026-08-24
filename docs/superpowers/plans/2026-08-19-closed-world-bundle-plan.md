# Closed-world Bundles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** A bundle stops reading the filesystem: every module and every
resolution is answered by the container, and a `require()` it cannot answer
is an error naming `--include`.

**Architecture:** `resolveSpecifier` keeps its algorithm and loses its
direct filesystem calls behind a `FileSource` interface, implemented twice
-- over the real disk for the producer, over the container's identity set
for the consumer -- so build-time and run-time resolution cannot drift. The
container gains the `package.json` files the resolver consulted, marked
non-requirable by a new record flag, and a `--include` flag adds roots that
static discovery cannot find. The disk fallback in `libjs/bundle-loader.js`
is deleted last, once everything needed to replace it exists.

**Tech Stack:** C++17, CMake + Ninja, GTest (`unittests/`), LLVM lit
(`test/`), Hermes Node-API.

Design: `docs/superpowers/specs/2026-08-19-closed-world-bundle-design.md`.
Progress: `docs/superpowers/plans/progress-aot-bundle.md` (same subsystem; append,
do not start a new file).

## Global Constraints

- New files carry `Copyright (c) Tzvetan Mikov.` -- NOT Meta Platforms.
- Commit messages are ASCII only, no emojis.
- Before every commit: `./utils/format.sh -f` then
  `cmake --build cmake-build-asan --target check-hermes-node`. Both must be
  clean.
- Never modify anything under `libjs-node/`.
- Do not edit a test to make a refactor pass. Tasks 1-6 are additive: no
  existing test may be weakened, deleted, or have an assertion removed to
  get it green. The one permitted edit is an expected *value* that a task
  deliberately changed -- Task 3 adds package.json records, so a container's
  module count legitimately moves -- and then the new value must be the
  correct one, computed from what the task now produces, never whatever
  makes the test pass. If you cannot explain why a number changed, it is a
  defect, not a new expectation. Task 7 changes behaviour deliberately and
  rewrites the tests that assert the old behaviour; it is the only task
  permitted to change what a test asserts rather than what it expects.
- `hermesNodeBundle` (`lib/bundle/bundle_format.cpp`, reader, writer,
  `bundle_resolve.cpp`) must not link the Hermes VM. `BundleFormatTest`,
  `BundleResolveTest` and `BundleToolsTest` run with no runtime and must
  keep doing so.
- The bundle format version becomes `2`. A version mismatch stays fatal in
  both `BundleReader::open` and `openForInspection`.
- The not-found error thrown at run time must set `code = 'MODULE_NOT_FOUND'`.
- Build directory for development and tests: `cmake-build-asan`. Use
  `cmake-build-release` only where a task says so.

---

### Task 1: Extract the FileSource seam

Pure refactor. `resolveSpecifier` gains a backend parameter; behaviour is
identical and every existing test passes untouched, which is the proof.

**Files:**
- Create: `include/hermes/node-compat/bundle/file_source.h`
- Create: `lib/bundle/disk_file_source.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_resolve.h`
- Modify: `lib/bundle/bundle_resolve.cpp`
- Modify: `lib/bundle/CMakeLists.txt` (add `disk_file_source.cpp` to
  `hermesNodeBundle`)
- Test: `unittests/BundleResolveTest.cpp` is NOT edited -- it calls the
  two-argument `resolveSpecifier`, which is kept.

**Interfaces:**
- Produces: `class FileSource` with
  `bool isRegularFile(const std::string &) const`,
  `bool isDirectory(const std::string &) const`,
  `std::optional<std::string> readPackageJson(const std::string &dir)`;
  `class DiskFileSource : public FileSource`;
  `std::optional<std::string> resolveSpecifier(FileSource &, std::string_view fromFile, std::string_view specifier)`
  plus the existing two-argument overload, which constructs a
  `DiskFileSource` and forwards.

- [ ] **Step 1: Write the failing test**

Add to `unittests/BundleResolveTest.cpp` (a new test; do not change
existing ones):

```cpp
// A backend that answers from an in-memory set rather than the disk is the
// whole point of the seam: if resolveSpecifier still reaches the real
// filesystem anywhere, this resolves against files that do not exist there
// and fails.
namespace {
class FakeFileSource : public FileSource {
 public:
  std::set<std::string> files;
  std::map<std::string, std::string> packageJson;

  bool isRegularFile(const std::string &path) const override {
    return files.count(path) != 0;
  }
  bool isDirectory(const std::string &path) const override {
    std::string prefix = path + "/";
    for (const std::string &f : files)
      if (f.compare(0, prefix.size(), prefix) == 0)
        return true;
    return false;
  }
  std::optional<std::string> readPackageJson(const std::string &dir) override {
    auto it = packageJson.find(dir);
    if (it == packageJson.end())
      return std::nullopt;
    return it->second;
  }
};
} // namespace

TEST(BundleResolveTest, ResolvesThroughAnInjectedFileSource) {
  FakeFileSource fs;
  fs.files.insert("/app/cli.js");
  fs.files.insert("/app/node_modules/dep/main.js");
  fs.packageJson["/app/node_modules/dep"] = "{\"main\": \"main.js\"}";

  EXPECT_EQ(
      resolveSpecifier(fs, "/app/cli.js", "dep"),
      std::optional<std::string>("/app/node_modules/dep/main.js"));
  EXPECT_FALSE(resolveSpecifier(fs, "/app/cli.js", "ghost").has_value());
}
```

- [ ] **Step 2: Run it and watch it fail to compile**

```bash
cmake --build cmake-build-asan --target BundleResolveTest
```
Expected: FAIL -- `FileSource` is not declared.

- [ ] **Step 3: Add the interface**

`include/hermes/node-compat/bundle/file_source.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_FILE_SOURCE_H
#define HERMES_NODE_COMPAT_BUNDLE_FILE_SOURCE_H

#include <optional>
#include <string>

namespace hermes {
namespace node_compat {

/// Everything module resolution needs to know about a file tree.
///
/// resolveSpecifier() implements the algorithm; this supplies the facts it
/// asks for. Two implementations exist: DiskFileSource, over the real
/// filesystem, used by the producer, and BundleFileSource, over a
/// container's identity set, used at run time. Sharing the algorithm across
/// both is the point -- a specifier that resolved one way at build time and
/// another at run time would load the wrong module silently.
class FileSource {
 public:
  virtual ~FileSource() = default;

  virtual bool isRegularFile(const std::string &path) const = 0;
  virtual bool isDirectory(const std::string &path) const = 0;

  /// The text of <dir>/package.json, or nullopt when there is none.
  /// Returning the text rather than the parsed "main" keeps the JSON
  /// parsing in one place, next to the algorithm that needs it.
  virtual std::optional<std::string> readPackageJson(
      const std::string &dir) = 0;
};

/// FileSource over the real filesystem.
class DiskFileSource : public FileSource {
 public:
  bool isRegularFile(const std::string &path) const override;
  bool isDirectory(const std::string &path) const override;
  std::optional<std::string> readPackageJson(const std::string &dir) override;
};

} // namespace node_compat
} // namespace hermes

#endif
```

- [ ] **Step 4: Move the filesystem calls into DiskFileSource**

`lib/bundle/disk_file_source.cpp` holds the three bodies currently in
`bundle_resolve.cpp`'s anonymous namespace: `isRegularFile` and
`isDirectory` verbatim (the `::stat` versions), and `readPackageJson`,
which is the file-reading half of today's `readPackageMain`:

```cpp
std::optional<std::string> DiskFileSource::readPackageJson(
    const std::string &dir) {
  std::ifstream f(dir + "/package.json", std::ios::binary);
  if (!f)
    return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
```

- [ ] **Step 5: Thread the backend through the resolver**

In `lib/bundle/bundle_resolve.cpp`: delete the local `isRegularFile` and
`isDirectory`; change `readPackageMain(const std::string &dir)` to
`readPackageMain(FileSource &src, const std::string &dir)` whose first act
is `std::optional<std::string> content = src.readPackageJson(dir); if
(!content) return std::nullopt;` and which then runs today's parsing over
`*content` unchanged; add `FileSource &src` as the first parameter of
`resolveBase` and `resolveSpecifier`, replacing each bare call with
`src.isRegularFile(...)` / `src.isDirectory(...)`.

Keep the old entry point, so no caller and no existing test changes:

```cpp
std::optional<std::string> resolveSpecifier(
    std::string_view fromFile,
    std::string_view specifier) {
  DiskFileSource disk;
  return resolveSpecifier(disk, fromFile, specifier);
}
```

- [ ] **Step 6: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS, including every pre-existing `BundleResolveTest` case
unmodified.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/bundle/file_source.h lib/bundle/disk_file_source.cpp \
  include/hermes/node-compat/bundle/bundle_resolve.h lib/bundle/bundle_resolve.cpp \
  lib/bundle/CMakeLists.txt unittests/BundleResolveTest.cpp
git commit -m "bundle: put resolution's filesystem access behind FileSource"
```

---

### Task 2: Format v2 -- a flags field on the module record

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_format.h`
- Modify: `include/hermes/node-compat/bundle/bundle_writer.h`
- Modify: `lib/bundle/bundle_writer.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h`
- Modify: `lib/bundle/bundle_reader.cpp`
- Modify: `lib/bundle/bundle_tools.cpp` (dump prints the flag)
- Modify: `lib/bundle/bundle_build.cpp` (pass `kRequirable` everywhere)
- Test: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `constexpr uint32_t kRequirable = 1u << 0;` in
  `bundle_format.h`; `BundleWriter::addModule(identity, kind, flags, payload)`;
  `uint32_t BundleReader::flags(uint32_t) const` and
  `bool BundleReader::isRequirable(uint32_t) const`.

- [ ] **Step 1: Write the failing test**

In `unittests/BundleFormatTest.cpp`:

```cpp
TEST(BundleFormatTest, RoundTripsModuleFlags) {
  BundleWriter w;
  uint32_t a = w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b = w.addModule("node_modules/dep/package.json",
                           ModuleKind::kJSON, /*flags*/ 0, "{}");
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_TRUE(r->isRequirable(a));
  EXPECT_FALSE(r->isRequirable(b));
  EXPECT_EQ(r->formatVersion(), 2u);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build cmake-build-asan --target BundleFormatTest && \
  cmake-build-asan/unittests/BundleFormatTest --gtest_filter='*ModuleFlags*'
```
Expected: FAIL to compile -- `addModule` takes three arguments.

- [ ] **Step 3: Change the format**

In `bundle_format.h`: `kBundleFormatVersion = 2`; add `uint32_t flags;` to
`BundleModuleRecord` after `kind`; add
`constexpr uint32_t kRequirable = 1u << 0;` with a comment saying a clear
bit means the record exists only so the resolver can read it (a
`package.json` consulted for `main`), and that `require()` must not see it.

- [ ] **Step 4: Thread it through writer, reader and dump**

`BundleWriter::addModule` takes `uint32_t flags` between `kind` and
`payload` and stores it. `BundleReader` gains `flags()` and
`isRequirable()`; its record validation rejects unknown bits
(`flags & ~kRequirable`) with
`"hermes-node bundle: module has unknown flags"`, for the same reason it
rejects an unknown kind. `bundle_tools.cpp`'s module table prints a
`resolve-only` marker in the kind column when the bit is clear.

Every existing `addModule` call in `bundle_build.cpp` passes `kRequirable`.

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS. `test/bundle-dump.js` may need no change; if the dump's
column widths shift, that is a lit failure to fix in the dump code, not in
the test.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/bundle lib/bundle unittests/BundleFormatTest.cpp
git commit -m "bundle: add a module record flags field, format v2"
```

---

### Task 3: The producer packages the package.json files it consulted

**Files:**
- Modify: `include/hermes/node-compat/bundle/file_source.h` (recording
  backend)
- Modify: `lib/bundle/disk_file_source.cpp`
- Modify: `lib/bundle/bundle_build.cpp`
- Test: `test/bundle-resolution-inputs.js` (new lit test)

**Interfaces:**
- Consumes: `DiskFileSource`, `kRequirable`, `addModule(..., flags, ...)`.
- Produces: `const std::vector<std::string> &DiskFileSource::readPackageJsonPaths() const`
  -- absolute paths of every `package.json` successfully read, in first-read
  order, deduplicated.

- [ ] **Step 1: Write the failing test**

`test/bundle-resolution-inputs.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The consumer resolves with the same algorithm the producer used, so it
// needs the same inputs: every package.json the producer read to answer a
// `main` has to be in the container. Nothing require()s these, so without
// this they were simply absent -- a container of the yargs example held
// zero package.json records.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/node_modules/dep
// RUN: echo '{ "main": "lib/entry.js" }' > %t.tree/node_modules/dep/package.json
// RUN: mkdir -p %t.tree/node_modules/dep/lib
// RUN: echo "module.exports = { v: 5 };" > %t.tree/node_modules/dep/lib/entry.js
// RUN: echo "console.log('GOT', require('dep').v);" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
// It is packaged for the resolver, not for the program: the dump marks it
// resolve-only in the kind column, which is what keeps
// require('dep/package.json') failing exactly where Node fails. The marker
// precedes the identity on the row, so this is one CHECK line, not a
// CHECK-SAME after the identity.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=DUMP %s
// DUMP: resolve-only{{.*}}node_modules/dep/package.json

// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=EXEC %s
// EXEC: GOT 5
```

- [ ] **Step 2: Run it and watch it fail**

```bash
python3 cmake-build-asan/bin/hermes-lit -v $(pwd)/test/bundle-resolution-inputs.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test
```
Expected: FAIL -- no `package.json` in the dump.

- [ ] **Step 3: Record what the resolver read**

`DiskFileSource` gains `std::vector<std::string> readPaths_` and a
`readPackageJsonPaths()` accessor; `readPackageJson` appends `dir +
"/package.json"` on a successful read if not already present. The producer
must therefore keep **one** `DiskFileSource` for the whole build rather
than constructing one per call.

- [ ] **Step 4: Package them**

In `buildBundle`, hold a `DiskFileSource disk;` alongside the worklist and
call `resolveSpecifier(disk, path, specifier)`. After the walk (before the
root is computed, so these files count toward it), add each recorded path
that is not already a module:

```cpp
for (const std::string &pkgPath : disk.readPackageJsonPaths()) {
  if (pathIndex.count(pkgPath) != 0)
    continue; // the program requires it too; it is already kRequirable
  std::string text;
  if (!readFile(pkgPath, &text))
    continue; // read once already; a disappearance now is not fatal
  FileInfo info;
  info.kind = ModuleKind::kJSON;
  info.flags = 0; // resolve-only: require() must not see it
  info.payload = std::move(text);
  pathIndex.emplace(pkgPath, static_cast<uint32_t>(paths.size()));
  paths.push_back(pkgPath);
  files.emplace(pkgPath, std::move(info));
}
```

`FileInfo` gains `uint32_t flags = kRequirable;` and the `addModule` call
passes `info.flags`.

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS. Note that bundles get bigger; `test/bundle-verbose.js`
asserts module and edge counts, and a fixture with a `package.json` will
shift them. If a count moves, update the expected number -- the count is
the assertion, and its new value is correct.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add lib/bundle include/hermes/node-compat/bundle test/bundle-resolution-inputs.js
git commit -m "bundle: package the package.json files resolution consulted"
```

---

### Task 4: BundleFileSource, and the agreement test

**Files:**
- Create: `lib/bundle/bundle_file_source.cpp`
- Modify: `include/hermes/node-compat/bundle/file_source.h` (declare
  `BundleFileSource`)
- Modify: `lib/bundle/CMakeLists.txt`
- Test: `unittests/BundleFileSourceTest.cpp` (new, added to
  `unittests/CMakeLists.txt`)

**Interfaces:**
- Consumes: `FileSource`, `BundleReader`.
- Produces:
  `BundleFileSource(const BundleReader &reader, std::string root)` --
  answers about the virtual tree rooted at `root`, whose files are the
  container's identities.

- [ ] **Step 1: Write the failing test**

`unittests/BundleFileSourceTest.cpp`, two groups. First, the mechanics:

```cpp
TEST(BundleFileSourceTest, AnswersFilesAndDirectories) {
  BundleWriter w;
  w.setEntry(w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A"));
  w.addModule("node_modules/dep/main.js", ModuleKind::kJavaScript,
              kRequirable, "B");
  w.addModule("node_modules/dep/package.json", ModuleKind::kJSON, 0,
              "{\"main\": \"main.js\"}");
  auto bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(r.has_value()) << error;

  BundleFileSource src(*r, "/app");
  EXPECT_TRUE(src.isRegularFile("/app/cli.js"));
  EXPECT_FALSE(src.isRegularFile("/app/nope.js"));
  EXPECT_TRUE(src.isDirectory("/app/node_modules/dep"));
  // A prefix that is not a whole segment is not a directory: "dep" must not
  // make "depot" one.
  EXPECT_FALSE(src.isDirectory("/app/node_modules/de"));
  // Outside the root there is nothing at all.
  EXPECT_FALSE(src.isRegularFile("/etc/passwd"));
  EXPECT_FALSE(src.isDirectory("/etc"));
  EXPECT_EQ(src.readPackageJson("/app/node_modules/dep"),
            std::optional<std::string>("{\"main\": \"main.js\"}"));
}
```

Second, the property this design exists for -- both backends agree:

```cpp
// Every specifier resolved twice: once against a real tree on disk, once
// against a container built from that same tree. A resolver change that
// affects only one backend fails here, which is the failure the shared
// algorithm exists to prevent.
TEST(BundleFileSourceTest, AgreesWithTheDiskBackend) {
  TempTree tree; // see the fixture helper below
  tree.write("cli.js", "require('dep'); require('./lib/util');");
  tree.write("lib/util.js", "module.exports = 1;");
  tree.write("node_modules/dep/package.json", "{\"main\": \"lib/entry.js\"}");
  tree.write("node_modules/dep/lib/entry.js", "module.exports = 2;");
  tree.write("node_modules/noMain/index.js", "module.exports = 3;");

  BundleFileSource bundleSrc = tree.buildContainerFileSource();
  DiskFileSource diskSrc;

  const char *froms[] = {"cli.js", "lib/util.js", "node_modules/dep/lib/entry.js"};
  const char *specs[] = {"dep", "noMain", "./lib/util", "./util", "..",
                         ".", "missing", "dep/lib/entry.js"};
  for (const char *from : froms) {
    for (const char *spec : specs) {
      auto onDisk = resolveSpecifier(diskSrc, tree.abs(from), spec);
      auto inBundle = resolveSpecifier(bundleSrc, tree.abs(from), spec);
      EXPECT_EQ(onDisk, inBundle) << "from " << from << " require " << spec;
    }
  }
}
```

`TempTree` is a fixture helper in the same file: it creates a temp
directory, writes files, and `buildContainerFileSource()` adds every
written file to a `BundleWriter` under its tree-relative identity (JS as
`kJavaScript`/`kRequirable`, `package.json` as `kJSON`/`0`), serializes,
opens a reader held by the fixture, and returns a `BundleFileSource` rooted
at the temp directory.

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build cmake-build-asan --target BundleFileSourceTest
```
Expected: FAIL -- `BundleFileSource` is not declared.

- [ ] **Step 3: Implement it**

`BundleFileSource` holds the reader, the root, and a sorted
`std::vector<std::string_view>` of identities built once in the
constructor. `isRegularFile`: strip the root (miss if the path is not under
it), then binary-search the identity vector. `isDirectory`: strip the root,
append `'/'`, and binary-search for the first identity with that prefix --
appending the separator before comparing is what makes the match
segment-aware, so `dep/` never matches `depot/...`. `readPackageJson`:
`isRegularFile(dir + "/package.json")` then return that module's payload as
a string.

Root stripping is lexical and requires an exact `root + "/"` prefix; no
`..` climbing and no symlink resolution, matching the producer's
no-realpath policy.

- [ ] **Step 4: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
./utils/format.sh -f
git add lib/bundle include/hermes/node-compat/bundle unittests
git commit -m "bundle: add BundleFileSource and pin it against the disk backend"
```

---

### Task 5: Expose resolution to the loader

Additive: the loader consults the container on a miss, and still falls back
to disk when the container has no answer. Nothing breaks yet.

**Files:**
- Modify: `lib/bundle/bundle_run.cpp`
- Modify: `libjs/bundle-loader.js`
- Test: `test/bundle-container-resolve.js` (new lit test)

**Interfaces:**
- Consumes: `BundleFileSource`, `resolveSpecifier(FileSource &, ...)`.
- Produces: the JS-visible `bundle.resolve(fromIdentity, request, paths)`
  returning an identity string, or `undefined`. `paths`, when given, is an
  array of directory strings; entries outside the bundle root resolve to
  nothing.

- [ ] **Step 1: Write the failing test**

`test/bundle-container-resolve.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A computed require() has no edge, so it used to go straight to disk.
// The container can answer it instead, with the same algorithm the
// producer used. The tree is deleted before the run, so a pass here cannot
// be the disk fallback answering.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/node_modules/dep
// RUN: echo '{ "main": "entry.js" }' > %t.tree/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 6 };" > %t.tree/node_modules/dep/entry.js
//
// pull.js names `dep` in a branch the run never takes. That is what puts it
// in the container -- the walk follows every literal require() whether or
// not the run reaches it -- while the only require that actually executes
// is the computed one in cli.js, which has no edge and must therefore be
// answered by the container's resolver.
// RUN: echo "if (globalThis.never) { require('dep'); }" > %t.tree/pull.js
// RUN: echo "require('./pull'); const n = 'd' + 'ep'; console.log('DYN', require(n).v);" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
//
// The tree goes before the run, so a pass here cannot be the disk fallback.
// RUN: rm -rf %t.tree/node_modules %t.tree/cli.js %t.tree/pull.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=DYN %s
// DYN: DYN 6
```

- [ ] **Step 2: Run it and watch it fail**

Use the lit command from Task 3, Step 2 with this file.
Expected: FAIL -- `Cannot find module 'dep'`, because the fallback has no
tree to read.

- [ ] **Step 2b: Validate identity shape when a container is opened**

Raised by the Task 4 review. Identities cannot contain `..` today only
because the producer derives them with `lexically_relative` against
`commonAncestor` -- `BundleReader` validates structure and never identity
shape. That was inert while identities only named payloads inside the
container. This task makes an identity into a `__filename`, a
`Module._cache` key, and the thing `BundleFileSource` answers questions
about, so a container carrying the identity `../etc/passwd` stops being
inert.

In `BundleReader::openImpl`, beside the existing kind and flags checks,
reject any identity that is empty, starts with `/`, contains a NUL, or has
a `.` or `..` path segment:

```cpp
if (!isValidIdentity(stringAt(m.identityString)))
  return fail("hermes-node bundle: module has a malformed identity");
```

Add the `RejectsMalformedIdentities` case to `unittests/BundleFormatTest.cpp`
covering each of those shapes, built through `BundleWriter` so the test
states what a container may contain rather than how bytes are laid out.

This is validation, not sanitisation: a container that fails it is rejected
outright, in both `open` and `openForInspection`, like every other
structural check.

- [ ] **Step 3: Add the native call**

In `bundle_run.cpp`, alongside `bundleLookupCallback`, add
`bundleResolveCallback`, registered as `__bundleResolve` / `resolve` in the
same `registerBundleFunction` block. It reads `fromIdentity` and `request`
as UTF-8 strings and an optional array third argument; builds
`state.root + "/" + fromIdentity` as the `fromFile`; and calls
`resolveSpecifier(fileSource, fromFile, request)` where `fileSource` is a
`BundleFileSource` constructed once and stored in `OpenBundle` beside the
reader. On success it strips the root back off and returns the identity; on
failure it returns `undefined`.

With `paths`, the same rule Node applies: for each entry, resolve as if
from `<entry>/x` and return the first hit. Entries outside the root simply
miss.

- [ ] **Step 4: Use it in the loader**

In `libjs/bundle-loader.js`'s `Module._load` wrapper, between the edge-table
miss and the fallback:

```js
      var resolved = bundle.resolve(importer, request);
      if (resolved !== undefined) return loadIdentity(resolved, parent, false);
```

and in `makeRequire`'s `resolve()`, after the edge-table lookup, including
the `options.paths` case:

```js
        var viaContainer = bundle.resolve(identity, request,
          hasPaths ? options.paths : undefined);
        if (viaContainer !== undefined) return path.join(root, viaContainer);
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS, including the new test and every existing bundle test.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add lib/bundle/bundle_run.cpp libjs/bundle-loader.js test/bundle-container-resolve.js
git commit -m "bundle: answer resolution from the container"
```

---

### Task 6: --include

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp` (parse the flag, pass it on)
- Modify: `include/hermes/node-compat/bundle/bundle_build.h`
- Modify: `lib/bundle/bundle_build.cpp`
- Test: `test/bundle-include.js` (new lit test)

**Interfaces:**
- Consumes: everything from Tasks 1-5.
- Produces: `buildBundle(napi_env, const std::string &entryPath, const
  std::string &outPath, bool verbose, const std::vector<std::string>
  &includes)`.

- [ ] **Step 1: Write the failing test**

`test/bundle-include.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --include packages what static discovery cannot see. The dependency here
// is named by a string the program assembles, which is the shape Babel's
// preset loading has, and nothing in the source mentions it.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/node_modules/late
// RUN: echo '{ "main": "index.js" }' > %t.tree/node_modules/late/package.json
// RUN: echo "module.exports = { v: 11 };" > %t.tree/node_modules/late/index.js
// RUN: echo "const n = 'la' + 'te'; console.log('LATE', require(n).v);" > %t.tree/cli.js

// Without it, the module is simply not there.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/plain.hbb --dump | %FileCheck --check-prefix=WITHOUT --implicit-check-not=late %s
// WITHOUT: MODULES

// With it, the module and its package.json are packaged, and the computed
// require finds them with the tree deleted.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --include=late %t.tree/cli.js
// RUN: rm -rf %t.tree/node_modules %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=WITH %s
// WITH: LATE 11

// An --include that does not resolve is a build error: the user named this
// one explicitly, so silence would be wrong.
// RUN: rm -rf %t.bad && mkdir -p %t.bad
// RUN: echo "console.log('x');" > %t.bad/cli.js
// RUN: %not %hermes-node --build-bundle=%t.bad/app.hbb --include=ghost %t.bad/cli.js 2>&1 | %FileCheck --check-prefix=BADINC %s
// BADINC: error: --include=ghost cannot be resolved
```

- [ ] **Step 2: Run it and watch it fail**

Use the lit command from Task 3, Step 2.
Expected: FAIL -- `--include` is not a recognized option.

- [ ] **Step 3: Parse the flag**

In `hermes-node.cpp`'s parse loop, accept repeated `--include=<value>` into
a `std::vector<std::string>`, and reject `--include` without
`--build-bundle` in `checkToolOptions()` with
`"--include requires --build-bundle"`, alongside the other rows of that
matrix.

- [ ] **Step 4: Walk the extra roots**

In `buildBundle`, after the entry is validated and before the worklist
loop, resolve each include from the entry's directory and seed the worklist:

```cpp
for (const std::string &spec : includes) {
  std::optional<std::string> resolved =
      resolveSpecifier(disk, absEntry, spec);
  if (!resolved) {
    std::fprintf(
        stderr, "error: --include=%s cannot be resolved\n", spec.c_str());
    return 1;
  }
  if (classifyFile(*resolved) == Packageability::kSkip) {
    std::fprintf(
        stderr,
        "error: --include=%s resolves to %s, which is not packageable (%s)\n",
        spec.c_str(), resolved->c_str(), formatSkipReason(*resolved).c_str());
    return 1;
  }
  if (pathIndex.count(*resolved) != 0)
    continue; // already reached from the entry
  pathIndex.emplace(*resolved, static_cast<uint32_t>(paths.size()));
  paths.push_back(*resolved);
  reporter.discovered(static_cast<uint32_t>(paths.size() - 1), *resolved);
}
```

The worklist loop then walks them exactly as it walks the entry, with no
change to the loop itself.

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add tools/hermes-node lib/bundle include/hermes/node-compat/bundle test/bundle-include.js
git commit -m "bundle: add --include for what static discovery cannot see"
```

---

### Task 7: Close the world

The deliberate breaking change. This is the only task allowed to change
existing test expectations.

**Files:**
- Modify: `libjs/bundle-loader.js` (delete the fallback, add the errors)
- Delete: `test/bundle-fallback.js`
- Modify: `test/bundle-run.js` (COLLIDE / BARE / THROW cases)
- Modify: `test/bundle-tolerant.js`, `test/bundle-require.js` as their
  expectations require
- Modify: `examples/babel-parser/run.sh`, `examples/babel-parser/README.md`
- Modify: `CLAUDE.md`, `docs/superpowers/plans/progress-aot-bundle.md`,
  `docs/superpowers/specs/2026-08-15-aot-bundle-design.md`

**Interfaces:**
- Consumes: everything above.
- Produces: no new API.

- [ ] **Step 1: Write the failing test**

Add to `test/bundle-tolerant.js`:

```js
// A bundle never reads code off the disk. A specifier the container cannot
// answer is an error naming the importer and the remedy, not a filesystem
// lookup -- which is both the point of shipping a bundle and the reason a
// computed specifier cannot be made to load arbitrary code.
// RUN: rm -rf %t.closed && mkdir -p %t.closed
// RUN: echo "module.exports = { v: 1 };" > %t.closed/ghost.js
// RUN: echo "const n = 'gh' + 'ost'; try { require('./' + n); } catch (e) { console.log('CLOSED', e.code); }" > %t.closed/cli.js
// RUN: %hermes-node --build-bundle=%t.closed/app.hbb %t.closed/cli.js
// RUN: %hermes-node --bundle=%t.closed/app.hbb | %FileCheck --check-prefix=CLOSED %s
// CLOSED: CLOSED MODULE_NOT_FOUND
```

The fixture keeps `ghost.js` on disk deliberately: with the fallback gone,
its presence must make no difference.

- [ ] **Step 2: Run it and watch it fail**

Use the lit command from Task 3, Step 2 against `test/bundle-tolerant.js`.
Expected: FAIL -- the fallback loads `ghost.js` from disk and nothing is
caught.

- [ ] **Step 3: Delete the fallback**

In `libjs/bundle-loader.js`'s `Module._load` wrapper, replace both
`originalLoad` calls. The no-bundled-importer branch keeps handing builtins
to the original loader -- builtins are embedded, not on disk -- but a
non-builtin with no bundled importer throws. The miss branch throws:

```js
      var err = new Error(
        "Cannot find module '" + request + "'\n" +
        "  required by " + importer + "\n" +
        "  Not in the bundle. Add it with:\n" +
        "    --include=" + request);
      err.code = 'MODULE_NOT_FOUND';
      throw err;
```

A request resolving to a `.node` addon gets its own message -- "native
addons are not supported in a bundle yet" -- keeping `code` unchanged so a
probing caller still sees what it expects. Keep the
`HERMES_NODE_DEBUG_NATIVE=BUNDLE` log line before the throw: it is now the
trace of what a failing bundle asked for.

Delete `originalLoad` itself once nothing references it.

- [ ] **Step 3b: Reconsider the options.paths bound now the fallback is gone**

Task 5 left `probeForContainer()` in `lib/bundle/bundle_run.cpp` bounding
the `options.paths` branch to the nearest `node_modules`, because an
unbounded walk against a container with incomplete knowledge could climb
past an ancestor whose `node_modules` was never packaged and land on a
different, real package several levels up -- a confident wrong answer that
`test/bundle-require.js`'s OPTPATHS case caught. The bound was correct
while a disk fallback stood behind it.

That justification is gone. With no fallback the container is the only
source, so there is no unpackaged ancestor to be wrong about: climbing past
a level with no records is now simply a miss, and the walk answers what the
producer's own walk answered. Delete `probeForContainer()` and
`isRelativeRequest()` and pass `request` through on both branches, so one
algorithm runs on both sides -- which is what this whole design is for.

Then rewrite OPTPATHS as a closed-world assertion: the container answers
NEAR, or the call throws. Keep the HOIST and PATHSHIT cases from Task 5;
they must still pass unmodified, since removing the bound can only widen
what resolves.

- [ ] **Step 4: Migrate the tests**

`git rm test/bundle-fallback.js` -- every case in it asserts the fallback,
and a test whose premise is gone is not a test to repair. Move its two
still-meaningful assertions into `test/bundle-run.js` in closed-world form:
a module reached both from the container and by a computed specifier is
instantiated once (now trivially, since both come from the container), and
`delete require.cache[require.resolve(x)]` forces a reload.

Rewrite the COLLIDE / BARE / THROW cases in `test/bundle-run.js` so each
asserts the container's answer instead of the disk's.

- [ ] **Step 5: Run everything, including the examples**

```bash
cmake --build cmake-build-asan --target check-hermes-node
cmake --build cmake-build-release --target hermes-node
./examples/run-examples.sh cmake-build-release
```
Expected: PASS. `test/bundle-yargs.js` is the one at genuine risk (the
design records this): if a computed require is on its live path, add the
needed `--include` to that test's build line. If it cannot be fixed that
way, stop and report -- that is a gap in the design, not a test to weaken.

- [ ] **Step 6: Make the example prove the feature**

In `examples/babel-parser/run.sh`, add a case that bundles the
**unmodified** `transform.js` with `--include=@babel/preset-env` and runs
it with the tree hidden, next to the existing `transform-static.js` case.
That is the end-to-end statement of this plan: the idiomatic source, no
edit, self-contained. Update `README.md`'s "Why there are two transform
scripts" to say `--include` is the other way to solve it.

- [ ] **Step 7: Update the docs**

`CLAUDE.md`'s AOT Bundles section: the fallback bullet is replaced by the
closed-world rule, `--include`, resolution inputs, and format v2. Its
`require.resolve` bullet ("edge table first, then `Module._resolveFilename`")
is stale as of Task 5 -- there is now a container-resolve step between the
two, and after this task there is no `Module._resolveFilename` step at all.
`2026-08-15-aot-bundle-design.md` gets a "Superseded 2026-08-19" note on
its fallback rows pointing at the new design.
`progress-aot-bundle.md` gets the outcome, including whether the yargs test
needed an `--include`.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add -A
git commit -m "bundle: close the world -- no module is ever read from disk"
```

---

## Notes for the executor

- Tasks 1-6 are additive; every existing test must pass untouched at the
  end of each. If one fails, that is a defect in the task, not a test to
  update.
- Task 3 and Task 6 both grow containers. `test/bundle-verbose.js` asserts
  exact module and edge counts; a changed count there is expected and the
  new number is the correct assertion.
- The one number that must not change: a container is byte-for-byte
  identical with and without `--verbose`. `test/bundle-verbose.js` has a
  `cmp` for this. It has held through every round of this subsystem's work.
