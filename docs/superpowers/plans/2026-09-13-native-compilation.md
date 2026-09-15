# Native Compilation with shermes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `hermes-node build-native app.js -o app` compiles a program's whole
`require()` graph to native code with Static Hermes and links a standalone
executable.

**Architecture:** A native build is an AOT bundle whose `kJavaScript` payloads
are empty. Discovery, resolution, the container, the closed world, addon
sidecars, preloads and the `--build-exe` kit are all reused unchanged; only
the payload step differs. Each module becomes one Static Hermes compilation
unit (`shermes -exported-unit=`), compiled through generated C to an object
file, linked in, and reached at run time through a table indexed by container
module index. Units initialize lazily, on first `require()`.

**Tech Stack:** C++17, CMake + Ninja, GTest (`unittests/`), LLVM lit
(`test/`), Python 3 (`utils/make-kit.py`), Static Hermes (`hermes/`
submodule).

**Spec:** `docs/superpowers/specs/2026-09-13-native-compilation-design.md`.
Read it before starting. This plan implements it; where the two disagree, the
spec is right and the plan has a bug.

## Global Constraints

- **Build directory:** `cmake-build-asan` is the primary development
  configuration. Always Clang, never GCC.
- **Running the JS test suite:** `check-hermes-node` crashes from Python
  multiprocessing in this sandbox. Run lit directly, with `-j1` and absolute
  paths:
  ```bash
  python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/<name>.js \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test \
    --param hello_addon=$(pwd)/cmake-build-asan/hello_addon.node \
    --param kit_dir=$(pwd)/cmake-build-asan/kit
  ```
  All eight params, matching `HERMES_NODE_LIT_PARAMS` in the root
  `CMakeLists.txt`. Omitting `hello_addon` or `kit_dir` does not skip the
  tests that need them -- it FAILS them, which reads as a regression from
  whatever you just changed. (Task 4 lost time to exactly that.) On Linux
  the addon is built to the same place. It is a Node-API `.node`, not a
  platform shared-library name -- confirm with
  `ls cmake-build-asan/hello_addon.node`, or read the path out of
  `test/build-exe-natives.js`, which uses it.
- **Re-cut the kit after any `hermes/` submodule change**, before running
  any kit-gated test: `cmake --build cmake-build-asan --target
  hermes-node-kit`. The kit pins a version string, so a stale one fails
  every `--build-exe` and `build-native` test at once.
- **Running one GTest binary:**
  `cmake-build-asan/unittests/<Name>Test --gtest_filter=...`
- **Before any commit:** `./utils/format.sh -f` (clang-format 18
  specifically; the script refuses another major version).
- **Commit messages:** ASCII only, no emojis, hard-wrapped to 72 columns,
  subject line <= 50 characters. Say only what the diff cannot: measured
  costs and decisions that look wrong without explanation. Delete anything a
  reviewer could derive from the diff itself.
- **Two known-flaky tests** (`test-inspect.js`, `test-repl-history.js`) fail
  intermittently under parallel load. A single red run naming one of them is
  not a regression.
- **hermes-node must not link `hermesCompilerDriver`** -- it defines
  `hermes::cl::compilerRuntimeFlags`, whose duplicate `llvh::cl` registration
  aborts at static-initialisation time.
- **The kit is `EXCLUDE_FROM_ALL`.** Re-cut it after touching runtime code:
  `cmake --build cmake-build-asan --target hermes-node-kit`.
- **Unit name format:** `hn_m` + container module index, zero-padded to six
  digits (`hn_m000017`). `isValidSHUnitName` permits alphanumerics and
  underscore only.
- **Optimization mapping**, one knob driving both tools:

  | flag | shermes | cc |
  | --- | --- | --- |
  | `-O0` | `-O0` | `-O0` |
  | `-O1` | `-Og` | `-O1` |
  | `-O2` | `-O` | `-O2` |
  | `-O3` (default) | `-O` | `-O3` |
  | `-Os` | `-Os` | `-Os` |

- **The compile-flag parity set**, which Task 4 makes a single struct and
  every later task must use rather than re-spelling:

  | meaning | bytecode | shermes |
  | --- | --- | --- |
  | ES6 block scoping | `enableES6BlockScoping = true` | `-Xes6-block-scoping` |
  | async generators | `enableAsyncGenerators = true` | `-Xasync-generators` |
  | generators | `enableGenerator = true` | default on, no option |
  | TypeScript | `enableTS` per `.ts` extension | `-transform-ts` |
  | source maps | `sourceMap = ""` | `-sm-comment=off` |

## File Structure

**New:**

| path | responsibility |
| --- | --- |
| `include/hermes/node-compat/build-native/build_native.h` | The pure half: `OptLevel`, staging paths, the argv builders, the job pool, the unit-symbol table, the link response file. No orchestration -- see `bundle_build_native.cpp`. |
| `lib/build-native/CMakeLists.txt` | `hermesNodeBuildNative`, VM-free, links `hermesNodeBundle` + `hermesNodeBuildExe`. |
| `lib/build-native/staging.cpp` | Writes CommonJS-wrapped sources to flat temp files; derives temp paths and unit names. |
| `lib/build-native/native_compile.cpp` | Builds the `shermes` and `cc` argv, and the linker response file. |
| `lib/build-native/job_pool.cpp` | Bounded worker pool over module indices. |
| `lib/build-native/unit_table.cpp` | Emits the unit-table assembly for both object formats. |
| `unittests/BuildNativeTest.cpp` | GTest for everything above. No runtime, no toolchain. |
| `test/build-native.js` | End-to-end, gated. |
| `test/build-native-parity.js` | Compile-flag parity forcing function, gated. |
| `test/build-native-errors.js` | Refusals, **ungated**. |
| `test/build-native-escapes.js` | Closed world, gated. |
| `test/build-native-natives.js` | Addon sidecars, gated. |
| `test/build-native-wasm.js` | Wasm under a native executable, gated. |
| `test/bundle-async-generator.js` | Regression for the scanner bug (Task 4), ungated. |
| `hermes/API/napi/hermes_napi_sh_unit.cpp` | `hermes_init_sh_unit`. |
| `hermes/unittests/VMRuntime/StaticHUnitTest.cpp` | Growable units array. |
| `hermes/test/shermes/source-name.js` | `-source-name=` lit test. |

**Modified:**

| path | change |
| --- | --- |
| `hermes/include/hermes/VM/sh_runtime.h` | `SHUnit *units[8]` -> `SHUnit **units; uint32_t units_size;` |
| `hermes/lib/VM/StaticHUnit.cpp` | `shUnitEnsureCapacity()`; unconditional capacity check. |
| `hermes/lib/VM/Runtime.cpp` | Five uses of `units`; `mallocSize()`; free in the destructor. |
| `hermes/include/hermes/VM/StaticHUtils.h` | Declare `shUnitEnsureCapacity()`. |
| `hermes/API/napi/hermes_napi.h` | `SHUnitCreator`, `hermes_init_sh_unit`. |
| `hermes/tools/shermes/shermes.cpp` | `-source-name=`. |
| `lib/bundle/require_scanner.cpp` | Configure the scanner `Context` from the shared flag set. |
| `include/hermes/node-compat/bundle/cjs_wrapper.h` | Add `JSCompileFlags`, the one copy of the parity set. |
| `include/hermes/node-compat/bundle/bundle_format.h` | `kBundleFlagNativeUnits`. |
| `lib/bundle/bundle_writer.cpp`, `bundle_reader.cpp` | Write and expose the bit. |
| `lib/bundle/bundle_run.cpp` | `openBundle()` refusal; `openEmbeddedBundle()` unit table; native branch in `bundleLoadCallback`. |
| `include/hermes/node-compat/bundle/bundle_run.h` | `openEmbeddedBundle()` signature. |
| `include/hermes/node-compat/bundle/bundle_build.h` | `NativeBuildOptions`, `buildNativeExecutable()`. |
| `lib/bundle/bundle_build_internal.h` (new, private) | `PayloadMode`, `PendingNativeModule`, `BuildProducts`, `buildBundleImpl()`. |
| `lib/bundle/bundle_build_native.cpp` (new) | The orchestration: kit, toolchain, temp tree, compile, link, sidecars. |
| `lib/bundle/bundle_build.cpp` | Becomes `buildBundleImpl` with a payload mode; `buildBundle` forwards. |
| `include/hermes/node-compat/build-exe/build_exe.h`, `lib/build-exe/build_exe.cpp` | `runCommand()` capture; `buildCompileCommand()`; unit table in `payloadAssembly()`. |
| `include/hermes/node-compat/build-exe/kit_manifest.h`, `lib/build-exe/kit_manifest.cpp` | `ccflag:`. |
| `include/hermes/node-compat/runtime/hermes_node_runtime.h`, `lib/runtime/hermes_node_runtime.cpp` | Native unit table on `HermesNodeConfig`. |
| `tools/hermes-node/hermes-node.cpp` | `build-native` subcommand. |
| `tools/hermes-node/bundle_main.cpp` | Pass the unit table. |
| `utils/make-kit.py` | Copy `shermes` and headers; write `ccflag:`. |
| `tools/hermes-node/CMakeLists.txt` | Hand `make-kit.py` the shermes path and the header roots. |
| `test/lit.cfg` | `shermes-available` feature. |
| `CLAUDE.md` | A "Native Compilation" section. |

---

## Task 1: Hermes -- growable SHUnit array

**Files:**
- Modify: `hermes/include/hermes/VM/sh_runtime.h:76-77`
- Modify: `hermes/include/hermes/VM/StaticHUtils.h`
- Modify: `hermes/lib/VM/StaticHUnit.cpp`
- Modify: `hermes/lib/VM/Runtime.cpp:253, 553, 665, 810, 1062`
- Modify: `hermes/unittests/VMRuntime/CMakeLists.txt`
- Test: `hermes/unittests/VMRuntime/StaticHUnitTest.cpp` (create)

**Interfaces:**
- Consumes: nothing.
- Produces: `bool hermes::vm::shUnitEnsureCapacity(Runtime &runtime, uint32_t index);`
  in `StaticHUtils.h`. Returns false only on overflow or allocation failure.
  Also `Runtime::units` becomes `SHUnit **` and `Runtime::units_size` becomes
  a `uint32_t`, both inherited from `SHRuntime`.

**What the unit test can and cannot reach.** The four load-bearing details
live in `shUnitEnsureCapacity`, so that is what the GTest exercises
directly. But the second-runtime bug is a bug about WHERE `_sh_unit_init`
calls it: leave the call inside `if (!*unit->index)` and every
`shUnitEnsureCapacity` test still passes. Covering that needs a real
`_sh_unit_init` on two runtimes, which needs a real `SHUnit`.

Step 1b is therefore **required**, and it is the only thing that covers the
placement. Task 13's twelve-module fixture does not: it uses one runtime, so
every index it sees is being assigned for the first time, which is the case
that works either way. Do not skip Step 1b on the grounds that the helper is
tested -- the helper being correct is not the bug.

- [ ] **Step 1: Write the failing test**

Create `hermes/unittests/VMRuntime/StaticHUnitTest.cpp`:

```cpp
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/StaticHUtils.h"

#include "VMRuntimeTestHelpers.h"
#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

TEST(StaticHUnitTest, StartsEmpty) {
  auto rt = Runtime::create(kTestRTConfig);
  EXPECT_EQ(nullptr, rt->units);
  EXPECT_EQ(0u, rt->units_size);
}

TEST(StaticHUnitTest, GrowsAndNullInitializes) {
  auto rt = Runtime::create(kTestRTConfig);
  ASSERT_TRUE(shUnitEnsureCapacity(*rt, 900));
  EXPECT_GT(rt->units_size, 900u);
  // Every new slot must read as "not registered in this runtime"; realloc
  // does not do this and the lookup depends on it.
  for (uint32_t i = 0; i <= 900; ++i)
    EXPECT_EQ(nullptr, rt->units[i]) << "slot " << i;
}

TEST(StaticHUnitTest, PreservesExistingEntriesAcrossGrowth) {
  auto rt = Runtime::create(kTestRTConfig);
  ASSERT_TRUE(shUnitEnsureCapacity(*rt, 1));
  SHUnit *marker = reinterpret_cast<SHUnit *>(0x1234);
  rt->units[1] = marker;
  ASSERT_TRUE(shUnitEnsureCapacity(*rt, 5000));
  EXPECT_EQ(marker, rt->units[1]);
  EXPECT_EQ(nullptr, rt->units[5000]);
  rt->units[1] = nullptr; // not a real unit; do not let teardown see it
}

TEST(StaticHUnitTest, AlreadyLargeEnoughIsANoOp) {
  auto rt = Runtime::create(kTestRTConfig);
  ASSERT_TRUE(shUnitEnsureCapacity(*rt, 100));
  uint32_t size = rt->units_size;
  SHUnit **buf = rt->units;
  ASSERT_TRUE(shUnitEnsureCapacity(*rt, 100));
  EXPECT_EQ(size, rt->units_size);
  EXPECT_EQ(buf, rt->units);
}

/// The second-runtime hazard. Unit indices are process-wide (a
/// function-local static in _sh_unit_init), the arrays are per-runtime, so a
/// unit assigned a high index by one runtime and then initialized in another
/// reads past the end of the second runtime's array unless capacity is
/// ensured unconditionally. Registering the HIGH index first in B is the
/// point: ascending registration passes even with the check in the wrong
/// place.
TEST(StaticHUnitTest, CapacityIsPerRuntimeAndHighIndexFirstWorks) {
  auto a = Runtime::create(kTestRTConfig);
  auto b = Runtime::create(kTestRTConfig);
  ASSERT_TRUE(shUnitEnsureCapacity(*a, 900));
  EXPECT_EQ(0u, b->units_size);
  ASSERT_TRUE(shUnitEnsureCapacity(*b, 900));
  EXPECT_GT(b->units_size, 900u);
  EXPECT_EQ(nullptr, b->units[900]);
}

TEST(StaticHUnitTest, MallocSizeCountsTheBackingStore) {
  auto rt = Runtime::create(kTestRTConfig);
  size_t before = rt->mallocSize();
  ASSERT_TRUE(shUnitEnsureCapacity(*rt, 4095));
  size_t after = rt->mallocSize();
  EXPECT_GE(after - before, 4096u * sizeof(SHUnit *));
}

} // namespace
```

Add `StaticHUnitTest.cpp` to the source list in
`hermes/unittests/VMRuntime/CMakeLists.txt`, in alphabetical position (after
`StackTracesTreeTest.cpp` if present, otherwise wherever the `S` entries sit).

- [ ] **Step 1b: Write the placement test**

Also in `StaticHUnitTest.cpp`. This is the one that fails if the capacity
call stays inside the index-assignment branch:

```cpp
/// A minimal unit. unit_main mirrors what SH.cpp emits for an empty global:
/// enter, leave, return undefined. Doing less corrupts the frame that
/// sh_unit_run set up.
static uint32_t g_testUnitIndex = 0;

static SHLegacyValue testUnitMain(SHRuntime *shr) {
  struct {
    SHLocals head;
  } locals;
  SHLegacyValue *frame = _sh_enter(shr, &locals.head, 1);
  locals.head.count = 0;
  _sh_leave(shr, &locals.head, frame);
  return _sh_ljs_undefined();
}

/// Model this initializer on the one SH.cpp emits (search SH.cpp for
/// "CREATE_THIS_UNIT"); take SHNativeFuncInfo's field values from
/// static_h.h rather than guessing them. Everything not needed by a unit
/// that defines no strings and no properties stays zero.
static SHUnit *createTestUnit() { /* ... */ }

/// The second-runtime hazard, through the real entry point. Runtime A
/// assigns the index; runtime B then sees an already-assigned index and
/// must still grow its own array before the lookup. With the capacity call
/// left inside `if (!*unit->index)`, this reads out of bounds.
TEST(StaticHUnitTest, InitInASecondRuntimeGrowsThatRuntimesArray) {
  auto a = Runtime::create(kTestRTConfig);
  SHLegacyValue v;
  ASSERT_TRUE(_sh_unit_init_guarded(
      getSHRuntime(*a), createTestUnit, &v));
  ASSERT_NE(0u, g_testUnitIndex);

  auto b = Runtime::create(kTestRTConfig);
  EXPECT_EQ(0u, b->units_size);
  ASSERT_TRUE(_sh_unit_init_guarded(
      getSHRuntime(*b), createTestUnit, &v));
  EXPECT_GT(b->units_size, g_testUnitIndex);
  EXPECT_NE(nullptr, b->units[g_testUnitIndex]);
}
```

- [ ] **Step 1c: Write the throwing-unit test**

The failure half of `_sh_unit_init_guarded`, and the only automated
coverage `hermes_init_sh_unit`'s error path gets (Task 3 explains why
nothing higher up can reach it). Same file:

```cpp
static uint32_t g_throwingUnitIndex = 0;

/// A unit whose top-level code throws, which a CommonJS-wrapped module
/// cannot do -- its global only creates a closure.
static SHLegacyValue throwingUnitMain(SHRuntime *shr) {
  struct {
    SHLocals head;
  } locals;
  SHLegacyValue *frame = _sh_enter(shr, &locals.head, 1);
  locals.head.count = 0;
  _sh_throw(shr, _sh_ljs_double(42));
  // _sh_throw does not return; the leave is here for shape only.
  _sh_leave(shr, &locals.head, frame);
  return _sh_ljs_undefined();
}

static SHUnit *createThrowingUnit() { /* as createTestUnit, other index */ }

TEST(StaticHUnitTest, GuardedInitReportsAThrowingUnit) {
  auto rt = Runtime::create(kTestRTConfig);
  SHLegacyValue resOrExc = _sh_ljs_undefined();
  EXPECT_FALSE(_sh_unit_init_guarded(
      getSHRuntime(*rt), createThrowingUnit, &resOrExc));
  // The thrown value comes back in resOrExc, already extracted and cleared
  // from the runtime by _sh_catch -- which is why the NAPI wrapper must
  // NOT ask the runtime for it again.
  EXPECT_EQ(42, _sh_ljs_get_double(resOrExc));
}
```

Check the real spellings of `_sh_throw`, `_sh_ljs_double` and
`_sh_ljs_get_double` in `static_h.h`; the shape is what matters here.

If building a usable `SHUnit` by hand fights back, the fallback is NOT to
drop the test -- nothing else reaches this code. Get a real one instead:
`shermes -exported-unit=` any trivial file, link the resulting object into
the test binary, and declare `extern "C" SHUnit *sh_export_<name>(void);`.
That is more build plumbing and less guesswork than transcribing the
initializer.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target HermesVMRuntimeTests 2>&1 | tail -20
```

Expected: FAIL to compile, `shUnitEnsureCapacity` undeclared and
`rt->units_size` not a member.

- [ ] **Step 3: Change the struct**

In `hermes/include/hermes/VM/sh_runtime.h`, replace:

```c
  /// The active SHUnits in this runtime.
  SHUnit *units[8];
```

with:

```c
  /// The active SHUnits in this runtime, indexed by the process-wide unit
  /// index. Generated code reads shr->units[unit_index] in the prologue of
  /// every function that touches its unit, which is why this is an array
  /// reached by one load rather than a std::vector.
  ///
  /// Heap-allocated and grown on demand by _sh_unit_init. It was a fixed
  /// SHUnit *units[8] until a program needed one unit per JavaScript module.
  /// Growth is safe while units run: a frame holds the SHUnit * itself, not
  /// a pointer into this array, and the SHUnit objects do not move.
  SHUnit **units;
  /// Number of slots in `units`. Per runtime, while unit indices are
  /// per process -- see shUnitEnsureCapacity().
  uint32_t units_size;
```

- [ ] **Step 4: Declare and implement the capacity function**

In `hermes/include/hermes/VM/StaticHUtils.h`, after `sh_unit_done`:

```cpp
/// Ensure \p runtime's unit array can be indexed by \p index, growing it and
/// null-initializing the new slots.
///
/// Called unconditionally before every lookup and store, NOT only when an
/// index is first assigned. Unit indices are process-wide and the arrays are
/// not, so a unit that was assigned index 900 by one runtime and is then
/// initialized in another would otherwise index past the second runtime's
/// array.
///
/// \return false on integer overflow or allocation failure, with the
/// existing array untouched.
bool shUnitEnsureCapacity(Runtime &runtime, uint32_t index);
```

In `hermes/lib/VM/StaticHUnit.cpp`, above `_sh_unit_init`:

```cpp
bool hermes::vm::shUnitEnsureCapacity(Runtime &runtime, uint32_t index) {
  if (index < runtime.units_size)
    return true;

  uint32_t newSize = runtime.units_size ? runtime.units_size : 8;
  while (newSize <= index) {
    if (newSize > std::numeric_limits<uint32_t>::max() / 2)
      return false;
    newSize *= 2;
  }
  if (newSize > std::numeric_limits<size_t>::max() / sizeof(SHUnit *))
    return false;

  // Into a temporary, committed only on success: realloc returning null
  // leaves the old block valid, and overwriting the member with it would
  // lose every registered unit on the way to reporting failure.
  SHUnit **grown = static_cast<SHUnit **>(
      realloc(runtime.units, (size_t)newSize * sizeof(SHUnit *)));
  if (!grown)
    return false;

  // The lookup reads a null slot as "not registered in this runtime". The
  // fixed array got that from a std::fill at construction; realloc gives
  // uninitialized memory.
  std::fill(grown + runtime.units_size, grown + newSize, nullptr);
  runtime.units = grown;
  runtime.units_size = newSize;
  return true;
}
```

Add `#include <limits>` and `#include <algorithm>` to that file if absent.

- [ ] **Step 5: Update the six other uses**

`hermes/lib/VM/Runtime.cpp` line ~253, in the constructor, replace the
`std::fill(std::begin(units), std::end(units), nullptr);` with:

```cpp
  units = nullptr;
  units_size = 0;
```

Each of the four `for (... unit : units)` loops becomes an index loop. By
function rather than by line, since lines move -- and note which one is the
destructor, because that is where the `free()` goes:

| function | approx line | what it does |
| --- | --- | --- |
| `Runtime::~Runtime` | 553 | `sh_unit_done` per unit |
| `Runtime::markRoots` | 665 | `sh_unit_mark_roots` per unit |
| `Runtime::markWeakRoots` | 810 | `sh_unit_mark_weak_roots` per unit |
| `Runtime::mallocSize` | 1062 | `sh_unit_additional_memory_size` per unit |

```cpp
  for (uint32_t i = 0; i < units_size; ++i)
    if (SHUnit *unit = units[i])
      /* ...existing body, unchanged... */;
```

In `mallocSize()`, after the existing per-unit sum, add the backing store:

```cpp
  // The array itself, not just what the units point to.
  shSize += (size_t)units_size * sizeof(SHUnit *);
```

In `Runtime::~Runtime`, after the `sh_unit_done` loop:

```cpp
  free(units);
  units = nullptr;
  units_size = 0;
```

- [ ] **Step 6: Make `_sh_unit_init` ensure capacity unconditionally**

In `hermes/lib/VM/StaticHUnit.cpp`, the index-assignment block loses its
capacity check and gains one after it:

```cpp
  {
    // Counter to assign a globally unique index to each unit.
    static uint32_t nextIndex = 1;
    static std::mutex idxMtx;
    std::lock_guard<std::mutex> lock(idxMtx);
    if (!*unit->index)
      *unit->index = nextIndex++;
  }

  // Unconditional, and outside the block above: the index may have been
  // assigned by a different runtime, whose array size says nothing about
  // this one's.
  if (!shUnitEnsureCapacity(runtime, *unit->index)) {
    fprintf(stderr, "Cannot grow the SH unit table\n");
    abort();
  }
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target HermesVMRuntimeTests 2>&1 | tail -5
cmake-build-asan/hermes/unittests/VMRuntime/HermesVMRuntimeTests \
  --gtest_filter='StaticHUnitTest.*'
```

Expected: PASS, 6 tests.

- [ ] **Step 8: Verify shermes still works end to end**

```bash
cmake --build cmake-build-asan --target shermes 2>&1 | tail -3
echo 'print("ok")' > /tmp/u.js
cmake-build-asan/bin/shermes -exec /tmp/u.js
```

Expected: `ok`. This is the smoke test that the generated prologue's
`shr->units[unit_index]` still reads the right thing through the new
indirection.

- [ ] **Step 9: Commit**

```bash
./utils/format.sh -f
git -C hermes add -A && git -C hermes commit -F - <<'MSG'
Make the SHUnit array growable

A program with one unit per JavaScript module needs thousands; the
array held eight. D58649567 justified the array -- generated code
reads shr->units[unit_index] as a single load, which the std::vector
it replaced could not give -- but never the number.

The capacity check moves out of the index-assignment branch and
becomes unconditional. Indices are process-wide and the arrays are
per runtime, so a unit assigned a high index by one runtime and then
initialized in another indexes past the second runtime's array. The
fixed size hid this because every array was the same size.
MSG
```

