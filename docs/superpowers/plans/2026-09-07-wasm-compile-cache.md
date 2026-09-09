# Wasm Compile Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a WebAssembly module compile to Hermes bytecode once and be
served from the on-disk compile cache on every later run, in every run mode.

**Architecture:** Hermes gains the ability to serialize a Wasm-compiled module
and an embedder cache hook (`lookup`/`store`/`discard`) consulted at
`createModuleFromBytes`, the one place Wasm bytes become a bytecode provider.
hermes-node implements those hooks over the existing `CompileCache`, keying
entries by SHA-256 of the module content with the digest as the filename, and
bounding them with a size budget configured from a file in the cache directory.

**Tech Stack:** C++17, CMake + Ninja, GTest (`unittests/`), LLVM lit
(`test/`), picohash (vendored), Hermes VM + NAPI.

**Spec:** `docs/superpowers/specs/2026-09-07-wasm-compile-cache-design.md`

## Global Constraints

- **Branches are already created. Do not create, switch, rebase or merge
  branches.** hermes-node-compat is on `work-wasm-cache`; the `hermes`
  submodule is on `wasm-compile-cache` (off a fast-forwarded local
  `hermes-node`, at `96f7a4103`). The user will direct all branch management
  at the end.
- **Never run `git add hermes`, `git add -A`, or `git add .` from the
  hermes-node-compat root.** Committing in the submodule makes the gitlink
  dirty (`M hermes`); that is expected and must stay unstaged until the user
  says otherwise. Stage explicit paths in every hermes-node-compat commit.
- **Two repositories.** Tasks 1-3 commit inside `hermes/` (the submodule
  working tree). Tasks 4-9 commit in hermes-node-compat. A task never spans
  both.
- **Copyright headers:** new files under `hermes/` get
  `Copyright (c) Meta Platforms, Inc. and affiliates.`; new files in
  hermes-node-compat get `Copyright (c) Tzvetan Mikov.` Both use the MIT
  boilerplate already on neighbouring files in the same directory.
- **Commit messages:** ASCII only, no emojis.
- **Format before every commit that touches C++:** `./utils/format.sh -f`
  from the hermes-node-compat root (clang-format 18 only). It does not format
  files under `hermes/`; for those, match surrounding style by hand.
- **Everything about the cache is best effort.** No failure in this feature
  may surface to the running program; every error path falls back to
  compiling the module normally.
- **All new Wasm-dependent code is inside `#ifdef HERMES_ENABLE_WASM`** on the
  Hermes side, matching `setWasmModuleResolver` in `Runtime.h`.
- Default budget is `268435456` (256 MB); default recency is `atime`.

## File Structure

**Hermes (`hermes/`, branch `wasm-compile-cache`):**

| File | Responsibility |
| --- | --- |
| `include/hermes/WasmFrontend/WasmCompile.h` (modify) | Declare the optional serialization out-param |
| `lib/WasmFrontend/WasmCompile.cpp` (modify) | Serialize the `BytecodeModule` when asked |
| `unittests/WasmFrontend/WasmCompileTest.cpp` (modify) | Roundtrip: serialize, then load as bytecode |
| `include/hermes/VM/WasmCacheHooks.h` (create) | POD callback set; no dependencies either way |
| `include/hermes/VM/Runtime.h` (modify) | Store the hooks, beside `setWasmModuleResolver` |
| `lib/VM/JSLib/WebAssembly/WebAssembly.cpp` (modify) | Consult the hooks in `createModuleFromBytes` |
| `API/napi/hermes_napi_wasm_cache.h` (create) | Public C ABI for installing hooks |
| `API/napi/hermes_napi_wasm_cache.cpp` (create) | Adapt the C struct onto `vm::Runtime` |
| `API/napi/CMakeLists.txt` (modify) | Add the new source |
| `unittests/napi/NapiWasmCacheTest.cpp` (create) | Hook contract and token lifecycle |
| `unittests/napi/CMakeLists.txt` (modify) | Add the new test |

**hermes-node-compat (branch `work-wasm-cache`):**

| File | Responsibility |
| --- | --- |
| `include/hermes/node-compat/compile-cache/cache_config.h` (create) | Cache-directory configuration type + loader |
| `lib/compile-cache/cache_config.cpp` (create) | Parse/write `config`, defaults on malformed input |
| `include/hermes/node-compat/compile-cache/compile_cache.h` (modify) | `kWasm`, digest helper, `lookupWasm`/`saveWasm`, eviction |
| `lib/compile-cache/compile_cache.cpp` (modify) | Implement the above |
| `include/hermes/node-compat/compile-cache/wasm_cache_hooks.h` (create) | `installWasmCacheHooks(env, cache)` |
| `lib/compile-cache/wasm_cache_hooks.cpp` (create) | Adapter + `StoreToken` ownership |
| `lib/compile-cache/CMakeLists.txt` (modify) | Add the two new sources |
| `lib/runtime/hermes_node_runtime.cpp` (modify) | Load config, install hooks after env creation |
| `unittests/CompileCacheTest.cpp` (modify) | Digest, filename shape, eviction, config parsing |
| `test/test-wasm-cache.js` (create) | End-to-end hit/miss, all run modes |
| `CLAUDE.md` (modify) | Document the feature |
| `docs/superpowers/plans/progress-wasm-compile-cache.md` (create) | Progress + measurements |

---

### Task 1: Serialize a Wasm-compiled module

**Files:**
- Modify: `hermes/include/hermes/WasmFrontend/WasmCompile.h`
- Modify: `hermes/lib/WasmFrontend/WasmCompile.cpp`
- Test: `hermes/unittests/WasmFrontend/WasmCompileTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `compileWasmToModuleData(buffer, size, errorMsg, test262 = false,
  std::string *serializedOut = nullptr)` — when `serializedOut` is non-null and
  compilation succeeds, it receives the serialized `.hbc` bytes.

- [ ] **Step 1: Write the failing test**

Append to `hermes/unittests/WasmFrontend/WasmCompileTest.cpp`. Read the top of
that file first and reuse whatever helper it already has for building a
minimal module; if it has none, use these bytes, which are `wat2wasm` output
for `(module (func (export "add") (param i32 i32) (result i32)
(i32.add (local.get 0) (local.get 1))))`:

```cpp
/// wat2wasm output for:
///   (module (func (export "add") (param i32 i32) (result i32)
///     (i32.add (local.get 0) (local.get 1))))
/// At file scope so both tests below share one definition.
static const uint8_t kAdd[] = {
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
      0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
      0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20,
    0x00, 0x20, 0x01, 0x6a, 0x0b};

TEST(WasmCompileTest, SerializesToLoadableBytecode) {
  std::string errorMsg;
  std::string serialized;
  auto data = hermes::compileWasmToModuleData(
      kAdd, sizeof(kAdd), errorMsg, /*test262*/ false, &serialized);

  ASSERT_TRUE(data) << errorMsg;
  EXPECT_FALSE(serialized.empty());

  // The bytes must be recognizable as Hermes bytecode and loadable.
  auto ref = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(serialized.data()), serialized.size());
  EXPECT_TRUE(hermes::hbc::BCProviderFromBuffer::isBytecodeStream(ref));

  auto buf = llvh::MemoryBuffer::getMemBufferCopy(
      llvh::StringRef(serialized.data(), serialized.size()));
  auto loaded = hermes::hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::make_unique<hermes::OwnedMemoryBuffer>(std::move(buf)));
  EXPECT_TRUE(loaded.first) << loaded.second;
}

TEST(WasmCompileTest, SerializationIsOptional) {
  std::string errorMsg;
  auto data =
      hermes::compileWasmToModuleData(kAdd, sizeof(kAdd), errorMsg);
  ASSERT_TRUE(data) << errorMsg;
  EXPECT_TRUE(data->bytecodeProvider);
}
```

Add these includes at the top of the test file if absent:

```cpp
#include "hermes/BCGen/HBC/BCProviderFromBuffer.h"
#include "hermes/Support/MemoryBuffer.h"
#include "llvh/Support/MemoryBuffer.h"
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target WasmFrontendTests
./cmake-build-asan/unittests/WasmFrontend/WasmFrontendTests --gtest_filter='WasmCompileTest.Serializ*'
```

Expected: compile error — `compileWasmToModuleData` takes four arguments, not
five.

- [ ] **Step 3: Declare the parameter**

In `hermes/include/hermes/WasmFrontend/WasmCompile.h`, extend the declaration
and its doc comment:

```cpp
/// \param serializedOut If non-null, receives the serialized .hbc bytes of the
///   compiled module. This is what lets an embedder cache the result of a
///   compile, and what a build-time producer would use to bake bytecode into
///   an artifact. Serialization is skipped entirely when null.
std::unique_ptr<WasmModuleData> compileWasmToModuleData(
    const uint8_t *buffer,
    size_t size,
    std::string &errorMsg,
    bool test262 = false,
    std::string *serializedOut = nullptr);
