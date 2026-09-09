# Baking Compiled WebAssembly Into A Bundle -- Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a WebAssembly module inside an AOT container compile once, at
build time, and load from the container on every run -- including from a
`--build-exe` executable on a machine whose compile cache is empty.

**Architecture:** A run-time flag `--record-wasm=<file>` writes a
self-contained record file holding the compiled bytecode of every Wasm module
the run compiled or looked up, keyed by the existing SHA-256 content digest.
`--build-bundle --bake-wasm=<file>` copies those entries into a new container
section (format v6). At run time the container is consulted as a cache tier
ahead of the disk compile cache, through the `hermes_set_wasm_cache` hooks
that already exist. A sixth tool verb, `--dump-wasm=<file>`, inspects a
record file.

**Tech Stack:** C++17, CMake + Ninja, GTest (`unittests/`), LLVM lit
(`test/`), picohash (vendored), Hermes VM + NAPI.

**Spec:** `docs/superpowers/specs/2026-09-09-wasm-bundle-bake-design.md`

## Global Constraints

- **No Hermes change.** `hermes_set_wasm_cache` is already sufficient. Do not
  edit anything under `hermes/`, do not `git add hermes`, and do not run
  `git submodule update`. The gitlink is mid-flight (see the Wasm compile
  cache work) and its management is the user's.
- **Never run `git add -A` or `git add .`** from the repository root. Stage
  explicit paths in every commit.
- **Copyright headers:** new files get `Copyright (c) Tzvetan Mikov.` with
  the MIT boilerplate already on neighbouring files in the same directory.
- **Commit messages:** ASCII only, no emojis.
- **Format before every commit that touches C++:** `./utils/format.sh -f`
  (clang-format 18 only).
- **Build and test with `cmake-build-asan`**, the primary development
  configuration. `cmake --build cmake-build-asan --target check-hermes-node`
  runs both suites.
- **Hits and misses are proven from `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`
  tracing, never from timing.** The suite runs 16-way parallel and both of
  its known flaky tests got that way through timing dependencies.
- **Wasm-dependent code is guarded by `#ifdef HERMES_ENABLE_WASM`** where it
  touches the Wasm hooks, matching `wasm_cache_hooks.cpp`. The record file
  format, the container section and the tool verb are NOT guarded: a
  container carrying Wasm entries must still dump and validate in a build
  configured without WebAssembly.
- **A container with no Wasm entries must not gain a `WASM` section**, and
  must run exactly as it did. Its bytes DO change -- the header grows and the
  format version is stamped by the writer -- so this is a behavioural
  invariant, not a byte-for-byte one. Do not write a test asserting the
  latter.
- **Every failure inside the compile cache stays swallowed** (see CLAUDE.md's
  "every failure in here is swallowed on purpose"). Two exceptions, both
  deliberate: `--record-wasm`'s own file writes, because it is a diagnostic
  the user asked for by name; and a container hit that comes back to
  `store`, which terminates the run, because that means the artifact's
  bytecode was refused. The cache is an optimisation and the container is
  not.

## File Structure