---

## Task 2: Hermes -- `shermes -source-name=<name>`

**Files:**
- Modify: `hermes/tools/shermes/shermes.cpp` (flag near `OutputFilename` ~line 91; use near `memoryBufferFromFile` ~line 959)
- Test: `hermes/test/shermes/source-name.js` (create)

**Interfaces:**
- Consumes: nothing.
- Produces: the `shermes` CLI flag `-source-name=<name>`, which replaces the
  source buffer's identifier. Rejected with more than one input file. Task 9
  passes it.

**Why:** the buffer identifier is what `SourceErrorManager::getSourceUrl()`
returns and therefore what lands in the unit's source-location table and in
stack traces. Without this flag the only lever is the path on the command
line, which forces a staging tree mirroring each identity and a
working-directory change per child -- and `posix_spawn_file_actions_addchdir_np`
needs glibc 2.29 while the Linux release image has 2.28, so that would mean
`fork()`/`chdir()`/`exec()` inside a worker pool, where only
async-signal-safe calls are allowed.

- [ ] **Step 1: Write the failing test**

Create `hermes/test/shermes/source-name.js`:

```js
// RUN: %shermes -source-name=my/module.js -exec %s | %FileCheck --match-full-lines %s
// RUN: not %shermes -source-name=x.js -emit-c -o %t.c %s %s 2>&1 | %FileCheck --check-prefix=MULTI %s

function inner() {
  throw new Error('boom');
}
try {
  inner();
} catch (e) {
  print(e.stack.split('\n')[1].trim());
}
// CHECK: at inner (my/module.js:{{[0-9]+}}:{{[0-9]+}})

// MULTI: -source-name can only be used with a single input file
```

Check the lit substitution for the binary: other files in
`hermes/test/shermes/` show whether it is `%shermes` or `%shermesc`. Match
them.

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 -v \
  $(pwd)/hermes/test/shermes/source-name.js
```

Expected: FAIL, `Unknown command line argument '-source-name=...'`.

- [ ] **Step 3: Add the flag**

In `hermes/tools/shermes/shermes.cpp`, beside `OutputFilename`:

```cpp
static cl::opt<std::string> SourceName(
    "source-name",
    cl::desc("Override the name recorded for the input file in source "
             "locations and stack traces. Single input file only."),
    cl::cat(CompilerCategory));
```

- [ ] **Step 4: Apply it, and reject multiple inputs**

In the input-reading loop (~line 959), replace:

```cpp
  for (llvh::StringRef filename : cli::InputFilenames) {
    std::unique_ptr<llvh::MemoryBuffer> fileBuf =
        memoryBufferFromFile(filename, "input file", true);
    if (!fileBuf)
      return false;
    fileBufs.push_back(std::move(fileBuf));
  }
```

with:

```cpp
  if (!cli::SourceName.empty() && cli::InputFilenames.size() != 1) {
    llvh::errs()
        << "Error: -source-name can only be used with a single input file.\n";
    return false;
  }

  for (llvh::StringRef filename : cli::InputFilenames) {
    std::unique_ptr<llvh::MemoryBuffer> fileBuf =
        memoryBufferFromFile(filename, "input file", true);
    if (!fileBuf)
      return false;
    if (!cli::SourceName.empty()) {
      // A copy rather than a re-wrap: getMemBuffer() would alias the bytes
      // of a buffer about to be destroyed at the end of this iteration.
      fileBuf = llvh::MemoryBuffer::getMemBufferCopy(
          fileBuf->getBuffer(), cli::SourceName);
    }
    fileBufs.push_back(std::move(fileBuf));
  }
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target shermes 2>&1 | tail -3
python3 cmake-build-asan/bin/hermes-lit -j1 -v \
  $(pwd)/hermes/test/shermes/source-name.js
```

Expected: PASS, both RUN lines.

- [ ] **Step 6: Assert that an explicit sourceURL still wins**

The design requires it, so it is a test, not an observation. Add a third RUN
line to `hermes/test/shermes/source-name.js`:

```js
// RUN: echo '//# sourceURL=real.js' > %t.url.js
// RUN: echo 'function f() { throw new Error("x"); }' >> %t.url.js
// RUN: echo 'try { f(); } catch (e) { print(e.stack.split("\n")[1].trim()); }' >> %t.url.js
// RUN: %shermes -source-name=staged.js -exec %t.url.js | %FileCheck --check-prefix=URL %s
// URL: at f (real.js:{{[0-9]+}}:{{[0-9]+}})
```

`JSParser` installs the magic-comment URL during parsing
(`hermes/lib/Parser/JSParser.cpp:52`), so replacing the buffer identifier
before the parse is what lets the comment win. If it does not, the
implementation set the URL rather than the identifier -- fix the
implementation, do not relax the test.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git -C hermes add -A && git -C hermes commit -F - <<'MSG'
Add shermes -source-name to rename a source

An embedder that stages generated or rewritten sources in a temp
directory wants stack traces to name the original module, not the
temp file. The only other lever is the path on the command line,
which forces a staging tree that mirrors the names and a working
directory change per invocation -- and there is no portable chdir
file action for posix_spawn before glibc 2.29, so that means fork and
exec by hand, which a multithreaded caller cannot do safely.
MSG
```

---

## Task 3: Hermes -- `hermes_init_sh_unit`

**Files:**
- Create: `hermes/API/napi/hermes_napi_sh_unit.cpp`
- Modify: `hermes/API/napi/hermes_napi.h`
- Modify: `hermes/API/napi/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  typedef struct SHUnit SHUnit;
  typedef SHUnit *(*SHUnitCreator)(void);
  NAPI_EXTERN napi_status NAPI_CDECL hermes_init_sh_unit(
      napi_env env, SHUnitCreator creator, napi_value *result);
  ```
  Task 13 calls it from `bundleLoadCallback`.

**There IS a NAPI test harness, and this task uses it.**
`hermes/unittests/napi/` has a fixture that creates a `napi_env` and
asserts `napi_pending_exception` (see `NapiCoercionTest.cpp:309`,
`NapiAsyncContextTest.cpp:289`). Combined with Task 1's hand-built units,
that covers both halves of this function directly -- add
`hermes/unittests/napi/NapiShUnitTest.cpp` in Step 2b below.

What Task 13 adds on top is the integration: a real shermes-produced unit,
reached through `require()`.

**No fixture built from bundled MODULES can reach the failure path, which
is a property of the wrapper rather than an accident** -- which is why the
unit test above uses a hand-built unit instead.
 Every module is compiled
inside `(function (exports, require, module, __filename, __dirname) { ... })`,
so the unit's global code does exactly one thing -- create the closure and
return it. Verified on the emitted C for a module whose body is nothing but
`throw`:

```c
static SHLegacyValue _0_global(SHRuntime *shr) {
  ...
  locals.t0 = _sh_ljs_create_closure(shr, NULL, _1_, &s_function_info_table[1], shUnit);
  _sh_leave(shr, &locals.head, frame);
  return locals.t0;
}
```

The `throw` runs when the LOADER CALLS that closure, which is after
`hermes_init_sh_unit` has already returned. So a module whose top level
throws does not fail unit initialization, and no fixture built out of
bundled modules can exercise the failure path.

So the failure path is exercised by the hand-built throwing unit from Task
1's Step 1c, through `hermes_init_sh_unit` itself, in the NAPI fixture. It
is required for correctness regardless -- an unwrapped unit reaches it, and
a silently swallowed exception is what it exists to prevent -- and it is
now actually tested rather than argued for.

Task 13's `boom.js` stays, but for a different property: that a throwing
module body propagates through `require()`, which is the behaviour users
see. Say so in that fixture's comment, so nobody later reads it as covering
this function.

- [ ] **Step 2b: Write the NAPI test**

Create `hermes/unittests/napi/NapiShUnitTest.cpp`, following the fixture
style of the neighbouring files (they define `env_` in a test fixture and
open a handle scope per test -- copy that, do not invent it). The two units
come from Task 1: declare them `extern` rather than duplicating the
construction, or move both creators into a small shared
`unittests/napi/ShUnitFixtures.h`.

```cpp
TEST_F(NapiShUnitTest, InitReturnsTheUnitsCompletionValue) {
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, hermes_init_sh_unit(env_, createTestUnit, &result));
  ASSERT_NE(nullptr, result);
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_undefined, type);  // the minimal unit returns undefined
}

/// The half no bundled module can reach: the wrapper means a module's
/// global only creates a closure, so a throwing module body throws when
/// the loader CALLS it, after this function has returned.
TEST_F(NapiShUnitTest, InitReportsAThrowingUnitAsAPendingException) {
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_pending_exception,
      hermes_init_sh_unit(env_, createThrowingUnit, &result));
  bool pending = false;
  ASSERT_EQ(napi_ok, napi_is_exception_pending(env_, &pending));
  EXPECT_TRUE(pending);
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));
  double value = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, exc, &value));
  EXPECT_EQ(42, value);
}

TEST_F(NapiShUnitTest, InitRejectsNullArguments) {
  napi_value result = nullptr;
  EXPECT_EQ(napi_invalid_arg, hermes_init_sh_unit(env_, nullptr, &result));
  EXPECT_EQ(napi_invalid_arg,
            hermes_init_sh_unit(env_, createTestUnit, nullptr));
}
```

Add the file to `hermes/unittests/napi/CMakeLists.txt`.

- [ ] **Step 2c: Run it**

```bash
cmake --build cmake-build-asan --target NapiTests 2>&1 | tail -5
cmake-build-asan/hermes/unittests/napi/NapiTests \
  --gtest_filter='NapiShUnitTest.*'
```

Check the real target name from `hermes/unittests/napi/CMakeLists.txt`.
Expected: PASS, 3 tests.

Task 13's `boom.js` stays: it pins that a throwing module body still
propagates through `require()`, which is the behaviour users see. It just
does not test this function's failure branch, and the plan should not claim
it does.

- [ ] **Step 1: Declare it**

In `hermes/API/napi/hermes_napi.h`, after the `hermes_set_wasm_cache`
declaration:

```c
/// An SH compilation unit, opaque to callers. Declared here so an embedder
/// can hold a creator function pointer without including static_h.h, whose
/// inline functions depend on the VM layout defines a caller has no way to
/// match.
typedef struct SHUnit SHUnit;
typedef SHUnit *(*SHUnitCreator)(void);

/// Register, initialize and run the compilation unit \p creator produces,
/// returning its top-level completion value in \p result.
///
/// For a unit compiled from a single expression statement -- a CommonJS
/// module wrapped in `(function (exports, require, module, __filename,
/// __dirname) { ... })` -- that value is the wrapper function, ready to be
/// called.
///
/// A second call for an already-registered unit re-runs it and returns a
/// fresh value, which is what makes a native module behave like a bytecode
/// one under `delete require.cache[...]`.
///
/// If the unit's top level throws, returns napi_pending_exception with the
/// thrown value pending -- the same shape hermes_run_bytecode reports for a
/// module whose top level threw, so a caller need not distinguish them.
NAPI_EXTERN napi_status NAPI_CDECL hermes_init_sh_unit(
    napi_env env,
    SHUnitCreator creator,
    napi_value *result);
```

- [ ] **Step 2: Implement it**

Create `hermes/API/napi/hermes_napi_sh_unit.cpp`. Model the preamble,
argument checking and scope handling on `hermes_run_bytecode` in
`hermes_napi.cpp` -- read that function first and mirror it, since the
macro names and the `napi_env` internals are what they are in this tree:

```cpp
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/static_h.h"

napi_status NAPI_CDECL hermes_init_sh_unit(
    napi_env env,
    SHUnitCreator creator,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, creator);
  CHECK_ARG(env, result);

  SHLegacyValue value;
  // The guarded form, not _sh_unit_init: it supplies the GCScope and
  // catches Static Hermes's longjmp unwind. Letting a _sh_throw cross this
  // boundary is not something the boundary is built for.
  // env->runtime is already a `hermes::vm::Runtime &`
  // (hermes_napi_impl.h:384) -- no cast. hermes_run_bytecode does
  // `Runtime &runtime = env->runtime;` (hermes_napi.cpp:742).
  bool ok = _sh_unit_init_guarded(
      hermes::vm::getSHRuntime(env->runtime), creator, &value);

  if (!ok) {
    // Deliberately NOT captureRuntimeException(): _sh_catch() has already
    // taken the thrown value out of the runtime and cleared it, so asking
    // the runtime again finds nothing pending.
    env->pendingException =
        hermes::vm::HermesValue::fromRaw(value.raw);
    env->hasPendingException = true;
    return napi_set_last_error(env, napi_pending_exception);
  }

  // Rooted before it leaves: SHLegacyValue and HermesValue share a
  // representation, so the reinterpretation is sound here, but handing an
  // unrooted value out as a napi_value is not.
  *result = env->addToCurrentScope(hermes::vm::HermesValue::fromRaw(value.raw));
  return napi_clear_last_error(env);
}
```

The exact spellings of `env->runtime`, `env->pendingException`,
`env->hasPendingException` and `env->addToCurrentScope` must be taken from
`hermes_napi_internal.h` / `hermes_napi_impl.h` and from how
`hermes_run_bytecode` does the same three things. Do not invent them.

Add the file to the source list in `hermes/API/napi/CMakeLists.txt`.

- [ ] **Step 3: Verify it compiles and links**

```bash
cmake --build cmake-build-asan --target hermes-node 2>&1 | tail -5
nm -gU cmake-build-asan/bin/hermes-node | grep hermes_init_sh_unit
```

Expected: build succeeds and the symbol is exported.

- [ ] **Step 4: Commit**

```bash
./utils/format.sh -f
git -C hermes add -A && git -C hermes commit -F - <<'MSG'
Add hermes_init_sh_unit to the NAPI surface

An embedder linking Static Hermes compilation units needs to run one
and get its completion value as a napi_value. getSHRuntime() is
already public, but converting SHLegacyValue and rooting it in the
current handle scope cannot be done outside Hermes, and doing it here
keeps embedders clear of static_h.h, whose inline functions depend on
VM layout defines a caller cannot match.

Uses the guarded init so the longjmp unwind does not cross the NAPI
boundary, and assigns the pending exception directly rather than
asking the runtime for it -- _sh_catch has already cleared it there.
MSG
```

---

## Task 4: Fix the scanner/compiler language-flag divergence

**Files:**
- Modify: `include/hermes/node-compat/bundle/cjs_wrapper.h`
- Modify: `lib/bundle/require_scanner.cpp:424-426`
- Test: `test/bundle-async-generator.js` (create)

**Interfaces:**
- Consumes: nothing.
- Produces: in `cjs_wrapper.h`,
  ```cpp
  struct JSLanguageFlags {
    bool es6BlockScoping = true;
    bool asyncGenerators = true;
    bool generators = true;
    bool typeScript = false;   // set per .ts extension
  };
  constexpr JSLanguageFlags kJSLanguageFlags{};
  ```
  Task 9 reads the same struct for the `shermes` argv.

  **Be honest about the reach of this.** It makes the scanner and the
  `shermes` command line one source. It does NOT reach the bytecode
  compiler: those values are hardcoded inside Hermes, in
  `hermes/API/napi/hermes_napi_compile.cpp:59`, and a hermes-node header
  cannot control them. So two implementations remain, and what keeps them
  agreeing is tests -- `test/bundle-async-generator.js` here and
  `test/build-native-parity.js` in Task 14 -- not a shared constant.
  Closing it properly means widening `hermes_compile_flags` so the producer
  supplies the fields, which is a Hermes API change and is out of scope.
  Say so in the header comment rather than implying the struct is
  authoritative.

**This is a bug fix, not new work.** `require_scanner.cpp:424` builds a bare
`hermes::Context`, whose defaults are `enableAsyncGenerators_{false}` and
`enableES6BlockScoping_{false}` (`hermes/include/hermes/AST/Context.h:276,281`),
while the compile step behind it sets both true
(`hermes/API/napi/hermes_napi_compile.cpp:59`). Async generators are a
**parse** error with the flag off, so the scanner rejects a module the
compiler would have accepted and the producer stubs it. Tracker issue
`01a09e14-0a0e`. It is a prerequisite for the native work because the whole
tolerance decision moves into the scanner.

- [ ] **Step 1: Write the failing test**

Create `test/bundle-async-generator.js`:

```js
// RUN: rm -rf %t && mkdir -p %t
// RUN: cp %s %t/app.js
// RUN: echo 'async function* g() { yield 1; }' > %t/gen.js
// RUN: echo 'module.exports = g;' >> %t/gen.js
// RUN: %hermes-node %t/app.js | %FileCheck --check-prefix=PLAIN %s
// RUN: %hermes-node --build-bundle=%t/app.hbb %t/app.js 2>&1 | %FileCheck --check-prefix=BUILD %s
// RUN: %hermes-node --bundle=%t/app.hbb | %FileCheck --check-prefix=BUNDLED %s

// An async generator is a parse error unless the compiler is told to allow
// them. The bundle producer's scanner and its compiler must agree about
// that, or the scanner stubs a module the compiler would have compiled --
// which is what it used to do. See dz 01a09e14-0a0e.

const g = require('./gen.js');
(async () => {
  for await (const v of g()) console.log('PASS', v);
})();

// PLAIN: PASS 1
// BUILD-NOT: cannot parse
// BUNDLED: PASS 1
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 -v \
  $(pwd)/test/bundle-async-generator.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: FAIL. `PLAIN` passes; `BUILD` fails on the `cannot parse` warning;
`BUNDLED` fails with `SyntaxError: async generators are unsupported`.

- [ ] **Step 3: Add the shared flag set**

In `include/hermes/node-compat/bundle/cjs_wrapper.h`, after `unwrapCoords`:

```cpp
/// The language features every JavaScript file in a bundle is compiled
/// with. Three things have to agree about them: the require() scanner
/// (lib/bundle/require_scanner.cpp), the bytecode compile step, and -- for
/// a native build -- the shermes command line.
///
/// This struct is the source for TWO of the three. The bytecode step
/// hardcodes the same values inside Hermes
/// (hermes/API/napi/hermes_napi_compile.cpp), which a header here cannot
/// reach; widening hermes_compile_flags so the producer supplies them would
/// close that, and has not been done. Until it is, what keeps the third in
/// line is test/bundle-async-generator.js and
/// test/build-native-parity.js, not this declaration. If you change a value
/// here, change it there too.
///
/// The scanner's Context used to take Hermes's own defaults, which disable
/// async generators and ES6 block scoping. Async generators are a PARSE
/// error when disabled, so the scanner rejected modules the compiler behind
/// it accepted and the producer packaged them as throwing stubs: a program
/// that ran from disk threw once bundled. See dz 01a09e14-0a0e.
///
/// Block scoping is the quieter half, and in the SCANNER it is
/// inert: Hermes consults that flag only in IRGen
/// (lib/IRGen/ESTreeIRGen-stmt.cpp), which the scanner never reaches. It
/// is set to track the compiler's configuration, and because the same
/// struct drives the shermes command line, where it IS load-bearing.
struct JSLanguageFlags {
  bool es6BlockScoping = true;
  bool asyncGenerators = true;
  bool generators = true;
  /// The one field that is per file rather than per project: set when the
  /// module's extension is `.ts`.
  bool typeScript = false;
};

/// The project-wide settings, with TypeScript off. A caller with a `.ts`
/// file copies this and sets `typeScript`.
constexpr JSLanguageFlags kJSLanguageFlags{};
```

- [ ] **Step 4: Configure the scanner from it**

In `lib/bundle/require_scanner.cpp`, replace:

```cpp
  auto context = std::make_shared<Context>();
  if (enableTS)
    context->setParseTS(true);
```

with:

```cpp
  auto context = std::make_shared<Context>();
  // These must match what the compile step behind this scan will use, or
  // the scan rejects files the compiler would accept. See JSLanguageFlags
  // in cjs_wrapper.h for what that cost.
  context->setEnableES6BlockScoping(kJSLanguageFlags.es6BlockScoping);
  context->setEnableAsyncGenerators(kJSLanguageFlags.asyncGenerators);
  if (enableTS)
    context->setParseTS(true);
```

`cjs_wrapper.h` is already included by this file. `Context` has no
`setEnableGenerator` -- generators are on unconditionally -- which is why
`kJSLanguageFlags.generators` is documentation rather than a setter call
here; Task 9 uses it the same way, since `shermes` also has no flag for it.

- [ ] **Step 5: Fix the comment that asserts the broken invariant**

At `lib/bundle/require_scanner.cpp:437-442`, the comment on the
`sema::resolveAST` failure currently claims "a failure here means the
compiler will reject the same wrapped text for the same reason, so treating
it as a scan failure is not a new restriction." That is now true and was not
before. Append one sentence so the next reader knows it is load-bearing
rather than incidental:

```
  /// That holds only because this Context is configured from
  /// kJSLanguageFlags above -- with Hermes's own defaults it was false, and
  /// an async generator was stubbed here and compiled fine ten lines later.
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target hermes-node 2>&1 | tail -3
python3 cmake-build-asan/bin/hermes-lit -j1 -v \
  $(pwd)/test/bundle-async-generator.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: PASS, all three RUN lines.

- [ ] **Step 6b: Cover the block-scoping half too**

The async-generator case covers `enableAsyncGenerators` because that flag
changes what PARSES. `enableES6BlockScoping` changes only binding
resolution, so it needs a case of its own -- and binding resolution is
exactly what the scanner uses to decide "is this identifier the module's
`require`". Add to `test/bundle-async-generator.js`, or a sibling file:

```js
// A block-scoped shadow of require. With ES6 block scoping off, `require`
// inside the block resolves to a function-scoped var and the scanner can
// attribute the call to the module's require parameter, discovering an
// edge that does not exist. With it on, the binding is the block's.
{
  const require = (x) => ({ shadowed: x });
  module.exports = require('./not-a-real-file.js');
}
```

Bundling this must succeed and must not warn about an unresolvable
`./not-a-real-file.js`, because with correct scoping the scanner sees that
`require` is not the module's.

- [ ] **Step 7: Check for fallout in the existing bundle tests**

The scanner now resolves bindings under block scoping, which can change
which `require` a shadowed identifier refers to. Run the whole bundle suite:

```bash
for t in $(ls test/bundle-*.js test/build-exe*.js); do
  python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/$t \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test \
    --param kit_dir=$(pwd)/cmake-build-asan/kit 2>&1 | tail -2
done
```

Expected: every file PASS or UNSUPPORTED. Any FAIL here is a real
consequence of this change and must be understood, not worked around.

- [ ] **Step 8: Close the tracker issue**

```bash
DZ_AUTHOR="$(git config user.name) <$(git config user.email)>" \
  node examples/ditz2/ditz2/dist/cli/main.js close 01a09e14-0a0e --as fixed \
  -m "Fixed: the scanner Context is now configured from JSLanguageFlags in cjs_wrapper.h, the single copy the compile step also reads. Regression test test/bundle-async-generator.js."
```

If `examples/ditz2/ditz2/dist` does not exist, build it first per CLAUDE.md.

- [ ] **Step 9: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Make the bundle scanner match the compiler

The scanner built a bare hermes::Context, whose defaults disable
async generators and ES6 block scoping, while the compile step behind
it enables both. Async generators are a parse error when disabled, so
the scanner rejected a module the compiler would have accepted and
the producer packaged it as one that throws when required: a program
printing PASS from disk threw SyntaxError once bundled.

The settings become one struct because three things must agree about
them -- the scanner, the bytecode compiler, and the shermes command
line a native build will construct.

Closes dz 01a09e14-0a0e.
MSG
```

---

## Task 5: A capturing, classifying subprocess runner

**Files:**
- Modify: `include/hermes/node-compat/build-exe/build_exe.h`
- Modify: `lib/build-exe/build_exe.cpp:188-224` (`runCommand`)
- Test: `unittests/BuildExeTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces, in `build_exe.h`:
  ```cpp
  struct CommandResult {
    enum class Outcome { Exited, Signalled, SpawnFailed, WaitFailed };
    Outcome outcome = Outcome::Exited;
    int status = 0;        // exit status, or the signal number
    std::string output;    // captured stdout+stderr, empty if not captured
    bool ok() const { return outcome == Outcome::Exited && status == 0; }
  };

  CommandResult runCommandCaptured(const std::vector<std::string> &argv);
  ```
  Task 10 calls it; Task 11 reports its failures.

**Why a structured result:** a native build runs one `shermes` and one `cc`
per module, and the difference between "the compiler rejected this source"
and "the compiler crashed, or the disk is full" decides whether the build
continues. The existing `runCommand` returns `bool` and lets the child write
straight to fd 2, so neither question can be answered and the diagnostic
cannot be attributed to a module.

**The deadlock:** the pipe must be drained *while the child runs*. Reading
after `waitpid` hangs the moment a compiler prints more than a pipe buffer's
worth of diagnostics, which for a page of template errors is routine.

- [ ] **Step 1: Write the failing test**

Add to `unittests/BuildExeTest.cpp`:

```cpp
TEST(BuildExeTest, RunCommandCapturedReportsCleanExit) {
  CommandResult r = runCommandCaptured({"/bin/sh", "-c", "exit 0"});
  EXPECT_EQ(CommandResult::Outcome::Exited, r.outcome);
  EXPECT_EQ(0, r.status);
  EXPECT_TRUE(r.ok());
}

TEST(BuildExeTest, RunCommandCapturedReportsExitStatus) {
  CommandResult r = runCommandCaptured({"/bin/sh", "-c", "exit 3"});
  EXPECT_EQ(CommandResult::Outcome::Exited, r.outcome);
  EXPECT_EQ(3, r.status);
  EXPECT_FALSE(r.ok());
}

TEST(BuildExeTest, RunCommandCapturedMergesStdoutAndStderr) {
  CommandResult r = runCommandCaptured(
      {"/bin/sh", "-c", "echo out; echo err 1>&2"});
  EXPECT_TRUE(r.ok());
  EXPECT_NE(std::string::npos, r.output.find("out"));
  EXPECT_NE(std::string::npos, r.output.find("err"));
}

TEST(BuildExeTest, RunCommandCapturedReportsASignal) {
  CommandResult r = runCommandCaptured({"/bin/sh", "-c", "kill -TERM $$"});
  EXPECT_EQ(CommandResult::Outcome::Signalled, r.outcome);
  EXPECT_EQ(SIGTERM, r.status);
  EXPECT_FALSE(r.ok());
}

TEST(BuildExeTest, RunCommandCapturedReportsSpawnFailure) {
  CommandResult r = runCommandCaptured(
      {"/nonexistent/definitely-not-a-program"});
  EXPECT_TRUE(r.outcome == CommandResult::Outcome::SpawnFailed ||
              (r.outcome == CommandResult::Outcome::Exited && r.status == 127))
      << "outcome=" << (int)r.outcome << " status=" << r.status;
  EXPECT_FALSE(r.ok());
}

/// The reason the pipe is drained while the child runs rather than after
/// waitpid. 1 MiB is comfortably past any platform's pipe buffer, so a
/// read-after-wait implementation deadlocks here instead of failing.
TEST(BuildExeTest, RunCommandCapturedDoesNotDeadlockOnALargeStream) {
  CommandResult r = runCommandCaptured(
      {"/bin/sh", "-c",
       "i=0; while [ $i -lt 16384 ]; do "
       "echo 0123456789012345678901234567890123456789012345678901234567890123;"
       " i=$((i+1)); done"});
  EXPECT_TRUE(r.ok());
  EXPECT_GT(r.output.size(), 1024u * 1024u);
}
```

Add `#include <csignal>` to the test file if absent.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BuildExeTest 2>&1 | tail -10
```

Expected: FAIL to compile, `runCommandCaptured` and `CommandResult`
undeclared.

- [ ] **Step 3: Declare the result type**

In `include/hermes/node-compat/build-exe/build_exe.h`, before
`buildExecutable`:

```cpp
/// What happened to a subprocess, in the detail a caller needs to decide
/// whether to continue.
///
/// The existing bool-returning runCommand() is enough for --build-exe, which
/// runs two commands and gives up on either. A native build runs two per
/// module and must tell "the compiler rejected this file" from "the
/// compiler crashed" -- one is about the program being built, the other is
/// about the machine building it, and only the second should ever be
/// silently retried or reported as a toolchain problem.
struct CommandResult {
  enum class Outcome {
    /// The child ran and exited; `status` is its exit status.
    Exited,
    /// The child was killed; `status` is the signal number.
    Signalled,
    /// posix_spawnp failed; `status` is the errno it reported.
    SpawnFailed,
    /// waitpid failed; `status` is errno.
    WaitFailed,
  };
  Outcome outcome = Outcome::Exited;
  int status = 0;
  /// The child's stdout and stderr, interleaved as the child wrote them.
  std::string output;

  bool ok() const {
    return outcome == Outcome::Exited && status == 0;
  }
};

/// Runs \p argv to completion with its stdout and stderr captured into the
/// result rather than inherited.
///
/// The pipe is drained while the child runs. Reading it after waitpid()
/// deadlocks as soon as the child writes more than a pipe buffer, which a
/// compiler emitting a page of diagnostics does routinely.
///
/// Both streams share one pipe, so their interleaving is the child's own --
/// which is what you want when the output is going to be quoted back to a
/// user as "what the compiler said".
CommandResult runCommandCaptured(const std::vector<std::string> &argv);
```

- [ ] **Step 4: Implement it**

In `lib/build-exe/build_exe.cpp`, beside the existing `runCommand` (which
stays, unchanged -- `--build-exe` still wants inherited streams so a
toolchain's colored, streamed diagnostics reach the terminal intact):

```cpp
CommandResult runCommandCaptured(const std::vector<std::string> &argv) {
  CommandResult result;

  // Close-on-exec, created atomically where the platform allows it. This
  // runner is called from several worker threads at once: with plain
  // pipe(), one worker's write end leaks into another worker's child, that
  // child holds it open, and the first worker's read never sees EOF -- a
  // hang, not a wrong answer. pipe2() closes the window entirely; the macOS
  // fallback leaves a small one, which is why the fcntl comes immediately.
  //
  // On the fallback path the window between pipe() and the two fcntl()s is
  // still a window, so a mutex serializes create-flag-spawn against other
  // threads in this process. It costs nothing measurable next to a compiler
  // invocation, and it is the only way to make the fallback actually safe
  // rather than merely narrower.
  int fds[2];
#ifdef __APPLE__
  static std::mutex spawnMutex;
  std::unique_lock<std::mutex> spawnLock(spawnMutex);
  if (pipe(fds) != 0) {
    result.outcome = CommandResult::Outcome::SpawnFailed;
    result.status = errno;
    return result;
  }
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    int saved = errno;
    close(fds[0]);
    close(fds[1]);
    result.outcome = CommandResult::Outcome::SpawnFailed;
    result.status = saved;
    return result;
  }
#else
  if (pipe2(fds, O_CLOEXEC) != 0) {
    result.outcome = CommandResult::Outcome::SpawnFailed;
    result.status = errno;
    return result;
  }
#endif

  std::vector<char *> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string &arg : argv)
    raw.push_back(const_cast<char *>(arg.c_str()));
  raw.push_back(nullptr);

  // One pipe for both streams, so the child's own interleaving survives.
  // adddup2 clears FD_CLOEXEC on the duplicate, which is what lets the
  // child keep fds 1 and 2 while every other descriptor closes. Each call
  // is checked: a silently failed action means the child runs with the
  // wrong descriptors and the output vanishes.
  posix_spawn_file_actions_t actions;
  // init separately: destroying an object whose init failed is undefined.
  if (int ierr = posix_spawn_file_actions_init(&actions)) {
    close(fds[0]);
    close(fds[1]);
    result.outcome = CommandResult::Outcome::SpawnFailed;
    result.status = ierr;
    return result;
  }
  int aerr = 0;
  if (aerr == 0)
    aerr = posix_spawn_file_actions_addclose(&actions, fds[0]);
  if (aerr == 0)
    aerr = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  if (aerr == 0)
    aerr = posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
  if (aerr == 0)
    aerr = posix_spawn_file_actions_addclose(&actions, fds[1]);
  if (aerr != 0) {
    posix_spawn_file_actions_destroy(&actions);
    close(fds[0]);
    close(fds[1]);
    result.outcome = CommandResult::Outcome::SpawnFailed;
    result.status = aerr;
    return result;
  }

  pid_t pid = 0;
  int rc = posix_spawnp(&pid, raw[0], &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
#ifdef __APPLE__
  // The child has been forked; nothing else can inherit these now.
  spawnLock.unlock();
#endif

  if (rc != 0) {
    close(fds[0]);
    result.outcome = CommandResult::Outcome::SpawnFailed;
    result.status = rc;
    return result;
  }

  // Drained here, before waitpid: a child that fills the pipe blocks
  // writing while we block waiting, and neither side ever moves.
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) != 0) {
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    result.output.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      result.outcome = CommandResult::Outcome::WaitFailed;
      result.status = errno;
      return result;
    }
  }

  if (WIFSIGNALED(status)) {
    result.outcome = CommandResult::Outcome::Signalled;
    result.status = WTERMSIG(status);
  } else {
    result.outcome = CommandResult::Outcome::Exited;
    result.status = WEXITSTATUS(status);
  }
  return result;
}
```

Add `#include <unistd.h>`, `#include <fcntl.h>` and `#include <mutex>` if absent. `runCommandCaptured` must be declared
in the `hermes::node_compat` namespace and NOT in the anonymous namespace
the file's helpers live in -- it is now part of the public header.