```

- [ ] **Step 4: Implement it**

In `hermes/lib/WasmFrontend/WasmCompile.cpp`, add the includes:

```cpp
#include "hermes/BCGen/HBC/BytecodeStream.h"
#include "llvh/Support/SHA1.h"
#include "llvh/Support/raw_ostream.h"
```

Match the signature, then insert immediately after the `provider` is created
and before it is stored into the returned `WasmModuleData`:

```cpp
  if (serializedOut) {
    // EmitBundle, not the Execute options used for generation: serialization
    // needs the bundle form, exactly as hermes_compile_to_bytecode does it
    // (API/napi/hermes_napi_compile.cpp).
    BytecodeGenerationOptions serOptions{OutputFormatKind::EmitBundle};
    serOptions.optimizationEnabled = true;
    serOptions.staticBuiltinsEnabled = context->getStaticBuiltinOptimization();

    // The .hbc header carries a source hash. There is no JavaScript source
    // here, so hash the Wasm bytes: it is the thing this bytecode was
    // produced from.
    auto sourceHash =
        llvh::SHA1::hash(llvh::makeArrayRef(buffer, size));

    llvh::raw_string_ostream os(*serializedOut);
    hbc::serializeBytecodeModule(
        *provider->getBytecodeModule(), sourceHash, os, serOptions);
    os.flush();
  }
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target WasmFrontendTests
./cmake-build-asan/unittests/WasmFrontend/WasmFrontendTests --gtest_filter='WasmCompileTest.*'
```

Expected: PASS.

- [ ] **Step 6: Verify no existing caller broke**

```bash
cmake --build cmake-build-asan --target hermes-node hermes
./cmake-build-asan/bin/hermes-node -e 'const b=new Uint8Array([0,97,115,109,1,0,0,0]); console.log(WebAssembly.validate(b))'
```

Expected: builds, prints `false` (that is a header-only module, correctly
invalid).

- [ ] **Step 7: Commit (in the submodule)**

```bash
cd hermes
git add include/hermes/WasmFrontend/WasmCompile.h lib/WasmFrontend/WasmCompile.cpp unittests/WasmFrontend/WasmCompileTest.cpp
git commit -m "Let a Wasm compile hand back serialized bytecode

compileWasmToModuleData builds its provider from a BytecodeModule, so the
bytes an embedder would need to cache the compile are already in hand and
merely discarded. Add an optional out-parameter that serializes them, the
same call hermes_compile_to_bytecode makes for JavaScript.

Defaulted, so no existing caller changes."
cd ..
```

---

### Task 2: The cache hook, from the VM to the C API

**Files:**
- Create: `hermes/include/hermes/VM/WasmCacheHooks.h`
- Modify: `hermes/include/hermes/VM/Runtime.h`
- Modify: `hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp` (`createModuleFromBytes`, line ~576)
- Create: `hermes/API/napi/hermes_napi_wasm_cache.h`, `hermes/API/napi/hermes_napi_wasm_cache.cpp`
- Modify: `hermes/API/napi/CMakeLists.txt`
- Test: `hermes/unittests/napi/NapiWasmCacheTest.cpp` (create), `hermes/unittests/napi/CMakeLists.txt`

**REQUIRED SKILL:** this task modifies `lib/VM/JSLib/WebAssembly/WebAssembly.cpp`
and `include/hermes/VM/Runtime.h`. `hermes/CLAUDE.md` requires invoking the
`gc-safe-coding` skill before writing or modifying C++ in `lib/VM/`,
`include/hermes/VM/` or `API/hermes/`. Invoke it first.

**Interfaces:**
- Consumes: `compileWasmToModuleData(..., std::string *serializedOut)` from Task 1.
- Produces:
  - `vm::WasmCacheHooks` (POD, fields below), `Runtime::setWasmCacheHooks()`,
    `Runtime::getWasmCacheHooks()`.
  - `hermes_wasm_cache_callbacks` and
    `napi_status hermes_set_wasm_cache(napi_env, const hermes_wasm_cache_callbacks *)`.

- [ ] **Step 1: Write the failing test**

Create `hermes/unittests/napi/NapiWasmCacheTest.cpp`:

```cpp
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"
#include "hermes_napi_wasm_cache.h"

#include <string>
#include <vector>

namespace {

using hermes::napi::NapiTestFixture;

/// wat2wasm output for:
///   (module (func (export "add") (param i32 i32) (result i32)
///     (i32.add (local.get 0) (local.get 1))))
static const uint8_t kAdd[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
    0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20,
    0x00, 0x20, 0x01, 0x6a, 0x0b};

/// A recording cache. Serves whatever it was last told to store.
struct FakeCache {
  int lookups = 0;
  int stores = 0;
  int discards = 0;
  int outstandingTokens = 0;
  uint32_t lastConfig = 0xffffffff;
  std::vector<uint8_t> stored;
  bool serve = false;

  static bool lookup(
      void *ctx, const uint8_t *, size_t, uint32_t config,
      const uint8_t **hbc, size_t *hbcSize,
      void (**finalizeCb)(const uint8_t *, size_t, void *),
      void **finalizeHint, void **storeToken) {
    auto *self = static_cast<FakeCache *>(ctx);
    self->lookups++;
    self->lastConfig = config;
    // A token is produced on every path, hit or miss.
    self->outstandingTokens++;
    *storeToken = self;
    if (!self->serve || self->stored.empty())
      return false;
    *hbc = self->stored.data();
    *hbcSize = self->stored.size();
    *finalizeCb = nullptr;
    *finalizeHint = nullptr;
    return true;
  }

  static void store(
      void *ctx, void *, const uint8_t *hbc, size_t hbcSize) {
    auto *self = static_cast<FakeCache *>(ctx);
    self->stores++;
    self->outstandingTokens--;
    self->stored.assign(hbc, hbc + hbcSize);
  }

  static void discard(void *ctx, void *) {
    auto *self = static_cast<FakeCache *>(ctx);
    self->discards++;
    self->outstandingTokens--;
  }

  hermes_wasm_cache_callbacks callbacks() {
    hermes_wasm_cache_callbacks cbs{};
    cbs.struct_size = sizeof(cbs);
    cbs.ctx = this;
    cbs.lookup = &FakeCache::lookup;
    cbs.store = &FakeCache::store;
    cbs.discard = &FakeCache::discard;
    return cbs;
  }
};

/// Compile kAdd through the JS API and return whether it succeeded.
static bool compileAdd(napi_env env) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env, &scope));
  napi_value global = nullptr, result = nullptr;
  EXPECT_EQ(napi_ok, napi_get_global(env, &global));
  // Build the module from a literal byte array so the test needs no fixtures.
  std::string src = "(function(){var b=new Uint8Array([";
  for (size_t i = 0; i < sizeof(kAdd); ++i)
    src += std::to_string(kAdd[i]) + ",";
  src += "]); var m=new WebAssembly.Module(b);"
         "return new WebAssembly.Instance(m).exports.add(2,3);})()";
  napi_value script = nullptr;
  EXPECT_EQ(napi_ok,
            napi_create_string_utf8(env, src.c_str(), NAPI_AUTO_LENGTH, &script));
  napi_status st = napi_run_script(env, script, &result);
  bool ok = false;
  if (st == napi_ok) {
    int32_t v = 0;
    EXPECT_EQ(napi_ok, napi_get_value_int32(env, result, &v));
    ok = (v == 5);
  } else {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
  }
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env, scope));
  return ok;
}

TEST_F(NapiTestFixture, WasmCache_MissCompilesAndStores) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));

  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(1, cache.lookups);
  EXPECT_EQ(1, cache.stores);
  EXPECT_EQ(0, cache.discards);
  EXPECT_EQ(0, cache.outstandingTokens);
  EXPECT_FALSE(cache.stored.empty());
}

TEST_F(NapiTestFixture, WasmCache_HitDiscardsTokenAndDoesNotStore) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));

  EXPECT_TRUE(compileAdd(env_));   // populates
  cache.serve = true;
  EXPECT_TRUE(compileAdd(env_));   // served

  EXPECT_EQ(2, cache.lookups);
  EXPECT_EQ(1, cache.stores);
  EXPECT_EQ(1, cache.discards);
  EXPECT_EQ(0, cache.outstandingTokens);
}

TEST_F(NapiTestFixture, WasmCache_RejectedHitFallsBackAndStores) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));

  // Serve bytes that are not bytecode at all.
  cache.stored = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33};
  cache.serve = true;

  EXPECT_TRUE(compileAdd(env_)) << "a bad hit must fall back to compiling";
  EXPECT_EQ(1, cache.lookups);
  EXPECT_EQ(1, cache.stores) << "the fallback compile must overwrite the entry";
  EXPECT_EQ(0, cache.outstandingTokens);
}

