# Bundle Preloads Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** A bundle carries its own preloads: `--build-bundle
--preload=<specifier>` packages a module and records that it runs before the
entry, `--bundle` runs it automatically, and run-time `-r` is refused.

**Architecture:** `--preload` is a third caller of the producer's existing
seed-a-root mechanism (the entry and `--include` are the other two), which
additionally appends the module's index to a new container section. The
consumer reads that section and runs each module through the same
`loadIdentity` every bundled module goes through, before the entry.

**Tech Stack:** C++17, CMake + Ninja, GTest (`unittests/`), LLVM lit
(`test/`), Hermes Node-API.

**Spec:** `docs/superpowers/specs/2026-08-20-bundle-preload-design.md`. Read it —
this plan argues from it, and the rationale for every decision is there.
Background: `docs/superpowers/specs/2026-08-19-closed-world-bundle-design.md`.
Progress: append outcomes to `docs/superpowers/plans/progress-aot-bundle.md`.

## Global Constraints

- New files carry `Copyright (c) Tzvetan Mikov.` -- NOT Meta Platforms.
- Commit messages are ASCII only, no emojis.
- Before every commit: `./utils/format.sh -f`, then
  `cmake --build cmake-build-asan --target check-hermes-node`. Both clean.
- Never modify anything under `libjs-node/`. `libjs/` is ours.
- Do not weaken an existing test. Where a task deliberately changes what the
  code emits (the format version string in `--dump` output moves from
  `v2` to `v3`), update the expected value to the new correct one and say
  why in your report. Anywhere else, a failing existing test is a defect in
  the task.
- `hermesNodeBundle` must not link the Hermes VM. `BundleFormatTest`,
  `BundleResolveTest`, `BundleFileSourceTest` and `BundleToolsTest` run with
  no runtime and must keep doing so.
- The bundle format version becomes `3`. A version mismatch stays fatal in
  both `BundleReader::open` and `openForInspection`.
- Build directory for development and tests: `cmake-build-asan`.
- Run one lit test with (paths must be absolute):
  `python3 cmake-build-asan/bin/hermes-lit -v $(pwd)/test/<name>.js --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node --param hermes=$(pwd)/cmake-build-asan/bin/hermes --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck --param not=$(pwd)/cmake-build-asan/bin/not --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test`
- A lit FileCheck prefix must NOT end in the letters `RUN` -- lit matches its
  `RUN:` keyword anywhere in a line, so such a prefix is executed as a shell
  command. Use a suffix such as `EXEC`.

---

### Task 1: Format v3 -- the preload table

The container gains a section. Nothing produces or consumes preloads yet;
this task is the format and its validation.

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_format.h`
- Modify: `include/hermes/node-compat/bundle/bundle_writer.h`
- Modify: `lib/bundle/bundle_writer.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h`
- Modify: `lib/bundle/bundle_reader.cpp`
- Modify: `lib/bundle/bundle_tools.cpp`
- Test: `unittests/BundleFormatTest.cpp`, `test/bundle-dump.js`

**Interfaces:**
- Consumes: nothing.
- Produces: `void BundleWriter::addPreload(uint32_t moduleIndex);`
  `uint32_t BundleReader::preloadCount() const;`
  `uint32_t BundleReader::preload(uint32_t i) const;` (the module index).

- [ ] **Step 1: Write the failing test**

In `unittests/BundleFormatTest.cpp`:

```cpp
TEST(BundleFormatTest, RoundTripsPreloads) {
  BundleWriter w;
  uint32_t entry = w.addModule("cli.js", ModuleKind::kJavaScript,
                               kRequirable, "A");
  uint32_t setup = w.addModule("setup.js", ModuleKind::kJavaScript,
                               kRequirable, "B");
  w.setEntry(entry);
  w.addPreload(setup);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());

  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->formatVersion(), 3u);
  ASSERT_EQ(r->preloadCount(), 1u);
  EXPECT_EQ(r->preload(0), setup);
}

// Order is the meaning of this table: it is why preloads are a section
// rather than another flag bit on the module record.
TEST(BundleFormatTest, PreservesPreloadOrder) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b = w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "B");
  uint32_t c = w.addModule("c.js", ModuleKind::kJavaScript, kRequirable, "C");
  w.setEntry(c);
  w.addPreload(b);
  w.addPreload(a);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  ASSERT_EQ(r->preloadCount(), 2u);
  EXPECT_EQ(r->preload(0), b);
  EXPECT_EQ(r->preload(1), a);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build cmake-build-asan --target BundleFormatTest
```
Expected: FAIL to compile -- `addPreload` does not exist.