- [ ] **Step 4b: Add the concurrency test**

The reason the descriptors are close-on-exec. Without it one worker's write
end leaks into another worker's child, which holds it open, and the first
worker's read never sees EOF:

```cpp
TEST(BuildExeTest, RunCommandCapturedIsSafeFromSeveralThreads) {
  // A long-lived child alongside short ones: if a short child inherits the
  // long one's pipe write end, the short read never terminates.
  std::vector<std::thread> threads;
  std::atomic<int> done{0};
  threads.emplace_back([&] {
    runCommandCaptured({"/bin/sh", "-c", "sleep 2; echo slow"});
    ++done;
  });
  for (int i = 0; i < 8; ++i)
    threads.emplace_back([&] {
      CommandResult r = runCommandCaptured({"/bin/sh", "-c", "echo quick"});
      EXPECT_TRUE(r.ok());
      EXPECT_NE(std::string::npos, r.output.find("quick"));
      ++done;
    });
  for (std::thread &t : threads)
    t.join();
  EXPECT_EQ(9, done.load());
}
```

Add `#include <thread>` and `#include <atomic>`. If this test times out
rather than failing, that IS the bug -- the descriptors are being inherited.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target BuildExeTest 2>&1 | tail -3
cmake-build-asan/unittests/BuildExeTest \
  --gtest_filter='BuildExeTest.RunCommandCaptured*'
```

Expected: PASS, 7 tests. Two of them fail by HANGING rather than by
reporting: the large-stream one if the drain is in the wrong place, the
concurrency one if the pipe is not close-on-exec. Run with a timeout.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Add a capturing, classifying subprocess runner

A native build runs a compiler twice per module, so it has to tell a
rejected source from a crashed compiler or a full disk: the first is
about the program being built and the second is about the machine
building it. A bool cannot say which, and a child writing straight to
fd 2 cannot have its diagnostics attributed to a module.

The pipe is drained before waitpid rather than after. A compiler that
prints more than a pipe buffer -- a page of diagnostics -- otherwise
blocks writing while the parent blocks waiting. The test writes a
megabyte for exactly that reason.

The existing runCommand stays: --build-exe wants inherited streams so
a toolchain's colored, streamed output reaches the terminal intact.
MSG
```

---

## Task 6: The kit carries shermes, the SH headers, and `ccflag:`

**Files:**
- Modify: `include/hermes/node-compat/build-exe/kit_manifest.h`
- Modify: `lib/build-exe/kit_manifest.cpp:8-23` (the format comment), `:128-135` (the key switch)
- Modify: `utils/make-kit.py:300-365`
- Modify: `tools/hermes-node/CMakeLists.txt:121-138`
- Test: `unittests/BuildExeTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `KitManifest::ccFlags` (a `std::vector<std::string>`, `{kit}`
  already substituted), and in a cut kit: `<kit>/shermes`,
  `<kit>/include/hermes/...`, `<kit>/include/libhermesvm-config.h`. Task 9
  reads `ccFlags`; Task 12 defaults `--shermes=` to `<kit>/shermes`.

**The header set** is what `clang -MM` names over a generated C file --
eleven files from the Hermes source tree plus the generated config header:

```
hermes/Support/sh_tryfast_fp_cvt.h        hermes/VM/sh_mirror.h
hermes/VM/SHRuntimeHermesValueFields.def  hermes/VM/sh_runtime.h
hermes/VM/sh_config.h                     hermes/VM/sh_segment_info.h
hermes/VM/sh_legacy_value.h               hermes/VM/sh_small_hermes_value.h
hermes/VM/sh_stack_frame.h                hermes/VM/static_h.h
hermes/VMLayouts/sh_stack_frame_layout.h
libhermesvm-config.h
```

The config header carries `HERMESVM_COMPRESSED_POINTERS`,
`HERMESVM_BOXED_DOUBLES`, `HERMESVM_LOG_HEAP_SEGMENT_SIZE`,
`HERMESVM_GCKIND` and `HERMESVM_MODEL`. Compiling the generated C against a
mismatched copy miscomputes `SHRuntime`'s layout; `_SH_MODEL()` in the
generated C turns that into a link error (the symbol is
`_sh_model_<HERMESVM_MODEL>`) rather than silent corruption.

- [ ] **Step 1: Write the failing test**

Add to `unittests/BuildExeTest.cpp`. Follow the existing manifest tests in
that file for how they write a temp manifest and call `readKitManifest`:

```cpp
TEST(BuildExeTest, ManifestReadsCcFlagsInOrderWithKitSubstitution) {
  TempDir dir;  // use whatever helper the existing manifest tests use
  writeFile(dir.path() + "/kit.manifest",
            "version: 1.2.3\n"
            "cc: /usr/bin/clang++\n"
            "ccflag: -DNDEBUG\n"
            "ccflag: -fno-strict-aliasing\n"
            "ccflag: -I{kit}/include\n"
            "driverflag: -arch\n"
            "linkarg: -lm\n");
  std::string error;
  auto m = readKitManifest(dir.path(), &error);
  ASSERT_TRUE(m.has_value()) << error;
  ASSERT_EQ(3u, m->ccFlags.size());
  EXPECT_EQ("-DNDEBUG", m->ccFlags[0]);
  EXPECT_EQ("-fno-strict-aliasing", m->ccFlags[1]);
  EXPECT_EQ(dir.path() + "/include", m->ccFlags[2].substr(2));
  EXPECT_EQ("-I", m->ccFlags[2].substr(0, 2));
}