| File | Responsibility |
| --- | --- |
| `include/hermes/node-compat/bundle/bundle_format.h` (modify) | v6: `BundleWasmRecord`, two header fields |
| `include/hermes/node-compat/bundle/wasm_record.h` (create) | Record-file writer and reader |
| `lib/bundle/wasm_record.cpp` (create) | Implement both; atomic rewrite on each new digest |
| `lib/bundle/bundle_writer.cpp` (modify) | `addWasm()`, lay out the table and its payloads |
| `include/hermes/node-compat/bundle/bundle_writer.h` (modify) | Declare `addWasm()` |
| `lib/bundle/bundle_reader.cpp` (modify) | Validate and expose the Wasm table |
| `include/hermes/node-compat/bundle/bundle_reader.h` (modify) | `wasmCount()`, `wasm(i)`, `wasmFor(digest)`, `wasmTableSize()` |
| `lib/bundle/CMakeLists.txt` (modify) | Add `wasm_record.cpp` to `hermesNodeBundle` |
| `lib/bundle/bundle_run.cpp` (modify) | `bundleWasmLookup()` over `openBundleState()` |
| `include/hermes/node-compat/bundle/bundle_run.h` (modify) | Declare it |
| `lib/bundle/bundle_build.cpp` (modify) | Read record files, bake, report under `--verbose` |
| `include/hermes/node-compat/bundle/bundle_build.h` (modify) | a `bakeWasmPaths` parameter on `buildBundle()` |
| `lib/bundle/bundle_tools.cpp` (modify) | `WASM` section in `--dump`; `dumpWasmRecord()` |
| `include/hermes/node-compat/bundle/bundle_tools.h` (modify) | Declare `dumpWasmRecord()` |
| `lib/compile-cache/wasm_cache_hooks.cpp` (modify) | Container tier, recorder, optional cache |
| `include/hermes/node-compat/compile-cache/wasm_cache_hooks.h` (modify) | New install signature |
| `lib/compile-cache/CMakeLists.txt` (modify) | `hermesNodeCompileCacheRun` links `hermesNodeBundleRun` |
| `lib/runtime/hermes_node_runtime.cpp` (modify) | Create the recorder, install hooks when either half is present |
| `include/hermes/node-compat/runtime/hermes_node_runtime.h` (modify) | `recordWasmPath` on the config |
| `tools/hermes-node/hermes-node.cpp` (modify) | Parse the three flags, `checkToolOptions` rows, `--dump-wasm` verb |
| `unittests/BundleFormatTest.cpp` (modify) | v6 round trip, alignment, validation; the v5 expectation at :61 |
| `unittests/BundleToolsTest.cpp` (modify) | the `format v5` expectation at :109 |
| `unittests/CompileCacheTest.cpp` (modify) | all eleven `lookupWasm` call sites |
| `test/bundle-dump.js` (modify) | the `format v5` expectation at :13 |
| `unittests/WasmRecordTest.cpp` (create) | Record-file round trip, truncation, misalignment |
| `unittests/CMakeLists.txt` (modify) | Add the new test |
| `test/test-wasm-record.js` (create) | Record a run; record again warm |
| `test/test-wasm-bake.js` (create) | Record, bake, run bundled: container hit |
| `test/test-wasm-bake-errors.js` (create) | Version mismatch, corrupt file, flag conflicts |
| `test/test-wasm-bake-build-exe.js` (create) | The same through a produced executable |
| `CLAUDE.md` (modify) | Document the feature |
| `docs/superpowers/plans/progress-wasm-bundle-bake.md` (create) | Progress and measurements |

## Task Order

Tasks 1-3 are the format layer and are testable with no runtime. Task 4 is
the producer, task 5 the run-time tier, task 6 the CLI surface, task 7 the
end-to-end tests, task 8 documentation. Each task ends in a commit.

---

### Task 1: The record file format

**Files:**
- Create: `include/hermes/node-compat/bundle/wasm_record.h`
- Create: `lib/bundle/wasm_record.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_format.h` (add `BundleWasmRecord`)
- Modify: `lib/bundle/CMakeLists.txt`
- Test: `unittests/WasmRecordTest.cpp`, `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: `atomic_write.h`, `mapped_file.h`.
- Produces: `WasmRecordWriter` (accumulate + atomic rewrite) and
  `WasmRecordReader` (validate + iterate).

`BundleWasmRecord` goes in `bundle_format.h` because both formats use it and
the container is the one with a version to bump:

```cpp
/// One compiled WebAssembly module, in a container's Wasm table or in a
/// --record-wasm file. The two formats share this record so that baking is
/// an append with relocated offsets rather than a translation.
///
/// `digest` is the raw SHA-256 the compile cache computes over the codegen
/// configuration and the module bytes -- the same value that names a disk
/// cache entry -- so it is a key, not metadata, and lives inline rather
/// than in the string table: every lookup binary-searches on it, and an
/// index would put a string-table dereference inside each step.
///
/// No checksum, in either file. Both are validated structurally -- magic,
/// version, and every offset, length and range -- and neither verifies the
/// bytes inside a payload. Do not add one: on the run path it would cost
/// every launch, and in the record file it would guard a file that travels
/// from a run to a build on one machine. Leaving them out ACCEPTS undetected
/// corruption rather than deferring a diagnostic -- damage bad enough for
/// Hermes to refuse the bytecode terminates the run, but damage that leaves
/// the header valid executes. That is the trade the container already makes
/// for every payload it holds (bundle_reader.h).
struct BundleWasmRecord {
  uint8_t digest[kNativeDigestBytes];
  uint32_t payloadOffset;
  uint32_t payloadSize;
};
static_assert(
    sizeof(BundleWasmRecord) == kNativeDigestBytes + 8,
    "BundleWasmRecord must have no padding: it is written to disk verbatim");