- [ ] **Step 3: Add the section to the format**

In `bundle_format.h`: `kBundleFormatVersion = 3`; add to `BundleHeader`,
after `edgeCount` and before `payloadOffset`:

```cpp
  uint32_t preloadTableOffset;
  uint32_t preloadCount;
```

Document it above the struct in the file's existing voice: the table is an
array of `uint32_t` module indices, in the order the modules run, and the
order is why this is a section rather than a flag bit.

- [ ] **Step 4: Write it**

In `bundle_writer.cpp::serialize`, the section goes between the edge table
and the payload, keeping the existing `alignUp` discipline:

```cpp
  size_t preloadTableOffset = edgeTableOffset + edgeTableSize;
  size_t preloadTableSize = preloads_.size() * sizeof(uint32_t);
  size_t payloadOffset =
      alignUp(preloadTableOffset + preloadTableSize, kBundlePayloadAlign);
```

Set `header.preloadTableOffset` / `header.preloadCount` alongside the other
header fields, and append each index with `appendPod` after the edge loop.
`BundleWriter` gains `std::vector<uint32_t> preloads_;` and:

```cpp
void BundleWriter::addPreload(uint32_t moduleIndex) {
  preloads_.push_back(moduleIndex);
}
```

- [ ] **Step 5: Validate it on open**

In `bundle_reader.cpp::openImpl`, beside the existing table checks:

```cpp
  if (!tableInRange(
          size,
          header->preloadTableOffset,
          header->preloadCount,
          sizeof(uint32_t)))
    return fail("hermes-node bundle: preload table out of range");
```

Add `header->preloadTableOffset % alignof(uint32_t) != 0` to the existing
misalignment check, and after the edge loop:

```cpp
  const auto *preloads =
      reinterpret_cast<const uint32_t *>(data + header->preloadTableOffset);
  for (uint32_t i = 0; i < header->preloadCount; ++i) {
    if (preloads[i] >= header->moduleCount)
      return fail("hermes-node bundle: preload references an unknown module");
    // A preload names something the run will require(). A resolution-input
    // package.json cannot be required, so naming one is a malformed
    // container rather than a run-time surprise.
    if ((modules[preloads[i]].flags & kRequirable) == 0)
      return fail("hermes-node bundle: preload names a non-requirable module");
  }
```

Add `preloadCount()` and `preload(uint32_t)` accessors to `BundleReader`.

- [ ] **Step 6: Test the two rejections**

```cpp
TEST(BundleFormatTest, RejectsPreloadIndexOutOfRange) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(a);
  w.addPreload(7); // there is one module
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());
  std::string error;
  EXPECT_FALSE(BundleReader::open(
                   bytes.data(), bytes.size(), bundleGenerationTag(), &error)
                   .has_value());
  EXPECT_NE(error.find("preload references an unknown module"),
            std::string::npos)
      << error;
}

TEST(BundleFormatTest, RejectsNonRequirablePreload) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t pkg = w.addModule("package.json", ModuleKind::kJSON, 0, "{}");
  w.setEntry(a);
  w.addPreload(pkg);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());
  std::string error;
  EXPECT_FALSE(BundleReader::open(
                   bytes.data(), bytes.size(), bundleGenerationTag(), &error)
                   .has_value());
  EXPECT_NE(error.find("non-requirable"), std::string::npos) << error;
}
```

- [ ] **Step 7: Print it in --dump**

In `bundle_tools.cpp`, after the EDGES block, print the table in order --
only when there is one, so an ordinary container's dump is unchanged:

```
PRELOADS (1)
  [1] setup.js
```

`test/bundle-dump.js` asserts `format v2`; that string is now `v3`. Update
it and say why in your report -- this is the one deliberately-changed
expected value in this task.