TEST(BuildExeTest, ManifestWithNoCcFlagsIsStillValid) {
  TempDir dir;
  writeFile(dir.path() + "/kit.manifest",
            "version: 1.2.3\ncc: /usr/bin/clang++\n");
  std::string error;
  auto m = readKitManifest(dir.path(), &error);
  ASSERT_TRUE(m.has_value()) << error;
  EXPECT_TRUE(m->ccFlags.empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BuildExeTest 2>&1 | tail -10
```

Expected: FAIL to compile, `ccFlags` is not a member of `KitManifest`.

- [ ] **Step 3: Add the field and parse the key**

In `include/hermes/node-compat/build-exe/kit_manifest.h`, in `KitManifest`
after `driverFlags`:

```cpp
  /// Flags for compiling a generated C file against this kit: the include
  /// path to its headers plus whatever the build that cut it required. Not
  /// the same list as driverFlags, which is about selecting a target for
  /// the link. `{kit}` already substituted.
  ///
  /// -fno-strict-aliasing and -fno-strict-overflow are not stylistic here:
  /// Static Hermes's generated C reads and writes C++ objects through
  /// mirroring C structs, so unrelated types alias by construction.
  std::vector<std::string> ccFlags; // {kit} already substituted
```

In `lib/build-exe/kit_manifest.cpp`, add to the key switch beside
`driverflag`:

```cpp
    } else if (key == "ccflag") {
      manifest.ccFlags.push_back(substituteKitDir(value, kitDir));
```

and add `ccflag` to the format comment at the top of the file, in the same
style as the others:

```
//   ccflag    -- repeated, ordered; flags for compiling a generated C
//                file against this kit's headers. `{kit}` substituted, as
//                for linkarg.
```

Note that `ccFlags` is optional: a kit cut before this change has none, and
the reader must not require it. (The reverse -- a new kit read by an old
binary -- is already an error, because unknown keys are rejected, which is
the intended behaviour.)

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target BuildExeTest 2>&1 | tail -3
cmake-build-asan/unittests/BuildExeTest --gtest_filter='BuildExeTest.Manifest*'
```

Expected: PASS.

- [ ] **Step 5: Teach make-kit.py to write it and copy the files**

In `utils/make-kit.py`, add two arguments to `main()`'s parser:

```python
    ap.add_argument("--shermes", required=True,
                    help="path to the shermes binary to copy into the kit")
    ap.add_argument("--sh-include", action="append", default=[],
                    help="a -I root the generated C compiles against; "
                         "repeatable, in search order")
```

Above `main()`, the header list and the copy:

```python
# The headers Static Hermes's generated C includes, transitively. This is
# what `clang -MM` reports over a generated file, not a guess -- eleven
# files from the source tree. They are copied under their include-relative
# paths so a single -I{kit}/include serves.
SH_HEADERS = [
    "hermes/Support/sh_tryfast_fp_cvt.h",
    "hermes/VM/SHRuntimeHermesValueFields.def",
    "hermes/VM/sh_config.h",
    "hermes/VM/sh_legacy_value.h",
    "hermes/VM/sh_mirror.h",
    "hermes/VM/sh_runtime.h",
    "hermes/VM/sh_segment_info.h",
    "hermes/VM/sh_small_hermes_value.h",
    "hermes/VM/sh_stack_frame.h",
    "hermes/VM/static_h.h",
    "hermes/VMLayouts/sh_stack_frame_layout.h",
]

# Generated by Hermes's CMake, and the reason a kit is configuration
# specific: it carries HERMESVM_COMPRESSED_POINTERS, HERMESVM_MODEL and the
# rest of the defines that decide SHRuntime's layout. It sits at the root of
# the include directory because that is how the other headers include it.
SH_CONFIG_HEADER = "libhermesvm-config.h"


def copy_sh_headers(include_roots, kit_include_dir):
    """Copy the SH headers into the kit, preserving include-relative paths.

    A header that cannot be found is fatal: a kit missing one produces a
    compile error at the user's machine naming a file they have never heard
    of, which is a far worse place to discover it than here.
    """
    wanted = SH_HEADERS + [SH_CONFIG_HEADER]
    for rel in wanted:
        src = None
        for root in include_roots:
            candidate = os.path.join(root, rel)
            if os.path.exists(candidate):
                src = candidate
                break
        if src is None:
            sys.exit("make-kit: cannot find %s in any of: %s"
                     % (rel, ", ".join(include_roots)))
        dst = os.path.join(kit_include_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    return len(wanted)
```

In `main()`, after the existing `shutil.copy2(force_loaded, ...)`:

```python
    shutil.copy2(args.shermes, os.path.join(args.kit_dir, "shermes"))
    kit_include = os.path.join(args.kit_dir, "include")
    n_headers = copy_sh_headers(args.sh_include, kit_include)
```

And in the manifest write, after the `driverflag` loop:

```python
        # The generated C is compiled, not merely assembled, so it needs its
        # own flag list. -fno-strict-aliasing and -fno-strict-overflow are
        # required, not tuning: the generated code reaches into C++ objects
        # through mirroring C structs.
        # Deliberately NOT the sanitizer or -arch flags: buildCompileCommand
        # forwards the whole driverflag list to the compile too, exactly as
        # buildAssembleCommand already does, so those arrive by that route.
        # Two lists carrying the same flag is two places to disagree.
        for c in ["-DNDEBUG", "-fno-strict-aliasing", "-fno-strict-overflow",
                  "-I{kit}/include"]:
            f.write("ccflag: %s\n" % c)
```

Extend `ADDITIONAL_CLEAN_FILES` handling by adding the new paths in
`tools/hermes-node/CMakeLists.txt` (next step).

- [ ] **Step 6: Wire the CMake target**

In `tools/hermes-node/CMakeLists.txt`, extend the `RULE_LAUNCH_LINK` and the
clean list:

```cmake
  RULE_LAUNCH_LINK
    "${Python_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/make-kit.py \
     --kit-dir ${HERMES_NODE_KIT_DIR} \
     --version-header ${HERMES_NODE_VERSION_HEADER} \
     --shermes $<TARGET_FILE:shermes> \
     --sh-include ${PROJECT_SOURCE_DIR}/hermes/include \
     --sh-include ${CMAKE_BINARY_DIR}/hermes/lib/config --"
```

and add to `ADDITIONAL_CLEAN_FILES`:

```
${HERMES_NODE_KIT_DIR}/shermes;\
${HERMES_NODE_KIT_DIR}/include
```

`shermes` must be built before the kit is cut:

```cmake
add_dependencies(hermes-node-kit shermes)
```

- [ ] **Step 7: Cut a kit and check it end to end**

```bash
rm -rf cmake-build-asan/kit
cmake --build cmake-build-asan --target hermes-node-kit 2>&1 | tail -5
ls cmake-build-asan/kit
grep ccflag cmake-build-asan/kit/kit.manifest
find cmake-build-asan/kit/include -type f | sort
cmake-build-asan/kit/shermes --version | head -1
```

Expected: `shermes` and `include/` present; twelve files under `include`;
`ccflag:` lines including `-I<abs kit>/include`; `shermes --version` runs.

- [ ] **Step 8: Prove the headers are sufficient**

The whole point of the header list is that a generated C file compiles
against the kit alone. Verify it rather than assume it:

```bash
printf 'print(1)\n' > /tmp/kitcheck.js
cmake-build-asan/kit/shermes -emit-c -exported-unit=kitcheck \
  -o /tmp/kitcheck.c /tmp/kitcheck.js
CC=$(grep '^cc: ' cmake-build-asan/kit/kit.manifest | cut -d' ' -f2)
$CC -x c -std=gnu11 -c /tmp/kitcheck.c -o /tmp/kitcheck.o \
  $(grep '^ccflag: ' cmake-build-asan/kit/kit.manifest | cut -d' ' -f2-)
echo "compiled: $?"
```

Expected: `compiled: 0`. A missing header fails here, naming it.

- [ ] **Step 9: Verify the existing build-exe tests still pass**

```bash
for t in build-exe build-exe-errors build-exe-tool-errors build-exe-natives build-exe-escapes; do
  python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/$t.js \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test \
    --param kit_dir=$(pwd)/cmake-build-asan/kit 2>&1 | tail -2
done
```

Expected: PASS or UNSUPPORTED for each.

- [ ] **Step 10: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Ship shermes and the SH headers in the kit

A native build compiles generated C on the user's machine, which needs
a compiler configuration the kit did not record and headers it did not
carry. The header list is what clang -MM reports over a generated
file, not a guess: eleven from the source tree plus the generated
config header, which is what makes a kit configuration specific --
it carries the defines that decide SHRuntime's layout, and
_SH_MODEL() turns a mismatch into a link error.

ccflag: is separate from driverflag: because the two answer different
questions -- one configures a compile, the other selects a target for
a link -- and a sanitizer build has to copy its -fsanitize flags into
both, or the generated C's structs disagree with the archives'.

shermes is 2.8 MB and links only system libraries.
MSG
```

---

## Task 7: The unit table in the generated assembly

**Files:**
- Modify: `include/hermes/node-compat/build-exe/build_exe.h` (`payloadAssembly` signature)
- Modify: `lib/build-exe/build_exe.cpp:262-308` (`payloadAssembly`)
- Test: `unittests/BuildExeTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  std::string payloadAssembly(
      const std::string &bundlePath,
      const std::vector<std::string> &unitSymbols,  // "" == no unit
      ObjectFormat format = hostObjectFormat());
  ```
  Tasks 10 and 13 use it; `--build-exe` passes an empty vector.

**Two symbols always, even for `--build-exe`:** `hermesNodeNativeUnits` and
`hermesNodeNativeUnitCount`, with a count of zero on the bytecode path. That
is what lets one `bundle_main.cpp` serve both configurations, with no weak
symbols and no second entry object in the kit.

**Section choice is the subtle part.** On ELF the payload stays in `.rodata`
-- it needs no relocations -- but a table of function pointers needs one per
entry, and in a position-independent executable those are dynamic
relocations applied at load time. Putting them in a read-only section is the
classic text-relocation problem, so the table goes in `.data.rel.ro`, which
RELRO makes read-only after relocation. On Mach-O the payload is already in
`__DATA,__const`, which is the same answer, so the table joins it there.

- [ ] **Step 1: Write the failing test**

Add to `unittests/BuildExeTest.cpp`:

```cpp
TEST(BuildExeTest, PayloadAssemblyEmitsAnEmptyUnitTableForELF) {
  std::string s = payloadAssembly("/tmp/app.hbb", {}, ObjectFormat::ELF);
  EXPECT_NE(std::string::npos, s.find(".section .rodata"));
  EXPECT_NE(std::string::npos, s.find("hermesNodeBundleStart:"));
  // The two symbols exist even with no units, so bundle_main.cpp links
  // against one definition in both configurations.
  EXPECT_NE(std::string::npos, s.find("hermesNodeNativeUnits:"));
  EXPECT_NE(std::string::npos, s.find("hermesNodeNativeUnitCount:"));
  EXPECT_NE(std::string::npos, s.find(".quad 0\n"));
  // A pointer table must not sit in .rodata in a PIE.
  EXPECT_NE(std::string::npos, s.find(".section .data.rel.ro"));
  // The GNU-stack note stays last.
  EXPECT_NE(std::string::npos, s.find(".note.GNU-stack"));
}

TEST(BuildExeTest, PayloadAssemblyEmitsUnitPointersAndNullsForELF) {
  std::string s = payloadAssembly(
      "/tmp/app.hbb", {"hn_m000000", "", "hn_m000002"}, ObjectFormat::ELF);
  EXPECT_NE(std::string::npos, s.find(".quad sh_export_hn_m000000\n"));
  EXPECT_NE(std::string::npos, s.find(".quad sh_export_hn_m000002\n"));
  // The hole is a JSON module, a native addon or a resolve-only
  // package.json: indexed by container module index, so it cannot be
  // compacted.
  EXPECT_NE(std::string::npos, s.find(".quad 0\n"));
  EXPECT_NE(std::string::npos, s.find(".quad 3\n"));  // the count
}

TEST(BuildExeTest, PayloadAssemblyUnderscoresSymbolsForMachO) {
  std::string s = payloadAssembly(
      "/tmp/app.hbb", {"hn_m000000"}, ObjectFormat::MachO);
  EXPECT_NE(std::string::npos, s.find("_hermesNodeNativeUnits:"));
  EXPECT_NE(std::string::npos, s.find(".quad _sh_export_hn_m000000\n"));
  EXPECT_NE(std::string::npos, s.find(".section __DATA,__const"));
  // Mach-O needs no GNU-stack note and must not emit one.
  EXPECT_EQ(std::string::npos, s.find(".note.GNU-stack"));
}

TEST(BuildExeTest, PayloadAssemblyAlignsTheUnitTable) {
  std::string s = payloadAssembly(
      "/tmp/app.hbb", {"hn_m000000"}, ObjectFormat::ELF);
  size_t table = s.find("hermesNodeNativeUnits:");
  ASSERT_NE(std::string::npos, table);
  size_t align = s.rfind(".p2align 3", table);
  EXPECT_NE(std::string::npos, align);
  EXPECT_LT(align, table);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BuildExeTest 2>&1 | tail -10
```

Expected: FAIL to compile, `payloadAssembly` takes two arguments.

- [ ] **Step 3: Change the signature**

In `include/hermes/node-compat/build-exe/build_exe.h`, replace the
`payloadAssembly` declaration, keeping the existing doc comment and adding:

```cpp
/// \p unitSymbols is one entry per container module, in module-index order:
/// the Static Hermes unit name for a natively compiled JavaScript module, or
/// an empty string for every record that has no unit -- a JSON module, a
/// native addon, a resolve-only package.json. Empty overall for a bytecode
/// --build-exe, which still gets the two table symbols with a count of zero,
/// so one bundle_main.cpp serves both configurations without weak symbols.
std::string payloadAssembly(
    const std::string &bundlePath,
    const std::vector<std::string> &unitSymbols,
    ObjectFormat format = hostObjectFormat());
```

- [ ] **Step 4: Emit the table**

In `lib/build-exe/build_exe.cpp`, at the end of `payloadAssembly` and
BEFORE the ELF `.note.GNU-stack` line (the note must stay last in the file):

```cpp
  // The unit table. Not in .rodata on ELF: each entry is the address of a
  // function, which in a PIE is a dynamic relocation applied at load time,
  // and relocations into a read-only section are the text-relocation
  // problem. .data.rel.ro is the section made read-only after relocation,
  // which is what a table of function pointers wants. Mach-O's
  // __DATA,__const already is that section, so the payload's own section
  // serves.
  //
  // Indexed by container module index with holes, never compacted: the
  // index is how bundleLoadCallback finds an entry, and a second mapping
  // from module index to table slot is a second thing that can be wrong.
  const char *prefix = format == ObjectFormat::MachO ? "_" : "";
  if (format == ObjectFormat::ELF)
    os << "\t.section .data.rel.ro\n";
  os << "\t.p2align 3\n"
     << "\t.globl " << prefix << "hermesNodeNativeUnits\n"
     << prefix << "hermesNodeNativeUnits:\n";
  for (const std::string &symbol : unitSymbols) {
    if (symbol.empty())
      os << "\t.quad 0\n";
    else
      os << "\t.quad " << prefix << "sh_export_" << symbol << "\n";
  }
  os << "\t.globl " << prefix << "hermesNodeNativeUnitCount\n"
     << prefix << "hermesNodeNativeUnitCount:\n"
     << "\t.quad " << unitSymbols.size() << "\n";
```

Restructure the existing `if (format == ObjectFormat::MachO)` block so the
ELF `.note.GNU-stack` line is emitted after this, not inside the payload
block. An empty `unitSymbols` emits the label with no `.quad` entries and a
count of `0`, which is exactly what the bytecode path wants.

- [ ] **Step 5: Update the existing caller**

`buildExecutable()` calls `payloadAssembly(bundlePath, hostObjectFormat())`
today. It becomes `payloadAssembly(bundlePath, {}, hostObjectFormat())`.
Any existing test calling it with two arguments needs the same edit.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target BuildExeTest 2>&1 | tail -3
cmake-build-asan/unittests/BuildExeTest --gtest_filter='BuildExeTest.PayloadAssembly*'
```

Expected: PASS, including the pre-existing `payloadAssembly` tests.

- [ ] **Step 7: Prove the ELF section choice with a real link**

On Linux only -- skip with a note on macOS, where `.data.rel.ro` is not
emitted at all:

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/build-exe.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit
```

Expected: PASS. `--build-exe` now emits a zero-length table, so this
confirms the restructured function still produces a linkable object and that
the GNU-stack note is still last (a `readelf -l` on the produced binary
should show `GNU_STACK` as `RW`, not `RWE`).

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Emit a native-unit table in the payload object

Both symbols are emitted unconditionally, with a count of zero for a
bytecode build. That is what lets one bundle_main.cpp serve both
configurations: a weak symbol or a second entry object in the kit
would be the alternative, and both are worse.

On ELF the table goes in .data.rel.ro rather than beside the payload
in .rodata. Each entry is a function address, which in a PIE is a
load-time relocation, and relocations into a read-only section are
the text-relocation problem; .data.rel.ro is read-only after
relocation. Mach-O's __DATA,__const already has that property, so the
payload's own section serves there.
MSG
```

---

## Task 8: `kBundleFlagNativeUnits`

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_format.h:59`
- Modify: `include/hermes/node-compat/bundle/bundle_writer.h:81`, `lib/bundle/bundle_writer.cpp:95, 242-243`
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h:137-139`, `lib/bundle/bundle_reader.cpp:147, 512-513`
- Test: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `constexpr uint32_t kBundleFlagNativeUnits = 1u << 1;`,
  `BundleWriter::setNativeUnits(bool)`, `BundleReader::hasNativeUnits()`.
  Tasks 11 and 13 use them.

**Why the bit exists at all**, since a native container is a build
intermediate: `.incbin` takes a path, so the container is serialized to the
temp directory, and `--keep-temp` retains it. That is the one way a container
with empty JavaScript payloads reaches a filesystem where someone can hand it
to `--bundle=`, which would reach `fatalBadPayload()` and report a damaged
artifact when it is merely the wrong kind.

**No version bump.** The flags word already exists (v5), and nothing about the
module data changes -- an empty payload is a payload the format already
permits. Note that `bundle_reader.cpp:147` rejects any bit it does not know,
so an old binary refuses a new container, which is the behaviour that wants
keeping.

- [ ] **Step 1: Write the failing test**

Add to `unittests/BundleFormatTest.cpp`, following how the existing
`AllowVmOptionsOverride` cases build a container:

```cpp
TEST(BundleFormatTest, NativeUnitsFlagRoundTrips) {
  BundleWriter writer(/* same construction the neighbouring tests use */);
  writer.setNativeUnits(true);
  /* ...add one module, serialize... */
  std::string bytes = writer.serialize();

  std::string error;
  auto reader = BundleReader::open(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(),
      kTestGenerationTag, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_TRUE(reader->hasNativeUnits());
  EXPECT_FALSE(reader->allowsVmOptionsOverride());
}

TEST(BundleFormatTest, NativeUnitsFlagDefaultsOff) {
  /* ...same, without setNativeUnits... */
  EXPECT_FALSE(reader->hasNativeUnits());
}

TEST(BundleFormatTest, NativeUnitsAndVmOverrideAreIndependentBits) {
  BundleWriter writer(/* ... */);
  writer.setNativeUnits(true);
  writer.setAllowVmOptionsOverride(true);
  /* ... */
  EXPECT_TRUE(reader->hasNativeUnits());
  EXPECT_TRUE(reader->allowsVmOptionsOverride());
}

/// A native container's JavaScript payloads are empty. The format permits a
/// zero-length payload but nothing has ever written one, so assert it
/// rather than assume it.
TEST(BundleFormatTest, ZeroLengthJavaScriptPayloadRoundTrips) {
  BundleWriter writer(/* ... */);
  writer.setNativeUnits(true);
  /* add a kJavaScript module whose payload is "" */
  /* serialize, open */
  EXPECT_EQ(0u, reader->payload(0).size());
  EXPECT_TRUE(reader->isRequirable(0));
}
```

Read the neighbouring tests in the file first and match their construction
helpers exactly; do not invent a `BundleWriter` API.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BundleFormatTest 2>&1 | tail -10
```

Expected: FAIL to compile, `setNativeUnits` / `hasNativeUnits` undeclared.

- [ ] **Step 3: Define the bit**

In `include/hermes/node-compat/bundle/bundle_format.h`, after
`kBundleFlagAllowVmOptionsOverride`:

```cpp
/// The container's JavaScript payloads are empty because its code is linked
/// into an executable as Static Hermes units rather than carried here.
///
/// Such a container is a build intermediate, not an artifact: the producer
/// writes it only because .incbin takes a path, and deletes it unless
/// --keep-temp keeps it. The bit exists for the case where --keep-temp did:
/// without it, `--bundle=` on that file reaches fatalBadPayload() and calls
/// a perfectly well-formed container damaged.
constexpr uint32_t kBundleFlagNativeUnits = 1u << 1;
```

Update the comment on `containerFlags` at line ~102, which currently says
"currently only kBundleFlagAllowVmOptionsOverride".

- [ ] **Step 4: Write and read it**

`bundle_writer.h`, beside `setAllowVmOptionsOverride`:

```cpp
  /// Record that this container's JavaScript payloads are empty and its
  /// code is linked. See kBundleFlagNativeUnits.
  void setNativeUnits(bool native);
```

`bundle_writer.h`: a `bool nativeUnits_ = false;` member in the class's
private section (~line 105, beside the other writer state -- not in the
`.cpp`). `bundle_writer.cpp`: the setter, and the header build at line ~242
becomes:

```cpp
  header.containerFlags =
      (allowVmOptionsOverride_ ? kBundleFlagAllowVmOptionsOverride : 0) |
      (nativeUnits_ ? kBundleFlagNativeUnits : 0);
```

`bundle_reader.cpp:147`, the unknown-bit rejection, gains the new bit:

```cpp
  constexpr uint32_t kKnownContainerFlags =
      kBundleFlagAllowVmOptionsOverride | kBundleFlagNativeUnits;
  if ((header->containerFlags & ~kKnownContainerFlags) != 0)
```

and beside `allowsVmOptionsOverride()`:

```cpp
bool BundleReader::hasNativeUnits() const {
  return (header_->containerFlags & kBundleFlagNativeUnits) != 0;
}
```

with the declaration and a doc comment in `bundle_reader.h`.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target BundleFormatTest 2>&1 | tail -3
cmake-build-asan/unittests/BundleFormatTest --gtest_filter='*NativeUnits*:*ZeroLength*'
```

Expected: PASS, 4 tests.

- [ ] **Step 6: Refuse a native container on the disk run path**

**Deferred to Task 15, deliberately.** Nothing in Task 8 can produce a
native container to point `--bundle=` at -- the producer does not exist
until Task 11 -- so a refusal implemented here would be untestable here, and
a task whose own steps cannot verify its deliverable is worse than a task
that does less. Task 15 both writes the code below and tests it against a
real `--keep-temp` container.

What Task 8 does implement is the reader half: `hasNativeUnits()` exists and
is round-tripped by the tests above, which is what Task 15 and Task 13 both
build on.

For reference, the code Task 15 adds to `lib/bundle/bundle_run.cpp`, in
`openBundle()` (line ~574), after `BundleReader::open()` succeeds and before
`file->release()`:

```cpp
  // openBundle() is the disk path; openEmbeddedBundle() is the linked copy.
  // They share BundleReader::open(), so the refusal has to be here -- doing
  // it in the reader would refuse every native executable, which is the one
  // thing that must work.
  if (reader->hasNativeUnits()) {
    *error = "hermes-node bundle: " + path +
        " holds no bytecode -- its code is linked into the executable that "
        "was built from it. Run that executable instead.";
    return false;
  }
```

`openForInspection()` is deliberately left accepting it, so a later `--dump`
needs no format change.

- [ ] **Step 7: Test the refusal**

Add to `test/build-native-errors.js` -- create the file now with just this
case; Task 12 adds the rest. It is **ungated**: no kit and no toolchain are
reached.

```js
// RUN: rm -rf %t && mkdir -p %t
// RUN: echo 'console.log("hi");' > %t/app.js
// RUN: %hermes-node --build-bundle=%t/app.hbb %t/app.js > /dev/null 2>&1
// RUN: %not %hermes-node --bundle=%t/app.hbb 2>&1 | %FileCheck --check-prefix=SANITY --allow-empty %s
//
// Deliberately ungated: every case here is refused before any toolchain is
// reached, so this is the coverage that survives a checkout with no kit.

// A normal container still runs. This RUN line exists so the negative case
// below cannot pass by refusing everything.
// SANITY-NOT: holds no bytecode
console.log('PASS');
```

Note: the sanity RUN line above uses `%not` against a container that
succeeds, so invert it -- write it as a plain run with `FileCheck`:

```js
// RUN: %hermes-node --bundle=%t/app.hbb | %FileCheck --check-prefix=SANITY %s
// SANITY: PASS
```

**Task 15** adds the native-container refusal and its test -- see this
task's Step 6. Not Task 12, which has no producer either.

- [ ] **Step 8: Run the whole bundle format suite**

```bash
cmake-build-asan/unittests/BundleFormatTest
cmake-build-asan/unittests/BundleToolsTest
```

Expected: all PASS. `BundleToolsTest` matters because `--dump` reads the
flags word.

- [ ] **Step 9: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Add kBundleFlagNativeUnits to the container header

A natively compiled program's container carries metadata and empty
JavaScript payloads; its code is linked into the executable. The
producer still writes that container to a file, because .incbin takes
a path, and --keep-temp retains it -- which is the one way such a
container reaches a filesystem where someone can hand it to --bundle=.
Without the bit that run reports a damaged artifact, which it is not.

The refusal sits in openBundle() rather than in BundleReader::open():
the embedded path shares that reader, so refusing there would refuse
every native executable. openForInspection() keeps accepting it, so a
later --dump needs no format change.

No version bump -- the flags word is already v5, and an empty payload
is one the format always permitted. Nothing had written one, so the
round trip is now asserted.
MSG
```

---

## Task 9: `lib/build-native` -- staging and command construction

**Files:**
- Create: `include/hermes/node-compat/build-native/build_native.h`
- Create: `lib/build-native/CMakeLists.txt`
- Create: `lib/build-native/staging.cpp`
- Create: `lib/build-native/native_compile.cpp`
- Create: `unittests/BuildNativeTest.cpp`
- Modify: the **root** `CMakeLists.txt` (there is no `lib/CMakeLists.txt`;
  libraries are added directly, `add_subdirectory(lib/...)` around line 123),
  and `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: `KitManifest` (Task 6), `JSLanguageFlags` and `wrapCJS` (Task 4),
  `CommandResult` (Task 5).
- Produces:
  ```cpp
  enum class OptLevel { O0, O1, O2, O3, Os };
  std::string nativeUnitName(uint32_t moduleIndex);
  std::string stagedSourcePath(const std::string &tempDir, uint32_t i);
  std::string stagedCPath(const std::string &tempDir, uint32_t i);
  std::string stagedObjectPath(const std::string &tempDir, uint32_t i);
  bool stageModule(const std::string &tempDir, uint32_t i,
                   std::string_view source, std::string *error);
  std::vector<std::string> buildShermesCommand(
      const std::string &shermesPath, const std::string &stagedPath,
      const std::string &outCPath, const std::string &sourceName,
      const std::string &unitName, bool typeScript, OptLevel opt);
  std::vector<std::string> buildCompileCommand(
      const KitManifest &manifest, const std::string &driver,
      bool driverIsClang, const std::string &cPath,
      const std::string &objPath, OptLevel opt);
  bool linkResponseFile(const std::vector<std::string> &objects,
                        std::string *out, std::string *error);
  ```
  Task 10 orchestrates them; Task 12 parses `OptLevel` from the command line.

**Header includes**, so later tasks do not have to rediscover them:
`build_native.h` includes `<hermes/node-compat/build-exe/build_exe.h>` (for
`CommandResult` and `ObjectFormat`) and
`<hermes/node-compat/build-exe/kit_manifest.h>` (for `KitManifest`).
`bundle_build.h` includes `build_native.h` in Task 11, for `OptLevel`.

- [ ] **Step 1: Write the failing test**

Create `unittests/BuildNativeTest.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-native/build_native.h>
#include <hermes/node-compat/bundle/cjs_wrapper.h>

#include "gtest/gtest.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace hermes::node_compat;

namespace {

bool has(const std::vector<std::string> &argv, const std::string &flag) {
  return std::find(argv.begin(), argv.end(), flag) != argv.end();
}

/// Index of \p flag, or -1.
int at(const std::vector<std::string> &argv, const std::string &flag) {
  auto it = std::find(argv.begin(), argv.end(), flag);
  return it == argv.end() ? -1 : (int)(it - argv.begin());
}

TEST(BuildNativeTest, UnitNamesArePaddedAndValid) {
  EXPECT_EQ("hn_m000000", nativeUnitName(0));
  EXPECT_EQ("hn_m000017", nativeUnitName(17));
  EXPECT_EQ("hn_m001483", nativeUnitName(1483));
  // isValidSHUnitName permits alphanumerics and underscore only.
  for (char c : nativeUnitName(42))
    EXPECT_TRUE(isalnum((unsigned char)c) || c == '_') << c;
  // Past six digits it must still be unique, not truncated.
  EXPECT_EQ("hn_m1234567", nativeUnitName(1234567));
}

/// Every row of the parity table the spec fixes, asserted on the argv,
/// because the alternative -- comparing program output -- cannot prove a
/// flag was passed, only that behaviour happened to match.
TEST(BuildNativeTest, ShermesCommandCarriesEveryParityFlag) {
  auto argv = buildShermesCommand(
      "/kit/shermes", "/tmp/b/0017.js", "/tmp/b/0017.c",
      "node_modules/foo/index.js", "hn_m000017", /*typeScript=*/false,
      OptLevel::O3);
  EXPECT_EQ("/kit/shermes", argv[0]);
  EXPECT_TRUE(has(argv, "-emit-c"));
  EXPECT_TRUE(has(argv, "-Xes6-block-scoping"));
  EXPECT_TRUE(has(argv, "-Xasync-generators"));
  EXPECT_TRUE(has(argv, "-sm-comment=off"));
  EXPECT_TRUE(has(argv, "-g2"));
  EXPECT_TRUE(has(argv, "-w"));
  EXPECT_TRUE(has(argv, "-exported-unit=hn_m000017"));
  EXPECT_TRUE(has(argv, "-source-name=node_modules/foo/index.js"));
  EXPECT_TRUE(has(argv, "-o"));
  EXPECT_EQ("/tmp/b/0017.c", argv[at(argv, "-o") + 1]);
  EXPECT_EQ("/tmp/b/0017.js", argv.back());
  EXPECT_FALSE(has(argv, "-transform-ts"));
}

TEST(BuildNativeTest, ShermesCommandAddsTransformTsOnlyForTypeScript) {
  auto argv = buildShermesCommand(
      "/kit/shermes", "/tmp/b/1.js", "/tmp/b/1.c", "a.ts", "hn_m000001",
      /*typeScript=*/true, OptLevel::O3);
  EXPECT_TRUE(has(argv, "-transform-ts"));
}

TEST(BuildNativeTest, ShermesOptLevelMapping) {
  auto level = [](OptLevel o) {
    auto argv = buildShermesCommand("s", "i.js", "o.c", "n", "u", false, o);
    for (const std::string &a : argv)
      if (a == "-O0" || a == "-Og" || a == "-Os" || a == "-O")
        return a;
    return std::string("<none>");
  };
  EXPECT_EQ("-O0", level(OptLevel::O0));
  EXPECT_EQ("-Og", level(OptLevel::O1));
  // shermes has four levels and -O is its highest, so O2 and O3 are the
  // same here and differ only on the cc side.
  EXPECT_EQ("-O", level(OptLevel::O2));
  EXPECT_EQ("-O", level(OptLevel::O3));
  EXPECT_EQ("-Os", level(OptLevel::Os));
}

/// -x c is mandatory and must precede the input: kit.manifest records the
/// C++ LINK driver, and a C++ driver compiles a .c file as C++ -- measured
/// as five hard errors on real generated code.
TEST(BuildNativeTest, CompileCommandForcesTheCLanguageBeforeTheInput) {
  KitManifest m;
  m.kitDir = "/kit";
  m.cc = "/usr/bin/clang++";
  m.ccFlags = {"-DNDEBUG", "-I/kit/include"};
  m.driverFlags = {"-arch", "arm64", "-isysroot", "/SDK"};
  auto argv = buildCompileCommand(m, "/usr/bin/clang++", /*driverIsClang=*/true,
                                  "/tmp/b/17.c", "/tmp/b/17.o", OptLevel::O3);
  int xc = at(argv, "-x");
  ASSERT_GE(xc, 0);
  EXPECT_EQ("c", argv[xc + 1]);
  int input = at(argv, "/tmp/b/17.c");
  ASSERT_GE(input, 0);
  EXPECT_LT(xc, input) << "-x c after the input does not apply to it";
  EXPECT_TRUE(has(argv, "-std=gnu11"));
  EXPECT_TRUE(has(argv, "-c"));
  EXPECT_TRUE(has(argv, "-O3"));
  EXPECT_TRUE(has(argv, "-DNDEBUG"));
  EXPECT_TRUE(has(argv, "-I/kit/include"));
  EXPECT_EQ("/tmp/b/17.o", argv[at(argv, "-o") + 1]);
  // The driver flags select a TARGET -- -arch, --target, -isysroot -- so
  // they must reach this compile as well as the assemble, or the object is
  // built for the host and the link cannot resolve it for the slice it was
  // not built for. buildAssembleCommand forwards them for exactly this
  // reason (build_exe.cpp:401).
  EXPECT_TRUE(has(argv, "-arch"));
  EXPECT_TRUE(has(argv, "arm64"));
  EXPECT_TRUE(has(argv, "-isysroot"));
  // Forwarding the whole list means link-only flags reach a compile that has
  // no use for them; this is the same suppression the assemble step uses,
  // and it is Clang-only.
  EXPECT_TRUE(has(argv, "-Qunused-arguments"));
}

TEST(BuildNativeTest, CompileCommandOmitsQunusedForNonClang) {
  KitManifest m;
  m.cc = "g++";
  auto argv = buildCompileCommand(m, "g++", /*driverIsClang=*/false, "a.c",
                                  "a.o", OptLevel::O3);
  // GCC rejects it outright, so guessing yes would turn an unknown driver
  // into a hard failure on first use.
  EXPECT_FALSE(has(argv, "-Qunused-arguments"));
}

TEST(BuildNativeTest, CompileOptLevelMapping) {
  KitManifest m;
  m.cc = "cc";
  auto level = [&m](OptLevel o) {
    auto argv = buildCompileCommand(m, "cc", /*driverIsClang=*/true, "a.c",
                                    "a.o", o);
    for (const std::string &a : argv)
      if (a.rfind("-O", 0) == 0)
        return a;
    return std::string("<none>");
  };
  EXPECT_EQ("-O0", level(OptLevel::O0));
  EXPECT_EQ("-O1", level(OptLevel::O1));
  EXPECT_EQ("-O2", level(OptLevel::O2));
  EXPECT_EQ("-O3", level(OptLevel::O3));
  EXPECT_EQ("-Os", level(OptLevel::Os));
}

TEST(BuildNativeTest, StagedPathsAreFlatAndDistinct) {
  EXPECT_EQ("/tmp/b/000017.js", stagedSourcePath("/tmp/b", 17));
  EXPECT_EQ("/tmp/b/000017.c", stagedCPath("/tmp/b", 17));
  EXPECT_EQ("/tmp/b/000017.o", stagedObjectPath("/tmp/b", 17));
  // Flat, not a tree mirroring the identity: -source-name carries the name
  // that matters, so the staged filename means nothing.
  EXPECT_EQ(std::string::npos, stagedSourcePath("/tmp/b", 17).find("node_modules"));
}

TEST(BuildNativeTest, LinkResponseFileQuotesAndOrders) {
  std::string out, error;
  ASSERT_TRUE(linkResponseFile(
      {"/tmp/b/payload.o", "/tmp/b/000000.o", "/Some Dir/000001.o"}, &out,
      &error))
      << error;
  // The payload object first: buildLinkCommand places its single blob
  // argument before the entry object and the archives, and lazy archive
  // resolution depends on that order.
  EXPECT_EQ(0u, out.rfind("\"/tmp/b/payload.o\"", 0));
  // A space is quoted, not refused: checkIncbinPath permits spaces and
  // macOS paths have them, so refusing here would reject containers the
  // assembler accepts.
  EXPECT_NE(std::string::npos, out.find("\"/Some Dir/000001.o\""));
  EXPECT_EQ(3u, std::count(out.begin(), out.end(), '\n'));
}

TEST(BuildNativeTest, LinkResponseFileRefusesUnquotablePaths) {
  std::string out, error;
  // The same four characters checkIncbinPath rejects, so the two agree.
  for (const char *bad : {"/tmp/a\"b.o", "/tmp/a\\b.o", "/tmp/a\rb.o",
                          "/tmp/a\nb.o"}) {
    error.clear();
    EXPECT_FALSE(linkResponseFile({bad}, &out, &error)) << bad;
    EXPECT_NE(std::string::npos, error.find("a")) << bad;
  }
}

TEST(BuildNativeTest, StageModuleWritesTheWrappedSource) {
  std::string dir = makeTempDir();  // use the helper the other tests use
  std::string error;
  ASSERT_TRUE(stageModule(dir, 3, "module.exports = 1;\n", &error)) << error;
  std::ifstream in(stagedSourcePath(dir, 3));
  std::ostringstream body;
  body << in.rdbuf();
  EXPECT_EQ(wrapCJS("module.exports = 1;\n"), body.str());
  EXPECT_EQ(0u, body.str().rfind(std::string(kCJSWrapperPrefix), 0));
}

} // namespace
```

If `unittests/` has no `makeTempDir` helper, copy the pattern
`BuildExeTest.cpp` uses for its temp directories rather than adding a new
one.

- [ ] **Step 2: Create the library skeleton so the test compiles and fails**

`include/hermes/node-compat/build-native/build_native.h` with the
declarations from the **Interfaces** block above, each with a doc comment.
The ones carrying non-obvious reasoning:

```cpp
/// How hard both compilers try. One knob for two tools so a single number
/// means one thing; shermes has four levels and -O is its highest, which is
/// why O2 and O3 differ only on the cc side.
enum class OptLevel { O0, O1, O2, O3, Os };

/// The Static Hermes unit name for container module \p moduleIndex:
/// "hn_m" and the index, zero-padded to six digits.
///
/// The index rather than the identity, because isValidSHUnitName permits
/// alphanumerics and underscore only -- an identity would have to be
/// mangled, and a mangling is a second thing that can collide. The index is
/// already unique, already the table's key, and already in the container.
std::string nativeUnitName(uint32_t moduleIndex);

/// The shermes argv that turns one staged module into generated C.
///
/// \p sourceName is the module's container identity and is what -source-name
/// puts into the unit's source-location table, so it is what stack traces
/// name. The staged path is a flat temp file and never appears in a trace.
///
/// Every language flag here is load-bearing and silent when missing. The
/// bytecode compiler enables ES6 block scoping and async generators;
/// shermes defaults both off, and block scoping off changes what a
/// let-in-loop closure captures with no diagnostic at build or run time --
/// measured as 3,3,3 where the same program prints 0,1,2. See
/// JSLanguageFlags in cjs_wrapper.h.
std::vector<std::string> buildShermesCommand(...);

/// The cc argv that turns generated C into an object.
///
/// -x c precedes the input and is mandatory: kit.manifest's `cc` is the C++
/// LINK driver, and a C++ driver compiles a .c file as C++ -- five hard
/// errors on real generated code. A separate C driver is deliberately not
/// recorded, because the manifest's sysroot, -arch and sanitizer flags came
/// from this one.
///
/// -std=gnu11 rather than the driver's default so the dialect cannot drift,
/// and gnu rather than c11 because the SH headers use zero-length arrays.
std::vector<std::string> buildCompileCommand(...);
```

`lib/build-native/CMakeLists.txt`, modelled on `lib/build-exe/CMakeLists.txt`:

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Producer for `build-native`. VM-free by construction: it stages sources,
# builds command lines and drives subprocesses, so it links the format layer
# and the build-exe layer and nothing else. That is what lets
# BuildNativeTest run with no runtime and no Hermes compiler -- the same
# property BundleFormatTest, BundleToolsTest and BuildExeTest rely on.
add_hermes_library(hermesNodeBuildNative STATIC
  staging.cpp
  native_compile.cpp
  job_pool.cpp
  unit_table.cpp
)

target_include_directories(hermesNodeBuildNative
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(hermesNodeBuildNative PUBLIC
  hermesNodeBundle
  hermesNodeBuildExe)
```

Create `job_pool.cpp` and `unit_table.cpp` as empty-but-compiling files for
now; Task 10 fills them. Add `add_subdirectory(lib/build-native)` to the
**root** `CMakeLists.txt` beside the other `add_subdirectory(lib/...)` lines
(around line 123) -- there is no `lib/CMakeLists.txt` -- after
`lib/build-exe` and `lib/bundle`, so its link dependencies exist. Add a
`BuildNativeTest` target to `unittests/CMakeLists.txt`, following the
`BuildExeTest` entry.

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake -B cmake-build-asan >/dev/null
cmake --build cmake-build-asan --target BuildNativeTest 2>&1 | tail -10
```

Expected: FAIL -- link errors for the undefined functions.

- [ ] **Step 4: Implement staging**

`lib/build-native/staging.cpp`:

```cpp
#include <hermes/node-compat/build-native/build_native.h>
#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/cjs_wrapper.h>

#include <cstdio>
#include <iomanip>
#include <sstream>

namespace hermes {
namespace node_compat {

namespace {
/// The six-digit stem every staged file for a module shares.
std::string stem(uint32_t moduleIndex) {
  std::ostringstream os;
  os << std::setw(6) << std::setfill('0') << moduleIndex;
  return os.str();
}
} // namespace

std::string nativeUnitName(uint32_t moduleIndex) {
  return "hn_m" + stem(moduleIndex);
}

std::string stagedSourcePath(const std::string &tempDir, uint32_t i) {
  return tempDir + "/" + stem(i) + ".js";
}

std::string stagedCPath(const std::string &tempDir, uint32_t i) {
  return tempDir + "/" + stem(i) + ".c";
}

std::string stagedObjectPath(const std::string &tempDir, uint32_t i) {
  return tempDir + "/" + stem(i) + ".o";
}

bool stageModule(
    const std::string &tempDir,
    uint32_t moduleIndex,
    std::string_view source,
    std::string *error) {
  // The same wrapper the scanner parsed and the bytecode producer compiles,
  // from the one header both already share -- so what shermes sees is what
  // the scan resolved require() bindings in.
  std::string wrapped = wrapCJS(source);
  return atomicWriteFile(
      stagedSourcePath(tempDir, moduleIndex), wrapped, error);
}

} // namespace node_compat
} // namespace hermes
```

Check the real name and signature of the atomic-write helper in
`include/hermes/node-compat/bundle/atomic_write.h` and use it; if its shape
does not fit (it may take different arguments), a plain
`std::ofstream` write is acceptable here and worth a one-line comment
saying why the atomic version was not used.

Note `nativeUnitName` must NOT truncate past six digits -- `std::setw(6)` is
a minimum width, which is the behaviour the test asserts for 1234567.

- [ ] **Step 5: Implement the two command builders**

`lib/build-native/native_compile.cpp`:

```cpp
#include <hermes/node-compat/build-native/build_native.h>
#include <hermes/node-compat/bundle/cjs_wrapper.h>

namespace hermes {
namespace node_compat {

namespace {

const char *shermesOptFlag(OptLevel opt) {
  switch (opt) {
    case OptLevel::O0: return "-O0";
    case OptLevel::O1: return "-Og";
    // shermes has four levels and -O is its highest; O2 and O3 differ only
    // on the cc side.
    case OptLevel::O2:
    case OptLevel::O3: return "-O";
    case OptLevel::Os: return "-Os";
  }
  return "-O";
}

const char *ccOptFlag(OptLevel opt) {
  switch (opt) {
    case OptLevel::O0: return "-O0";
    case OptLevel::O1: return "-O1";
    case OptLevel::O2: return "-O2";
    case OptLevel::O3: return "-O3";
    case OptLevel::Os: return "-Os";
  }
  return "-O3";
}

} // namespace

std::vector<std::string> buildShermesCommand(
    const std::string &shermesPath,
    const std::string &stagedPath,
    const std::string &outCPath,
    const std::string &sourceName,
    const std::string &unitName,
    bool typeScript,
    OptLevel opt) {
  std::vector<std::string> argv{
      shermesPath,
      "-emit-c",
      // Source locations, which is what puts identity:line:column into a
      // stack trace. At -g0 a native frame prints "(native)" with no
      // location at all.
      "-g2",
      shermesOptFlag(opt),
      // A CommonJS module reports primordials, internalBinding and process
      // as undeclared globals -- three in the first hundred lines of
      // libjs-node/net.js. The bytecode path prints none of them either.
      "-w",
      // Parity, not caution: shermes defaults to -sm-comment=file, so a
      // published package's //# sourceMappingURL= would make it load a map
      // and rewrite the names in the location table, while the bytecode
      // path passes sourceMap="" and ignores the comment.
      "-sm-comment=off",
  };

  // See JSLanguageFlags in cjs_wrapper.h. Absent, these are silent: block
  // scoping off makes every let-in-loop closure capture the wrong binding.
  if (kJSLanguageFlags.es6BlockScoping)
    argv.push_back("-Xes6-block-scoping");
  if (kJSLanguageFlags.asyncGenerators)
    argv.push_back("-Xasync-generators");
  // kJSLanguageFlags.generators has no shermes flag: generators are on
  // unconditionally, in both compilers.
  if (typeScript)
    argv.push_back("-transform-ts");

  argv.push_back("-source-name=" + sourceName);
  argv.push_back("-exported-unit=" + unitName);
  argv.push_back("-o");
  argv.push_back(outCPath);
  argv.push_back(stagedPath);
  return argv;
}

std::vector<std::string> buildCompileCommand(
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    const std::string &cPath,
    const std::string &objPath,
    OptLevel opt) {
  std::vector<std::string> argv{driver};
  // The same suppression buildAssembleCommand uses, and for the same
  // reason: the whole driver-flag list is forwarded below, so link-only
  // flags reach a compile that has no use for them. Clang-only; GCC
  // rejects it.
  if (driverIsClang)
    argv.push_back("-Qunused-arguments");
  // Before the input, because -x applies to the files that follow it. The
  // manifest's driver is the C++ LINK driver, which would otherwise compile
  // this .c as C++ and fail.
  argv.push_back("-x");
  argv.push_back("c");
  argv.push_back("-std=gnu11");
  argv.push_back(ccOptFlag(opt));
  argv.push_back("-c");
  // The driver flags SELECT A TARGET: -arch on a universal macOS kit, a
  // --target or -isysroot on a cross-compiling one. Compile without them
  // and the object is host-only, and the link cannot resolve it for the
  // slice it was not built for. The whole list is forwarded rather than a
  // hand-picked subset, for the reason buildAssembleCommand gives: a
  // hand-maintained list of "which flags select a target, per driver" is
  // the thing the manifest exists to abolish.
  for (const std::string &flag : manifest.driverFlags)
    argv.push_back(flag);
  for (const std::string &flag : manifest.ccFlags)
    argv.push_back(flag);
  argv.push_back(cPath);
  argv.push_back("-o");
  argv.push_back(objPath);
  return argv;
}

bool linkResponseFile(
    const std::vector<std::string> &objects,
    std::string *out,
    std::string *error) {
  out->clear();
  for (const std::string &path : objects) {
    // Response-file syntax is neither shell nor assembler: GNU ld and ld64
    // split on whitespace and both honour " quoting and \ escaping. So a
    // path is written quoted, which makes a space harmless -- and it has to
    // be, since checkIncbinPath() permits spaces and a macOS path routinely
    // has them. These four cannot be expressed and are refused instead, the
    // same four checkIncbinPath() refuses, so the two agree about what a
    // usable path is.
    for (char c : path) {
      if (c == '"' || c == '\\' || c == '\r' || c == '\n') {
        *error = "cannot put this path in a linker response file: " + path;
        return false;
      }
    }
    out->push_back('"');
    out->append(path);
    out->append("\"\n");
  }
  return true;
}

} // namespace node_compat
} // namespace hermes
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target BuildNativeTest 2>&1 | tail -3
cmake-build-asan/unittests/BuildNativeTest
```

Expected: PASS, 11 tests.

- [ ] **Step 7: Prove the constructed commands actually work**

The unit tests assert the shape; this asserts the shape is right. Run both
commands by hand against the real kit:

```bash
rm -rf /tmp/bn && mkdir -p /tmp/bn
printf 'module.exports = 1;\n' > /tmp/bn/src.js
python3 - <<'PY'
import io
prefix = "(function(exports, require, module, __filename, __dirname) {"
src = open('/tmp/bn/src.js').read()
open('/tmp/bn/000000.js','w').write(prefix + src + "\n})")
PY
KIT=$(pwd)/cmake-build-asan/kit
$KIT/shermes -emit-c -g2 -O -w -sm-comment=off \
  -Xes6-block-scoping -Xasync-generators \
  -source-name=src.js -exported-unit=hn_m000000 \
  -o /tmp/bn/000000.c /tmp/bn/000000.js
CC=$(grep '^cc: ' $KIT/kit.manifest | cut -d' ' -f2)
$CC -x c -std=gnu11 -O3 -c \
  $(grep '^ccflag: ' $KIT/kit.manifest | cut -d' ' -f2-) \
  /tmp/bn/000000.c -o /tmp/bn/000000.o
nm -g /tmp/bn/000000.o | grep sh_export_hn_m000000
```

Expected: both commands exit 0 and the symbol is present (with a leading
underscore on macOS).

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Add build-native staging and command building

Every language flag on the shermes line is asserted by the unit test
rather than inferred from program output, because output cannot prove
a flag was passed -- only that behaviour happened to match. The one
that matters most is silent: without -Xes6-block-scoping a let-in-loop
closure captures the wrong binding, measured as 3,3,3 where the same
program prints 0,1,2, with no diagnostic anywhere.

-x c precedes the input because the kit records the C++ link driver,
which compiles a .c file as C++: five hard errors on real generated
code. Recording a second, C driver would reintroduce the kit-versus-
driver mismatch driverCandidates() exists to prevent.

Staging is flat. -source-name carries the name that reaches stack
traces, so the staged filename means nothing, and a tree mirroring
each identity would need a working-directory change per child.
MSG
```

---

## Task 10: `lib/build-native` -- the job pool and the unit table

**Files:**
- Modify: `include/hermes/node-compat/build-native/build_native.h`
- Create content in: `lib/build-native/job_pool.cpp`, `lib/build-native/unit_table.cpp`
- Test: `unittests/BuildNativeTest.cpp`

**Interfaces:**
- Consumes: Task 9's builders, Task 5's `CommandResult`, Task 7's
  `payloadAssembly`.
- Produces:
  ```cpp
  struct NativeModuleJob {
    uint32_t moduleIndex = 0;
    std::string identity;     // what -source-name records
    std::string source;       // unwrapped module text
    bool typeScript = false;
  };
  enum class NativeStage { Stage, Shermes, Compile };
  struct NativeModuleResult {
    uint32_t moduleIndex = 0;
    bool ok = false;
    NativeStage failedStage = NativeStage::Stage;
    std::string diagnostics;                 // captured child output
    std::vector<std::string> failedCommand;  // empty for a staging failure
    std::string message;                     // one-line reason
    size_t sourceBytes = 0, cBytes = 0, objectBytes = 0;
    double milliseconds = 0;
  };
  using CommandRunner =
      std::function<CommandResult(const std::vector<std::string> &)>;
  std::vector<NativeModuleResult> compileModules(
      const std::vector<NativeModuleJob> &jobs, const std::string &tempDir,
      const std::string &shermesPath, const KitManifest &manifest,
      const std::string &driver, bool driverIsClang, OptLevel opt,
      unsigned parallelism, const CommandRunner &run);
  std::vector<std::string> unitSymbolTable(
      uint32_t moduleCount, const std::vector<NativeModuleJob> &jobs);
  ```
  Task 11 calls `compileModules` and `unitSymbolTable`.

**The runner is injected** so the pool, the ordering and the failure
classification are testable with no toolchain and no filesystem races. The
real caller passes `runCommandCaptured`.

- [ ] **Step 1: Write the failing test**

Add to `unittests/BuildNativeTest.cpp`:

```cpp
/// A runner that records what it was asked to do and answers from a script.
struct FakeRunner {
  std::mutex mu;
  std::vector<std::vector<std::string>> calls;
  /// Whether a successful call creates the file its -o names.
  bool writeOutputs = true;
  /// argv[0] substring -> result.
  std::function<CommandResult(const std::vector<std::string> &)> behaviour;

  CommandResult operator()(const std::vector<std::string> &argv) {
    {
      std::lock_guard<std::mutex> lock(mu);
      calls.push_back(argv);
    }
    CommandResult r = behaviour ? behaviour(argv) : CommandResult{};
    // A successful compiler writes its -o file. The pool checks for it --
    // a tool that exits 0 and produces nothing is a failure -- so a fake
    // that does not would make every success look like that bug. Set
    // writeOutputs=false to reproduce that bug on purpose.
    if (r.ok() && writeOutputs) {
      auto it = std::find(argv.begin(), argv.end(), "-o");
      if (it != argv.end() && std::next(it) != argv.end())
        std::ofstream(*std::next(it)) << "x";
    }
    return r;
  }
};

std::vector<NativeModuleJob> threeJobs() {
  return {{0, "app.js", "require('./a');\n", false},
          {1, "a.js", "module.exports = 1;\n", false},
          {2, "b.ts", "export const x: number = 1;\n", true}};
}

TEST(BuildNativeTest, CompileModulesRunsTwoCommandsPerModule) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  auto results = compileModules(threeJobs(), dir, "/kit/shermes", m, "cc",
                                /*driverIsClang=*/true, OptLevel::O3, 1,
                                std::ref(fake));
  ASSERT_EQ(3u, results.size());
  for (const auto &r : results)
    EXPECT_TRUE(r.ok) << r.message;
  EXPECT_EQ(6u, fake.calls.size());
}

TEST(BuildNativeTest, CompileModulesStagesTheWrappedSource) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  compileModules(threeJobs(), dir, "/kit/shermes", m, "cc",
                 /*driverIsClang=*/true, OptLevel::O3, 1, std::ref(fake));
  std::ifstream in(stagedSourcePath(dir, 1));
  std::ostringstream body;
  body << in.rdbuf();
  EXPECT_EQ(wrapCJS("module.exports = 1;\n"), body.str());
}

TEST(BuildNativeTest, CompileModulesResultsAreInModuleIndexOrder) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  // Parallelism must not reorder results: the unit table is indexed by
  // module index and the verbose log reads better in order.
  auto results = compileModules(threeJobs(), dir, "/kit/shermes", m, "cc",
                                /*driverIsClang=*/true, OptLevel::O3, 4,
                                std::ref(fake));
  ASSERT_EQ(3u, results.size());
  EXPECT_EQ(0u, results[0].moduleIndex);
  EXPECT_EQ(1u, results[1].moduleIndex);
  EXPECT_EQ(2u, results[2].moduleIndex);
}

TEST(BuildNativeTest, CompileModulesReportsTheStageThatFailed) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &argv) {
    CommandResult r;
    bool isShermes = argv[0].find("shermes") != std::string::npos;
    if (isShermes && argv.back().find("000001") != std::string::npos) {
      r.outcome = CommandResult::Outcome::Exited;
      r.status = 1;
      r.output = "a.js:1:1: error: nope\n";
    }
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  auto results = compileModules(threeJobs(), dir, "/kit/shermes", m, "cc",
                                /*driverIsClang=*/true, OptLevel::O3, 1,
                                std::ref(fake));
  EXPECT_TRUE(results[0].ok);
  EXPECT_FALSE(results[1].ok);
  EXPECT_EQ(NativeStage::Shermes, results[1].failedStage);
  EXPECT_NE(std::string::npos, results[1].diagnostics.find("error: nope"));
  EXPECT_FALSE(results[1].failedCommand.empty());
  EXPECT_TRUE(results[2].ok);
}