```

- [ ] **Step 1: Write the failing test**

Create `unittests/WasmRecordTest.cpp`. Cover, at minimum: a round trip of two
entries; that `open()` rejects bad magic, a wrong format version, a truncated
file, a misaligned `recordTableOffset`, an out-of-range payload range, and a
that `record()` with a digest already
present replaces the payload; and that a writer flushed with no entries
produces a file `open()` accepts with `count() == 0`.

Build the corrupt cases by serializing a good file and mutating bytes, not by
hand-assembling headers -- a hand-assembled header drifts from the writer.

- [ ] **Step 2: Register the target, then run it and watch it fail to compile**

`unittests/CMakeLists.txt` uses a helper, not bare `add_executable`:

```cmake
add_node_compat_unittest(WasmRecordTest WasmRecordTest.cpp)
target_link_libraries(WasmRecordTest hermesNodeBundle)
```

```bash
cmake --build cmake-build-asan --target WasmRecordTest
```

Expected: a compile error -- `wasm_record.h` does not exist yet.

- [ ] **Step 3: Declare the format**

`include/hermes/node-compat/bundle/wasm_record.h`:

```cpp
/// Eight bytes, no NUL, like kBundleMagic.
constexpr char kWasmRecordMagic[8] = {'H','N','W','A','S','M','R','C'};

/// Bumped when the layout changes. A mismatch is a hard error at bake time.
constexpr uint32_t kWasmRecordFormatVersion = 1;

/// Fixed-width. Everything variable is reached through an offset, so that
/// the record table is always 4-byte aligned however long the build version
/// string is -- a length-prefixed string written inline ahead of the table
/// would leave it at an arbitrary offset, and reading a uint32 there is
/// undefined behaviour whatever x86-64 tolerates.
struct WasmRecordHeader {
  char magic[8];
  uint32_t formatVersion;
  uint32_t count;
  uint32_t versionOffset;
  uint32_t versionLength;
  uint32_t recordTableOffset;
  uint32_t payloadOffset;
  uint32_t payloadSize;
};
```

- [ ] **Step 4: Implement the writer**

`WasmRecordWriter(std::string path, std::string buildVersion)` accumulates
`{digest, bytecode}` in a `std::vector`, sorted by digest on serialize.

```cpp
/// Add or replace the entry for \p digest, then rewrite the file.
///
/// Replacing rather than keeping the first is required, not incidental: a
/// lookup records the bytes it returned, and Hermes may then REJECT them and
/// compile (see the design's "Rejected hits"). The store that follows must
/// overwrite what the lookup recorded, or the next bake carries the bad
/// entry forward.
///
/// A record whose digest and bytes already match rewrites nothing.
bool record(const uint8_t *digest, const uint8_t *bytecode, size_t size);

/// Write the file as it stands, including with no entries. Called once when
/// the RUNTIME is created -- after the flag-conflict checks, before user code
/// runs. Not at flag-parse time: the same-file refusal in Task 6 lives in
/// checkToolOptions(), which runs after the parse loop, so an earlier write
/// would overwrite `app.js` in `--record-wasm=app.js app.js` before the
/// guard meant to stop it had run.
bool flush();
```

Serialization order: header, version bytes, pad to 4, record table, pad to 8,
payloads each padded to 8. Write through
`writeFileAtomically(outPath, data, size, err)`
(`include/hermes/node-compat/bundle/atomic_write.h`).

- [ ] **Step 5: Implement the reader**

`WasmRecordReader::open(data, size, error)` validates in this order: size at
least `sizeof(WasmRecordHeader)`, magic, format version, every offset and
length in range, `recordTableOffset % alignof(BundleWasmRecord) == 0`,
`payloadOffset % 8 == 0`, then each record's payload range, then each
`payloadOffset % 8 == 0`, then each record's payload range. Return
`std::nullopt` with a message naming what failed.

Validation is structural only. It stops a malformed file from being read out
of bounds; it does not verify payload bytes, and nothing should.

- [ ] **Step 6: Rebuild, run the tests, then format and commit**

```bash
cmake --build cmake-build-asan --target WasmRecordTest
./cmake-build-asan/unittests/WasmRecordTest
./utils/format.sh -f
git add include/hermes/node-compat/bundle/wasm_record.h lib/bundle/wasm_record.cpp \
        include/hermes/node-compat/bundle/bundle_format.h lib/bundle/CMakeLists.txt \
        unittests/WasmRecordTest.cpp unittests/CMakeLists.txt
