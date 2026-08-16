# Bundle Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make an AOT bundle inspectable -- a verbose producer, a container
dump, single-module extraction, and disassembly of a bytecode file.

**Architecture:** Three read-only verbs (`--dump`, `--extract-module`,
`--dump-bytecode`) handled in `main()` before any runtime exists, plus a
`--verbose` flag on the existing producer. Two new libraries: one VM-free for
reading containers, one linking the VM for Hermes's disassembler.

**Tech Stack:** C++17, Hermes `BytecodeDisassembler` + `BCProviderFromBuffer`,
CMake + Ninja, GTest, LLVM Lit.

**Spec:** `history/plans/2026-08-15-bundle-tooling-design.md`

## Global Constraints

- Copyright header on every new file: `Copyright (c) Tzvetan Mikov.` (NOT Meta Platforms).
- Commit messages: ASCII only, no emojis.
- Never `git add hermes`; the submodule pin must not change.
- Do not modify anything under `libjs-node/`.
- Do not edit an existing file under `test/` to make a change pass. Adding
  cases to a test file this plan creates is fine.
- Never commit `examples/*/node_modules`. If you move one aside to test,
  restore it and confirm `git status` is clean.
- Build with Clang, never GCC. Primary config is `cmake-build-asan`.
- Before every commit: `./utils/format.sh --check` and
  `cmake --build cmake-build-asan --target check-hermes-node`.
  Use `--check`, never `-f` (which formats the *last commit*, not the worktree).
- **`--build-bundle` must produce byte-for-byte identical containers before
  and after this work, with or without `--verbose`.** Diagnostics never
  change the artifact.
- `hermesNodeBundle` must stay free of any Hermes VM link dependency --
  `BundleFormatTest` depends on running with no runtime.
- Diagnostics come from explicit flags only. Nothing added here reads the
  environment; `HERMES_NODE_DEBUG_NATIVE` is untouched.
- Verbose output goes to **stderr**; dump output goes to **stdout**.
- lit portability: `dd conv=notrunc` and `head -c`, never GNU-only
  `truncate`, and never `dd status=none` (silence dd with `2>/dev/null`).

## File structure

| File | Responsibility |
| --- | --- |
| `include/hermes/node-compat/bundle/bundle_reader.h` | Gains `openForInspection` and the header/edge read accessors a dump needs. |
| `lib/bundle/bundle_reader.cpp` | Implements them; `open()` keeps its signature and its hard error. |
| `include/hermes/node-compat/bundle/bundle_tools.h` | `dumpBundle`, `extractModule`. No napi, no Hermes. |
| `lib/bundle/bundle_tools.cpp` | Implementation. |
| `include/hermes/node-compat/bytecode-dump/bytecode_dump.h` | `dumpBytecodeFile`. |
| `lib/bytecode-dump/bytecode_dump.cpp` | Hermes disassembler wiring. Links `hermesvm_a`. |
| `lib/bundle/bundle_build.cpp` | Gains verbose reporting. |
| `tools/hermes-node/hermes-node.cpp` | Flag parsing, the flag-conflict matrix, and the pre-runtime verb dispatch. |
| `unittests/BundleFormatTest.cpp` | Inspection-mode cases. |
| `unittests/BundleToolsTest.cpp` | Dump and extract, no runtime. |
| `test/bundle-verbose.js`, `bundle-dump.js`, `bundle-extract.js`, `bundle-dump-bytecode.js`, `bundle-tool-errors.js` | Lit behavior tests. |

---

### Task 1: Reader inspection API