- [ ] **Step 8: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/bundle lib/bundle unittests/BundleFormatTest.cpp test/bundle-dump.js
git commit -m "bundle: add a preload table to the container, format v3"
```

---

### Task 2: `--preload` in the producer

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp`
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h`
- Modify: `lib/runtime/hermes_node_runtime.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_build.h`
- Modify: `lib/bundle/bundle_build.cpp`
- Test: `test/bundle-preload.js` (new)

**Interfaces:**
- Consumes: `BundleWriter::addPreload(uint32_t)`.
- Produces: `buildBundle(napi_env, const std::string &entryPath, const
  std::string &outPath, bool verbose, const std::vector<std::string>
  &includes, const std::vector<std::string> &preloads)`.

- [ ] **Step 1: Write the failing test**

`test/bundle-preload.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --preload records a module that runs before the entry. The fixture's
// setup.js is required by NOTHING -- which is what a register or polyfill
// module looks like -- so it is in the container only because the flag put
// it there, and the walk would never have found it.

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "globalThis.SETUP = 'ran';" > %t.tree/setup.js
// RUN: echo "console.log('ENTRY sees', globalThis.SETUP);" > %t.tree/cli.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --preload=./setup %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=DUMP %s
// DUMP: setup.js
// DUMP: PRELOADS (1)
// DUMP: setup.js

// Without the flag it is not in the container at all.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/plain.hbb --dump | %FileCheck --check-prefix=PLAIN --implicit-check-not=setup.js --implicit-check-not=PRELOADS %s
// PLAIN: MODULES

// A --preload that does not resolve is a build error: the user named this
// one explicitly, so silence would be wrong.
// RUN: %not %hermes-node --build-bundle=%t.tree/bad.hbb --preload=./ghost %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=BADPRE %s
// BADPRE: error: --preload=./ghost cannot be resolved

// --preload without --build-bundle is a flag conflict, reported after the
// whole parse so flag order cannot change the outcome.
// RUN: %not %hermes-node --preload=./setup %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=NOBUILD %s
// NOBUILD: --preload requires --build-bundle

// A --preload naming something unpackageable is a build error too, and says
// which reason: the user named this file explicitly, so skipping it with a
// warning the way the walk does would leave a container that cannot run.
// RUN: printf 'not really an addon\n' > %t.tree/native.node
// RUN: %not %hermes-node --build-bundle=%t.tree/bad2.hbb --preload=./native.node %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=BADKIND %s
// BADKIND: error: --preload=./native.node resolves to {{.*}}native.node, which is not packageable

// The same value twice records ONE entry: the second load would be a
// Module._cache hit and execute nothing, so a second table row would
// promise something the loader cannot do.
// RUN: rm -rf %t.dup && mkdir -p %t.dup
// RUN: echo "globalThis.N = (globalThis.N || 0) + 1;" > %t.dup/setup.js
// RUN: echo "console.log('DUP N =', globalThis.N);" > %t.dup/cli.js
// RUN: %hermes-node --build-bundle=%t.dup/app.hbb --preload=./setup --preload=./setup %t.dup/cli.js
// RUN: %hermes-node --bundle=%t.dup/app.hbb --dump | %FileCheck --check-prefix=DUP %s
// DUP: PRELOADS (1)

// A --preload that the entry already reaches is packaged once and recorded
// once: seeding a root that is already on the worklist must not duplicate
// the module.
// RUN: rm -rf %t.both && mkdir -p %t.both
// RUN: echo "module.exports = { v: 1 };" > %t.both/setup.js
// RUN: echo "console.log('BOTH', require('./setup').v);" > %t.both/cli.js
// RUN: %hermes-node --build-bundle=%t.both/app.hbb --preload=./setup %t.both/cli.js
// RUN: %hermes-node --bundle=%t.both/app.hbb --dump | %FileCheck --check-prefix=BOTH %s
// BOTH: MODULES (2)
// BOTH: PRELOADS (1)
```

- [ ] **Step 2: Run it and watch it fail**

Use the lit command from Global Constraints.
Expected: FAIL -- `--preload` is not a recognized option.

- [ ] **Step 3: Parse the flag**

In `hermes-node.cpp`, accept repeated `--preload=<value>` into
`config.preloadModules` (a `std::vector<std::string>`), exactly as
`--include` accumulates into `config.includeModules`. In
`checkToolOptions()`, with the other rows and after the parse loop:

```cpp
  if (!config.preloadModules.empty() && config.buildBundlePath.empty()) {
    std::fprintf(stderr, "Error: --preload requires --build-bundle.\n");
    return false;
  }