```

---

### Task 2: Container format v6

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_format.h`
- Modify: `include/hermes/node-compat/bundle/bundle_writer.h`, `lib/bundle/bundle_writer.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h`, `lib/bundle/bundle_reader.cpp`
- Test: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Produces: `BundleWriter::addWasm(rawDigest, bytecode)`;
  `BundleReader::wasmCount()`, `wasm(i)`, `wasmFor(rawDigest)`,
  `wasmTableSize()`.

- [ ] **Step 1: Write the failing tests**

In `BundleFormatTest.cpp`: a container with two Wasm entries round-trips;
`wasmFor()` finds each and misses a digest that is not there; a container
with no Wasm entries still opens and reports `wasmCount() == 0`; a v5
container is refused by **both** `open()` and `openForInspection()`; a
misaligned `wasmTableOffset` is refused; a Wasm payload range outside the
payload region is refused.

- [ ] **Step 2: Extend the header**

Add to `BundleHeader`, after the VM-options pair and before `containerFlags`
-- **appending inside the struct is what forces the version bump**; do not
try to make this backward compatible:

```cpp
  // The Wasm table: BundleWasmRecord[wasmCount], sorted by digest so a
  // lookup binary-searches it with memcmp. A section of its own for the
  // reason the preload and native tables are ones -- a real container has
  // ~1500 modules and one or two Wasm entries.
  uint32_t wasmTableOffset;
  uint32_t wasmCount;
```

Bump `kBundleFormatVersion` to 6.

- [ ] **Step 3: Writer**

`addWasm()` appends to a `PendingWasm` vector. In `serialize()`: lay the Wasm
table out after the VM-options table with `kTableAlign`, sort by digest, and
assign each entry a payload offset from the **same cursor module payloads
use**, so a baked entry is relocated rather than copied.

- [ ] **Step 4: Reader**

Validate `wasmTableOffset` alongside the four existing tables (alignment,
then bounds), then each record's payload range. Add the accessors; `wasmFor`
is a `std::lower_bound` with `memcmp`.

- [ ] **Step 5: Update the existing v5 expectations**

The version bump breaks every assertion that names 5 explicitly. Fix them in
this task, not in Task 7, or the tree does not go green at this commit. Find
them rather than working from a list that will drift:

```bash
grep -n '5u\|format v5' unittests/BundleFormatTest.cpp unittests/BundleToolsTest.cpp
grep -rn 'format v5' test/
```

At the time of writing that is four `EXPECT_EQ(...formatVersion(), 5u)` in
`BundleFormatTest.cpp` (:61, :545, :622, :786), one `format v5` in
`BundleToolsTest.cpp` (:109) and one in `test/bundle-dump.js` (:13).

**`test/bundle-corrupt-payload.js` also breaks here**, and not through a
version string: it reads the header by field index, and this task inserts two
fields before `containerFlags`. Every index at or after it moves up by two,
in the `F` table near the top of that file:

```
CONTAINER_FLAGS: 15 -> 17
PAYLOAD_OFFSET:  16 -> 18
PAYLOAD_SIZE:    17 -> 19
```

The indices before those are unaffected. The test asserts its own assumption
and fails loudly rather than corrupting nothing, so a missed update is a red
suite and not a silently useless test -- but it is this task's to fix.
Assertions written against `kBundleFormatVersion` rather than a literal need
no change -- that is the difference to look for.

- [ ] **Step 6: Rebuild, run, format, commit**

```bash
cmake --build cmake-build-asan --target BundleFormatTest
./cmake-build-asan/unittests/BundleFormatTest
```

---