TEST_F(NapiTestFixture, WasmCache_HitSkipsCompilationEntirely) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));

  EXPECT_TRUE(compileAdd(env_)); // populates cache.stored
  cache.serve = true;

  // Hand WebAssembly.Module bytes that could never compile, while the cache
  // serves bytecode for a module that works. If the result works, the
  // compile was genuinely skipped -- a fake that only counted calls could
  // not distinguish that from a compile that happened anyway.
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));
  const char *src =
      "(function(){var b=new Uint8Array([0,97,115,109,9,9,9,9,1,2,3]);"
      "var m=new WebAssembly.Module(b);"
      "return new WebAssembly.Instance(m).exports.add(2,3);})()";
  napi_value script = nullptr, result = nullptr;
  ASSERT_EQ(napi_ok,
            napi_create_string_utf8(env_, src, NAPI_AUTO_LENGTH, &script));
  ASSERT_EQ(napi_ok, napi_run_script(env_, script, &result));
  int32_t v = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, result, &v));
  EXPECT_EQ(5, v) << "the cached module should have been used verbatim";
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, WasmCache_NoHooksStillCompiles) {
  EXPECT_TRUE(compileAdd(env_));
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build cmake-build-asan --target NapiTests 2>&1 | tail -5
```

Expected: compile error — `hermes_napi_wasm_cache.h` does not exist. (If the
napi unittest target has a different name, find it with
`grep -rn add_hermes_unittest hermes/unittests/napi/CMakeLists.txt`.)

- [ ] **Step 3: Add the POD and the Runtime storage**

Create `hermes/include/hermes/VM/WasmCacheHooks.h`:

```cpp
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_WASMCACHEHOOKS_H
#define HERMES_VM_WASMCACHEHOOKS_H

#include <cstddef>
#include <cstdint>

namespace hermes {
namespace vm {

/// An embedder-supplied cache for compiled Wasm modules.
///
/// Plain C function pointers, and no dependency in either direction: the VM
/// does not know about NAPI and the public C header does not include VM
/// headers, so the API layer copies its own struct onto this one field by
/// field.
///
/// Bytes handed back by \c lookup are TRUSTED, exactly as the bytes from an
/// IWasmModuleResolver are: they take the precompiled path without validation
/// or content sniffing. An embedder returning anything other than .hbc
/// produced by this Hermes version has the same consequences as handing
/// untrusted bytes to any other bytecode entry point.
///
/// OWNERSHIP: \c lookup always produces a store token, on hit and on miss
/// alike, and the VM calls exactly one of \c store or \c discard for it.
struct WasmCacheHooks {
  void *ctx = nullptr;

  /// Consult the cache for \p wasm.
  /// On a hit: sets \p hbc / \p hbcSize, and the finalizer the VM calls when
  /// the bytecode is no longer referenced (either may be null for a buffer
  /// the embedder manages itself), and returns true.
  /// On a miss: returns false. Sets \p storeToken either way.
  bool (*lookup)(
      void *ctx,
      const uint8_t *wasm,
      size_t wasmSize,
      uint32_t codegenConfig,
      const uint8_t **hbc,
      size_t *hbcSize,
      void (**finalizeCb)(const uint8_t *, size_t, void *),
      void **finalizeHint,
      void **storeToken) = nullptr;

  /// Persist \p hbc against the identity in \p storeToken, and release it.
  void (*store)(
      void *ctx, void *storeToken, const uint8_t *hbc, size_t hbcSize) =
      nullptr;

  /// Release \p storeToken without persisting anything.
  void (*discard)(void *ctx, void *storeToken) = nullptr;

  bool installed() const {
    return lookup != nullptr && store != nullptr && discard != nullptr;
  }
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_WASMCACHEHOOKS_H
```

In `hermes/include/hermes/VM/Runtime.h`, add `#include
"hermes/VM/WasmCacheHooks.h"` with the other VM includes, and inside the
existing `#ifdef HERMES_ENABLE_WASM` block that holds `setWasmModuleResolver`
(around line 281-297), append:

```cpp
  /// Install the cache consulted before compiling a Wasm module, and asked to
  /// persist the result afterwards. There is at most one; installing replaces
  /// any previous one. See WasmCacheHooks for the ownership contract.
  void setWasmCacheHooks(const WasmCacheHooks &hooks) {
    wasmCacheHooks_ = hooks;
  }

  /// \return the installed hooks; `installed()` is false if the embedder has
  /// not installed any.
  const WasmCacheHooks &getWasmCacheHooks() const {
    return wasmCacheHooks_;
  }
```

and next to the `wasmModuleResolver_` member (around line 1312):

```cpp
  /// Embedder Wasm bytecode cache, empty unless one was installed.
  /// See setWasmCacheHooks().
  WasmCacheHooks wasmCacheHooks_{};
```

- [ ] **Step 4: Consult the hooks at the compile site**

In `hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp`, add near the top of the
file:

```cpp
/// The compile-time configuration that changes generated Wasm code, as a
/// value the embedder folds into its cache key. Anything added here that
/// affects codegen MUST be added to this value, or a cache will serve
/// bytecode built under different rules.
static uint32_t wasmCodegenConfig(Runtime &runtime) {
  return runtime.test262 ? 1u : 0u;
}
```

Then replace the `else` branch of `createModuleFromBytes` (the `.wasm` path,
around line 622) with:

```cpp
  } else {
    // .wasm path — consult the embedder cache, then compile if needed.
    const WasmCacheHooks &hooks = runtime.getWasmCacheHooks();
    const uint32_t codegenConfig = wasmCodegenConfig(runtime);
    void *storeToken = nullptr;
    bool cacheUsable = hooks.installed();

    if (cacheUsable) {
      const uint8_t *cachedHbc = nullptr;
      size_t cachedSize = 0;
      void (*finalizeCb)(const uint8_t *, size_t, void *) = nullptr;
      void *finalizeHint = nullptr;
      if (hooks.lookup(
              hooks.ctx, data, size, codegenConfig, &cachedHbc, &cachedSize,
              &finalizeCb, &finalizeHint, &storeToken)) {
        auto llvmBuf = llvh::MemoryBuffer::getMemBufferCopy(
            llvh::StringRef(
                reinterpret_cast<const char *>(cachedHbc), cachedSize));
        auto ret = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
            std::make_unique<OwnedMemoryBuffer>(std::move(llvmBuf)));
        if (finalizeCb)
          finalizeCb(cachedHbc, cachedSize, finalizeHint);
        if (ret.first) {
          // A good hit: the token is not needed.
          hooks.discard(hooks.ctx, storeToken);
          bcProvider = std::shared_ptr<hbc::BCProviderBase>(std::move(ret.first));
          cacheUsable = false; // nothing left to store
          storeToken = nullptr;
        }
        // A rejected hit keeps storeToken and falls through to compiling,
        // which overwrites the bad entry because the key is content-derived.
      }
    }

    if (!bcProvider) {
      std::string serialized;
      auto compiledData = hermes::compileWasmToModuleData(
          data, size, errorMsg, runtime.test262,
          cacheUsable ? &serialized : nullptr);
      if (!compiledData) {
        if (storeToken)
          hooks.discard(hooks.ctx, storeToken);
        return nullptr;
      }

      if (cacheUsable && !serialized.empty()) {
        hooks.store(
            hooks.ctx, storeToken,
            reinterpret_cast<const uint8_t *>(serialized.data()),
            serialized.size());
        storeToken = nullptr;
        // Load from the bytes that were stored, so a hit and a miss run
        // identical bytecode.
        auto llvmBuf = llvh::MemoryBuffer::getMemBufferCopy(
            llvh::StringRef(serialized.data(), serialized.size()));
        auto ret = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
            std::make_unique<OwnedMemoryBuffer>(std::move(llvmBuf)));
        if (ret.first)
          bcProvider = std::shared_ptr<hbc::BCProviderBase>(std::move(ret.first));
      }
      if (storeToken) {
        hooks.discard(hooks.ctx, storeToken);
        storeToken = nullptr;
      }
      if (!bcProvider)
        bcProvider = compiledData->bytecodeProvider;
    }
  }
```

Add `#include "hermes/VM/WasmCacheHooks.h"` if `Runtime.h` does not already
bring it in transitively.

- [ ] **Step 5: Add the C API**

Create `hermes/API/napi/hermes_napi_wasm_cache.h`:

```cpp
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NAPI_HERMES_NAPI_WASM_CACHE_H
#define HERMES_NAPI_HERMES_NAPI_WASM_CACHE_H

#include "hermes/napi/node_api.h"

#include <stddef.h>
#include <stdint.h>

/// An embedder cache for compiled WebAssembly modules. The first field is the
/// struct size for ABI-stable extensibility, as with hermes_compile_flags.
///
/// Bytes returned by `lookup` are TRUSTED and are loaded as Hermes bytecode
/// without validation or content sniffing, exactly like any other precompiled
/// .hbc the embedder ships.
///
/// OWNERSHIP: `lookup` always sets *store_token, on hit and miss alike, and
/// Hermes calls exactly one of `store` or `discard` for it.
struct hermes_wasm_cache_callbacks {
  size_t struct_size;
  void *ctx;

  /// Return true on a hit, having set *hbc/*hbc_size and, optionally, the
  /// finalizer Hermes calls when it is done with the buffer. Return false on
  /// a miss. Set *store_token either way.
  ///
  /// `codegen_config` identifies the compile-time configuration that affects
  /// generated code; it MUST be part of the cache key, or the cache will
  /// serve bytecode built under different rules.
  bool (*lookup)(
      void *ctx,
      const uint8_t *wasm,
      size_t wasm_size,
      uint32_t codegen_config,
      const uint8_t **hbc,
      size_t *hbc_size,
      void (**finalize_cb)(const uint8_t *, size_t, void *),
      void **finalize_hint,
      void **store_token);

  /// Persist freshly compiled bytecode against the identity in `store_token`,
  /// and release the token.
  void (*store)(
      void *ctx, void *store_token, const uint8_t *hbc, size_t hbc_size);

  /// Release `store_token` without persisting anything.
  void (*discard)(void *ctx, void *store_token);
};

/// Install \p callbacks on \p env's runtime. Passing NULL removes any
/// installed cache. Returns napi_invalid_arg if `struct_size` is not
/// sizeof(hermes_wasm_cache_callbacks) or a required callback is NULL.
NAPI_EXTERN napi_status NAPI_CDECL hermes_set_wasm_cache(
    napi_env env, const hermes_wasm_cache_callbacks *callbacks);

#endif // HERMES_NAPI_HERMES_NAPI_WASM_CACHE_H
```

Create `hermes/API/napi/hermes_napi_wasm_cache.cpp`:

```cpp
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_wasm_cache.h"
#include "hermes_napi_internal.h"

#include "hermes/VM/WasmCacheHooks.h"

napi_status NAPI_CDECL hermes_set_wasm_cache(
    napi_env env, const hermes_wasm_cache_callbacks *callbacks) {
#ifdef HERMES_ENABLE_WASM
  CHECK_ENV(env);
  auto &runtime = getRuntime(env);

  if (callbacks == nullptr) {
    runtime.setWasmCacheHooks(hermes::vm::WasmCacheHooks{});
    return napi_clear_last_error(env);
  }
  if (callbacks->struct_size != sizeof(hermes_wasm_cache_callbacks) ||
      callbacks->lookup == nullptr || callbacks->store == nullptr ||
      callbacks->discard == nullptr) {
    return napi_set_last_error(env, napi_invalid_arg);
  }

  hermes::vm::WasmCacheHooks hooks{};
  hooks.ctx = callbacks->ctx;
  hooks.lookup = callbacks->lookup;
  hooks.store = callbacks->store;
  hooks.discard = callbacks->discard;
  runtime.setWasmCacheHooks(hooks);
  return napi_clear_last_error(env);
#else
  (void)callbacks;
  CHECK_ENV(env);
  return napi_set_last_error(env, napi_generic_failure);
#endif
}
```

These are the real names, taken from `hermes_napi_compile.cpp`: the preamble
is `NAPI_PREAMBLE(env)`, argument checks are `CHECK_ARG(env, x)`, the runtime
is `env->runtime`, and success returns `napi_clear_last_error(env)`. Rewrite
the body above to use them:

```cpp
napi_status NAPI_CDECL hermes_set_wasm_cache(
    napi_env env, const hermes_wasm_cache_callbacks *callbacks) {
#ifdef HERMES_ENABLE_WASM
  NAPI_PREAMBLE(env);
  hermes::vm::Runtime &runtime = env->runtime;

  if (callbacks == nullptr) {
    runtime.setWasmCacheHooks(hermes::vm::WasmCacheHooks{});
    return napi_clear_last_error(env);
  }
  CHECK_ARG(env, callbacks->lookup);
  CHECK_ARG(env, callbacks->store);
  CHECK_ARG(env, callbacks->discard);
  if (callbacks->struct_size != sizeof(hermes_wasm_cache_callbacks))
    return napi_set_last_error(env, napi_invalid_arg);

  hermes::vm::WasmCacheHooks hooks{};
  hooks.ctx = callbacks->ctx;
  hooks.lookup = callbacks->lookup;
  hooks.store = callbacks->store;
  hooks.discard = callbacks->discard;
  runtime.setWasmCacheHooks(hooks);
  return napi_clear_last_error(env);
#else
  (void)callbacks;
  NAPI_PREAMBLE(env);
  return napi_set_last_error(env, napi_generic_failure);
#endif
}
```

Add `hermes_napi_wasm_cache.cpp` to the source list in
`hermes/API/napi/CMakeLists.txt`, and `NapiWasmCacheTest.cpp` to
`hermes/unittests/napi/CMakeLists.txt`, following the existing entries.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-asan --target NapiTests
./cmake-build-asan/unittests/napi/NapiTests --gtest_filter='*WasmCache*'
```

Expected: 5 tests PASS.

- [ ] **Step 7: Verify a Wasm-off build still compiles**

```bash
cmake -B /tmp/wasmoff -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMES_ENABLE_WASM=OFF >/dev/null
cmake --build /tmp/wasmoff --target hermes-node 2>&1 | tail -3
```

Expected: builds. Then `rm -rf /tmp/wasmoff`.

- [ ] **Step 8: Commit (in the submodule)**

```bash
git -C hermes add include/hermes/VM/WasmCacheHooks.h include/hermes/VM/Runtime.h \
  lib/VM/JSLib/WebAssembly/WebAssembly.cpp \
  API/napi/hermes_napi_wasm_cache.h API/napi/hermes_napi_wasm_cache.cpp \
  API/napi/CMakeLists.txt \
  unittests/napi/NapiWasmCacheTest.cpp unittests/napi/CMakeLists.txt
git -C hermes commit -m "Let an embedder cache compiled Wasm modules

createModuleFromBytes is the one place Wasm bytes become a bytecode provider,
and it already had the two branches a cache needs: load precompiled .hbc, or
compile. Consult an embedder-supplied lookup/store/discard set between them.

Bytes from the cache take the trusted precompiled path an IWasmModuleResolver
already uses, so no gate moves and nothing reachable from JavaScript gains a
way to feed bytecode in.

Ownership is single and total: lookup always produces a store token, and
exactly one of store or discard is called for it -- including when a hit is
rejected, where compiling overwrites the bad entry.

The embedder gets codegen_config from Hermes rather than deriving it, so a
future flag affecting Wasm codegen cannot silently invalidate nothing."
```

---

### Task 3: Verify the Hermes half end to end

**Files:**
- Test: none created; this task runs existing suites.

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: nothing.

- [ ] **Step 1: Run the Hermes Wasm suite**

```bash
python3 cmake-build-asan/bin/hermes-lit -s \
  --param test_exec_root=$(pwd)/cmake-build-asan/hermes/test \
  --param wasm_enabled=ON \
  --param wat2wasm=$(pwd)/cmake-build-asan/hermes/external/wabt/wabt/wat2wasm \
  --param wast2json=$(pwd)/cmake-build-asan/hermes/external/wabt/wabt/wast2json \
  --param wasm_testsuite=$(pwd)/hermes/external/wasm-testsuite/tests \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param hermesc=$(pwd)/cmake-build-asan/bin/hermesc \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param hbcdump=$(pwd)/cmake-build-asan/bin/hbcdump \
  --param python=python3 \
  $(pwd)/hermes/test/wasm 2>&1 | tail -8
```

Expected: the same counts as before this work — 237 expected passes, 10
expected failures, and one unexpected failure in `compile-mixed-imports.wat`,
which is an artifact of this invocation not passing `%FileCheckOrRegen` and
not a real failure.

- [ ] **Step 2: Run the hermes-node suite**

```bash
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```

Expected: 333 unit, 196 JS, exit 0. Nothing here changes hermes-node
behaviour yet — no hooks are installed — so any failure is a regression in
Task 1 or 2.

- [ ] **Step 3: Confirm no behaviour changed without hooks**

```bash
cd examples/hermes-parser-ast-wasm && ./run.sh ../../cmake-build-release; cd ../..
```

Expected: `PASS: hermes-parser-ast-wasm`. (Build `cmake-build-release` first
if needed: `cmake --build cmake-build-release --target hermes-node`.)

- [ ] **Step 4: No commit**

This task produces no changes. If any step failed, fix it in Task 1 or 2 and
amend that commit rather than adding a new one.

---

### Task 4: Cache directory configuration

**Files:**
- Create: `include/hermes/node-compat/compile-cache/cache_config.h`
- Create: `lib/compile-cache/cache_config.cpp`
- Modify: `lib/compile-cache/CMakeLists.txt`
- Test: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class CacheRecency : uint8_t { kAtime, kMtime };`
  - `struct CacheConfig { CacheRecency recency = CacheRecency::kAtime;
    uint64_t maxWasmBytes = 268435456ull; };`
  - `CacheConfig cacheConfigParse(std::string_view text);`
  - `CacheConfig cacheConfigLoadOrCreate(const std::string &root);`
  - `inline constexpr const char *kCacheConfigFileName = "config";`

- [ ] **Step 1: Write the failing tests**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
#include <hermes/node-compat/compile-cache/cache_config.h>

TEST(CacheConfigTest, DefaultsAreAtimeAnd256MB) {
  CacheConfig c = cacheConfigParse("");
  EXPECT_EQ(CacheRecency::kAtime, c.recency);
  EXPECT_EQ(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ParsesBothKeys) {
  CacheConfig c = cacheConfigParse(
      "# a comment\n"
      "recency: mtime\n"
      "max_wasm_bytes: 1024\n");
  EXPECT_EQ(CacheRecency::kMtime, c.recency);
  EXPECT_EQ(1024ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ToleratesWhitespaceAndBlankLines) {
  CacheConfig c = cacheConfigParse("\n   recency:   mtime   \n\n");
  EXPECT_EQ(CacheRecency::kMtime, c.recency);
}

TEST(CacheConfigTest, UnknownKeyIsIgnored) {
  CacheConfig c = cacheConfigParse("nonsense: yes\nrecency: mtime\n");
  EXPECT_EQ(CacheRecency::kMtime, c.recency);
}

TEST(CacheConfigTest, BadValueFallsBackToTheDefault) {
  CacheConfig c = cacheConfigParse("recency: yesterday\nmax_wasm_bytes: lots\n");
  EXPECT_EQ(CacheRecency::kAtime, c.recency);
  EXPECT_EQ(268435456ull, c.maxWasmBytes);
}

TEST(CacheConfigTest, ZeroBudgetIsHonoured) {
  // 0 means "evict everything", not "no limit"; it must not be mistaken for
  // an unset value.
  CacheConfig c = cacheConfigParse("max_wasm_bytes: 0\n");
  EXPECT_EQ(0ull, c.maxWasmBytes);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='CacheConfigTest.*'
```

Expected: compile error — no such header.

- [ ] **Step 3: Write the header**

Create `include/hermes/node-compat/compile-cache/cache_config.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// Which timestamp eviction orders by.
enum class CacheRecency : uint8_t {
  /// Real LRU. Under relatime -- the common mount default -- atime is
  /// updated whenever it is older than mtime/ctime or more than 24 hours
  /// old, which is ample resolution for a cache measured in weeks.
  kAtime,
  /// Insertion order. For noatime mounts and filesystems where atime means
  /// nothing, where this is at least wrong knowingly.
  kMtime,
};

/// Configuration read from the cache directory. Every field has a default
/// that is used when the file is absent, unreadable or malformed: a broken
/// configuration must degrade to a working cache, never break a program.
struct CacheConfig {
  CacheRecency recency = CacheRecency::kAtime;
  /// Budget over Wasm entries. 0 means evict everything, not "unlimited".
  uint64_t maxWasmBytes = 268435456ull; // 256 MB
};

/// File name, at the cache ROOT -- outside v1/<generation>/, so generation
/// pruning can never delete it.
inline constexpr const char *kCacheConfigFileName = "config";

/// Parse \p text. Unknown keys and unparseable values are ignored in favour
/// of the default.
CacheConfig cacheConfigParse(std::string_view text);

/// Read <root>/config, writing it with the defaults first if it is absent.
/// Never fails: an unwritable directory just means defaults.
CacheConfig cacheConfigLoadOrCreate(const std::string &root);

} // namespace node_compat
} // namespace hermes
```

- [ ] **Step 4: Write the implementation**

Create `lib/compile-cache/cache_config.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/compile-cache/cache_config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hermes {
namespace node_compat {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

/// The text written when the file is created. Kept beside the parser so the
/// two cannot describe different keys.
const char *kDefaultConfigText =
    "# hermes-node compile cache configuration.\n"
    "#\n"
    "# recency: atime | mtime\n"
    "#   Which timestamp Wasm entry eviction orders by. atime gives real LRU\n"
    "#   under relatime; use mtime on a noatime mount.\n"
    "# max_wasm_bytes: <integer>\n"
    "#   Budget over Wasm bytecode entries. Oldest are evicted first once a\n"
    "#   new entry would exceed it.\n"
    "#\n"
    "# Edited values take effect on the next run.\n"
    "recency: atime\n"
    "max_wasm_bytes: 268435456\n";

} // namespace

CacheConfig cacheConfigParse(std::string_view text) {
  CacheConfig config;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line =
        text.substr(pos, nl == std::string_view::npos ? text.size() - pos
                                                      : nl - pos);
    pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

    line = trim(line);
    if (line.empty() || line.front() == '#')
      continue;
    size_t colon = line.find(':');
    if (colon == std::string_view::npos)
      continue;
    std::string_view key = trim(line.substr(0, colon));
    std::string_view value = trim(line.substr(colon + 1));

    if (key == "recency") {
      if (value == "atime")
        config.recency = CacheRecency::kAtime;
      else if (value == "mtime")
        config.recency = CacheRecency::kMtime;
      // Anything else keeps the default.
    } else if (key == "max_wasm_bytes") {
      std::string v(value);
      char *end = nullptr;
      errno = 0;
      unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
      if (errno == 0 && end != v.c_str() && *end == '\0')
        config.maxWasmBytes = parsed;
    }
    // Unknown keys are ignored.
  }
  return config;
}

CacheConfig cacheConfigLoadOrCreate(const std::string &root) {
  if (root.empty())
    return CacheConfig{};
  std::string path = root + "/" + kCacheConfigFileName;

  std::ifstream in(path, std::ios::binary);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return cacheConfigParse(ss.str());
  }

  // Absent: write the defaults so the knobs are discoverable by looking.
  // Best effort -- an unwritable directory just means defaults in memory.
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (out)
    out << kDefaultConfigText;
  return CacheConfig{};
}

} // namespace node_compat
} // namespace hermes
```

Add `cache_config.cpp` to the sources in `lib/compile-cache/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='CacheConfigTest.*'
```

Expected: 6 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hermes/node-compat/compile-cache/cache_config.h \
  lib/compile-cache/cache_config.cpp lib/compile-cache/CMakeLists.txt \
  unittests/CompileCacheTest.cpp
git commit -m "Add compile cache directory configuration

Two knobs the Wasm entry budget needs: which timestamp eviction orders by,
and how many bytes to keep. In a file at the cache root rather than in the
source, because 256 MB is arbitrary as a constant and reasonable as a
default -- and because a produced executable's user has no command line to
put a flag on.

At the root, outside v1/<generation>/, so generation pruning cannot delete
it. Written with its defaults when absent, so the knobs are discoverable by
looking. Every malformed value falls back to its default; a broken
configuration must degrade to a working cache."
```

---

### Task 5: Content-keyed Wasm entries

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Modify: `lib/compile-cache/compile_cache.cpp`
- Test: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Consumes: `CompileCacheEntry`, `compileCacheWriteEntry`,
  `compileCacheReadEntry`, `CompileCache` (existing).
- Produces:
  - `CompileCacheKind::kWasm` (value 3)
  - `std::string compileCacheWasmDigest(uint32_t codegenConfig, const uint8_t *wasm, size_t size);`
    — 64 lowercase hex characters.
  - `bool CompileCache::lookupWasm(CompileCacheEntry &entry, const uint8_t *wasm, size_t size, uint32_t codegenConfig);`
  - `void CompileCache::saveWasm(const CompileCacheEntry &entry, const uint8_t *hbc, size_t hbcSize);`

- [ ] **Step 1: Write the failing tests**

Append to `unittests/CompileCacheTest.cpp`:

```cpp
TEST(CompileCacheTest, WasmDigestIsStableAndLowercaseHex) {
  const uint8_t bytes[] = {1, 2, 3, 4};
  std::string a = compileCacheWasmDigest(0, bytes, sizeof(bytes));
  std::string b = compileCacheWasmDigest(0, bytes, sizeof(bytes));
  EXPECT_EQ(a, b);
  EXPECT_EQ(64u, a.size());
  for (char c : a)
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
}

TEST(CompileCacheTest, WasmDigestDiffersByContent) {
  const uint8_t a[] = {1, 2, 3, 4};
  const uint8_t b[] = {1, 2, 3, 5};
  EXPECT_NE(
      compileCacheWasmDigest(0, a, sizeof(a)),
      compileCacheWasmDigest(0, b, sizeof(b)));
}

TEST(CompileCacheTest, WasmDigestDiffersByCodegenConfig) {
  // This is the silent failure the whole scheme guards against: the same
  // module compiled under a different configuration must not share an entry.
  const uint8_t bytes[] = {1, 2, 3, 4};
  EXPECT_NE(
      compileCacheWasmDigest(0, bytes, sizeof(bytes)),
      compileCacheWasmDigest(1, bytes, sizeof(bytes)));
}

TEST(CompileCacheTest, WasmDigestOfEmptyInputIsWellDefined) {
  EXPECT_EQ(64u, compileCacheWasmDigest(0, nullptr, 0).size());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='CompileCacheTest.Wasm*'
```

Expected: compile error — `compileCacheWasmDigest` not declared.

- [ ] **Step 3: Declare the additions**

In `include/hermes/node-compat/compile-cache/compile_cache.h`, add to
`CompileCacheKind`:

```cpp
  /// A WebAssembly module, cached by the embedder hooks Hermes calls before
  /// compiling one. Unlike the JavaScript kinds this is keyed by CONTENT,
  /// because Wasm bytes usually arrive without a path.
  kWasm = 3,
```

and, after `compileCacheKey`:

```cpp
/// Lowercase hex SHA-256 over \p codegenConfig (four little-endian bytes)
/// followed by the Wasm bytes. Used as the entry's file name, which is what
/// lets Wasm entries reuse the existing on-disk format unchanged.
///
/// SHA-256 rather than the CRC-32 the JavaScript kinds use because the roles
/// differ: there the CRC is a guard behind a path key, here it would be the
/// key itself, and a collision would serve another module's bytecode with
/// nothing left to detect it.
std::string compileCacheWasmDigest(
    uint32_t codegenConfig, const uint8_t *wasm, size_t size);
```

and inside `class CompileCache`, beside `lookup`/`save`:

```cpp
  /// Fill \p entry's identity from the module's content and try to load it.
  /// Returns true on a hit, in which case the caller owns entry.mapping.
  bool lookupWasm(
      CompileCacheEntry &entry,
      const uint8_t *wasm,
      size_t size,
      uint32_t codegenConfig);

  /// Persist freshly compiled Wasm bytecode. Because the key is derived from
  /// content, this overwrites in place, which is what makes a rejected hit
  /// self-healing.
  void saveWasm(
      const CompileCacheEntry &entry, const uint8_t *hbc, size_t hbcSize);
```

- [ ] **Step 4: Implement**

In `lib/compile-cache/compile_cache.cpp`, add `#include <picohash_wrapper.h>`
(match how `lib/bundle/native_digest.cpp` includes it) and implement:

```cpp
std::string compileCacheWasmDigest(
    uint32_t codegenConfig, const uint8_t *wasm, size_t size) {
  uint8_t configBytes[4] = {
      static_cast<uint8_t>(codegenConfig & 0xff),
      static_cast<uint8_t>((codegenConfig >> 8) & 0xff),
      static_cast<uint8_t>((codegenConfig >> 16) & 0xff),
      static_cast<uint8_t>((codegenConfig >> 24) & 0xff)};

  picohash_ctx_t ctx;
  ph_init_sha256(&ctx);
  ph_update(&ctx, configBytes, sizeof(configBytes));
  if (size != 0)
    ph_update(&ctx, wasm, size);
  uint8_t digest[PICOHASH_SHA256_DIGEST_LENGTH];
  ph_final(&ctx, digest);

  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(sizeof(digest) * 2);
  for (uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xf]);
  }
  return out;
}

bool CompileCache::lookupWasm(
    CompileCacheEntry &entry,
    const uint8_t *wasm,
    size_t size,
    uint32_t codegenConfig) {
  if (!enabled_)
    return false;

  std::string digest = compileCacheWasmDigest(codegenConfig, wasm, size);
  // The digest is the file name; the CRC and size stay as the cheap
  // truncation guard the header already carries.
  entry.key = compileCacheCrc32(digest.data(), digest.size());
  entry.sourceCrc = compileCacheCrc32(wasm, size);
  entry.sourceSize = static_cast<uint32_t>(size);
  entry.cacheFilePath =
      generationDir_ + "/" + digest.substr(0, 2) + "/w" + digest;

  bool hit = compileCacheReadEntry(entry);
  trace(hit ? "wasm hit" : "wasm miss", digest);
  return hit;
}

void CompileCache::saveWasm(
    const CompileCacheEntry &entry, const uint8_t *hbc, size_t hbcSize) {
  // save() already creates the fanout directory from entry.cacheFilePath and
  // writes through the temp-and-rename path, so there is nothing Wasm-
  // specific about persisting the bytes. The sweep in Task 6 is the only
  // thing this adds.
  save(entry, hbc, hbcSize);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='CompileCacheTest.*'
```

Expected: all PASS, including the pre-existing cases.

- [ ] **Step 6: Add a roundtrip test through a real directory**

Append. `unittests/CompileCacheTest.cpp` already has `#include "TempTree.h"`
and `using hermes::node_compat::test::TempTree;` near the top; `TempTree()`
makes a temporary directory and `path()` returns it.

```cpp
TEST(CompileCacheTest, WasmEntryRoundTripsThroughADirectory) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  const uint8_t wasm[] = {0, 97, 115, 109, 1, 0, 0, 0};
  const uint8_t bytecode[] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

  CompileCacheEntry miss;
  EXPECT_FALSE(cache.lookupWasm(miss, wasm, sizeof(wasm), 0));
  cache.saveWasm(miss, bytecode, sizeof(bytecode));

  CompileCacheEntry hit;
  ASSERT_TRUE(cache.lookupWasm(hit, wasm, sizeof(wasm), 0));
  ASSERT_EQ(sizeof(bytecode), hit.bytecodeSize);
  EXPECT_EQ(0, memcmp(bytecode, hit.bytecode, sizeof(bytecode)));
  hit.mapping->destroy();

  // A different codegen config must miss.
  CompileCacheEntry other;
  EXPECT_FALSE(cache.lookupWasm(other, wasm, sizeof(wasm), 1));

  // A one-byte edit must miss.
  uint8_t edited[sizeof(wasm)];
  memcpy(edited, wasm, sizeof(wasm));
  edited[7] = 1;
  CompileCacheEntry edit;
  EXPECT_FALSE(cache.lookupWasm(edit, edited, sizeof(edited), 0));
}
```

Run: `./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='*WasmEntryRoundTrips*'`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/compile-cache/compile_cache.h \
  lib/compile-cache/compile_cache.cpp unittests/CompileCacheTest.cpp
git commit -m "Key compile cache entries by content for Wasm

Wasm bytes usually arrive without a path -- the motivating case has them
base64-encoded inside a JavaScript file -- so the path key the JavaScript
kinds use cannot apply. The digest becomes the file name, which is what lets
these entries reuse the on-disk format unchanged.

SHA-256 rather than CRC-32 because the roles differ: there the CRC guards a
path key, here it would be the key, and a collision would serve another
module's bytecode with nothing left to detect it.

codegenConfig is mixed into the digest rather than into the generation name,
which would otherwise duplicate the whole JavaScript cache into a second
directory the first time a program ran with -test262."
```

---

### Task 6: Bounding Wasm entries

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`
- Modify: `lib/compile-cache/compile_cache.cpp`
- Test: `unittests/CompileCacheTest.cpp`

**Interfaces:**
- Consumes: `CacheConfig`, `CacheRecency` (Task 4); `CompileCache` (Task 5).
- Produces:
  - `void CompileCache::setConfig(const CacheConfig &config);`
  - `void compileCacheEvictWasm(const std::string &generationDir, const CacheConfig &config);`
    — public so it can be tested directly.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CompileCacheTest, EvictionKeepsTheBudget) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  CacheConfig config;
  config.maxWasmBytes = 300; // room for two of the entries below, not three
  config.recency = CacheRecency::kMtime;
  cache.setConfig(config);

  // Three entries of ~100 bytes of payload each, written oldest first.
  std::vector<uint8_t> payload(100, 0xab);
  for (int i = 0; i < 3; ++i) {
    uint8_t wasm[] = {0, 97, 115, 109, 1, 0, 0, static_cast<uint8_t>(i)};
    CompileCacheEntry e;
    EXPECT_FALSE(cache.lookupWasm(e, wasm, sizeof(wasm), 0));
    cache.saveWasm(e, payload.data(), payload.size());
    // Distinct timestamps, since the sweep orders by them.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  }

  compileCacheEvictWasm(cache.generationDir(), config);

  // The oldest is gone; the newest survives.
  uint8_t oldest[] = {0, 97, 115, 109, 1, 0, 0, 0};
  uint8_t newest[] = {0, 97, 115, 109, 1, 0, 0, 2};
  CompileCacheEntry a, b;
  EXPECT_FALSE(cache.lookupWasm(a, oldest, sizeof(oldest), 0));
  EXPECT_TRUE(cache.lookupWasm(b, newest, sizeof(newest), 0));
  if (b.mapping)
    b.mapping->destroy();
}

TEST(CompileCacheTest, EvictionLeavesJavaScriptEntriesAlone) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path(), "test-generation"));

  CompileCacheEntry js;
  std::string source = "1 + 1";
  EXPECT_FALSE(
      cache.lookup(js, source, "/tmp/x.js", CompileCacheKind::kCommonJS));
  const uint8_t bytecode[] = {1, 2, 3, 4};
  cache.save(js, bytecode, sizeof(bytecode));

  CacheConfig config;
  config.maxWasmBytes = 0; // evict every Wasm entry
  compileCacheEvictWasm(cache.generationDir(), config);

  CompileCacheEntry again;
  EXPECT_TRUE(
      cache.lookup(again, source, "/tmp/x.js", CompileCacheKind::kCommonJS));
  if (again.mapping)
    again.mapping->destroy();
}
```

Add `#include <chrono>`, `#include <thread>` and `#include <vector>` at the
top of the test file.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='CompileCacheTest.Eviction*'
```

Expected: compile error — `setConfig` and `compileCacheEvictWasm` not declared.

- [ ] **Step 3: Declare and implement**

In the header, add near `compileCachePruneGenerations`:

```cpp
/// Delete Wasm entries under \p generationDir, oldest first by the timestamp
/// \p config selects, until their total size is within
/// \p config.maxWasmBytes.
///
/// Only files named "w<hex>" are considered, so JavaScript entries -- which
/// are keyed by path and rewritten in place, and so never accumulate the way
/// content-keyed entries do -- are untouched.
///
/// Best effort: failures are ignored.
void compileCacheEvictWasm(
    const std::string &generationDir, const CacheConfig &config);