TEST(BuildNativeTest, CompileModulesDoesNotRunCcWhenShermesFailed) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &argv) {
    CommandResult r;
    if (argv[0].find("shermes") != std::string::npos)
      r.status = 1;
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  compileModules({threeJobs()[0]}, dir, "/kit/shermes", m, "cc",
                 /*driverIsClang=*/true, OptLevel::O3, 1, std::ref(fake));
  ASSERT_EQ(1u, fake.calls.size());
  EXPECT_NE(std::string::npos, fake.calls[0][0].find("shermes"));
}

TEST(BuildNativeTest, CompileModulesReportsASignalDistinctly) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &) {
    CommandResult r;
    r.outcome = CommandResult::Outcome::Signalled;
    r.status = SIGSEGV;
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  auto results = compileModules({threeJobs()[0]}, dir, "/kit/shermes", m, "cc",
                                /*driverIsClang=*/true, OptLevel::O3, 1,
                                std::ref(fake));
  EXPECT_FALSE(results[0].ok);
  EXPECT_NE(std::string::npos, results[0].message.find("signal"));
}

TEST(BuildNativeTest, CompileModulesFailsWhenAToolWritesNoOutput) {
  std::string dir = makeTempDir();
  KitManifest m;
  m.cc = "cc";
  // Exits 0 and writes nothing. Without the existence check this counts as
  // success and the missing object surfaces much later as an undefined
  // symbol at link time, attributed to nothing.
  FakeRunner silent;
  silent.writeOutputs = false;
  auto results = compileModules({threeJobs()[0]}, dir, "/kit/shermes", m, "cc",
                                /*driverIsClang=*/true, OptLevel::O3, 1,
                                std::ref(silent));
  EXPECT_FALSE(results[0].ok);
  EXPECT_EQ(NativeStage::Shermes, results[0].failedStage);
  EXPECT_NE(std::string::npos, results[0].message.find("no output"));
}

TEST(BuildNativeTest, UnitSymbolTableIsIndexedByModuleIndexWithHoles) {
  // Five container modules, of which 0, 1 and 3 are compiled JavaScript.
  std::vector<NativeModuleJob> jobs = {
      {0, "app.js", "", false}, {1, "a.js", "", false}, {3, "c.js", "", false}};
  auto table = unitSymbolTable(5, jobs);
  ASSERT_EQ(5u, table.size());
  EXPECT_EQ("hn_m000000", table[0]);
  EXPECT_EQ("hn_m000001", table[1]);
  EXPECT_EQ("", table[2]);  // JSON, an addon, or a resolve-only package.json
  EXPECT_EQ("hn_m000003", table[3]);
  EXPECT_EQ("", table[4]);
}
```

Add `#include <csignal>`, `<functional>`, `<mutex>` to the test file.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target BuildNativeTest 2>&1 | tail -10
```

Expected: FAIL, `compileModules` and `unitSymbolTable` undeclared.

- [ ] **Step 3: Declare the types**

Add the **Interfaces** block above to `build_native.h`, with doc comments.
The ones worth writing out:

```cpp
/// Where a module's compile stopped. The distinction is what decides
/// whether a build continues: see the failure policy in the design.
enum class NativeStage { Stage, Shermes, Compile };

/// Runs an argv and reports what happened. Injected rather than called
/// directly so the pool, the ordering and the failure classification are
/// testable with no toolchain; the real caller passes runCommandCaptured.
using CommandRunner =
    std::function<CommandResult(const std::vector<std::string> &)>;