### Task 3: `--dump` section and the `--dump-wasm` verb

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_tools.h`, `lib/bundle/bundle_tools.cpp`
- Test: `unittests/BundleToolsTest.cpp`

- [ ] **Step 1: Write the failing tests**

A container with Wasm entries dumps a `WASM` section listing each digest
(truncated to 16 hex characters without `--verbose`, full with it) and byte
length; a container without one prints no such section; `SECTIONS` gains a
`wasm` row unconditionally, like `vmopts`. `dumpWasmRecord()` on a good file
prints the build version, whether it matches, and one line per entry; on a
corrupt file it returns non-zero with the reader's message.

- [ ] **Step 2: Implement**

Follow `NATIVES` exactly for the section (print only when non-empty; the
truncation rule and its comment are already there to copy). `dumpWasmRecord`
lives here, not in `hermesNodeBundleBuild`: it needs no VM, which is what
keeps `BundleToolsTest` runtime-free.

- [ ] **Step 3: Run, format, commit**

---

### Task 4: Baking at build time

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_build.h`, `lib/bundle/bundle_build.cpp`

**Interfaces:**
- Consumes: `WasmRecordReader`, `BundleWriter::addWasm`.
- Produces: a `bakeWasmPaths` parameter on `buildBundle()`.

**There is no build-options object.** `buildBundle()` takes individual
arguments (`bundle_build.h:123`) and `hermes_node_runtime.cpp:1492` forwards
them from `HermesNodeConfig`. So this task is three edits, and all three must
land together or the tree does not build:

1. `std::vector<std::string> bakeWasmPaths` on `HermesNodeConfig`
   (`hermes_node_runtime.h`), beside `includeModules` and `preloadModules`.
2. A `const std::vector<std::string> &bakeWasmPaths` parameter on
   `buildBundle()`, after `preloads`.
3. The forwarding argument at the `buildBundle()` call site.

The flag that fills the config field is Task 6; until then the vector is
always empty, which is a working intermediate state.

- [ ] **Step 1: Implement, after compilation and beside the native copies**

For each path, in flag order: map it, `WasmRecordReader::open` (a failure is
a hard build error naming the file and the reason), compare its build version
to `HERMES_NODE_VERSION_STRING` for exact equality (a mismatch is a hard
error printing both), then `addWasm` each entry. A digest already added by an
earlier file is skipped -- first file wins, and `--verbose` says so.

A file with zero entries warns and the build continues:

```
warning: <path> records no WebAssembly modules
```

- [ ] **Step 2: Verbose reporting**

```
wasm: <digest[0:16]> <bytes> bytes (from <record file>)
wasm: 1 module baked, 2054332 bytes
```

- [ ] **Step 3: Test through the lit suite in Task 7**

There is no unit-level entry point for the producer; its coverage is the
end-to-end tests. This task still commits on its own: with no flag to fill
`bakeWasmPaths` yet, the build compiles and every existing test passes.

---

### Task 5: The run-time tier and the recorder

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_run.h`, `lib/bundle/bundle_run.cpp`
- Modify: `include/hermes/node-compat/compile-cache/wasm_cache_hooks.h`, `lib/compile-cache/wasm_cache_hooks.cpp`
- Modify: `include/hermes/node-compat/compile-cache/compile_cache.h`, `lib/compile-cache/compile_cache.cpp`
- Modify: `lib/compile-cache/CMakeLists.txt`, `lib/runtime/hermes_node_runtime.cpp`
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h`

**This is the task with the design's three corrections in it.** Read the
design's "What changes at run time" in full before starting.

- [ ] **Step 1: `bundleWasmLookup()` in `bundle_run.cpp`**

`OpenBundle::containerPath` (the `.hbb` for `--bundle`, the executable for an
embedded one) already exists, added when `fatalBadPayload()` landed, because
`root` is only the directory and a message about a damaged container has to
name the file to rebuild.

**It is not reachable from `wasm_cache_hooks.cpp`**, and neither is anything
else about the open container: `OpenBundle` and `openBundleState()` live in
an anonymous namespace, and `bundleWasmLookup` hands back only bytes. So
export the *reporting*, not the path:

```cpp
/// Report a baked Wasm entry this runtime refused, and leave. The digest is
/// raw, kNativeDigestBytes long.
///
/// Here rather than in the caller because the message names the container,
/// which only this file knows -- the same reason fatalBadPayload() lives
/// here for a JavaScript module. Keeping both messages in one place is also
/// what stops them drifting into two different accounts of the same fault.
[[noreturn]] void bundleFatalWasmRefused(const uint8_t *rawDigest);
```