```

and in the class:

```cpp
  /// Configuration read from the cache directory. Must be set before the
  /// first saveWasm() for the budget to be honoured.
  void setConfig(const CacheConfig &config) {
    config_ = config;
  }
```

with members:

```cpp
  CacheConfig config_{};
  /// The sweep runs at most once per process, on the first Wasm save. A save
  /// only happens on a miss, which is already paying a full compile, so a
  /// directory walk is free by comparison -- and a process that only ever
  /// hits does no extra work.
  bool sweptWasm_ = false;
```

In `saveWasm`, after the write:

```cpp
  if (!sweptWasm_) {
    sweptWasm_ = true;
    compileCacheEvictWasm(generationDir_, config_);
  }
```

Add `#include <sys/stat.h>`, `<dirent.h>`, `<algorithm>`, `<vector>` and
`<unistd.h>`, then implement:

```cpp
void compileCacheEvictWasm(
    const std::string &generationDir, const CacheConfig &config) {
  if (generationDir.empty())
    return;

  struct Victim {
    std::string path;
    uint64_t size;
    int64_t stamp;
  };
  std::vector<Victim> entries;
  uint64_t total = 0;

  DIR *root = ::opendir(generationDir.c_str());
  if (!root)
    return;
  while (struct dirent *fan = ::readdir(root)) {
    if (fan->d_name[0] == '.')
      continue;
    std::string fanDir = generationDir + "/" + fan->d_name;
    DIR *sub = ::opendir(fanDir.c_str());
    if (!sub)
      continue;
    while (struct dirent *ent = ::readdir(sub)) {
      // Only Wasm entries. JavaScript entries are keyed by path and
      // rewritten in place, so they never accumulate the way these do.
      if (ent->d_name[0] != 'w')
        continue;
      std::string path = fanDir + "/" + ent->d_name;
      struct stat st;
      if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        continue;
      int64_t stamp = config.recency == CacheRecency::kAtime
          ? static_cast<int64_t>(st.st_atime)
          : static_cast<int64_t>(st.st_mtime);
      entries.push_back({path, static_cast<uint64_t>(st.st_size), stamp});
      total += static_cast<uint64_t>(st.st_size);
    }
    ::closedir(sub);
  }
  ::closedir(root);

  if (total <= config.maxWasmBytes)
    return;

  // Oldest first.
  std::sort(
      entries.begin(), entries.end(), [](const Victim &a, const Victim &b) {
        return a.stamp < b.stamp;
      });

  for (const Victim &v : entries) {
    if (total <= config.maxWasmBytes)
      break;
    // Best effort: a file another process is still mapping unlinks fine on
    // POSIX -- the inode survives until the last mapping is dropped, which
    // is the same property compileCachePruneGenerations relies on.
    if (::unlink(v.path.c_str()) == 0)
      total -= v.size;
  }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target CompileCacheTest
./cmake-build-asan/unittests/CompileCacheTest --gtest_filter='CompileCacheTest.*'
```