/// Compiles every job, at most \p parallelism at a time, and returns one
/// result per job IN JOB ORDER regardless of completion order -- the unit
/// table is indexed by module index, and a verbose log that jumps around is
/// harder to read for no gain.
///
/// A module whose shermes step fails does not reach cc: the C file it would
/// compile does not exist, and a second failure for the same module would
/// only bury the first.
std::vector<NativeModuleResult> compileModules(...);

/// The per-module-index symbol table payloadAssembly() wants: the unit name
/// for a module that was compiled, an empty string for every other record
/// in the container -- a JSON module, a native addon, a resolve-only
/// package.json.
///
/// Indexed by container module index with holes rather than compacted,
/// because the index is how bundleLoadCallback finds an entry and a second
/// mapping is a second thing that can be wrong.
std::vector<std::string> unitSymbolTable(
    uint32_t moduleCount,
    const std::vector<NativeModuleJob> &jobs);
```

- [ ] **Step 4: Implement the pool**

`lib/build-native/job_pool.cpp`:

```cpp
#include <hermes/node-compat/build-native/build_native.h>

#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <thread>

namespace hermes {
namespace node_compat {

namespace {

size_t fileSize(const std::string &path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
}

/// A one-line reason for a failed child, in the words the producer prints.
std::string describe(const CommandResult &r, const char *tool) {
  switch (r.outcome) {
    case CommandResult::Outcome::Exited:
      return std::string(tool) + " failed with exit status " +
          std::to_string(r.status);
    case CommandResult::Outcome::Signalled:
      return std::string(tool) + " was killed by signal " +
          std::to_string(r.status);
    case CommandResult::Outcome::SpawnFailed:
      return std::string("cannot run ") + tool + ": " + strerror(r.status);
    case CommandResult::Outcome::WaitFailed:
      return std::string("waiting for ") + tool + ": " + strerror(r.status);
  }
  return tool;
}

/// One module, start to object file.
NativeModuleResult runOne(
    const NativeModuleJob &job,
    const std::string &tempDir,
    const std::string &shermesPath,
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    OptLevel opt,
    const CommandRunner &run) {
  NativeModuleResult result;
  result.moduleIndex = job.moduleIndex;
  result.sourceBytes = job.source.size();
  auto start = std::chrono::steady_clock::now();

  std::string error;
  if (!stageModule(tempDir, job.moduleIndex, job.source, &error)) {
    result.failedStage = NativeStage::Stage;
    result.message = error;
    return result;
  }

  std::string cPath = stagedCPath(tempDir, job.moduleIndex);
  std::vector<std::string> shermesArgv = buildShermesCommand(
      shermesPath,
      stagedSourcePath(tempDir, job.moduleIndex),
      cPath,
      job.identity,
      nativeUnitName(job.moduleIndex),
      job.typeScript,
      opt);
  CommandResult sh = run(shermesArgv);
  if (!sh.ok()) {
    result.failedStage = NativeStage::Shermes;
    result.diagnostics = sh.output;
    result.failedCommand = shermesArgv;
    result.message = describe(sh, "shermes");
    return result;
  }
  // A tool that exits 0 without producing its output is a failure of THIS
  // module at THIS stage. fileSize() cannot tell "missing" from "empty", so
  // check existence: without this the module sails through and the absence
  // surfaces much later as an undefined symbol at link time, attributed to
  // nothing.
  result.cBytes = fileSize(cPath);
  if (result.cBytes == 0) {
    result.failedStage = NativeStage::Shermes;
    result.failedCommand = shermesArgv;
    result.diagnostics = sh.output;
    result.message = "shermes exited 0 but wrote no output file";
    return result;
  }

  std::string objPath = stagedObjectPath(tempDir, job.moduleIndex);
  std::vector<std::string> ccArgv =
      buildCompileCommand(manifest, driver, driverIsClang, cPath, objPath, opt);
  CommandResult cc = run(ccArgv);
  if (!cc.ok()) {
    result.failedStage = NativeStage::Compile;
    result.diagnostics = cc.output;
    result.failedCommand = ccArgv;
    result.message = describe(cc, "the C compiler");
    return result;
  }
  result.objectBytes = fileSize(objPath);
  if (result.objectBytes == 0) {
    result.failedStage = NativeStage::Compile;
    result.failedCommand = ccArgv;
    result.diagnostics = cc.output;
    result.message = "the C compiler exited 0 but wrote no object file";
    return result;
  }

  result.ok = true;
  result.milliseconds =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();
  return result;
}

} // namespace

std::vector<NativeModuleResult> compileModules(
    const std::vector<NativeModuleJob> &jobs,
    const std::string &tempDir,
    const std::string &shermesPath,
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    OptLevel opt,
    unsigned parallelism,
    const CommandRunner &run) {
  // Slots pre-sized and written by index, so results come back in job order
  // whatever order the workers finish in -- no mutex on the output, and no
  // sort afterwards.
  std::vector<NativeModuleResult> results(jobs.size());

  if (parallelism == 0)
    parallelism = 1;
  unsigned workers =
      (unsigned)std::min<size_t>(parallelism, std::max<size_t>(jobs.size(), 1));

  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= jobs.size())
        return;
      results[i] =
          runOne(jobs[i], tempDir, shermesPath, manifest, driver,
                 driverIsClang, opt, run);
    }
  };

  if (workers <= 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i)
      pool.emplace_back(worker);
    for (std::thread &t : pool)
      t.join();
  }
  return results;
}

std::vector<std::string> unitSymbolTable(
    uint32_t moduleCount,
    const std::vector<NativeModuleJob> &jobs) {
  std::vector<std::string> table(moduleCount);
  for (const NativeModuleJob &job : jobs)
    if (job.moduleIndex < moduleCount)
      table[job.moduleIndex] = nativeUnitName(job.moduleIndex);
  return table;
}

} // namespace node_compat
} // namespace hermes
```

Add `#include <cstring>` for `strerror` and `#include <algorithm>`.

- [ ] **Step 5: Make `unit_table.cpp` the assembly writer**

`lib/build-native/unit_table.cpp` holds the one function that turns a
finished build into the generated `.s`, so Task 11 has a single call:

```cpp
#include <hermes/node-compat/build-exe/build_exe.h>
#include <hermes/node-compat/build-native/build_native.h>

namespace hermes {
namespace node_compat {

std::string nativePayloadAssembly(
    const std::string &containerPath,
    const std::vector<std::string> &unitSymbols) {
  // One generated .s carries both the container and the table, so the two
  // cannot get out of step and the link gains one object rather than two.
  return payloadAssembly(containerPath, unitSymbols, hostObjectFormat());
}

} // namespace node_compat
} // namespace hermes
```

Declare it in `build_native.h`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target BuildNativeTest 2>&1 | tail -3
cmake-build-asan/unittests/BuildNativeTest
```

Expected: PASS, 15 tests.

- [ ] **Step 7: Run it under the thread sanitizer's eye**

The pool writes `results[i]` from several threads. Distinct indices make
that safe, but assert it rather than reason about it -- the ASAN build will
catch an out-of-range write, and running the ordering test repeatedly will
catch a race in the `FakeRunner`:

```bash
for i in $(seq 1 20); do
  cmake-build-asan/unittests/BuildNativeTest \
    --gtest_filter='*CompileModulesResultsAreInModuleIndexOrder*' \
    > /dev/null || echo "FAILED on run $i"
done
echo done
```

Expected: no failures.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Add the build-native job pool and unit table

The command runner is injected so the pool, the result ordering and
the failure classification are testable with no toolchain; the real
caller passes runCommandCaptured.

Results are written into pre-sized slots by index rather than
appended, so they come back in job order whatever order the workers
finish in. The unit table is indexed by container module index, the
verbose log reads in order, and neither needs a sort or a mutex.

A module whose shermes step fails does not reach the C compiler: the
file it would compile does not exist, and a second failure for the
same module only buries the first.
MSG
```

---

## Task 11: Native mode in the producer

**Files:**
- Create: `lib/bundle/bundle_build_internal.h`
- Create: `lib/bundle/bundle_build_native.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_build.h`
- Modify: `lib/bundle/bundle_build.cpp` (steps 4 and 5; see below)
- Modify: `lib/bundle/CMakeLists.txt`
- Test: covered end to end by Task 13; the pure parts are already covered by
  Tasks 9 and 10.

**`buildBundle` is the public function itself** (`bundle_build.cpp:725`),
not a thin wrapper around an internal one -- so there is no existing
internal entry point to extend, and anything declared inside
`bundle_build.cpp` cannot be named from another translation unit. Hence the
private header below. Do not try to reach into the `.cpp`.

**Two things the native path needs that `buildBundle` computes locally and
does not return:** the bundle root (`rootPath`, `bundle_build.cpp:1198`) and
the stub count. Both come back in the result struct.

**Addon sidecars are the trap.** `sidecarDir` is derived from the output
path -- `fs::path(outPath).parent_path()` at `bundle_build.cpp:1265` -- so a
native build writing its container to `<tempDir>/container.hbb` would copy
every native addon into the temp directory and then delete it with the rest
of the temp tree, while the produced executable looks for sidecars beside
*itself*. The sidecar destination therefore becomes a separate parameter.

**Layering correction to the spec.** The spec puts the orchestration in
`lib/build-native/` and says that library stays VM-free. It cannot: the
orchestration has to call `buildBundle`, which lives in
`hermesNodeBundleBuild` and links `hermesvm_a` for the parser. So the split
is the other way round --

- `hermesNodeBuildNative` keeps **only** the pure helpers from Tasks 9 and 10
  (staging, argv construction, the pool, the symbol table). It stays VM-free,
  which is what keeps `BuildNativeTest` running with no runtime.
- `buildNativeExecutable()` lives in **`lib/bundle/bundle_build_native.cpp`**,
  part of `hermesNodeBundleBuild`, which gains
  `hermesNodeBuildNative` and `hermesNodeBuildExe` as link dependencies.

The property the spec actually wanted -- `BuildNativeTest` needs no runtime
-- is preserved; only the file the orchestration sits in moves.

**Interfaces:**
- Consumes: everything from Tasks 5-10.
- Produces, in `bundle_build.h`:
  ```cpp
  struct NativeBuildOptions {
    std::string entryPath, outPath, kitDir, ccOverride, shermesOverride;
    std::vector<std::string> includes, preloads, bakeWasmPaths, vmOptions;
    bool allowVmOptionsOverride = false;
    unsigned jobs = 0;        // 0 -> hardware_concurrency()
    OptLevel opt = OptLevel::O3;
    bool keepTemp = false, verbose = false;
  };
  int buildNativeExecutable(const NativeBuildOptions &options);
  ```
  Task 12 calls it.

**The authoritative module index is captured in step 5.** `buildBundle`'s step 4
compiles, and step 5 assigns indices by adding modules to the `BundleWriter`
in `paths` order. A unit name is derived from the index, so native
compilation must happen **after** step 5. Discovery does assign every path a stable
order in `pathIndex` (`bundle_build.cpp:797`) and step 5 adds modules in
that same order, so `idx == i` happens to hold -- but take the index from
`moduleIndex.at(path)` rather than depending on that.

- [ ] **Step 1: Create the private header and the shared implementation**

Create `lib/bundle/bundle_build_internal.h` -- private to `lib/bundle`, not
under `include/`, because nothing outside this library may name these:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Shared between bundle_build.cpp (the bytecode producer) and
// bundle_build_native.cpp (the native one). Private on purpose: buildBundle
// is the public entry point and these are the seams the two producers share
// behind it. Discovery, resolution, classification and container assembly
// must stay ONE implementation -- two that packaged different graphs for
// the same entry would be the same class of defect as a specifier resolving
// differently at build and run time.

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUILD_INTERNAL_H
#define HERMES_NODE_COMPAT_BUNDLE_BUILD_INTERNAL_H

#include <node_api.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace hermes {
namespace node_compat {

/// What the payload of each kJavaScript module becomes.
enum class PayloadMode {
  /// Compile to bytecode with hermes_compile_to_bytecode. Needs a napi_env.
  Bytecode,
  /// Leave the payload empty and report each module's source, for a caller
  /// that will compile it to native code. Needs no napi_env.
  NativeSources,
};

/// One JavaScript module a native build still has to compile, collected
/// while the container is assembled because that is where the source and
/// the identity are both in hand.
struct PendingNativeModule {
  std::string path;
  std::string identity;  // relative to the bundle root; what -source-name gets
  std::string source;    // unwrapped; the caller applies the CJS wrapper
  uint32_t moduleIndex = 0;
  bool typeScript = false;
};

/// What the shared producer computed that a native build needs and the
/// public signature does not return.
struct BuildProducts {
  /// Empty unless mode was NativeSources, and already carrying each
  /// module's container index -- which is only assigned in step 5, after
  /// the payload step runs.
  std::vector<PendingNativeModule> pendingNative;
  /// The deepest common directory of every packaged file. Computed locally
  /// by the walk and needed by the caller to turn a path into an identity.
  std::string root;
  /// Total modules in the container, which is the size the unit table must
  /// have.
  uint32_t moduleCount = 0;
  /// Modules packaged as throwing stubs, for the summary line.
  uint32_t stubbedModules = 0;
  /// Native addon copies still to be made, as (source path, destination
  /// path) pairs already resolved against sidecarDir. Empty in Bytecode
  /// mode, where the copies have already happened.
  std::vector<std::pair<std::string, std::string>> sidecarCopies;
};

/// The one implementation behind both producers.
///
/// \p env may be null if and only if \p mode is NativeSources -- the only
/// things that use it, hermes_compile_to_bytecode and takeCompileErrorText,
/// are on the bytecode payload path.
///
/// \p sidecarDir is where native addons' bytes are copied. Separate from
/// \p outPath because a native build writes its container to a temp
/// directory that it then deletes, while the addons have to land beside the
/// produced executable. Empty means "the directory \p outPath is in", which
/// is what the bytecode producer wants and what the code did before this
/// parameter existed.
///
/// In NativeSources mode the copies are NOT performed here. The bytecode
/// producer copies after compiling, which narrows the window in which a
/// failed build leaves this run's sidecars beside the last run's artifact
/// to the container write alone; a native build has a compile AND a link
/// still to fail after this point, which would widen that window right back
/// out. So the plan is returned in BuildProducts::sidecarCopies and the
/// orchestration performs it once the executable exists.
int buildBundleImpl(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    const std::string &sidecarDir,
    bool verbose,
    const std::vector<std::string> &includes,
    const std::vector<std::string> &preloads,
    const std::vector<std::string> &bakeWasmPaths,
    const std::vector<std::string> &vmOptions,
    bool allowVmOptionsOverride,
    PayloadMode mode,
    BuildProducts *products);

} // namespace node_compat
} // namespace hermes

#endif
```

Then in `lib/bundle/bundle_build.cpp`: rename the existing `buildBundle`
body to `buildBundleImpl` with the signature above, and make the public
`buildBundle` a one-line forwarder:

```cpp
int buildBundle(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    bool verbose,
    const std::vector<std::string> &includes,
    const std::vector<std::string> &preloads,
    const std::vector<std::string> &bakeWasmPaths,
    const std::vector<std::string> &vmOptions,
    bool allowVmOptionsOverride) {
  BuildProducts products;
  return buildBundleImpl(
      env, entryPath, outPath, /*sidecarDir=*/"", verbose, includes, preloads,
      bakeWasmPaths, vmOptions, allowVmOptionsOverride, PayloadMode::Bytecode,
      &products);
}
```

Inside the implementation, honour the new sidecar parameter at
`bundle_build.cpp:1265` (the local is renamed to `sidecarRoot`, since
`sidecarDir` is now the parameter):

```cpp
  // Where addon bytes are copied. Normally beside the container, but a
  // native build's container lives in a temp directory it deletes, so the
  // caller names the produced executable's directory instead.
  fs::path sidecarRoot = sidecarDir.empty()
      ? fs::path(outPath).parent_path()
      : fs::path(sidecarDir);
```

- [ ] **Step 1b: Make the implementation able to skip the bytecode step**

`#include "bundle_build_internal.h"`, then at the top of step 4's loop body,
after the `kJavaScript` check:

```cpp
    if (mode == PayloadMode::NativeSources) {
      // The payload goes into the container empty; the code will be linked
      // in as a Static Hermes unit. The source is kept here because the
      // unit name is derived from the module index, which step 5 has not
      // assigned yet -- moduleIndex is filled in below, not here.
      products->pendingNative.push_back(
          {path, /*identity=*/"", info.payload, 0, hasExtension(path, ".ts")});
      info.payload.clear();
      continue;
    }
```

Everything below it -- `wrapCJS`, `hermes_compile_flags`,
`hermes_compile_to_bytecode`, `takeCompileErrorText`, the throwing stub -- is
now unreachable in native mode, which is what makes `env` unused there.

- [ ] **Step 3: Mark the container and fill in the products**

In step 5, after the `BundleWriter writer;` declaration:

```cpp
  if (mode == PayloadMode::NativeSources)
    writer.setNativeUnits(true);
```

and after the module-adding loop, where `moduleIndex` and `rootPath` are
both in scope:

```cpp
  products->root = rootPath.generic_string();
  products->moduleCount = static_cast<uint32_t>(paths.size());
  for (PendingNativeModule &pending : products->pendingNative) {
    pending.moduleIndex = moduleIndex.at(pending.path);
    // The identity is what -source-name records, so it is what a stack
    // trace names. Same derivation the container's own identities use.
    pending.identity = fs::path(pending.path)
                           .lexically_relative(rootPath)
                           .generic_string();
  }
```

`products->stubbedModules` is incremented wherever `reporter.stubbed()` is
called -- both sites -- so the summary line in Step 6b can print it.

- [ ] **Step 4: Confirm nothing else on the shared path uses `env`**

The spec requires this check rather than assuming it:

```bash
grep -n "env" lib/bundle/bundle_build.cpp | grep -v "^.*://" | \
  grep -v hermes_compile_to_bytecode | grep -v takeCompileErrorText | \
  grep -v hermes_free_bytecode
```

Expected: no hits outside the bytecode payload path. If there are, they must
either move into the `PayloadMode::Bytecode` branch or the native path must
pass a real `env` -- record which in the commit message.

- [ ] **Step 5: Write the orchestration**

Create `lib/bundle/bundle_build_native.cpp` with `buildNativeExecutable()`.
Its sequence, each step with the reason it is in this order:

1. Read the kit manifest (`readKitManifest`). Fail naming the
   `cmake --build <dir> --target hermes-node-kit` command, as
   `buildExecutable` already does.
2. Resolve `shermes`: `--shermes=` if given, else `<kitDir>/shermes`. A
   missing or non-executable one is a hard error naming it -- unlike the
   C driver, there is no second candidate worth trying, because the kit's
   headers and this binary's generation tag both came from that build.
3. Resolve the C driver with the existing `resolveDriver()` /
   `driverCandidates()` path, exactly as `buildExecutable` does. Then
   determine `driverIsClang` -- `captureDriverVersion()` is a static helper
   inside `build_exe.cpp` and is not reachable from here, so run
   `{driver, "--version"}` through `runCommandCaptured` (Task 5, public) and
   pass its output to `versionOutputIsClang()` (already public in
   `build_exe.h`). One subprocess, and it doubles as the "can this driver be
   run at all" probe, exactly as it does for `--build-exe`.
4. Create the temp directory (`mkdtemp` under `TMPDIR`). Remember it for
   cleanup; on success remove it unless `keepTemp`.
5. Call `buildBundleImpl` with `PayloadMode::NativeSources`,
   `env == nullptr`, the container at `<tempDir>/container.hbb`, and
   **`sidecarDir` set to the directory of `options.outPath`** -- the
   produced executable's own directory, which is where its addon sidecars
   have to land and where the run path looks for them. Passing the default
   would copy them into the temp tree and then delete them. A non-zero
   return is returned as is; its diagnostics are already printed.
6. Turn `products.pendingNative` into `NativeModuleJob`s -- the index and
   identity are already filled in -- then call `compileModules` with
   `options.jobs ? options.jobs : std::thread::hardware_concurrency()` and
   `runCommandCaptured`.
7. **Any failed result is a hard build error.** Print, per failure:
   ```
   error: <identity>: <result.message>
   <result.diagnostics>
     command: <formatCommandLine(result.failedCommand)>
   ```
   and return 1 after reporting **all** of them, not just the first -- a
   parallel build finds several at once and re-running to see the next one
   is a waste of minutes.

   Why not a throwing stub, which is what the bytecode producer does for a
   compile failure: a `shermes` failure cannot be classified from outside
   the process. It returns the same plain failure from parse, sema, IR
   verification, optimization, the backend, and from being unable to write
   its output. The tolerance that matters is decided earlier and on both
   paths alike, by the scanner -- `import()` inside a `.cjs`, the case the
   stub exists for, is a parse error in `shermes` and `hermesc` alike -- so
   what reaches here is a defect, and a defect should stop the build.
8. `unitSymbolTable(products.moduleCount, jobs)`, then
   `nativePayloadAssembly()`, written to `<tempDir>/payload.s`.
9. Assemble it with `buildAssembleCommand()`, then link.

   **`buildLinkCommand()` takes exactly one blob object**
   (`build_exe.cpp:410`) and places it before the entry object and the
   archives -- an order that is load-bearing, because lazy archive
   resolution finds nothing if the objects come after. So the unit objects
   cannot simply be appended. Write a response file
   `<tempDir>/objects.rsp` containing the payload object **first** and then
   every unit object, one absolute path per line, and pass `@<that path>`
   as `buildLinkCommand`'s `blobObject` argument. The linker expands it in
   place, so the position rule is preserved and the argv stays short
   whatever N is.

   **The writer is a public function in `lib/build-native`, not a local
   helper**, so it can be tested without reaching into the orchestration.
   Task 9 declares, implements and tests it; this step only calls it and
   writes the result to `<tempDir>/objects.rsp`:

   ```cpp
   /// The contents of a linker response file listing \p objects, in order.
   ///
   /// \return false with \p error set if a path cannot be expressed.
   bool linkResponseFile(
       const std::vector<std::string> &objects,
       std::string *out,
       std::string *error);
   ```

   Response-file quoting is not shell quoting and not the assembler's
   either: GNU ld and ld64 split on whitespace and both honour `"` quoting
   and `\` escaping. So each path is written **quoted**, which means a
   space is fine -- and it has to be, since `checkIncbinPath()` deliberately
   permits spaces and a macOS path routinely has them; rejecting them here
   would refuse containers the assembler accepts. What is rejected, naming
   the path, is `"`, `\`, CR and LF: the same four `checkIncbinPath()`
   rejects, so the two agree.

   Add `BuildNativeTest` cases: the first line is the payload object; a path
   with a space is quoted rather than refused; a path with a quote or a
   backslash is refused with the path in the message.
10. **Now** perform `products.sidecarCopies`, after the executable exists.
    Deferred to here for the reason the header gives: the bytecode producer
    copies after compiling so that a failed build cannot leave this run's
    sidecars beside the last run's artifact, and a native build has a
    compile and a link still to fail at the point the copies would
    otherwise happen.
11. Print the summary line: the output path, its size, modules compiled,
    stubs, and wall clock.

`--verbose` narrates per the spec's Diagnostics section: the kit, the
resolved `shermes` and driver and why, per-module source/C/object bytes and
timing plus both commands, the assemble and link commands, and the summary.

- [ ] **Step 6: Wire the build**

`lib/bundle/CMakeLists.txt`: add `bundle_build_native.cpp` to
`hermesNodeBundleBuild`, and add to its `PUBLIC` link libraries:

```cmake
    # The pure half of the native producer -- staging, command
    # construction, the job pool. VM-free, which is why it is a separate
    # target: BuildNativeTest links it alone.
    hermesNodeBuildNative
    # buildAssembleCommand/buildLinkCommand/payloadAssembly/resolveDriver,
    # the same toolchain-driving code --build-exe uses. Also VM-free.
    hermesNodeBuildExe
```

- [ ] **Step 6b: Print the stub count unconditionally, on BOTH producers**

The spec asks for one change that is not native-specific: the number of
modules packaged as throwing stubs goes in the end-of-build summary line
whether or not `--verbose` was given. A build that quietly turned three
modules into stubs should say so in its last line; today that count is only
visible in the per-module warnings, which scroll away, and in the verbose
summary, which most builds do not ask for.

In `lib/bundle/bundle_build.cpp`, find where the non-verbose success line is
printed and add the count when it is non-zero, e.g.
`bundle: 412 modules, 3 packaged as throwing stubs`. `buildNativeExecutable`
prints the same field in its own summary.

Add a RUN line to `test/bundle-tolerant.js` asserting the count appears
without `--verbose`, so the behaviour is pinned rather than incidental.