Do not widen `OpenBundle` or publish `containerPath` instead: the path is
the only thing a caller would want it for, and handing out the path invites a
second, differently-worded message.


```cpp
/// Find a baked Wasm entry in the container this process is running, if any.
/// Returns false when no container is open or the digest is not baked.
///
/// The payload is NOT verified: a container payload is unchecked here as it
/// is everywhere else. On success the bytes point into the container's
/// mapping, which outlives the run -- so the caller hands them to Hermes
/// with a NULL finalizer.
bool bundleWasmLookup(
    const uint8_t *rawDigest, const uint8_t **bytes, size_t *size);
```

It reads `openBundleState()` on every call. Do NOT capture the reader at
install time: `installWasmCacheHooks` runs during runtime creation, and
`openBundle()` runs later, from `runBundle(env, ...)`.

- [ ] **Step 2: `lookupWasm` takes the digest**

One change to `CompileCache`:

```cpp
/// As today, but takes the digest rather than deriving it.
bool lookupWasm(CompileCacheEntry &entry, const std::string &digest,
                const uint8_t *wasm, size_t size);
```

So the hook computes the digest once for every tier and no two derivations
can drift.

**No separate "prepare the entry" step, and this is worth stating because an
earlier draft of this plan had one.** The concern was that a container hit
returns without ever calling `lookupWasm`, leaving `cacheFilePath` empty, so
a `store` arriving afterwards would hit `save()`'s silent empty-path return
(`compile_cache.cpp:424`) and the bytecode would never reach the disk cache.
That store is now fatal -- it means the container's bytecode was refused --
so nothing is written and there is nothing to prepare for. Every path that
does write to disk goes through `lookupWasm` first, which fills the entry as
it does today. Do not reintroduce the split.

`unittests/CompileCacheTest.cpp` calls `lookupWasm` in **eleven** places.
Find them rather than trusting a list that will drift:

```bash
grep -n 'lookupWasm' unittests/CompileCacheTest.cpp   # expect 11 hits
```

All of them must be updated in this task, or the unit suite does not compile
at its commit.

- [ ] **Step 3: Rewrite the hooks**

```cpp
struct WasmCacheContext {
  CompileCache *cache = nullptr;        // may be null
  WasmRecordWriter *recorder = nullptr; // may be null
};

/// What lookup() hands back for store()/discard(). Carries the identity on
/// EVERY path, hit and miss alike: Hermes discards the token only for an
/// ACCEPTED hit. A rejected hit keeps it, compiles, and calls store()
/// (hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp:679-710).
struct StoreToken {
  WasmCacheContext *ctx;
  std::array<uint8_t, kNativeDigestBytes> digest;
  CompileCacheEntry entry; // only meaningful when ctx->cache exists
  /// Which tier answered the lookup. Hermes passes no origin to store()
  /// (WebAssembly.cpp:707), so a store arriving after a container hit is
  /// recognizable only from what the lookup recorded here -- and that is
  /// exactly the rejected-hit case.
  enum class Origin { kMiss, kContainer, kDisk } origin = Origin::kMiss;
};
```

`lookup`:
1. Compute the digest once (all tiers key on it).
2. Put the digest on the token. It lives there independently of the disk
   entry, because the recorder and the fatality message need it whether or
   not a cache exists.
3. Container tier. On a hit, trace `wasm container hit`, set
   `origin = kContainer`, record the bytes, and return them with
   `finalize_cb = nullptr`; otherwise trace `wasm container miss` and fall
   through. With no container open, trace nothing -- there is no tier to
   report on.
4. Disk tier, when there is a cache: `cache->lookupWasm(...)`, which traces
   `wasm hit` / `wasm miss` as it does today. On a hit, record and return.
5. Otherwise return false; the token still carries the digest and the
   prepared entry.