Expected: all PASS. The eviction test takes about 3 seconds because of the
timestamp separation; that is expected.

- [ ] **Step 5: Commit**

```bash
git add include/hermes/node-compat/compile-cache/compile_cache.h \
  lib/compile-cache/compile_cache.cpp unittests/CompileCacheTest.cpp
git commit -m "Bound Wasm compile cache entries

Content keys lose the property that keeps the JavaScript cache bounded: a
path-keyed entry is rewritten in place, so editing a file leaves nothing
behind, while every edit to a Wasm module yields a new digest and a new file.
At the measured 3.6x bytecode-to-module ratio, iterating on a Wasm build
leaves a full entry behind on every rebuild.

So Wasm entries get a size budget, swept oldest-first at most once per
process on the first save -- which only happens on a miss, already paying a
full compile. JavaScript entries are identified by name and left alone: one
LRU over both would evict a cheap-to-recreate entry to make room for an
expensive one without knowing the difference."
```

---

### Task 7: Install the hooks

**Files:**
- Create: `include/hermes/node-compat/compile-cache/wasm_cache_hooks.h`
- Create: `lib/compile-cache/wasm_cache_hooks.cpp`
- Modify: `lib/compile-cache/CMakeLists.txt`
- Modify: `lib/runtime/hermes_node_runtime.cpp`
- Test: `test/test-wasm-cache.js` (created in Task 8; this task is verified by hand)