- [ ] **Step 7: Verify it compiles and the bytecode path is unchanged**

```bash
cmake --build cmake-build-asan --target hermes-node 2>&1 | tail -5
for t in bundle-build bundle-run bundle-require bundle-preload bundle-natives; do
  python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/$t.js \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test 2>&1 | tail -2
done
```

Expected: PASS for each. Nothing in this task changes the bytecode path, so
a failure here is a refactoring mistake.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Add native mode to the bundle producer

Discovery, resolution, classification and container assembly stay one
implementation with a mode on the payload step. Two producers that
packaged different graphs for the same entry would be the same class
of defect as a specifier resolving differently at build and run time,
which is why there is one resolver with two backends.

Native compilation runs after step 5, not in step 4 where the
bytecode compile sits: a unit name is derived from the module index,
and indices are assigned by step 5. The index comes from the map
rather than the loop counter, which happens to agree today.

Every shermes or cc failure is a hard build error, where the bytecode
path would package a throwing stub. A shermes failure cannot be
classified from outside the process -- parse, sema, IR verification,
the backend and a failed output write all look alike -- and the
tolerance that matters is decided earlier by the scanner, on both
paths, so what reaches here is a defect. All failures are reported,
not just the first: a parallel build finds several at once.

The orchestration lives in lib/bundle rather than lib/build-native
because it calls buildBundle, which links the parser. lib/build-native
keeps only the pure half, so BuildNativeTest still needs no runtime.
MSG
```

---

## Task 12: The `build-native` subcommand

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp` (`main()` ~line 941; a new
  `printBuildNativeUsage()` and `runBuildNativeSubcommand()` beside
  `printCacheUsage`/`runCacheSubcommand` at ~line 778-930)
- Modify: `tools/hermes-node/CMakeLists.txt` if a link dependency is needed
- Test: `test/build-native-errors.js` (CREATE it here -- Task 8 was asked to
  and correctly declined, because until this task exists every case in it
  would assert nothing and its header comment would be false)

**Interfaces:**
- Consumes: `buildNativeExecutable` and `NativeBuildOptions` (Task 11).
- Produces: the CLI. Tasks 13-16 invoke it.

**Why a subcommand and not a flag.** The ordinary parse loop stops at the
first positional and everything after it belongs to the program being run --
that invariant is what keeps `process.argv.slice(2)` meaning what it means.
Every tool verb so far is a flag bolted onto that line, which is why
`checkToolOptions()` has grown into a conflict matrix and why `--build-exe`
needed a special refusal of `-`-prefixed arguments after its container. This
verb wants more flags than any of them. A subcommand parses its own argv with
flags in any position and joins no matrix.

`build-native` in `argv[1]` is **always** the subcommand, whether or not a
file of that name exists -- the same rule as `cache`, and for the same
reason: a grammar whose meaning changed with the contents of the working
directory is a worse surprise than the shadowing it avoids. `./build-native`
runs a script so named.

- [ ] **Step 1: Write the failing test**

Replace `test/build-native-errors.js` with the full set. **Ungated on
purpose**, like `build-exe-errors.js`: every case is refused before a kit,
a `shermes` or a C compiler is reached, so this is the coverage that
survives a checkout with no kit.

```js
// Refusals only. Deliberately NOT gated on linker-available or
// shermes-available: every case below is rejected before any toolchain is
// reached, so this file is the build-native coverage that survives a
// checkout with no kit.
//
// RUN: rm -rf %t && mkdir -p %t/src
// RUN: echo 'console.log("hi");' > %t/src/app.js
//
// RUN: %not %hermes-node build-native 2>&1 | %FileCheck --check-prefix=NOENTRY %s
// NOENTRY: requires an entry
//
// RUN: %not %hermes-node build-native %t/src/app.js 2>&1 | %FileCheck --check-prefix=NOOUT %s
// NOOUT: requires -o
//
// RUN: %not %hermes-node build-native -o %t/app %t/src/nope.js 2>&1 | %FileCheck --check-prefix=NOFILE %s
// NOFILE: nope.js
//
// RUN: %not %hermes-node build-native -o %t/app --jobs=0 %t/src/app.js 2>&1 | %FileCheck --check-prefix=JOBS %s
// JOBS: --jobs
//
// RUN: %not %hermes-node build-native -o %t/app --jobs=x %t/src/app.js 2>&1 | %FileCheck --check-prefix=JOBSX %s
// JOBSX: --jobs
//
// RUN: %not %hermes-node build-native -o %t/app --record-wasm=%t/w.bin %t/src/app.js 2>&1 | %FileCheck --check-prefix=RECWASM %s
// RECWASM: --record-wasm
//
// RUN: %not %hermes-node build-native -o %t/app --nonsense %t/src/app.js 2>&1 | %FileCheck --check-prefix=UNKNOWN %s
// UNKNOWN: unknown option '--nonsense'
//
// RUN: %not %hermes-node build-native -o '' %t/src/app.js 2>&1 | %FileCheck --check-prefix=EMPTYOUT %s
// EMPTYOUT: -o
//
// RUN: %not %hermes-node build-native -o %t/app --kit= %t/src/app.js 2>&1 | %FileCheck --check-prefix=EMPTYKIT %s
// EMPTYKIT: --kit
//
// RUN: %not %hermes-node build-native -o %t/app %t/src/app.js %t/src/app.js 2>&1 | %FileCheck --check-prefix=TWO %s
// TWO: one entry
//
// A flag AFTER the entry is fine here, unlike --build-exe: a subcommand
// parses its own argv, so position carries no meaning.
// RUN: %not %hermes-node build-native %t/src/app.js -o %t/app --kit=%t/nokit 2>&1 | %FileCheck --check-prefix=AFTER %s
// AFTER: kit.manifest
//
// RUN: %hermes-node build-native --help 2>&1 | %FileCheck --check-prefix=HELP %s
// HELP: Usage:
// HELP: build-native

console.log('unused');
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 -v $(pwd)/test/build-native-errors.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: FAIL -- `build-native` is treated as a script path and reported
missing.

- [ ] **Step 3: Add the usage text**

Beside `printCacheUsage` in `tools/hermes-node/hermes-node.cpp`:

```cpp
/// Usage for the `build-native` subcommand.
static void printBuildNativeUsage(const char *argv0) {
  std::printf(
      "Usage: %s build-native <entry.js> -o <file> [options]\n"
      "\n"
      "Compile a program's whole require() graph to native code with\n"
      "Static Hermes and link a standalone executable.\n"
      "\n"
      "Options:\n"
      "  -o <file>                 Output executable (required)\n"
      "  --include=<specifier>     Package a module static discovery cannot\n"
      "                            see; repeatable\n"
      "  --preload=<specifier>     Package and run before the entry;\n"
      "                            repeatable\n"
      "  --vm=<flag>               Bake a Hermes VM option in; repeatable\n"
      "  --allow-vm-options-override\n"
      "                            Let HERMES_NODE_VM_OPTIONS override them\n"
      "  --bake-wasm=<file>        Bake a --record-wasm file's entries in;\n"
      "                            repeatable\n"
      "  --jobs=<n>                Parallel compiles (default: CPU count)\n"
      "  -O0 -O1 -O2 -O3 -Os       Optimization level (default: -O3)\n"
      "  --kit=<dir>               Kit directory (default: beside this\n"
      "                            binary)\n"
      "  --cc=<path>               C compiler driver\n"
      "  --shermes=<path>          shermes binary (default: <kit>/shermes)\n"
      "  --keep-temp               Keep the build's temporary directory\n"
      "  --verbose                 Narrate to stderr\n"
      "\n"
      "Note: `%s build-native` always means this subcommand. To run a\n"
      "script named `build-native`, write `%s ./build-native`.\n",
      argv0,
      argv0,
      argv0);
}
```

- [ ] **Step 4: Parse and dispatch**

Add `runBuildNativeSubcommand(int argc, char **argv)` beside
`runCacheSubcommand`, with the same doc comment about why it sits outside
the ordinary parse loop. Parse from `i = 2`, accepting flags and the single
positional entry in any order. Rules that must be enforced with the messages
the test asserts:

- no positional -> `Error: 'build-native' requires an entry script`
- two positionals -> `Error: 'build-native' takes one entry, got 'a' and 'b'`
- no `-o` -> `Error: 'build-native' requires -o <file>`
- `-o` with an empty value -> name the flag, not a missing file
- `--kit=` / `--cc=` / `--shermes=` with an empty value -> name the flag
- `--jobs=` non-numeric or `0` -> `Error: --jobs requires a positive number`
  (zero is refused rather than silently meaning "unlimited")
- `--record-wasm` in any spelling ->
  `Error: --record-wasm cannot be used with 'build-native': it never runs`
  `       the program. Record with a plain run, then --bake-wasm here.`
- any other `-`-prefixed argument ->
  `Error: unknown option '<x>' for 'build-native'` plus the usage
- `--help` / `-h` -> usage, exit 0

Resolve the kit the same way `--build-exe` does -- reuse `resolveKitDir()`
rather than re-deriving it -- and default `--shermes` to `<kitDir>/shermes`
inside `buildNativeExecutable` (Task 11), not here, so the default follows
`--kit`.

The entry is made absolute and checked for existence here, so a typo is
reported before a temp directory is created.

In `main()`, beside the `cache` dispatch:

```cpp
  if (argc > 1 && std::strcmp(argv[1], "build-native") == 0)
    return runBuildNativeSubcommand(argc, argv);
```

- [ ] **Step 5: Mention it in the main usage**

Add to the top-level usage text, beside the `cache` entry, so the
subcommand is discoverable:

```
      "Subcommands:\n"
      "  cache <action>                 Manage the compile cache\n"
      "  build-native <entry> -o <file> Compile to native code and link\n"
      "                                 (see `build-native --help`)\n"
```

Match the surrounding formatting exactly rather than this sketch.

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target hermes-node 2>&1 | tail -3
python3 cmake-build-asan/bin/hermes-lit -j1 -v $(pwd)/test/build-native-errors.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: PASS, every RUN line.

- [ ] **Step 7: Confirm the shadowing rule**

```bash
cd /tmp && rm -rf shadow && mkdir shadow && cd shadow
echo 'console.log("SCRIPT");' > build-native
$OLDPWD/../cmake-build-asan/bin/hermes-node build-native 2>&1 | head -2
$OLDPWD/../cmake-build-asan/bin/hermes-node ./build-native
```

Expected: the first prints the "requires an entry script" error (the
subcommand won, even though the file exists); the second prints `SCRIPT`.
Adjust the paths to your checkout.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Add the build-native subcommand

A subcommand rather than a --build-native flag. The ordinary parse
loop stops at the first positional and everything after belongs to
the program being run; every tool verb so far is a flag bolted onto
that line, which is why checkToolOptions is a conflict matrix and why
--build-exe had to refuse flags after its container. This verb wants
more flags than any of them and joins no matrix.

argv[1] is always the subcommand, never conditional on whether a file
of that name exists -- the cache rule, for the cache reason.

--jobs=0 is refused rather than meaning "unlimited", and
--record-wasm is refused by name: this verb never runs the program,
which is the same reason --build-bundle refuses it.
MSG
```

---

## Task 13: Run time -- the unit table reaches `__bundleLoad`

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_run.h`, `lib/bundle/bundle_run.cpp`
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h:157-158`, `lib/runtime/hermes_node_runtime.cpp:1597-1600`
- Modify: `tools/hermes-node/bundle_main.cpp:33-46`
- Modify: `test/lit.cfg:62-65`
- Test: `test/build-native.js` (create)

**Interfaces:**
- Consumes: `hermes_init_sh_unit` (Task 3), `kBundleFlagNativeUnits`
  (Task 8), everything the producer builds (Tasks 11-12).
- Produces: a working `build-native`. Tasks 14-16 test more of it.

**This task is also Task 3's test.** `hermes_init_sh_unit` has no unit test,
deliberately -- exercising it needs a linked SH unit, which is what this
task first produces.

- [ ] **Step 1: Add the lit feature**

In `test/lit.cfg`, beside the `linker-available` block:

```python
# build-native needs a shermes in the kit as well as a linker. Separate from
# linker-available because a kit cut before shermes was added to it has one
# and not the other, and reporting UNSUPPORTED beats failing on a missing
# file.
if kit_dir and os.path.exists(os.path.join(kit_dir, 'shermes')):
    config.available_features.add('shermes-available')
```

- [ ] **Step 2: Write the failing test**

Create `test/build-native.js`:

```js
// REQUIRES: linker-available, shermes-available
// RUN: rm -rf %t && mkdir -p %t/src/lib
//
// RUN: cp %s %t/src/app.js
// RUN: echo 'module.exports = { n: 41 };' > %t/src/lib/a.js
// RUN: echo '{"v": 1}' > %t/src/data.json
// RUN: echo 'function inner() { throw new Error("boom"); }' > %t/src/lib/thrower.js
// RUN: echo 'module.exports = inner;' >> %t/src/lib/thrower.js
// RUN: echo 'throw new Error("top level");' > %t/src/lib/boom.js
// RUN: echo 'module.exports = 1;' > %t/src/lib/c1.js
// RUN: echo 'module.exports = 2;' > %t/src/lib/c2.js
// RUN: echo 'module.exports = 3;' > %t/src/lib/c3.js
// RUN: echo 'module.exports = 4;' > %t/src/lib/c4.js
// RUN: echo 'module.exports = 5;' > %t/src/lib/c5.js
// RUN: echo 'module.exports = 6;' > %t/src/lib/c6.js
// RUN: echo 'module.exports = 7;' > %t/src/lib/c7.js
//
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --kit=%kit_dir
// RUN: %t/app one two | %FileCheck %s
// RUN: %not %t/app --fail > /dev/null 2>&1
//
// The program must not need its source tree: that is the whole point of
// linking it in.
// RUN: mv %t/src %t/src-away
// RUN: %t/app one two | %FileCheck %s
//
// More than eight modules on purpose. SHUnit indices used to be capped at
// eight per process, and the twelve here would have aborted before the
// growable array landed.

const a = require('./lib/a.js');
const data = require('./data.json');
const thrower = require('./lib/thrower.js');

// Literal, one per line, NOT require('./lib/' + n + '.js'). A computed
// specifier is a scanner GAP (require_scanner.cpp records it and does not
// follow it), so a loop would package none of these and the test would
// silently stop covering more than eight units -- which is the thing it is
// here for.
require('./lib/c1.js');
require('./lib/c2.js');
require('./lib/c3.js');
require('./lib/c4.js');
require('./lib/c5.js');
require('./lib/c6.js');
require('./lib/c7.js');

// A module whose top level throws. Note what this does and does not
// cover: the CommonJS wrapper means the unit's global code only creates a
// closure, so the throw happens when the LOADER calls it, after
// hermes_init_sh_unit has returned. So this pins that a throwing module
// body propagates through require() -- the behaviour users see -- and NOT
// hermes_init_sh_unit's failure branch, which Task 1's hand-built
// throwing unit covers instead.
try {
  require('./lib/boom.js');
  console.log('TOPLEVEL no-throw');
} catch (e) {
  console.log('TOPLEVEL', e.message);
}
// CHECK: TOPLEVEL top level

console.log('SUM', a.n + data.v);
// CHECK: SUM 42

console.log('ARGV', process.argv.slice(2).join(','));
// CHECK: ARGV one,two

try {
  thrower();
} catch (e) {
  // -g2 puts identity:line:column into the plain stack string.
  console.log('STACK', e.stack.split('\n')[1].trim());
}
// CHECK: STACK at inner (lib/thrower.js:{{[0-9]+}}:{{[0-9]+}})

// The known phase-1 gap, asserted rather than skipped so that fixing it
// later fails here and is noticed. Structured CallSites carry no location
// for a native frame at any -g level, while the string above does.
Error.prepareStackTrace = (err, frames) =>
  frames.map((f) => f.getFileName() + ':' + f.getLineNumber()).join('|');
try {
  thrower();
} catch (e) {
  console.log('CALLSITE', e.stack.split('|')[0]);
}
// CHECK: CALLSITE null:null
Error.prepareStackTrace = undefined;

if (process.argv[2] === '--fail') process.exitCode = 1;
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 -v $(pwd)/test/build-native.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit
```

Expected: FAIL. The build may even succeed; the produced binary will not
load a module, because nothing plumbs the unit table yet.

- [ ] **Step 4: Carry the table on the config**

`include/hermes/node-compat/runtime/hermes_node_runtime.h`, beside
`embeddedBundleData`:

```cpp
  /// The native unit table the payload object defines, or null for a
  /// bytecode build. Indexed by container module index; a null entry means
  /// that record has no unit -- a JSON module, an addon, a resolve-only
  /// package.json.
  ///
  /// An opaque pointer rather than SHUnitCreator so this header stays clear
  /// of Hermes VM headers, exactly as the rest of it is.
  const void *const *nativeUnits = nullptr;
  size_t nativeUnitCount = 0;
```

- [ ] **Step 5: Take the table in `openEmbeddedBundle`**

`bundle_run.h` / `bundle_run.cpp`: add two parameters,
`const void *const *nativeUnits, size_t nativeUnitCount`, and after the
reader opens:

```cpp
  // A native container's code is in the table; a container that says it is
  // native and arrives without one, or with one of the wrong size, is a
  // producer bug that would otherwise surface as a module that will not
  // load.
  if (reader->hasNativeUnits()) {
    if (nativeUnits == nullptr) {
      *error =
          "hermes-node bundle: the embedded container is native but this "
          "executable carries no unit table";
      return false;
    }
    if (nativeUnitCount != reader->moduleCount()) {
      *error = "hermes-node bundle: the unit table has " +
          std::to_string(nativeUnitCount) + " entries for " +
          std::to_string(reader->moduleCount()) + " modules";
      return false;
    }
  }
```

Store both on `OpenBundle`. Use the reader's real module-count accessor --
check `bundle_reader.h` for its name.

- [ ] **Step 6: The native branch in `bundleLoadCallback`**

In `lib/bundle/bundle_run.cpp`, after the `kJSON` return and before the
bytecode magic check:

```cpp
  // A natively compiled module: its code is linked in, and its unit's
  // top-level completion value is the CommonJS wrapper function this
  // callback exists to return -- the same thing running its bytecode
  // yields. Initialized here, on first require(), not at startup.
  if (state.nativeUnits && *index < state.nativeUnitCount &&
      state.nativeUnits[*index] != nullptr) {
    napi_value result;
    // A throwing top level comes back as napi_pending_exception with the
    // value pending, exactly as hermes_run_bytecode reports one, so it
    // propagates through require() the same way. There is no
    // fatalBadPayload() analogue: a linked unit cannot be a corrupt
    // payload -- a damaged object fails at link time.
    if (hermes_init_sh_unit(
            env,
            reinterpret_cast<SHUnitCreator>(
                const_cast<void *>(state.nativeUnits[*index])),
            &result) != napi_ok)
      return nullptr;
    return result;
  }
```

`bundle_run.cpp` includes `hermes_napi.h` already for `hermes_run_bytecode`;
`SHUnitCreator` comes from there too.

- [ ] **Step 7: Pass it through from both mains**

In `lib/runtime/hermes_node_runtime.cpp`, `runHermesNode` does not call
`openEmbeddedBundle` directly: it calls the helper `runEmbeddedBundle`
(defined ~line 703, called ~line 1598), and the `openEmbeddedBundle` call is
inside that helper, where `config` is not in scope. So give
`runEmbeddedBundle` two more parameters and forward them from the call in
`runHermesNode`:

```cpp
int runEmbeddedBundle(
    napi_env env,
    napi_value loader,
    const uint8_t *data,
    size_t size,
    const void *const *nativeUnits,
    size_t nativeUnitCount);
```

`openBundle`'s call site is unaffected -- a disk container is never native,
which Task 8's refusal enforces.

`tools/hermes-node/bundle_main.cpp`, beside the payload symbols:

```cpp
/// Defined by the generated payload object, in both configurations: a
/// bytecode build emits the same two symbols with a count of zero, which is
/// what lets this one file serve both without a weak symbol.
extern void *const hermesNodeNativeUnits[];
extern const size_t hermesNodeNativeUnitCount;
```

and in `main()`:

```cpp
  config.nativeUnits = hermesNodeNativeUnits;
  config.nativeUnitCount = hermesNodeNativeUnitCount;
```

The assembly emits `hermesNodeNativeUnitCount` as a `.quad`, so the C type
must be a 64-bit value; use `uint64_t` and convert, rather than `size_t`, if
the target could differ.

- [ ] **Step 8: Run the test to verify it passes**

```bash
cmake --build cmake-build-asan --target hermes-node 2>&1 | tail -3
cmake --build cmake-build-asan --target hermes-node-kit 2>&1 | tail -3
python3 cmake-build-asan/bin/hermes-lit -j1 -v $(pwd)/test/build-native.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit
```

Expected: PASS, every RUN line including the source-tree-removed one.

- [ ] **Step 9: Confirm `--build-exe` still works**

The zero-count table is new on that path:

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/build-exe.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit
```

Expected: PASS.

- [ ] **Step 10: Measure, and record it**

The spec says no end-to-end build has been measured. Do it now, on the
Release build, and put the numbers in the commit message:

```bash
cmake --build cmake-build-release --target hermes-node hermes-node-kit
cd examples/tetris && npm install --silent && cd -
time cmake-build-release/bin/hermes-node build-native \
  examples/tetris/play.js -o /tmp/tetris-native \
  --kit=$(pwd)/cmake-build-release/kit --verbose 2>/tmp/native.log
tail -3 /tmp/native.log
ls -l /tmp/tetris-native | awk '{print $5}'
# For comparison, the bytecode artifact:
cmake-build-release/bin/hermes-node --build-bundle=/tmp/tetris.hbb examples/tetris/play.js
cmake-build-release/bin/hermes-node --build-exe=/tmp/tetris-bc /tmp/tetris.hbb \
  --kit=$(pwd)/cmake-build-release/kit
ls -l /tmp/tetris-bc | awk '{print $5}'
```

- [ ] **Step 11: Commit**

```bash
./utils/format.sh -f
git add -A && git commit -F - <<'MSG'
Load native modules through the unit table

__bundleLoad already promised exactly what a Static Hermes unit
delivers: a CommonJS wrapper function. A unit's top-level completion
value IS that wrapper, so the run-time change is one branch ahead of
the bytecode path and libjs/bundle-loader.js is untouched. Units
initialize on first require(), where bytecode modules are run.

openEmbeddedBundle refuses a native container arriving without a unit
table, or with one of the wrong size: that is a producer bug, and
without the check it surfaces much later as a module that will not
load.

The test uses twelve modules deliberately -- SHUnit indices were
capped at eight per process until this stack, and it would have
aborted.

<fill in the measured build time, binary size, and the bytecode
--build-exe size for comparison>
MSG
```

---

## Task 14: The compile-flag parity forcing function

**Files:**
- Test: `test/build-native-parity.js` (create)

**Interfaces:**
- Consumes: the whole feature.
- Produces: nothing; this is the test that stops the parity table from
  becoming a comment.

**Why a separate test.** Every row of the parity table is silent when wrong.
`-Xes6-block-scoping` missing turns `0,1,2` into `3,3,3` with no diagnostic
at build time or run time -- that is the single most dangerous defect this
feature can have, and the one a reviewer cannot see. Running the same
program two ways and diffing is the only check that fails when it happens.
It is the shape `test/bundle-builtins.js` already uses for the same reason.

- [ ] **Step 1: Write the test**

Create `test/build-native-parity.js`:

```js
// REQUIRES: linker-available, shermes-available
//
// The forcing function for the compile-flag parity table in
// docs/superpowers/specs/2026-09-13-native-compilation-design.md. Every row
// of that table is SILENT when it is wrong: shermes defaults ES6 block
// scoping and async generators off, while the bytecode compiler enables
// both, and block scoping off changes what a let-in-loop closure captures
// with no error anywhere. So the check is to run one program two ways and
// diff, not to inspect anything.
//
// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %s %t/src/app.js
// A typed declaration plus module.exports, NOT `export const`: every module
// is compiled inside the CommonJS wrapper, and an export declaration is not
// top-level there. test/bundle-build.js:119 uses this same form.
// RUN: echo 'const n: number = 7;' > %t/src/typed.ts
// RUN: echo 'module.exports = { n };' >> %t/src/typed.ts
//
// RUN: %hermes-node %t/src/app.js > %t/plain.txt 2>&1
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --kit=%kit_dir
// RUN: %t/app > %t/native.txt 2>&1
// RUN: diff -u %t/plain.txt %t/native.txt
// RUN: %FileCheck %s < %t/native.txt

// --- block scoping: the loop-closure case, measured at 3,3,3 without the
// --- flag and 0,1,2 with it.
const fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
console.log('LET', fns.map((f) => f()).join(','));
// CHECK: LET 0,1,2