```

Thread `preloadModules` through `HermesNodeConfig` to the `buildBundle`
call, as `includeModules` already is.

- [ ] **Step 4: Seed the roots and record them**

In `buildBundle`, immediately after the `--include` seeding loop and before
the worklist loop, so preloads are roots on the same footing:

```cpp
  // --preload is a third caller of the seed-a-root mechanism above: the
  // walk below packages it exactly as it packages the entry and each
  // --include. What makes it a preload is the index recorded here. A
  // recorded preload that was not packaged would be a container that
  // cannot run, which is why this seeds rather than requiring the user to
  // pass --include as well.
  std::vector<std::string> preloadPaths;
  for (const std::string &spec : preloads) {
    std::optional<std::string> resolved =
        resolveSpecifier(disk, absEntry, spec);
    if (!resolved) {
      std::fprintf(
          stderr, "error: --preload=%s cannot be resolved\n", spec.c_str());
      return 1;
    }
    if (classifyFile(*resolved) == Packageability::kSkip) {
      std::fprintf(
          stderr,
          "error: --preload=%s resolves to %s, which is not packageable "
          "(%s)\n",
          spec.c_str(),
          resolved->c_str(),
          formatSkipReason(*resolved).c_str());
      return 1;
    }
    // The same module named twice runs once -- the second load is a
    // Module._cache hit -- so recording it twice would promise something
    // the loader cannot do.
    if (std::find(preloadPaths.begin(), preloadPaths.end(), *resolved) !=
        preloadPaths.end())
      continue;
    preloadPaths.push_back(*resolved);
    if (pathIndex.count(*resolved) != 0)
      continue; // already reached from the entry or an --include
    pathIndex.emplace(*resolved, static_cast<uint32_t>(paths.size()));
    paths.push_back(*resolved);
    reporter.discovered(static_cast<uint32_t>(paths.size() - 1), *resolved);
  }
```

Then, where the writer is populated (after every module has an index), in
`preloadPaths` order:

```cpp
  for (const std::string &p : preloadPaths)
    writer.addPreload(pathIndex.at(p));
```

- [ ] **Step 5: Narrate it under --verbose**

`BuildReporter` gains, in the style of its neighbours:

```cpp
  /// A module recorded to run before the entry. Reported separately from
  /// discovered() because being packaged and being a preload are different
  /// facts about the same module.
  void preloaded(uint32_t index, const std::string &path) {
    if (!enabled_)
      return;
    std::fprintf(stderr, "preload [%u] %s\n", index, path.c_str());
  }
```

Call it once per entry in `preloadPaths` after the indices are known, and
add `preloads: <n>` to the summary block when non-zero.

- [ ] **Step 6: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add tools/hermes-node lib include test/bundle-preload.js
git commit -m "bundle: add --preload, recording a module that runs before the entry"
```

---

### Task 3: The consumer runs them

**Files:**
- Modify: `lib/bundle/bundle_run.cpp`
- Modify: `libjs/bundle-loader.js`
- Test: `test/bundle-preload.js` (extend)

**Interfaces:**
- Consumes: `BundleReader::preloadCount()`, `BundleReader::preload(i)`.
- Produces: JS-visible `bundle.preloads()` returning an array of identity
  strings in run order; `installBundleLoader(...)` now returns `run()`
  rather than `runEntry()`.

- [ ] **Step 1: Write the failing test**

Append to `test/bundle-preload.js`. Every run deletes the tree first, so a
pass cannot be the disk answering:

```js
// The preload runs before the entry, and its own require() resolves from
// the container like any bundled module's.
// RUN: rm -rf %t.run && mkdir -p %t.run
// RUN: echo "module.exports = { v: 3 };" > %t.run/dep.js
// RUN: echo "globalThis.SETUP = require('./dep').v; console.log('PRELOAD ran');" > %t.run/setup.js
// RUN: echo "console.log('ENTRY ran, SETUP =', globalThis.SETUP);" > %t.run/cli.js
// RUN: %hermes-node --build-bundle=%t.run/app.hbb --preload=./setup %t.run/cli.js
// RUN: rm -f %t.run/setup.js %t.run/cli.js %t.run/dep.js
// RUN: %hermes-node --bundle=%t.run/app.hbb | %FileCheck --check-prefix=ORDER %s
// ORDER: PRELOAD ran
// ORDER-NEXT: ENTRY ran, SETUP = 3

// Two preloads run in flag order.
// RUN: rm -rf %t.two && mkdir -p %t.two
// RUN: echo "console.log('FIRST');" > %t.two/one.js
// RUN: echo "console.log('SECOND');" > %t.two/two.js
// RUN: echo "console.log('THIRD');" > %t.two/cli.js
// RUN: %hermes-node --build-bundle=%t.two/app.hbb --preload=./one --preload=./two %t.two/cli.js
// RUN: rm -f %t.two/one.js %t.two/two.js %t.two/cli.js
// RUN: %hermes-node --bundle=%t.two/app.hbb | %FileCheck --check-prefix=TWO %s
// TWO: FIRST
// TWO-NEXT: SECOND
// TWO-NEXT: THIRD

// A preload is not the main module. It runs before the entry exists, so
// require.main is not yet set -- the same thing Node's -r observes, and for
// the same reason.
// RUN: rm -rf %t.main && mkdir -p %t.main
// RUN: echo "console.log('PRE main is', typeof require.main);" > %t.main/setup.js
// RUN: echo "console.log('ENTRY main is', require.main === module);" > %t.main/cli.js
// RUN: %hermes-node --build-bundle=%t.main/app.hbb --preload=./setup %t.main/cli.js
// RUN: rm -f %t.main/setup.js %t.main/cli.js
// RUN: %hermes-node --bundle=%t.main/app.hbb | %FileCheck --check-prefix=MAIN %s
// MAIN: PRE main is undefined
// MAIN: ENTRY main is true

// A throwing preload stops the run before the entry executes.
// RUN: rm -rf %t.throw && mkdir -p %t.throw
// RUN: echo "throw new Error('preload exploded');" > %t.throw/setup.js
// RUN: echo "console.log('ENTRY SHOULD NOT RUN');" > %t.throw/cli.js
// RUN: %hermes-node --build-bundle=%t.throw/app.hbb --preload=./setup %t.throw/cli.js
// RUN: rm -f %t.throw/setup.js %t.throw/cli.js
// RUN: %not %hermes-node --bundle=%t.throw/app.hbb 2>&1 | %FileCheck --check-prefix=THROW --implicit-check-not="ENTRY SHOULD NOT RUN" %s
// THROW: preload exploded
```

- [ ] **Step 2: Run it and watch it fail**

Use the lit command from Global Constraints.
Expected: FAIL -- the preload never runs; `ORDER` sees only the entry.

- [ ] **Step 3: Expose the table to JavaScript**

In `bundle_run.cpp`, add `bundlePreloadsCallback` beside
`bundleEntryCallback`, registered through the same helper as
`__bundlePreloads` / `preloads`. It builds a JS array of identity strings:

```cpp
  const OpenBundle &state = openBundleState();
  uint32_t n = state.reader->preloadCount();
  napi_value result;
  if (napi_create_array_with_length(env, n, &result) != napi_ok)
    return nullptr;
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view id = state.reader->identity(state.reader->preload(i));
    napi_value item;
    if (napi_create_string_utf8(env, id.data(), id.size(), &item) != napi_ok)
      return nullptr;
    napi_set_element(env, result, i, item);
  }
  return result;
```

Identities rather than indices: JavaScript already speaks identities
everywhere else in this file, and `loadIdentity` takes one.

- [ ] **Step 4: Run them before the entry**

In `libjs/bundle-loader.js`, replace the returned `runEntry` with:

```js
    // Runs the bundle: its recorded preloads, in order, then its entry.
    // Each goes through loadIdentity like every other bundled module, so a
    // preload's own require() is a container require and there is no second
    // code path. A preload is not the main module -- the entry is -- and it
    // runs before the entry exists, so require.main is unset while it runs,
    // exactly as it is for Node's -r.
    return function run() {
      var preloads = bundle.preloads();
      for (var i = 0; i < preloads.length; i++)
        loadIdentity(preloads[i], null, false);
      return loadIdentity(bundle.entry(), null, true);
    };
```