`store`, when `token->origin == Origin::kContainer`: **this is fatal.**
Reaching `store` after a container hit means Hermes refused the container's
bytecode. Trace `wasm container refused` first, so a trace being read
explains the exit instead of stopping mid-story, then call
`bundleFatalWasmRefused(digest)` from Step 1 -- the message names the
container, which only `bundle_run.cpp` knows. Do not print it here. That
helper ends in `fatalExit(1)`
(`include/hermes/node-compat/process/node_process.h`). It flushes stdio,
restores the terminal, and outranks a requested exit already in progress --
`process.exit()` flushes by running the event loop, so a queued callback can
reach a fatal error from inside it, and without the precedence the clean
exit's status would stand. It also deliberately does not run the loop
itself, so no further JavaScript executes and no callback can hang the exit.
Both properties landed with `fatalBadPayload()` and are pinned by
`test/bundle-corrupt-payload.js` and `test/test-process-exit-code.js`.

Do not save, do not record, do not continue with the module Hermes compiled
instead. A broken artifact is not a slow artifact, and `BundleReader::open()`
already treats a structurally invalid container as fatal; `fatalBadPayload()`
in `bundle_run.cpp` is the same rule for a JavaScript module, and its comment
carries the reasoning.

Note that this path needs no magic check of its own, unlike the JavaScript
one: Hermes calling `store` after a container hit *is* the signal, so the
detection here is exact where `fatalBadPayload`'s is approximate.

`store` otherwise: save to the disk cache when there is one, and record the
bytes, **replacing** whatever a lookup recorded for that digest. Trace
`wasm store`. A refused *disk* entry lands here and falls back silently, on
purpose -- the cache is best effort and the container is not.

**The fatal branch does not reach every refusal, and the design lists the
gaps.** Do not write a comment or a test claiming it does: a refusal whose
recompile fails or serializes to nothing ends in `discard` instead
(`WebAssembly.cpp:698-704`, `:706`, `:723`), and a provider Hermes accepts
and then fails to run has already had its token discarded (`:681`, `:741`).

`discard`: free the token, releasing a disk mapping if one is held.

Recording traces `wasm record <tier>` so a test can tell which tier the bytes
came from. Every event above is required by the design's Tracing table; a
test asserts the container miss, so it cannot be dropped as redundant with
the disk tier's.

Nothing on this path verifies the payload. If a baked entry is damaged,
Hermes either refuses it -- fatal, see `store` below -- or runs something
subtly wrong, exactly as for a damaged module payload.

- [ ] **Step 4: Install on any configured consumer**

```cpp
void installWasmCacheHooks(napi_env env, WasmCacheContext *ctx, bool bundleMode);
```

Install when `ctx->cache`, `ctx->recorder` or `bundleMode` is set, where
`bundleMode` is `!config.bundlePath.empty() || config.embeddedBundleData`.
Dropping the null-cache early return is the CRITICAL review finding: with it,
a baked container is invisible to `--no-compile-cache`, to a missing
`$HOME/.cache` and to a shipped executable whose user disabled the cache.

**Do not simplify this to installing unconditionally.** Hermes sets
`cacheUsable` from `hooks.installed()`, not from what a lookup returns
(`WebAssembly.cpp:659`), so an installed hook that always misses still makes
every Wasm compile serialize its bytecode, SHA-1 the input and call `store`.
Conditioning on the two config fields costs nothing: both are already what
`hermes_node_runtime.cpp:1481-1485` branches on to choose a run mode.

- [ ] **Step 5: Wire it in `hermes_node_runtime.cpp`**

Create the `WasmRecordWriter` when `config.recordWasmPath` is set, `flush()`
it immediately, and **fail the run loudly if that write fails** -- before
user code, so the error cannot be mistaken for the program's own output.
This is the one loud failure in a feature whose neighbour swallows
everything; the design says why.

**Create it here, not during flag parsing.** `checkToolOptions()` runs after
the parse loop (`hermes-node.cpp:955`) and holds the same-file refusal in
Task 6; a recorder that wrote at parse time would already have overwritten
`app.js` in `--record-wasm=app.js app.js` before the guard meant to stop it
had run.

**A rewrite that fails later needs its own policy, because the ABI has no
channel for one.** `lookup` returning false means "cache miss" and `store`
returns void (`hermes_napi.h:294`, `:323`), so a failed `record()` cannot be
reported through Hermes. Do this instead:

1. Print the reason to stderr immediately, once per run -- not once per
   failure, which would flood a full disk.
2. Set a `recordingFailed` flag on the context, and make `runHermesNode`
   return non-zero when it is set.