**Interfaces:**
- Consumes: `hermes_set_wasm_cache`, `hermes_wasm_cache_callbacks` (Task 2);
  `CompileCache::lookupWasm`/`saveWasm` (Task 5); `CacheConfig` (Task 4).
- Produces: `void installWasmCacheHooks(napi_env env, CompileCache *cache);`

- [ ] **Step 1: Write the header**

Create `include/hermes/node-compat/compile-cache/wasm_cache_hooks.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Install \p cache as the Wasm bytecode cache for \p env, so that Hermes
/// consults it before compiling a module and persists the result afterwards.
///
/// A null \p cache installs nothing, which is how --no-compile-cache and
/// --inspect end up uncached: both already make createCompileCache return
/// null.
///
/// \p cache must outlive \p env.
void installWasmCacheHooks(napi_env env, CompileCache *cache);

} // namespace node_compat
} // namespace hermes
```

- [ ] **Step 2: Write the implementation**

Create `lib/compile-cache/wasm_cache_hooks.cpp`. The token is a heap-allocated
`CompileCacheEntry`; Hermes guarantees exactly one of `store`/`discard` per
token, so ownership is unambiguous:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/compile-cache/wasm_cache_hooks.h"

#include "hermes_napi_wasm_cache.h"

namespace hermes {
namespace node_compat {

namespace {

/// What lookup() hands back for store()/discard(). Carries the identity
/// lookup already derived, so store never recomputes the digest -- two
/// derivations from the same inputs can drift, and the failure would be an
/// entry the next lookup never finds.
struct StoreToken {
  CompileCache *cache;
  CompileCacheEntry entry;
};

bool cacheLookup(
    void *ctx,
    const uint8_t *wasm,
    size_t wasmSize,
    uint32_t codegenConfig,
    const uint8_t **hbc,
    size_t *hbcSize,
    void (**finalizeCb)(const uint8_t *, size_t, void *),
    void **finalizeHint,
    void **storeToken) {
  auto *cache = static_cast<CompileCache *>(ctx);
  auto *token = new StoreToken{cache, CompileCacheEntry{}};
  *storeToken = token;

  if (!cache->lookupWasm(token->entry, wasm, wasmSize, codegenConfig))
    return false;

  // Hand the mapping to Hermes, which releases it through the finalizer when
  // the bytecode provider dies -- exactly as hermes_run_bytecode does for
  // JavaScript entries.
  *hbc = token->entry.bytecode;
  *hbcSize = token->entry.bytecodeSize;
  *finalizeCb = &CacheMapping::finalizer;
  *finalizeHint = token->entry.mapping;
  token->entry.mapping = nullptr; // ownership transferred
  return true;
}

void cacheStore(
    void *ctx, void *storeToken, const uint8_t *hbc, size_t hbcSize) {
  (void)ctx;
  auto *token = static_cast<StoreToken *>(storeToken);
  token->cache->saveWasm(token->entry, hbc, hbcSize);
  delete token;
}

void cacheDiscard(void *ctx, void *storeToken) {
  (void)ctx;
  auto *token = static_cast<StoreToken *>(storeToken);
  if (token->entry.mapping)
    token->entry.mapping->destroy();
  delete token;
}

} // namespace

void installWasmCacheHooks(napi_env env, CompileCache *cache) {
  if (cache == nullptr || !cache->enabled())
    return;

  hermes_wasm_cache_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.ctx = cache;
  callbacks.lookup = &cacheLookup;
  callbacks.store = &cacheStore;
  callbacks.discard = &cacheDiscard;
  (void)hermes_set_wasm_cache(env, &callbacks);
}

} // namespace node_compat
} // namespace hermes
```

Add `wasm_cache_hooks.cpp` to `lib/compile-cache/CMakeLists.txt`. The
Hermes NAPI headers are already reachable from this library; if the include
of `hermes_napi_wasm_cache.h` does not resolve, add
`${PROJECT_SOURCE_DIR}/hermes/API/napi` to the target's include directories
the way another target in the tree already does.

- [ ] **Step 3: Load the config and install the hooks**

In `lib/runtime/hermes_node_runtime.cpp`, in `createCompileCache` (around line
498), after `cache->enable(...)` succeeds, load and apply the configuration:

```cpp
  cache->setConfig(cacheConfigLoadOrCreate(root));