The C++ caller in `hermes_node_runtime.cpp` calls the returned function as
it already does; only the name in its comment changes.

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add lib/bundle/bundle_run.cpp libjs/bundle-loader.js test/bundle-preload.js
git commit -m "bundle: run a container's recorded preloads before its entry"
```

---

### Task 4: Refuse run-time `-r`, and document

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp`
- Modify: `CLAUDE.md`
- Modify: `docs/superpowers/plans/progress-aot-bundle.md`
- Test: `test/bundle-errors.js` (extend)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

- [ ] **Step 1: Write the failing test**

Append to `test/bundle-errors.js`:

```js
// -r/--require is refused in bundle mode. A bundle carries its own
// preloads (--preload at build time); the operator of a sealed artifact
// does not get to insert code into it. This is also what makes the
// injection point unreachable: a preload running before the bundle loader
// was installed could plant Module._cache[<root>/<identity>] and replace a
// bundled module's exports, and there is now no such phase to occupy.
// RUN: rm -rf %t.rtree && mkdir -p %t.rtree
// RUN: echo "console.log('ok');" > %t.rtree/cli.js
// RUN: %hermes-node --build-bundle=%t.rtree/app.hbb %t.rtree/cli.js
// RUN: echo "console.log('PRELOAD RAN');" > %t.rtree/pre.js
// RUN: %not %hermes-node --bundle=%t.rtree/app.hbb -r %t.rtree/pre.js 2>&1 | %FileCheck --check-prefix=NORFLAG --implicit-check-not="PRELOAD RAN" %s
// NORFLAG: --bundle cannot be combined with -r or --require
```

- [ ] **Step 2: Run it and watch it fail**

Use the lit command from Global Constraints.
Expected: FAIL -- the preload runs and prints `PRELOAD RAN`.

- [ ] **Step 3: Add the row**

In `checkToolOptions()`, with the other rows -- again reading `config`, not
`tools`:

```cpp
  if (!config.bundlePath.empty() && !config.requireModules.empty()) {
    std::fprintf(
        stderr,
        "error: --bundle cannot be combined with -r or --require\n");
    return false;
  }
```

`-r` with `--build-bundle` is deliberately untouched: a build runs in the
disk world.

- [ ] **Step 4: Update CLAUDE.md**

In the AOT Bundles section, add a bullet: a bundle carries its own
preloads; `--preload=<specifier>` at build time resolves from the entry's
directory exactly as `--include` does and additionally records the module
in the container's preload table, which `--bundle` runs in order before the
entry; run-time `-r` is refused, because the artifact decides what runs
inside it. Note format v3 and that `--dump` prints the table.

- [ ] **Step 5: Update the progress file**

Append to `docs/superpowers/plans/progress-aot-bundle.md`: what shipped, that this
resolves the `-r` preload injection point recorded there, and the design
doc's two risks -- a container can now run code before its entry, visible
only through `--dump`; and `examples/flow-bundler`, the natural end-to-end
case, is still not bundled.

- [ ] **Step 6: Run everything, including the examples**

```bash
cmake --build cmake-build-asan --target check-hermes-node
cmake --build cmake-build-release --target hermes-node
./examples/run-examples.sh cmake-build-release
```
Expected: PASS. `examples/flow-bundler/run.sh` uses `-r` with no bundle, so
it is unaffected; confirm that rather than assuming it.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add tools/hermes-node CLAUDE.md docs/superpowers/plans test/bundle-errors.js
git commit -m "bundle: refuse -r in bundle mode; a bundle carries its own preloads"
```

---

## Notes for the executor

- Task 1 changes the format version, so every container built by an earlier
  binary is rejected. That is intended and needs no compatibility path.
- Tasks 2 and 3 are separable on purpose: after Task 2 a container records
  preloads that nothing runs, which `--dump` can show and a reviewer can
  check independently of the run-time half.
- The invariant that has held through every round of this subsystem: a
  container is byte-for-byte identical with and without `--verbose`.
  `test/bundle-verbose.js` has a `cmp` for it. Do not break it -- in
  particular, `BuildReporter::preloaded()` must not influence what is
  written.