State the limit in the code comment rather than pretending it is airtight:
the stderr line is guaranteed, the exit status is not, because
`process.exit()` bypasses the reconciliation that would apply it. That is the
same asymmetry `process.exitCode` already has, and the message is the half
that matters -- a recording run whose file is wrong must not look successful.

- [ ] **Step 6: CMake**

`hermesNodeCompileCacheRun` links `hermesNodeBundleRun` (VM-free below the
napi layer). Add a comment saying why the compile cache now knows about
containers: the hook callback is the one place both tiers meet.

- [ ] **Step 7: Build, run the unit suite, format, commit**

---

### Task 6: The CLI surface

**Files:**
- Modify: `tools/hermes-node/hermes-node.cpp`
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h`

- [ ] **Step 1: Parse the three flags**

`--record-wasm=<file>` and `--bake-wasm=<file>` (repeatable) onto
`HermesNodeConfig` -- there is no build-options object, see Task 4 --
and `--dump-wasm=<file>` onto `ToolOptions`.
All before the first positional, like every other flag.

- [ ] **Step 2: `checkToolOptions()` rows**

Every row from the design's flag surface table, each naming both flags. Put
them with the existing rows, after the parse loop, so flag order never
matters.

- [ ] **Step 3: The same-file refusal**

Refuse `--record-wasm` naming the same file as `--bundle`'s container or the
script being run, via `isSameFile()`. Without it the recorder's rename
replaces a container that the running process keeps reading from its existing
mapping -- the run succeeds and the artifact is destroyed.

- [ ] **Step 4: Dispatch `--dump-wasm` in `runToolVerb()`**

Beside the other five, before `runHermesNode`, so it needs no runtime.

- [ ] **Step 5: Build, format, commit**

---

### Task 7: End-to-end tests

**Files:**
- Create: `test/test-wasm-record.js`, `test/test-wasm-bake.js`,
  `test/test-wasm-bake-refused.js`, `test/test-wasm-record-exit.js`,
  `test/test-wasm-bake-errors.js`, `test/test-wasm-bake-build-exe.js`

Every case in the design's Testing section. Two rules that have already cost
this project time:

- **Never assert a hit or miss from timing.** Use
  `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` and the new trace events.
- **`test/lit.cfg` disables the compile cache for the whole suite, with an
  environment variable that is checked BEFORE `--compile-cache=`**
  (`hermes_node_runtime.cpp:533`). So a case needing a live disk cache must
  run through the **`%hermes-node-cc`** substitution (`test/lit.cfg:34`),
  which clears it, and then pass its own `--compile-cache=%t.cache`.
  `--compile-cache=` alone leaves the cache off and the case silently proves
  nothing. A produced executable takes no flags at all, so a build-exe case
  sets the environment explicitly -- `test/test-wasm-cache-build-exe.js:18`
  is the worked example.

The regression test for the CRITICAL finding is the case that runs a baked
container with the cache disabled and still asserts `wasm container hit`.

Two further cases exist because a unit test cannot reach them -- they need a
real `hermes_set_wasm_cache` round trip:

- `test/test-wasm-bake-refused.js`: overwrite a baked payload with zeros,
  leaving every offset and length valid, so the bytecode magic fails and
  Hermes refuses it. Assert the process **exits non-zero** with a message
  naming the container and the digest. A second case does the same to a disk
  cache entry and asserts the opposite -- the run succeeds and compiles --
  which is what pins the asymmetry between an artifact and a cache. Patching
  either file is a few lines over `fs`.
- `test/test-wasm-record-exit.js`: a program that instantiates a module and
  then calls `process.exit(0)`, asserting the record file is complete. Both
  exit paths call `_exit()`, so a write-at-the-end recorder would lose it.

- [ ] Run the full suite: `cmake --build cmake-build-asan --target check-hermes-node`
- [ ] Commit

---

### Task 8: Documentation

- [ ] Add a "Baked Wasm entries" subsection to CLAUDE.md under AOT Bundles,
      cross-referencing the Compile Cache section rather than restating it.
- [ ] Create `docs/superpowers/plans/progress-wasm-bundle-bake.md` with
      measurements: cold, warm-disk-cache and baked launch times for
      `examples/hermes-parser-ast-wasm`, and the container size delta.
- [ ] Commit