```

where `root` is the cache root the function already computed (the directory
holding `v1/`, not the generation directory). Add
`#include <hermes/node-compat/compile-cache/cache_config.h>`.

Then, at the point where `runtimeState->compileCache` is assigned (around line
789) and the `napi_env` exists, add:

```cpp
  // Hermes consults this before compiling a WebAssembly module. Installed
  // after the env exists and before any user code runs. A null cache --
  // --no-compile-cache, or --inspect -- installs nothing.
  installWasmCacheHooks(env, runtimeState->compileCache);
```

Add `#include <hermes/node-compat/compile-cache/wasm_cache_hooks.h>`. Use
whatever the local variable for the `napi_env` is at that point.

- [ ] **Step 4: Verify by hand end to end**

```bash
cmake --build cmake-build-release --target hermes-node
rm -rf /tmp/wcache
cd examples/hermes-parser-ast-wasm
time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/wcache ast.js sample.js > /tmp/a.json
time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/wcache ast.js sample.js > /tmp/b.json
cmp /tmp/a.json /tmp/b.json && echo "IDENTICAL"
find /tmp/wcache -name 'w*' | head
cd ../..
```

Expected: the second run is markedly faster than the first, the outputs are
identical, and at least one `w<hex>` file exists under `/tmp/wcache`.

- [ ] **Step 5: Verify tracing names the outcomes**

```bash
HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE \
  ./cmake-build-release/bin/hermes-node --compile-cache=/tmp/wcache \
  examples/hermes-parser-ast-wasm/ast.js \
  examples/hermes-parser-ast-wasm/sample.js 2>&1 >/dev/null | grep -i wasm
```

Expected: a `wasm hit` line.

- [ ] **Step 6: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/compile-cache/wasm_cache_hooks.h \
  lib/compile-cache/wasm_cache_hooks.cpp lib/compile-cache/CMakeLists.txt \
  lib/runtime/hermes_node_runtime.cpp
git commit -m "Install the Wasm bytecode cache

Adapts CompileCache onto the hooks Hermes consults before compiling a
WebAssembly module. The token lookup hands back carries the identity it
already derived, so store never recomputes the digest.

A hit's mapping is handed to Hermes with CacheMapping::finalizer, the same
ownership transfer hermes_run_bytecode already uses for JavaScript entries,
because the module holds its provider well past the call that made it.