The dump needs to read header fields and walk the edge table, neither of
which the reader exposes today, and it must tolerate a generation mismatch.

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h`
- Modify: `lib/bundle/bundle_reader.cpp`
- Test: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Consumes: `BundleHeader`, `BundleModuleRecord`, `BundleEdgeRecord` from `bundle_format.h`.
- Produces, all `const` on `BundleReader`:
  - `static std::optional<BundleReader> openForInspection(const uint8_t *data, size_t size, std::string *error)`
  - `uint32_t formatVersion() const`
  - `uint32_t generationTag() const`
  - `uint32_t edgeCount() const`
  - `struct EdgeView { uint32_t importer; std::string_view specifier; uint32_t target; }`
  - `EdgeView edge(uint32_t edgeIndex) const`
  - `uint32_t payloadOffset(uint32_t moduleIndex) const`
  - `uint32_t stringsSize() const`, `moduleTableSize() const`, `edgeTableSize() const`, `payloadSize() const`

- [ ] **Step 1: Write the failing tests**

Add to `unittests/BundleFormatTest.cpp`:

```cpp
TEST(BundleFormatTest, OpenForInspectionAcceptsGenerationMismatch) {
  BundleWriter writer;
  uint32_t entry = writer.addModule("a.js", ModuleKind::kJavaScript, "bc-a");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(0xAAAAAAAA);

  std::string error;
  // The enforcing entry point refuses it.
  EXPECT_FALSE(
      BundleReader::open(bytes.data(), bytes.size(), 0xBBBBBBBB, &error));
  EXPECT_NE(error.find("generation"), std::string::npos);

  // The inspecting one accepts it and reports the tag as stored.
  error.clear();
  auto reader = BundleReader::openForInspection(
      bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(reader) << error;
  EXPECT_EQ(reader->generationTag(), 0xAAAAAAAAu);
  EXPECT_EQ(reader->formatVersion(), kBundleFormatVersion);
}

TEST(BundleFormatTest, OpenForInspectionStillRejectsStructuralDamage) {
  BundleWriter writer;
  uint32_t entry = writer.addModule("a.js", ModuleKind::kJavaScript, "bc-a");
  writer.setEntry(entry);
  std::vector<uint8_t> good = writer.serialize(0xAAAAAAAA);

  // Bad magic.
  std::vector<uint8_t> badMagic = good;
  badMagic[0] = 'X';
  std::string error;
  EXPECT_FALSE(BundleReader::openForInspection(
      badMagic.data(), badMagic.size(), &error));
  EXPECT_NE(error.find("magic"), std::string::npos);

  // Bad format version.
  std::vector<uint8_t> badVersion = good;
  reinterpret_cast<BundleHeader *>(badVersion.data())->formatVersion =
      kBundleFormatVersion + 1;
  error.clear();
  EXPECT_FALSE(BundleReader::openForInspection(
      badVersion.data(), badVersion.size(), &error));
  EXPECT_NE(error.find("format version"), std::string::npos);

  // Truncated below the header.
  error.clear();
  EXPECT_FALSE(BundleReader::openForInspection(good.data(), 8, &error));
  EXPECT_NE(error.find("truncated"), std::string::npos);
}

TEST(BundleFormatTest, EdgeAccessorMatchesLookup) {
  BundleWriter writer;
  uint32_t a = writer.addModule("a.js", ModuleKind::kJavaScript, "bc-a");
  uint32_t b = writer.addModule("b.js", ModuleKind::kJavaScript, "bc-b");
  writer.addEdge(a, "./b", b);
  writer.addEdge(b, "./a", a);
  writer.setEntry(a);
  std::vector<uint8_t> bytes = writer.serialize(0xAAAAAAAA);

  std::string error;
  auto reader =
      BundleReader::openForInspection(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(reader) << error;
  ASSERT_EQ(reader->edgeCount(), 2u);

  // Every edge the table holds must be findable by lookup, with the same
  // target. This is the property the dump relies on when it prints the
  // table in stored order.
  for (uint32_t i = 0; i < reader->edgeCount(); ++i) {
    BundleReader::EdgeView e = reader->edge(i);
    std::optional<uint32_t> found = reader->lookup(e.importer, e.specifier);
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, e.target);
  }
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
cmake --build cmake-build-asan --target BundleFormatTest
```
Expected: compile error, `openForInspection` is not a member.

- [ ] **Step 3: Implement**

In `bundle_reader.h`, add the nested struct and the accessors, then declare
the second entry point next to `open` with this comment:

```cpp
  /// Opens without enforcing the generation tag, for inspection tools.
  /// Every structural check open() performs still applies: magic, format
  /// version, and every offset, length and index.
  ///
  /// Bytecode from a mismatched generation must never be executed, which is
  /// why this is a separate entry point rather than a parameter on open() --
  /// a caller cannot reach it without meaning to.
  static std::optional<BundleReader> openForInspection(
      const uint8_t *data,
      size_t size,
      std::string *error);
```

In `bundle_reader.cpp`, extract everything `open()` does into a private
helper taking a `bool enforceGeneration`, and have both entry points call
it. Do not copy the validation sequence into a second function: two copies
of a bounds-check chain is precisely how one of them ends up missing a
check.

Accessors are one-liners over `header_`, `modules_`, and `edges_`. Bounds:
`edge()` and `payloadOffset()` are only valid for an index below the
corresponding count, exactly like the existing `identity()`; state that in
the header rather than adding a runtime check the other accessors lack.

- [ ] **Step 4: Run to verify they pass**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
./utils/format.sh --check
git add include/hermes/node-compat/bundle/bundle_reader.h lib/bundle/bundle_reader.cpp unittests/BundleFormatTest.cpp
git commit -m "Add BundleReader inspection mode and header accessors"
```

---

### Task 2: `--verbose` for `--build-bundle`

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_build.h`, `lib/bundle/bundle_build.cpp`
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h`, `lib/runtime/hermes_node_runtime.cpp`
- Modify: `tools/hermes-node/hermes-node.cpp`
- Test: `test/bundle-verbose.js`

**Interfaces:**
- Consumes: nothing new.
- Produces: `int buildBundle(napi_env env, const std::string &entryPath, const std::string &outPath, bool verbose)` -- the existing three-argument signature gains a fourth parameter. `HermesNodeConfig` gains `bool verbose = false;`.

- [ ] **Step 1: Write the failing test**

Create `test/bundle-verbose.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A tree with a shared dependency (both cli.js and dep.js require util),
// so discovery must report the second reference as already known.
// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib
// RUN: echo "const d = require('./lib/dep'); const u = require('./lib/util'); console.log('V', d.v + u.v);" > %t.tree/cli.js
// RUN: echo "const u = require('./util'); module.exports = { v: u.v + 1 };" > %t.tree/lib/dep.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: printf 'not really an addon\n' > %t.tree/lib/native.node
// RUN: echo "require('./native.node'); module.exports = {};" >> %t.tree/lib/dep.js

// Verbose output goes to stderr and names discovery, provenance and totals.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --verbose %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=VERB %s
// VERB: entry:
// VERB: generation: 0x{{[0-9a-f]+}}
// VERB: discover {{.*}}cli.js
// VERB: require './lib/dep'
// VERB: discover {{.*}}lib/dep.js
// VERB: skip {{.*}}native.node
// VERB: known './util'
// VERB: compile {{.*}}src -> {{[0-9]+}} bc
// VERB: modules: 3
// VERB: edges: 3

// Without --verbose none of it appears: the default output is the warning
// and the root line, exactly as before.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js 2>&1 | %FileCheck --check-prefix=QUIET %s
// QUIET-NOT: discover
// QUIET-NOT: compile
// QUIET-NOT: modules:
// QUIET: warning: skipping
// QUIET: bundle root:

// The artifact must not depend on the diagnostics.
// RUN: cmp %t.tree/app.hbb %t.tree/plain.hbb
```

Note the ordering of the `QUIET-NOT` lines before the first positive match:
`CHECK-NOT` only applies between surrounding positive matches, so they must
precede `QUIET: warning:` to scan the whole output.

- [ ] **Step 2: Run to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/bundle-verbose.js --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node --param hermes=$(pwd)/cmake-build-asan/bin/hermes --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck --param not=$(pwd)/cmake-build-asan/bin/not --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test
```
Expected: FAIL, `unknown option '--verbose'`.

- [ ] **Step 3: Thread the flag through**

In `hermes-node.cpp`, parse `--verbose` into `config.verbose` next to the
existing `--build-bundle=` handling. In `hermes_node_runtime.cpp` step 13,
pass `config.verbose` to `buildBundle`.

- [ ] **Step 4: Report from the producer**

In `bundle_build.cpp`, add a small reporter rather than scattering
`if (verbose)` through the walk:

```cpp
namespace {
/// Verbose build reporting. Every method is a no-op when disabled, so call
/// sites stay unconditional and the quiet path stays exactly as it was.
class BuildReporter {
 public:
  explicit BuildReporter(bool enabled) : enabled_(enabled) {}

  void config(
      const std::string &entry,
      const std::string &out,
      uint32_t generation,
      bool optimized);
  void discovered(uint32_t index, const std::string &identity);
  void resolved(const std::string &specifier, const std::string &target);
  void known(const std::string &specifier, uint32_t index);
  void skipped(const std::string &specifier, const std::string &reason);
  void compiled(
      uint32_t index,
      const std::string &identity,
      size_t sourceBytes,
      size_t bytecodeBytes,
      double milliseconds);
  void summary(/* counts and sizes */);

 private:
  bool enabled_;
  double totalCompileMs_ = 0;
};
} // namespace
```

Timings use `std::chrono::steady_clock` around the compile call only.

The `skip` reason must be the same string the existing warning prints. Do
not build a second reason vocabulary -- have the warning path and the
reporter share one function that formats the reason.

- [ ] **Step 5: Run the test**

Expected: PASS, including the `cmp` proving the containers are identical.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle include/hermes/node-compat/runtime lib/runtime tools/hermes-node test/bundle-verbose.js
git commit -m "Add --verbose to the bundle producer"
```

---

### Task 3: `--bundle=<file> --dump`

**Files:**
- Create: `include/hermes/node-compat/bundle/bundle_tools.h`, `lib/bundle/bundle_tools.cpp`
- Modify: `lib/bundle/CMakeLists.txt` (new `hermesNodeBundleTools` target)
- Modify: `tools/hermes-node/hermes-node.cpp`
- Create: `unittests/BundleToolsTest.cpp`; modify `unittests/CMakeLists.txt`
- Test: `test/bundle-dump.js`

**Interfaces:**
- Consumes: `BundleReader::openForInspection` and the Task 1 accessors.
- Produces:
  - `int dumpBundle(const std::string &bundlePath, uint32_t runningGeneration, bool verbose, std::ostream &out, std::ostream &err)`
  - A file-mapping helper shared with Task 4, in the same header.

Streams are parameters, not hardcoded, so `BundleToolsTest` can assert on
the text without a subprocess.

- [ ] **Step 1: Write the failing unit test**

Create `unittests/BundleToolsTest.cpp` (copyright header first), building a
container with `BundleWriter`, writing it to a temp file, and asserting on
`dumpBundle`'s output stream: the header line, a module row, an edge row,
the section totals, and -- with a deliberately different
`runningGeneration` -- the `MISMATCH` line plus a zero return.

Use a thread-unique temp path, matching how `CompileCacheRunTest` does it.

- [ ] **Step 2: Write the failing lit test**

Create `test/bundle-dump.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib
// RUN: echo "const u = require('./lib/util'); const c = require('./cfg.json'); console.log('D', u.v + c.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 2 }' > %t.tree/cfg.json
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js

// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck %s
// CHECK: bundle: {{.*}}app.hbb   format v1  generation 0x{{[0-9a-f]+}}
// CHECK: entry:  [0] cli.js
// CHECK: MODULES (3)
// CHECK-DAG: js {{.*}} cli.js
// CHECK-DAG: js {{.*}} lib/util.js
// CHECK-DAG: json {{.*}} cfg.json
// CHECK: EDGES (2)
// CHECK-DAG: cli.js {{.*}}'./lib/util'{{.*}}->
// CHECK-DAG: cli.js {{.*}}'./cfg.json'{{.*}}->
// CHECK: SECTIONS
// CHECK: total {{[0-9]+}} bytes

// Dumping must not run the program.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=NORUN %s
// NORUN-NOT: D 3
// NORUN: MODULES

// A container from another generation still dumps, and says so.
// RUN: cp %t.tree/app.hbb %t.tree/old.hbb
// RUN: printf '\xff\xff\xff\xff' | dd of=%t.tree/old.hbb bs=1 seek=12 count=4 conv=notrunc 2>/dev/null
// RUN: %hermes-node --bundle=%t.tree/old.hbb --dump | %FileCheck --check-prefix=MISMATCH %s
// MISMATCH: MISMATCH (this binary requires 0x{{[0-9a-f]+}})
// MISMATCH: MODULES (3)

// But it still refuses to RUN.
// RUN: %not %hermes-node --bundle=%t.tree/old.hbb 2>&1 | %FileCheck --check-prefix=NORUNOLD %s
// NORUNOLD: generation mismatch
```

Offset 12 is `generationTag` (8-byte magic + 4-byte formatVersion). Confirm
against `BundleHeader` before relying on it.

- [ ] **Step 3: Implement `dumpBundle`**

Format exactly as the spec shows. Column widths from the widest value
actually present, not a fixed guess, so long identities do not wrap.

Print the edge table in stored order -- the sorted order the runtime binary
searches -- and say so in a comment. Regrouping would hide a sort bug, which
is the one bug this dump is uniquely able to reveal.

- [ ] **Step 4: Add the CMake target**

```cmake
# Tools layer: reads a container and reports on it. Links the format layer
# and nothing else -- dumping and extracting need no VM, so keeping this
# target VM-free is what lets BundleToolsTest run with no runtime, the same
# property BundleFormatTest relies on.
add_hermes_library(hermesNodeBundleTools STATIC
  bundle_tools.cpp
)
```

- [ ] **Step 5: Wire the flag, before the runtime**

In `main()`, after parsing and before `runHermesNode`, dispatch the
read-only verbs and return. A dump must not boot a JS runtime.

- [ ] **Step 6: Run both tests, then commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle unittests tools/hermes-node test/bundle-dump.js
git commit -m "Add --bundle --dump"
```

---

### Task 4: `--bundle=<file> --extract-module=<identity> --out=<path>`

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_tools.h`, `lib/bundle/bundle_tools.cpp`
- Modify: `tools/hermes-node/hermes-node.cpp`
- Modify: `unittests/BundleToolsTest.cpp`
- Test: `test/bundle-extract.js`

**Interfaces:**
- Consumes: Task 3's mapping helper and `openForInspection`.
- Produces: `int extractModule(const std::string &bundlePath, const std::string &identity, const std::string &outPath, std::ostream &err)`

- [ ] **Step 1: Write the failing lit test**

Create `test/bundle-extract.js`. The load-bearing assertion is that an
extracted JavaScript payload is a bytecode file Hermes accepts, and an
extracted JSON payload is byte-identical to the source file:

```js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=lib/util.js --out=%t.util.hbc
// RUN: %hermes-node --dump-bytecode=%t.util.hbc | %FileCheck --check-prefix=BC %s
// BC: Bytecode File Information

// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=cfg.json --out=%t.cfg.json
// RUN: cmp %t.cfg.json %t.tree/cfg.json

// An unknown identity errors and suggests.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --extract-module=lib/utl.js --out=%t.x 2>&1 | %FileCheck --check-prefix=TYPO %s
// TYPO: no module 'lib/utl.js'
// TYPO: did you mean
// TYPO: lib/util.js

// Extraction must not have written anything on failure.
// RUN: test ! -e %t.x
```

`--dump-bytecode` arrives in Task 5. Until then this file's `BC` block will
fail; write the rest first and enable that block in Task 5. Say so in a
comment in the test rather than leaving a silently disabled check.

- [ ] **Step 2: Implement**

Payload bytes are written verbatim -- no header, no transformation, so a
JavaScript extraction is directly loadable and a JSON extraction round-trips.

Unknown identity: list up to three closest identities by Levenshtein
distance, only those within a distance of a third of the identity's length,
so a wild typo does not produce three irrelevant suggestions.

Write to a temp file in the destination directory and rename, so a failure
part-way leaves no partial output. This mirrors what the producer already
does when writing a container.

- [ ] **Step 3: Add unit coverage**

Extend `BundleToolsTest`: extraction of each kind round-trips the exact
payload bytes; an unknown identity returns non-zero, writes nothing, and its
message names the identity.

- [ ] **Step 4: Run, then commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle unittests tools/hermes-node test/bundle-extract.js
git commit -m "Add --bundle --extract-module"
```

---

### Task 5: `--dump-bytecode=<file>`

**Files:**
- Create: `include/hermes/node-compat/bytecode-dump/bytecode_dump.h`, `lib/bytecode-dump/bytecode_dump.cpp`, `lib/bytecode-dump/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt` (add the subdirectory), `tools/hermes-node/CMakeLists.txt`
- Modify: `tools/hermes-node/hermes-node.cpp`
- Test: `test/bundle-dump-bytecode.js`; enable Task 4's `BC` block

**Interfaces:**
- Consumes: `hermes::hbc::BCProviderFromBuffer`, `hermes::hbc::BytecodeDisassembler`, `hermes::hbc::DisassemblyOptions`.
- Produces: `int dumpBytecodeFile(const std::string &path, bool verbose, std::ostream &out, std::ostream &err)`

- [ ] **Step 1: Write the failing test**

Create `test/bundle-dump-bytecode.js`: compile a fixture to a bundle,
extract a module, disassemble it, and assert on a global-function line and a
section-range line. Then the compile-cache case, and a non-bytecode file
erroring cleanly with a non-zero exit and no crash.

Find the exact strings by running Hermes's own disassembler first rather
than guessing:

```bash
cmake-build-asan/bin/hbcdump -mode=objdump <file>
```

- [ ] **Step 2: Implement**

```cpp
#include "hermes/BCGen/HBC/BCProviderFromBuffer.h"
#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/BCGen/HBC/DisassemblyOptions.h"
#include "hermes/Support/MemoryBuffer.h"
#include "llvh/Support/MemoryBuffer.h"
```

Read the file with `llvh::MemoryBuffer::getFile`, which returns an aligned
buffer. Detect a compile-cache entry by its magic; if present, copy the
bytecode past `kCompileCacheHeaderSize` into an aligned buffer, because a
buffer pointing part-way into a mapping is not aligned and
`BCProviderFromBuffer` requires alignment.

Options: `Pretty | IncludeFunctionIds | IncludeVirtualOffsets`, plus
`IncludeSource` under `--verbose`.

`createBCProviderFromBuffer` returns a pair of provider and error string;
report the error verbatim rather than inventing a message for a case Hermes
already describes.

- [ ] **Step 3: Add the CMake target**

```cmake
# Disassembly layer: the only part of the tooling that needs the Hermes VM
# libraries, which is why it is separate from hermesNodeBundleTools. It does
# not depend on the bundle format at all -- it reads a bytecode file, and
# that one can be extracted from a container is not its concern.
add_hermes_library(hermesNodeBytecodeDump STATIC
  bytecode_dump.cpp
)
target_link_libraries(hermesNodeBytecodeDump PRIVATE hermesvm_a)
```

- [ ] **Step 4: Enable Task 4's deferred block**

Remove the comment in `test/bundle-extract.js` and confirm the `BC` block
now passes.

- [ ] **Step 5: Run, then commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bytecode-dump lib/bytecode-dump CMakeLists.txt tools/hermes-node test/bundle-dump-bytecode.js test/bundle-extract.js
git commit -m "Add --dump-bytecode"
```

---

### Task 6: Flag conflicts, docs, progress file

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp` (the conflict matrix, `printUsage`)
- Modify: `README.md`, `CLAUDE.md`
- Create: `history/plans/progress-bundle-tooling.md`
- Test: `test/bundle-tool-errors.js`

- [ ] **Step 1: Write the failing test**

Create `test/bundle-tool-errors.js` with one case per row of the spec's
flag-surface table. Every case asserts the specific diagnostic, never a bare
`error:` -- a check that matches any message passes when the binary fails
for an unrelated reason:

```js
// RUN: %not %hermes-node --dump 2>&1 | %FileCheck --check-prefix=NOBUNDLE %s
// NOBUNDLE: --dump requires --bundle

// RUN: %not %hermes-node --bundle=%t.hbb --dump --extract-module=x 2>&1 | %FileCheck --check-prefix=TWOVERBS %s
// TWOVERBS: --dump cannot be combined with --extract-module
...
```

- [ ] **Step 2: Implement the matrix**

All checks go together, after the parse loop, so flag order never matters --
the same shape as the existing `--bundle` + `--inspect` refusal. Each
message names both flags involved.

- [ ] **Step 3: Update `printUsage`**

```
  --verbose                      Report progress while building a bundle
  --dump                         With --bundle: print the container's contents
  --extract-module=<identity>    With --bundle: write one module to --out
  --out=<file>                   Destination for --extract-module
  --dump-bytecode=<file>         Disassemble a Hermes bytecode file
```

- [ ] **Step 4: Document**

`README.md`: a short subsection under the bundle section. Show the four
commands. State that `--dump` and `--extract-module` work on a container the
running binary would refuse to execute, since that is the non-obvious part.

`CLAUDE.md`: extend the AOT Bundles section with the flags, the two new
library targets and why they are split, and the fact that the verbs run
before the runtime.

Do not overclaim: hermes-node is early and experimental. Concrete,
checkable statements only.

- [ ] **Step 5: Write the progress file**

`history/plans/progress-bundle-tooling.md`, naming this plan, with a table
of the six tasks and their status, matching the format of
`history/plans/progress-aot-bundle.md`.

- [ ] **Step 6: Full suite, format check, commit**

```bash
./utils/format.sh --check
cmake --build cmake-build-asan --target check-hermes-node
git add tools/hermes-node README.md CLAUDE.md history/plans/progress-bundle-tooling.md test/bundle-tool-errors.js
git commit -m "Document bundle tooling and reject conflicting flag combinations"
```

---

## Self-review notes

**Spec coverage.** Flag surface -> Tasks 2-6. Inspection mode -> Task 1.
`--verbose` -> Task 2. Dump -> Task 3. Extract -> Task 4. Disassembly ->
Task 5. Every row of the flag-conflict table -> Task 6. Library structure ->
Tasks 3 and 5. Testing section -> one task each.

**Ordering constraint.** Task 4's test references `--dump-bytecode`, which
Task 5 builds. The plan defers that one block explicitly rather than leaving
a check that silently asserts nothing.

**Not yet verified.** The exact disassembler output strings in Task 5, which
is why its first step is to run `hbcdump` and read them rather than guess.
The `generationTag` byte offset of 12 used by Task 3's corruption test,
which the task says to confirm against `BundleHeader` first.