// const in a loop body, the other half of per-iteration binding
const consts = [];
for (const v of ['a', 'b']) consts.push(() => v);
console.log('CONST', consts.map((f) => f()).join(','));
// CHECK: CONST a,b

// --- async generators: a PARSE error without the flag, so its absence
// --- fails the build rather than the output. Here for completeness of the
// --- table, and because the scanner used to reject this file outright.
async function* ag() {
  yield 1;
  yield 2;
}

// --- plain generators: on in both compilers, no flag. A row in the table
// --- so that a future change turning them off is caught.
function* g() {
  yield 3;
}
console.log('GEN', [...g()].join(','));
// CHECK: GEN 3

// --- TypeScript, per .ts extension
const typed = require('./typed.ts');
console.log('TS', typed.n);
// CHECK: TS 7

(async () => {
  const seen = [];
  for await (const v of ag()) seen.push(v);
  console.log('ASYNCGEN', seen.join(','));
  // CHECK: ASYNCGEN 1,2
})();
```

- [ ] **Step 2: Run it and confirm it passes**

```bash
python3 cmake-build-asan/bin/hermes-lit -j1 -v $(pwd)/test/build-native-parity.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit
```

Expected: PASS.

- [ ] **Step 3: Prove the test actually catches the defect**

A parity test that passes with the flags removed is worthless. Temporarily
delete the `-Xes6-block-scoping` push in
`lib/build-native/native_compile.cpp`, rebuild, and re-run:

```bash
cmake --build cmake-build-asan --target hermes-node 2>&1 | tail -2
python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/build-native-parity.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit 2>&1 | tail -20
```

Expected: FAIL, with `diff` showing `LET 3,3,3` against `LET 0,1,2`. Then
restore the line, rebuild, and confirm PASS again. Record the observed
failure output in the commit message -- it is the evidence that the test
works.

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -F - <<'MSG'
Add the native compile-flag parity test

Every row of the parity table is silent when wrong, so the check runs
one program twice and diffs rather than inspecting anything. Verified
by removing -Xes6-block-scoping and watching it fail: LET 3,3,3
against LET 0,1,2, which is what the defect looks like in a user's
program with nothing printed at build or run time.
MSG
```

---

## Task 15: Closed world, addons, and WebAssembly

**Files:**
- Modify: `lib/bundle/bundle_run.cpp` (`openBundle()`'s native refusal,
  deferred here from Task 8 because only now can a native container be
  produced to test it against)
- Test: `test/build-native-escapes.js`, `test/build-native-natives.js`,
  `test/build-native-wasm.js` (create)
- Modify: `test/build-native-errors.js` (add the native-container refusal)

**Interfaces:** consumes the whole feature; produces nothing.

- [ ] **Step 1: Closed world**

Create `test/build-native-escapes.js` by reading
`test/build-exe-escapes.js` and porting each case to `build-native`. The
property is identical -- a bundled program resolves nothing from disk -- and
the two routes that used to get around it are `globalThis.require` (reachable
as `(0, eval)('require')`, `global.require`, `new Function('return require')()`)
and `Module.createRequire()`. Do not invent new cases: port the existing ones,
so a divergence between the two artifact kinds is visible as a difference
between two files that should read alike.

- [ ] **Step 2: Native addons**

Create `test/build-native-natives.js` the same way, from
`test/build-exe-natives.js`: an addon packaged as a `kNative` record with its
bytes copied to a flat sidecar beside the **executable**, loaded by
`process.dlopen` at run time, and a recorded addon whose sidecar is missing
throwing `MODULE_NOT_FOUND` rather than `ERR_DLOPEN_FAILED`. None of this is
new code -- Task 11 reuses the producer's addon handling unchanged -- which
is exactly why it needs a test: nothing else would notice it breaking.

- [ ] **Step 3: WebAssembly**

Create `test/build-native-wasm.js`:

```js
// REQUIRES: wasm, linker-available, shermes-available
//
// A native executable is not a Wasm-free one. It installs the Wasm cache
// hooks and uses the disk cache exactly as a --build-exe artifact does, and
// baked entries live in the same v6 container this producer already writes,
// so --bake-wasm works here too. What has never been exercised is native
// code and Wasm in one process, which is what this file is for.
//
// Hits and misses are read from the tracing, never from timing: the suite
// runs 16-way parallel and both of its known flaky tests got that way
// through a timing dependency.
//
// RUN: rm -rf %t && mkdir -p %t/src
// ... build a program that instantiates a module from
// ... test/fixtures/wasm/modules.js, exactly as test-wasm-cache-build-exe.js
// ... does; then:
//   1. build it with build-native, run twice with
//      HERMES_NODE_COMPILE_CACHE=%t/cache in the environment -- NOT
//      --compile-cache=, which a produced executable treats as a program
//      argument, as test-wasm-cache-build-exe.js:16 records -- and assert
//      "wasm miss" then "wasm hit" from
//      HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE
//   2. record with --record-wasm on a plain run, build with
//      --bake-wasm=<file>, run with HERMES_NODE_DISABLE_COMPILE_CACHE=1,
//      and assert "wasm container hit"
```

Read `test/test-wasm-cache-build-exe.js` and `test/test-wasm-bake-build-exe.js`
and follow their structure; the tracing strings are `wasm hit`, `wasm miss`,
`wasm container hit`, `wasm container miss`.

- [ ] **Step 4: Implement and test the native-container refusal**

First the code, deferred here from Task 8 -- the block quoted in that task's
Step 6, added to `openBundle()` in `lib/bundle/bundle_run.cpp` after
`BundleReader::open()` succeeds and before `file->release()`.

Then the test. Producing a native container needs the toolchain, so this
case is **gated** and goes in `test/build-native-escapes.js` (or a small
`test/build-native-container.js`) rather than in the ungated errors file --
gating the whole of `build-native-errors.js` for one case would cost the
coverage that survives a kitless checkout, which is the reason that file
exists:

`--keep-temp` must print the retained directory on one line the test can
parse. Make it print exactly `build-native: temp directory: <path>` to
stderr -- fix the format here, because a test that greps for a sentence is
a test that breaks when someone rewords the sentence.

```js
// RUN: rm -rf %t/keep && mkdir -p %t/keep
// RUN: %hermes-node build-native %t/src/app.js -o %t/keep/app \
// RUN:     --kit=%kit_dir --keep-temp 2>%t/keep/err.txt
// RUN: sed -n 's/^build-native: temp directory: //p' %t/keep/err.txt > %t/keep/dir.txt
// RUN: %not %hermes-node --bundle="$(cat %t/keep/dir.txt)/container.hbb" 2>&1 \
// RUN:     | %FileCheck --check-prefix=NATIVE %s
// NATIVE: holds no bytecode
```

- [ ] **Step 4b: Test that a failed build leaves sidecars alone**

The reason sidecar copying was deferred until after the link (Task 11,
step 10). Without a test, a later refactor moving it back into the shared
producer would go unnoticed until somebody's rebuild half-updated an
artifact.

In `test/build-native-natives.js`: build once so a good executable and its
sidecar exist; record the sidecar's bytes; then run a build that fails
**after** compilation -- the cheapest reliable way is `--cc=/bin/false`,
which resolves (it runs) and then fails every compile -- and assert that
the build exits non-zero, the old executable is unchanged, and the sidecar
still has its original bytes.

**The source addon must be MUTATED before the failing rebuild**, or the
test proves nothing: recopying an unchanged file produces identical bytes,
so `cmp` passes whether the copy was deferred or not. Append a byte to the
source `.node` first, and then `cmp` against the sidecar captured before
that -- now a premature copy is visible as a difference.

```js
// RUN: %hermes-node build-native %t/src/app.js -o %t/n/app --kit=%kit_dir
// RUN: cp %t/n/app %t/n/app.first
// RUN: cp %t/n/binding.node %t/n/sidecar.first
//
// Make the source differ, so a copy performed during the failing build
// below would change the sidecar's bytes. Without this the assertion is
// vacuous: recopying the same file yields the same bytes.
// RUN: printf 'x' >> %t/src/binding.node
//
// /bin/false resolves and runs, then fails every compile -- a build that
// fails AFTER the shared producer would have copied sidecars, which is the
// case the deferral exists for.
// RUN: %not %hermes-node build-native %t/src/app.js -o %t/n/app \
// RUN:     --kit=%kit_dir --cc=/bin/false
//
// RUN: cmp %t/n/app %t/n/app.first
// RUN: cmp %t/n/binding.node %t/n/sidecar.first
```

Name the sidecar explicitly rather than globbing: the producer prints
`native: <sidecar> (from <identity>)`, so the fixture knows what it is
called, and a glob that matches two files silently compares the wrong one.

- [ ] **Step 5: Run all four**

```bash
for t in build-native-escapes build-native-natives build-native-wasm build-native-errors; do
  python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test/$t.js \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-asan/test \
    --param kit_dir=$(pwd)/cmake-build-asan/kit 2>&1 | tail -2
done
```

Expected: PASS or UNSUPPORTED for each.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -F - <<'MSG'
Test closed world, addons and Wasm natively

The escapes and addon files are ports of the --build-exe ones rather
than new cases, so a divergence between the two artifact kinds reads
as a difference between two files that should look alike.

None of this is new code -- the producer's addon handling and the
closed world are reused unchanged -- which is the reason it needs
tests: nothing else would notice them breaking.

Native code and Wasm in one process had never been exercised. Hits
and misses come from the tracing, never from timing.
MSG
```

---

## Task 16: The worked example, the measurements, and the documentation

**Files:**
- Modify: `examples/tetris/run.sh`
- Modify: `CLAUDE.md`
- Modify: `docs/superpowers/specs/2026-09-13-native-compilation-design.md`
  (the Measurements section)
- Create: `docs/superpowers/plans/progress-native-compilation.md`
- Modify: `hermes/unittests/VMRuntime/StaticHUnitTest.cpp` (the GC
  benchmark; a Hermes-submodule change, so it commits separately and the
  gitlink moves with it)

**Interfaces:** consumes the whole feature; produces the documentation the
next person reads.

**tetris is the right first example** for the same reason it is the right
`--build-exe` one: entirely CommonJS, no computed requires, no addons, so
the producer emits no warnings and the example is about the artifact rather
than about `--include`.

- [ ] **Step 1: Add the native arm to the example**

In `examples/tetris/run.sh`, beside the existing `--build-exe` arm, add a
`build-native` one that builds, moves `node_modules` aside, runs the binary
through `examples/pty-run.py` (tetris needs a real terminal: `setRawMode`
does not exist on a pipe), and moves it back. Follow the existing arm's
structure exactly, including its assertion that the producer emits no
warnings.

- [ ] **Step 2: Measure, on Release**

```bash
cmake --build cmake-build-release --target hermes-node hermes-node-kit
cd examples/tetris && npm install --silent && cd -
KIT=$(pwd)/cmake-build-release/kit
HN=$(pwd)/cmake-build-release/bin/hermes-node

# native, cold
/usr/bin/time -p $HN build-native examples/tetris/play.js \
  -o /tmp/t-native --kit=$KIT --verbose 2>&1 | tail -20

# the same at -O0, which the spec expects to be roughly 4x faster to build
/usr/bin/time -p $HN build-native examples/tetris/play.js \
  -o /tmp/t-native-O0 --kit=$KIT -O0 2>&1 | tail -5

# bytecode, for comparison
$HN --build-bundle=/tmp/t.hbb examples/tetris/play.js
/usr/bin/time -p $HN --build-exe=/tmp/t-bc /tmp/t.hbb --kit=$KIT

ls -l /tmp/t-native /tmp/t-native-O0 /tmp/t-bc /tmp/t.hbb | awk '{print $5, $9}'
```

Record: module count, build wall clock at `-O3` and `-O0`, the native binary
size, the bytecode binary size, and the ratio. The spec predicts roughly
7.4x on object size against bytecode from one file; the whole-artifact ratio
is smaller because the runtime dominates both, and the measured number is
what belongs in the docs.

- [ ] **Step 2b: Measure at ~1,500 modules, not only at tetris scale**

tetris is a few dozen modules, which says nothing about the cost the design
actually flagged: every GC visits every initialized unit, and a long-lived
collection scans each unit's whole symbol and property-cache arrays, so
full-GC pause time grows with the number of units rather than with live
data. The design requires this measured on a realistic ~1,500-module graph
and it is the most likely unpleasant surprise in the feature.

Use `examples/flow-bundler`, whose graph is ~1,500 files:

```bash
cd examples/flow-bundler && npm install --silent && cd -
/usr/bin/time -p $HN build-native examples/flow-bundler/<entry> \
  -o /tmp/fb-native --kit=$KIT --jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu) \
  --verbose 2>&1 | tail -10
ls -l /tmp/fb-native | awk '{print $5}'
# Startup and a forced full GC, against the bytecode artifact:
/usr/bin/time -p /tmp/fb-native <a no-op argument> 2>&1 | grep real
```

Record: module count, build wall clock, peak RSS during the build (`/usr/bin/time -l`
on macOS, `-v` on GNU), binary size, and startup time -- the last as the
median of five runs of an entry that requires the whole graph and exits, so
the figure is dominated by unit initialization rather than by the kernel.

**Full-GC pause cannot be measured from the produced executable**:
`-gc-print-stats` is one of the flags hermes-node refuses by name (its
output has no reader outside `ConsoleHost`), there is no `global.gc()`, and
`/usr/bin/time` around the process measures the process, not a collection.

It CAN be measured one level down, and that is where the design's actual
question lives -- does pause time grow with the number of units? Add a
benchmark to `hermes/unittests/VMRuntime/StaticHUnitTest.cpp`, building on
the hand-built unit from Task 1:

```cpp
/// The design's open question: a full GC visits every initialized unit and
/// scans its whole symbol and property-cache arrays, so pause time should
/// grow with unit COUNT rather than with live data. Measured rather than
/// argued about.
TEST(StaticHUnitTest, DISABLED_FullGCPauseVersusUnitCount) {
  for (uint32_t n : {1u, 100u, 1000u}) {
    auto rt = Runtime::create(kTestRTConfig);
    /* register n distinct units, each with ~200 symbols -- see below */
    auto start = std::chrono::steady_clock::now();
    rt->collect("benchmark");
    auto ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start).count();
    llvh::errs() << n << " units: " << ms << " ms\n";
  }
}
```

**How to get a thousand distinct units with realistic tables**, since this
is the part that decides whether the benchmark measures anything. Not with
`shermes`: a thousand generated files is a thousand compiles at test-build
time. Build them in the test instead, which is both cheaper and lets the
symbol count be a knob:

- The cost being measured is the GC walking each unit's `symbols` array and
  property caches, and that walk does not care where the strings came from.
  So synthesize one shared ASCII pool and one shared `strings` table of
  ~200 entries at test start, and point every unit's `ascii_pool` and
  `strings` at them. Only the per-unit `symbols`, cache arrays and index
  variable must be distinct.
- "Distinct" means a distinct `uint32_t *index`, because that is what
  `_sh_unit_init` writes and what makes two `SHUnit`s two units rather than
  one registered twice. Allocate a `std::vector<uint32_t> indices(n)`, zero
  it, and give unit *i* `&indices[i]`. A creator function cannot carry
  state, so use `_sh_unit_init`'s sibling directly, or a thread-local
  "which one to build next" the creator reads -- whichever is less ugly
  once you see the real signature.
- Every unit still needs its own `symbols` array (`num_symbols` entries,
  zero-filled) and its own read/write/private-name cache arrays, since the
  GC writes through them.

`DISABLED_` because it is a measurement, not an assertion -- run it by hand
with `--gtest_also_run_disabled_tests` and put the three numbers in the
progress file. Check `Runtime::collect`'s real signature before writing
this.

If the unit-count curve turns out flat, say so: that is a useful answer and
it retires the design's largest open worry.

If that turns out to cost more than the measurement is worth, say so in the
progress file and file it -- but do not report the feature as measured on
this axis when it is not.

If the numbers are bad, that is a finding for the progress file and the
tracker, not a reason to hold the feature: the design accepted per-unit
overhead knowingly and named this as the thing to watch.

Also measure startup at tetris scale, which the spec flags as unmeasured:

```bash
for i in 1 2 3 4 5; do /usr/bin/time -p /tmp/t-bc --version 2>&1 | grep real; done
for i in 1 2 3 4 5; do /usr/bin/time -p /tmp/t-native --version 2>&1 | grep real; done
```

If tetris has no `--version`, use a one-line entry that prints and exits.

- [ ] **Step 3: Replace the spec's Measurements section**

The spec says "No end-to-end build has been measured yet; that belongs to
the implementation." Replace that sentence with the numbers from Step 2,
naming the machine and the build configuration, in the same table style the
section already uses. Keep the per-file `net.js` table -- it is still the
per-module cost and it is what the job pool is sized against.

- [ ] **Step 4: Write the progress file**

Create `docs/superpowers/plans/progress-native-compilation.md`, following the
format of `docs/superpowers/plans/progress-single-executable.md`: which plan
it tracks, one entry per task with its outcome, and the context notes that do
not belong in a commit message -- anything measured, anything that turned out
differently from the plan, and anything deliberately left undone.

- [ ] **Step 5: Document it in CLAUDE.md**

Add a `## Native Compilation` section after `## Single-File Executables`.
It must say, because none of it is derivable from the code:

- What the verb is and that it is a subcommand, with the `argv[1]` rule.
- That a native build is an AOT bundle with empty JavaScript payloads and
  linked units, and that everything else -- closed world, resolver, addon
  sidecars, preloads, baked VM options -- is the same code.
- The compile-flag parity requirement, the `3,3,3` measurement, and that
  `test/build-native-parity.js` is the forcing function. Point at
  `JSLanguageFlags` in `cjs_wrapper.h` as the one copy.
- That `kit.manifest`'s `cc` is the C++ **link** driver and the compile step
  passes `-x c`, with the five-errors measurement.
- The three Hermes changes, and that the units array was capped at eight
  with no recorded reason for the number.
- The failure policy: the scanner decides, a `shermes` failure is a hard
  error, and why -- with the `import()`-in-`.cjs` measurement showing the
  tolerance case is a parse error on both paths.
- Stack traces: the string carries `identity:line:column` from `-g2`, and
  structured CallSites carry nothing at any `-g` level. Name the tracker
  issue.
- The measured build time and artifact size from Step 2.
- What is not in scope: built-in JavaScript stays interpreted bytecode, no
  object cache, no cross-module optimization, no runnable native container,
  and macOS releases ship no kit.
- The test list.

Do not copy the tracker's content into it. This file describes how the
system works; the tracker describes what is wrong with it.

- [ ] **Step 6: File what is left open**

Three things deserve tracker issues, because nothing else records them.
The third is not in the spec's not-in-scope list but is left undone by this
plan: `sh_unit_additional_memory_size()` undercounts every unit --
`StaticHUnit.cpp:206` uses `sizeof(unit->runtime_ext)`, the pointer, and
omits the generated `UnitData` allocation entirely. Task 1 adds only the
pointer array to `mallocSize()`. Fixing it properly needs an emitted
allocation-size field on the unit, which is a Hermes codegen change; at
eight units nobody noticed, and at 1,500 the heap accounting is simply
wrong. File it with the Step 2b numbers attached.

```bash
DZ=examples/ditz2/ditz2/dist/cli/main.js
export DZ_AUTHOR="$(git config user.name) <$(git config user.email)>"
node $DZ add --type bug "Native frames carry no location in structured CallSites" -m -
node $DZ add --type task "build-native has no object cache, so every build recompiles everything" -m -
node $DZ add --type bug "sh_unit_additional_memory_size undercounts every SH unit" -m -
```

For each, write a body with the measurement and the reasoning from the spec
-- the CallSite one should record that the plain `e.stack` string is correct
while `getFileName()` returns null at every `-g` level, and that
`test/build-native.js` asserts the current behaviour so a fix fails the test.

- [ ] **Step 7: Run the whole suite**

```bash
cmake --build cmake-build-asan --target hermes-node hermes-node-kit
python3 cmake-build-asan/bin/hermes-lit -j1 $(pwd)/test \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test \
  --param kit_dir=$(pwd)/cmake-build-asan/kit 2>&1 | tail -30
cmake --build cmake-build-asan --target check-hermes-node-unit 2>&1 | tail -10
```

Expected: all PASS or UNSUPPORTED, except possibly `test-inspect.js` or
`test-repl-history.js`, which are known flaky under load -- confirm with six
isolated runs before treating either as a regression.

- [ ] **Step 8: Commit**

The GC benchmark from Step 2b lives in the **Hermes submodule**, so it is a
separate commit there, and the outer repository's gitlink moves with it --
the same two-step every Hermes change in this plan needs:

```bash
./utils/format.sh -f
git -C hermes add -A && git -C hermes commit -F - <<'HMSG'
Add a disabled benchmark for full-GC pause versus unit count

A full collection visits every initialized unit and scans its symbol
and property-cache arrays, so pause time tracks unit count rather than
live data. Disabled because it is a measurement, not an assertion.

<numbers for 1, 100 and 1000 units>
HMSG
git add -A && git commit -F - <<'MSG'
Document native compilation; tetris native arm

<Fill in the measured numbers: module count, build wall clock at -O3
and -O0, native binary size against the bytecode one, and startup
time for both. They are the part of this that cannot be derived from
the diff, and the startup figure is the one the design flagged as
unmeasured -- every GC visits every unit, so the pause-time curve was
the most likely unpleasant surprise.>
MSG
```

---

## Self-Review

Run against the spec after the last task, before declaring the feature done.

**Spec coverage.** Walk the spec's sections and name the task that
implements each:

| spec section | task |
| --- | --- |
| Reuse of the bundle machinery | 11 |
| One unit per module, lazy init | 13 |
| Per-unit overhead, GC cost | 16 (measured) |
| The container flag bit | 8 |
| Growable units array | 1 |
| `hermes_init_sh_unit` | 3 |
| `shermes -source-name=` | 2 |
| The subcommand and its flags | 12 |
| Staging, shermes argv, cc argv | 9 |
| The job pool | 10 |
| The generated assembly | 7 |
| Run time | 13 |
| The kit | 6 |
| Failure policy | 11 (and 4, which decides it) |
| Stack traces | 13 (asserted), 16 (tracker) |
| Measurements | 13, 16 |
| Diagnostics | 11, 12 |
| Testing | 12-15 |
| Not in scope | 16 (documented) |

Anything with no task is a gap: add one rather than leaving it.

**Known divergences from the spec**, all deliberate and each recorded where
it happens:

1. The orchestration lives in `lib/bundle/bundle_build_native.cpp`, not
   `lib/build-native/`, because it calls `buildBundle`, which links the
   parser. `lib/build-native` keeps the pure half, so the property the spec
   wanted -- `BuildNativeTest` needs no runtime -- holds. (Task 11.)
2. `payloadAssembly()` gains a parameter rather than a sibling function, so
   the bytecode and native paths cannot emit structurally different objects.
   (Task 7.)
3. The `openBundle()` refusal of a native container is implemented in Task
   15 rather than Task 8, so that the task implementing it can also test it
   -- a native container cannot exist before the producer does. (Tasks 8
   and 15.)
4. `hermes_init_sh_unit`'s failure branch is unreachable from any bundled
   module -- the CommonJS wrapper means a unit's global only creates a
   closure, so a throwing module body throws when the loader CALLS it,
   after init has returned (verified on the emitted C). It is therefore
   tested with a hand-built throwing unit through the NAPI fixture rather
   than through any `.js` fixture. Not a gap, but worth knowing before
   writing a test that tries. (Tasks 3 and 1.)
5. Full-GC pause is not measurable from a produced executable
   (`-gc-print-stats` is refused, there is no `global.gc()`), so it is
   measured one level down by a `DISABLED_` GTest calling
   `Runtime::collect()` over 1, 100 and 1000 units. If even that is
   skipped, the progress file must say so rather than the feature reading
   as measured. (Task 16.)
6. `JSLanguageFlags` is the source for the scanner and the `shermes` argv,
   but NOT for the bytecode compiler, which hardcodes the same values inside
   Hermes. The spec implies one copy; there are two, kept in line by
   `bundle-async-generator.js` and `build-native-parity.js`. Closing it is a
   `hermes_compile_flags` change and is out of scope. (Task 4.)
7. `sh_unit_additional_memory_size()` is left undercounting. The spec calls
   it "worth fixing while we are here"; fixing it properly needs an emitted
   allocation-size field, which is a Hermes codegen change. Filed instead.
   (Task 16.)

**Before claiming done:** the full lit suite and `check-hermes-node-unit`
both green (Task 16, Step 7), `./utils/format.sh -f` clean, and the
measured numbers actually in `CLAUDE.md` and the spec rather than left as
placeholders.