Nothing is installed when the cache is null, which is how --no-compile-cache
and --inspect stay uncached: createCompileCache already returns null for
both."
```

---

### Task 8: End-to-end tests

**Files:**
- Create: `test/test-wasm-cache.js`
- Test: itself

**Interfaces:**
- Consumes: everything above.
- Produces: nothing.

- [ ] **Step 1: Write the test**

Create `test/test-wasm-cache.js`. Read `test/test-compile-cache*.js` first (if
any exist) and follow their shape; use `%hermes-node-cc`, the substitution
that clears the suite-wide disable, since the suite sets
`HERMES_NODE_DISABLE_COMPILE_CACHE=1`:

```javascript
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm
// RUN: rm -rf %t.cache && mkdir -p %t.cache
// RUN: %hermes-node-cc --compile-cache=%t.cache %s | %FileCheck %s
// RUN: %hermes-node-cc --compile-cache=%t.cache %s | %FileCheck %s
// An entry exists, and its name marks it as a Wasm entry.
// RUN: find %t.cache -name 'w*' | head -1 | %FileCheck --check-prefix=ENTRY %s
// The configuration file is written where generation pruning cannot reach it.
// RUN: cat %t.cache/config | %FileCheck --check-prefix=CONFIG %s
// The second run must report a hit rather than merely being fast.
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc \
// RUN:   --compile-cache=%t.cache %s 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=TRACE %s
// With the cache off, nothing is written.
// RUN: rm -rf %t.nocache && mkdir -p %t.nocache
// RUN: %hermes-node --no-compile-cache --compile-cache=%t.nocache %s \
// RUN:   | %FileCheck %s
// RUN: find %t.nocache -name 'w*' | wc -l | %FileCheck --check-prefix=NONE %s

// Caching a compiled WebAssembly module.
//
// Hit and miss are asserted from tracing, never from timing: these fixtures
// are tiny and the suite runs 16-way parallel, so a timing assertion would
// measure scheduling noise. That is how both known flaky tests got that way.

var mods = require('./fixtures/wasm/modules.js');

var inst = new WebAssembly.Instance(new WebAssembly.Module(mods.ADD));
console.log('add', inst.exports.add(19, 23));

// A second, different module must get its own entry rather than colliding.
var fib = new WebAssembly.Instance(new WebAssembly.Module(mods.FIB), {
  env: { log: function () {} },
});
console.log('fib', fib.exports.fib(10));

// CHECK: add 42
// CHECK: fib 55
// ENTRY: {{w[0-9a-f]+}}
// CONFIG: recency: atime
// CONFIG: max_wasm_bytes: 268435456
// TRACE: wasm hit
// NONE: 0
```

- [ ] **Step 2: Run it**

```bash
python3 cmake-build-asan/bin/hermes-lit -v $(pwd)/test/test-wasm-cache.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-asan/test
```

Expected: PASS. If a `RUN` line's expectation is wrong (for instance the trace
wording), fix the test to match what the implementation actually prints —
adjust the CHECK, not the implementation, unless the output is genuinely
unhelpful.

- [ ] **Step 3: Add the bundle case**

Append these RUN lines after the existing ones, and confirm they pass:

```javascript
// A bundled program uses the cache too: a container carries no compiled Wasm,
// so it compiles at every launch without one.
// RUN: rm -rf %t.btree %t.bcache && mkdir -p %t.btree %t.bcache
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.btree/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.btree/app.js
// RUN: %hermes-node --build-bundle=%t.btree/app.hbb %t.btree/app.js
// RUN: %hermes-node-cc --compile-cache=%t.bcache --bundle=%t.btree/app.hbb | %FileCheck --check-prefix=BUNDLE %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc \
// RUN:   --compile-cache=%t.bcache --bundle=%t.btree/app.hbb 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=TRACE %s

// BUNDLE: add 42
```

- [ ] **Step 4: Add the remaining cases from the spec**

Append these RUN lines and CHECK prefixes, then rerun the single-test command
from Step 2.

```javascript
// A corrupt entry recovers rather than surfacing as a broken program, and is
// rewritten -- content keys make that automatic, since the fresh store lands
// on the same file name.
// RUN: rm -rf %t.corrupt && mkdir -p %t.corrupt
// RUN: %hermes-node-cc --compile-cache=%t.corrupt %s | %FileCheck %s
// RUN: for f in $(find %t.corrupt -name 'w*'); do printf 'junk' > $f; done
// RUN: %hermes-node-cc --compile-cache=%t.corrupt %s | %FileCheck %s
// RUN: env HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc \
// RUN:   --compile-cache=%t.corrupt %s 2>&1 >/dev/null \
// RUN:   | %FileCheck --check-prefix=TRACE %s

// Two distinct modules get two entries rather than colliding. This file
// compiles ADD and FIB, so exactly two must appear.
// RUN: rm -rf %t.two && mkdir -p %t.two
// RUN: %hermes-node-cc --compile-cache=%t.two %s | %FileCheck %s
// RUN: find %t.two -name 'w*' | wc -l | %FileCheck --check-prefix=TWO %s

// --inspect disables the cache, because entries are compiled at THROWING and
// the debugger needs ALL. Nothing must be written.
// RUN: rm -rf %t.insp && mkdir -p %t.insp
// RUN: %hermes-node-cc --compile-cache=%t.insp --inspect-brk %s < /dev/null \
// RUN:   > /dev/null 2>&1 || true
// RUN: find %t.insp -name 'w*' | wc -l | %FileCheck --check-prefix=NONE %s

// TWO: 2
```

If the `--inspect-brk` line hangs waiting for a debugger, drop that case and
instead assert the same property from a unit test on `createCompileCache`,
which already returns null for `config.inspect || config.inspectBrk`. Do not
leave a hanging RUN line in the suite.

- [ ] **Step 5: Add the executable case**

```javascript
// A produced executable uses the cache too. Gated, like the other build-exe
// tests, on a linker and a cut kit.
// REQUIRES: linker-available
// RUN: rm -rf %t.exe && mkdir -p %t.exe/ship %t.exe/cache
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t.exe/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t.exe/app.js
// RUN: %hermes-node --build-bundle=%t.exe/app.hbb %t.exe/app.js
// RUN: %hermes-node --build-exe=%t.exe/ship/app --kit=%kit_dir %t.exe/app.hbb
// RUN: env HERMES_NODE_COMPILE_CACHE=%t.exe/cache %t.exe/ship/app | %FileCheck --check-prefix=BUNDLE %s
// RUN: env HERMES_NODE_COMPILE_CACHE=%t.exe/cache HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t.exe/ship/app 2>&1 >/dev/null | %FileCheck --check-prefix=TRACE %s
```

A `REQUIRES:` line applies to the whole file, so this case cannot live in
`test-wasm-cache.js` alongside the ungated ones. Put it in its own file,
`test/test-wasm-cache-build-exe.js`, with `REQUIRES: wasm, linker-available`
and its own copy of the fixture-loading entry script. Run it with the
single-test command from Step 2 plus
`--param kit_dir=$(pwd)/cmake-build-asan/kit`.

- [ ] **Step 6: Run the whole suite**

```bash
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```

Expected: 333 unit tests and 198 JS tests, exit 0 (197 if the build-exe
case reports UNSUPPORTED for want of a kit). Two known flaky tests
(`test-repl-history.js`, `test-inspect.js`) can fail under parallel load;
confirm any failure in isolation before treating it as a regression, per the
"Known flaky tests" section of CLAUDE.md.

- [ ] **Step 7: Commit**

```bash
git add test/test-wasm-cache.js test/test-wasm-cache-build-exe.js
git commit -m "Test the Wasm compile cache end to end

Two runs against one cache directory, with the second asserted to be a hit
from tracing rather than from timing -- the fixtures are tiny and the suite
runs 16-way parallel, so a timing assertion would measure scheduling noise.

Covers a bundled run as well, which is not incidental: a container carries no
compiled Wasm, so without a cache it compiles at every launch."
```

---

### Task 9: Documentation and measurement

**Files:**
- Modify: `CLAUDE.md`
- Create: `docs/superpowers/plans/progress-wasm-compile-cache.md`

**Interfaces:**
- Consumes: everything above.
- Produces: nothing.

- [ ] **Step 1: Measure**

```bash
cmake --build cmake-build-release --target hermes-node
rm -rf /tmp/wmeasure
cd examples/hermes-parser-ast-wasm
echo "cold:"; time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/wmeasure ast.js sample.js >/dev/null
echo "warm:"; time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/wmeasure ast.js sample.js >/dev/null
du -sh /tmp/wmeasure
cd ../..
```

Record the three numbers. For a larger case, run the same two commands
against `examples/flow-bundler` with `FLOW_BUNDLER_PARSER=wasm`.

- [ ] **Step 2: Write the progress file**

Create `docs/superpowers/plans/progress-wasm-compile-cache.md` recording:
which plan it tracks (`docs/superpowers/plans/2026-09-07-wasm-compile-cache.md`),
which tasks are complete, the measured cold/warm numbers and cache size from
Step 1, and anything discovered during implementation that the design did not
anticipate.

- [ ] **Step 3: Document in CLAUDE.md**

Add a subsection to the existing "Compile Cache" section covering: that Wasm
modules are cached, that entries are content-keyed by SHA-256 with the digest
as the file name while JavaScript entries stay path-keyed, that the cache
applies in every run mode including a produced executable, the configuration
file at the cache root with its two keys and defaults, that the budget covers
Wasm entries only and why, and that the Hermes side is a `lookup`/`store`/
`discard` hook consulted at `createModuleFromBytes` whose bytes take the
trusted precompiled path so no trust gate moves. Cite the measured numbers
from Step 1. Follow the section's existing voice: say what is true now and
why the non-obvious parts are the way they are.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/superpowers/plans/progress-wasm-compile-cache.md
git commit -m "Document the Wasm compile cache"
```

- [ ] **Step 5: Report the submodule state**

The `hermes` submodule is on branch `wasm-compile-cache` with two commits, and
the gitlink in hermes-node-compat is deliberately unstaged. Tell the user:
the two Hermes commit subjects, that the branch is ready to be grafted onto
`wasm-new`, and that nothing has been staged for them. Do not stage, bump,
rebase or merge anything.
