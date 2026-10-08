# Native WebAssembly in build-native Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A default `hermes-node build-native` artifact runs every WebAssembly module its recording run saw as natively compiled code, reached through Hermes's own `new WebAssembly.Module`, and says on stderr when a module falls back to bytecode.

**Architecture:** Four Hermes changes give the embedder Wasm cache an optional content-keyed `lookup_native` that returns a unit creator, stamp every Wasm unit's static registration with the codegen configuration it was compiled under (checked before the unit runs), and add an optional `accepted` notification. hermes-node records each module's source bytes (record format v2, committed only when Hermes accepted the module), compiles them with `shermes` and `cc` in the existing `build-native` job pool, links a sorted digest-to-creator table into the payload object, and answers `lookup_native` by SHA-256 plus binary search.

**Tech Stack:** C++17 (hermes-node libraries are `-fno-exceptions`; the Hermes VM has neither exceptions nor RTTI), C (generated units, addon glue), CMake + Ninja, GTest, LLVM lit + FileCheck, picohash SHA-256, Static Hermes (`shermes`), wabt `wat2wasm`.

**Spec:** `docs/superpowers/specs/2026-10-08-native-wasm-design.md`. Read it before any task; every task argues from it.

## Global Constraints

- **Two repositories, never in one task.** Tasks 2-6 commit inside `hermes/` (the submodule working tree) on the local branch `native-wasm-lookup`, which Task 2 Step 1 creates at `80ea8dc99`. Tasks 1 and 7-13 commit in hermes-node-compat on `work-static-compile`.
- **Never `git add hermes`, `git add -A` or `git add .` from the hermes-node-compat root.** Committing in the submodule makes the gitlink dirty (`M hermes`); it stays dirty and unstaged for the whole plan. The Hermes commits change public ABI and must land upstream on `hermes-node` before any gitlink may pin them; the user decides when. Stage explicit paths in every hermes-node-compat commit.
- **Never push. Create no branch other than `native-wasm-lookup` in the submodule.**
- **Hermes commits never mention hermes-node** (message, comments, tests).
- **Copyright headers:** new files under `hermes/` get `Copyright (c) Meta Platforms, Inc. and affiliates.`; new hermes-node-compat files get `Copyright (c) Tzvetan Mikov.` Both with the MIT boilerplate the neighbouring files carry.
- **Commit messages:** ASCII only, no emojis, ending with
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ
  ```
- **Formatting:** clang-format 18 only. hermes-node-compat: `./utils/format.sh -f` from the root before every commit. Submodule: `clang-format-18 -i <each C/C++ file you touched under hermes/>` (Hermes has its own `.clang-format`).
- **Build tree:** `cmake-build-asan` (Debug, Clang, ASAN). Hermes's own tests run from the same tree: `cmake-build-asan/hermes/unittests/napi/NapiTests`, `cmake-build-asan/hermes/unittests/VMRuntime/VMRuntimeTests`, and lit through `LIT_FILTER='<regex>' cmake --build cmake-build-asan --target check-hermes` (lit matches the whole path, so anchor with `\.wat$`, not a bare `$`).
- **Before every hermes-node-compat commit:** `cmake --build cmake-build-asan --target check-hermes-node` is green except for the known flakes listed in CLAUDE.md ("Known flaky tests") and `test/build-native-builtins.js`, which already fails on x86-64 (dz `01a11c51`).
- **Values fixed by the spec, verbatim:**
  - unit name: `w` + the 64 lowercase hex digits of SHA-256 over the module bytes **alone**;
  - codegen configuration string: `hermes-wasm;bc=<n>;cg=<n>;t262=<0|1>`;
  - payload symbols `hermesNodeNativeWasmUnits` (40-byte entries: 32 digest bytes then an 8-byte creator pointer, sorted by digest, `.p2align 3`, `.data.rel.ro` on ELF), `hermesNodeNativeWasmUnitCount`, `hermesNodeNativeWasmPolicy` -- all three emitted unconditionally;
  - producer line: `wasm: native <first 16 hex digits> (<n> bytes)`;
  - the two run-time warnings, exactly:
    ```
    warning: WebAssembly module <16 hex> was not compiled natively (it is not in the recording); falling back to bytecode
    warning: WebAssembly module <16 hex>: its native unit is unavailable or incompatible with this run (e.g. -test262); falling back to bytecode
    ```
  - trace lines `wasm native hit` / `wasm native miss` under `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`;
  - opt-out flag `--bytecode-wasm`; record format version 2.
- **Do not bump `WASM_CODEGEN_VERSION` or `BYTECODE_VERSION`.** Nothing here changes generated code. The SH unit-ABI component of the model symbol *is* bumped (Task 2), because `SHWasmUnitReg`'s layout changes.
- **Hit and miss are asserted from tracing, never from timing.**
- **While iterating on Hermes with the gitlink uncommitted, the generation tag sticks at `-dirty`** (CLAUDE.md, "Bumping it by hand is load-bearing"), so any manual hermes-node run during this plan uses `--no-compile-cache`. Lit already disables the cache suite-wide.

## Decisions the spec left open, made here

1. **Policy bit.** `hermesNodeNativeWasmPolicy` is 1 for every `build-native` artifact built without `--bytecode-wasm`, whether or not `--bake-wasm` was given. The spec's criterion is completeness; an artifact compiling Wasm at run time because nobody recorded is exactly what the warning exists to surface. Consequence: with no recording at all, the first warning still says "it is not in the recording".
2. **One configuration function.** Header-only `hermes/include/hermes/WasmFrontend/WasmCodegenConfig.h` defines `inline std::string hermes::wasmCodegenConfigString(bool test262)`. The runtime (`WebAssembly.cpp`) describes itself with it; the SH backend (`SH.cpp`) stamps it into each registration from the emitting compiler's constants and the module's own `Context` `test262` setting.
3. **Registry lookup by creator:** `SHERMES_EXPORT const SHWasmUnitReg *_sh_wasm_find_unit_reg(SHUnitCreator creator);` returns the static node itself. Nodes are never unregistered, and nothing but `next` changes after registration, so the pointer and the strings stay valid after the mutex is released.
4. **The v1 refusal runs first thing in `buildNativeExecutable`**, before the kit is read, so it is testable without a toolchain. The bake step keeps a defensive version check.
5. **The effective `-test262` parse runs for every `build-native`**, with or without `--bake-wasm`. Every invalid baked `--vm=` therefore becomes a build-native build error (spec section 3 accepts this). `--build-bundle` is unchanged.
6. **One pool.** `compileNativeBatch()` runs JavaScript and Wasm jobs in the same worker pool, so `--jobs` bounds both. `compileModules()` becomes a wrapper with an empty Wasm list.
7. **Record v2 layout:** source payloads share the payload area with bytecode payloads, with offsets relative to `payloadOffset`, exactly as bytecode offsets are.
8. **Hermes NAPI harness = a CMake-built addon** (`napi_wasm_lookup_addon`) holding the Wasm units, loaded by the `hermes` binary through `loadNativeModule()`. Verified feasible on 2026-10-08: a Wasm unit compiled with `shermes -emit-c` and built into a `.node` without linking the VM loads into the ASAN `hermes` binary and runs, because `hermes` links `-rdynamic` (`hermes/tools/hermes/CMakeLists.txt`).
9. **Hand-written registrations take their configuration from a real unit.** In the NAPI addon, at module init (every constructor in the addon has run by then). In `native-unit-throws.wat`, from a header the RUN lines extract from the good unit's emitted C. Hard-coding the string would drift with every version bump.

## Review Focus

1. **Feature-detection probe.** A native artifact whose program compiles bytes Hermes rejects (`try { new WebAssembly.Module(probe) } catch {}`) prints the first warning once, then the program gets the same `CompileError` it gets under `--bytecode-wasm`. Pinned in Task 12 (`GARBAGE`).
2. **Every entry point hits.** `new WebAssembly.Module`, `WebAssembly.compile`, `WebAssembly.instantiate(bytes)` and a repeat of the same bytes all take the native tier, with no warning. Pinned in Task 12.
3. **The same module in two recordings.** One recording made plain and one under `--vm=-test262` both carry the same module: it is compiled once, with one `wasm: native` line and one table entry. Pinned in Task 12.
4. **A recording made entirely from a warm disk cache** (every module a disk hit) still carries source bytes and still builds natively. Pinned in Task 8 (`CompileCacheRunTest`) and Task 12 (lit).
5. **Baked `-test262` hits.** Units compiled under a baked `--vm=-test262` run natively, and an out-of-bounds store traps from native code. Pinned in Task 12.

---

### Task 1: Measure `cc` on the largest real module (gate)

The spec's open risk (section 5) is that `cc` cannot practically compile the C that `shermes` generates from hermes-parser's 768,303-byte module. Measure it before building anything.

**Files:**
- Create: `docs/superpowers/plans/progress-native-wasm.md`
- Scratch (not committed): `$S` = this session's scratchpad directory

**Interfaces:**
- Consumes: nothing.
- Produces: the numbers in the progress file; a go/stop decision.

- [ ] **Step 1: Bring the Release tree up to date**

The Release tree was last built before the `80ea8dc99` submodule update. Its `shermes` predates Wasm input and parses a `.wasm` file as JavaScript.

```bash
cmake --build cmake-build-release --target shermes hermes-node hermes-node-kit
python3 -c "open('$S/empty.wasm','wb').write(b'\0asm\1\0\0\0')"
cmake-build-release/bin/shermes -emit-c -exported-unit=probe -o $S/empty.c $S/empty.wasm
grep -c 'SHWasmUnitReg s_wasm_reg' $S/empty.c
```
Expected: `1`.

- [ ] **Step 2: Extract hermes-parser's module bytes**

Write `$S/dump-wasm.js`:

```js
'use strict';
// Preloaded with node -r: writes the first WebAssembly module over 100 KB
// that the program compiles to $WASM_DUMP, whichever entry point it uses.
const fs = require('fs');
const out = process.env.WASM_DUMP;
let written = false;
function capture(b) {
  if (written || !b) return;
  const u8 = ArrayBuffer.isView(b)
    ? new Uint8Array(b.buffer, b.byteOffset, b.byteLength)
    : b instanceof ArrayBuffer ? new Uint8Array(b) : null;
  if (u8 && u8.byteLength > 100000) {
    fs.writeFileSync(out, u8);
    written = true;
  }
}
const M = WebAssembly.Module;
WebAssembly.Module = function (b) { capture(b); return new M(b); };
WebAssembly.Module.prototype = M.prototype;
const inst = WebAssembly.instantiate;
WebAssembly.instantiate = function (b, i) {
  if (!(b instanceof M)) capture(b);
  return inst.call(WebAssembly, b, i);
};
const comp = WebAssembly.compile;
WebAssembly.compile = function (b) { capture(b); return comp.call(WebAssembly, b); };
process.on('exit', () => {
  if (!written) { console.error('dump-wasm: nothing over 100 KB'); process.exitCode = 1; }
});
```

```bash
cd examples/hermes-parser-ast-wasm
WASM_DUMP=$S/hp.wasm node -r $S/dump-wasm.js ast.js sample.js > /dev/null
cd ../..
wc -c < $S/hp.wasm
```
Expected: `768303`. Record the actual number either way.

- [ ] **Step 3: Generate C and time it**

```bash
/usr/bin/time -v cmake-build-release/bin/shermes -emit-c -O \
  -exported-unit=whp -o $S/hp.c $S/hp.wasm 2> $S/shermes.time
wc -c < $S/hp.c
grep -E 'Elapsed|Maximum resident' $S/shermes.time
```

- [ ] **Step 4: Compile it the way `build-native` will, at -O0 and -O3**

This is `buildCompileCommand()` (`lib/build-native/native_compile.cpp`), assembled from the Release kit's manifest:

```bash
K=$(pwd)/cmake-build-release/kit
CC=$(sed -n 's/^cc: //p' $K/kit.manifest)
DF=$(sed -n 's/^driverflag: //p' $K/kit.manifest | sed "s#{kit}#$K#g")
CF=$(sed -n 's/^ccflag: //p' $K/kit.manifest | sed "s#{kit}#$K#g")
for O in -O0 -O3; do
  /usr/bin/time -v $CC -Qunused-arguments -x c -std=gnu11 $O -c $DF $CF \
    $S/hp.c -o $S/hp$O.o 2> $S/cc$O.time
  echo "$O exit $?"; grep -E 'Elapsed|Maximum resident' $S/cc$O.time
  wc -c < $S/hp$O.o
done
```
If the compile fails for a missing `hermes/VM/static_h.h`, the manifest's `ccflag` lines do not carry the include path in this kit. Add `-I$K/include`, note that in the progress file, and re-run.

- [ ] **Step 5: Write the progress file**

Create `docs/superpowers/plans/progress-native-wasm.md`:

```markdown
# Implementation Progress

Tracks `docs/superpowers/plans/2026-10-08-native-wasm.md` (implementation
plan) and its companion design doc
`docs/superpowers/specs/2026-10-08-native-wasm-design.md`.

## Measurements

### cc on hermes-parser's module (Task 1; Release, <machine>, <date>)

| what | value |
| --- | --- |
| module (.wasm) | <bytes> |
| generated C | <bytes> |
| shermes -emit-c -O | <wall>, <max RSS> |
| cc -O0 | <wall>, <max RSS>, object <bytes> |
| cc -O3 | <wall>, <max RSS>, object <bytes> |

<one paragraph: what the numbers mean for the design>

## Status

- [x] Task 1: measured (see above)
- [ ] Task 2: H2, codegen configuration on Wasm unit registrations
- [ ] Task 3: H1, lookup_native slot
- [ ] Task 4: H3, native tier in createModuleFromBytes
- [ ] Task 5: H4, accepted notification
- [ ] Task 6: Hermes NAPI harness
- [ ] Task 7: record format v2
- [ ] Task 8: commit-boundary recording
- [ ] Task 9: run-time native tier
- [ ] Task 10: build-native toolchain pieces
- [ ] Task 11: build-native compiles recorded modules natively
- [ ] Task 12: end-to-end coverage
- [ ] Task 13: example, measurements, docs
```
Fill in every placeholder with the measured values.

- [ ] **Step 6: Gate**

- **STOP:** if `cc -O3` peaked above 8 GiB RSS or took longer than 15 minutes. Report the table to the user and do not start Task 2. The spec says an impractical module is a finding to bring back, not something to work around.
- **Continue but flag:** above 4 GiB or 5 minutes. Say so in the progress file's paragraph.
- **Otherwise:** continue.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/plans/progress-native-wasm.md
git commit -m "Native Wasm: measure cc on hermes-parser's module

<one sentence with the -O3 wall time and peak RSS>

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 2: H2 -- a Wasm unit's registration carries its codegen configuration (Hermes)

**Files:**
- Create: `hermes/include/hermes/WasmFrontend/WasmCodegenConfig.h`
- Modify: `hermes/include/hermes/VM/static_h.h:69-93` (`SHWasmUnitReg`, new declaration)
- Modify: `hermes/lib/VM/StaticHWasm.cpp:74-98` (registration check, lookup by creator)
- Modify: `hermes/lib/BCGen/SH/SH.cpp:3267-3290` (emission)
- Modify: `hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp:592-612` (`wasmCodegenConfig`) and `:1367-1384` (`fromNativeUnit`)
- Modify: `hermes/lib/CMakeLists.txt:349-354` (`_u2` -> `_u3`)
- Modify: `hermes/test/wasm/native-register-emit.wat`, `hermes/test/wasm/native-unit-throws.wat`, `hermes/test/wasm/native-unit-throws-shim.c`
- Create: `hermes/test/wasm/native-config-mismatch.wat`, `hermes/test/wasm/native-config-mismatch-driver.js_`
- Create: `hermes/unittests/VMRuntime/WasmUnitRegistryTest.cpp`; Modify: `hermes/unittests/VMRuntime/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `namespace hermes { inline std::string wasmCodegenConfigString(bool test262); }`
  - `typedef struct SHWasmUnitReg { const char *name; SHUnitCreator creator; const char *codegen_config; struct SHWasmUnitReg *next; } SHWasmUnitReg;`
  - `SHERMES_EXPORT const SHWasmUnitReg *_sh_wasm_find_unit_reg(SHUnitCreator creator);`
  - `fromNativeUnit` raises `TypeError` on a configuration mismatch.

- [ ] **Step 1: Create the submodule branch**

```bash
git -C hermes rev-parse HEAD        # expect 80ea8dc99...
git -C hermes switch -c native-wasm-lookup
LIT_FILTER='wasm/' cmake --build cmake-build-asan --target check-hermes 2>&1 | tail -8
```
Record the pass/fail counts of that last command in your task notes. They are the baseline Step 13 compares against.

- [ ] **Step 2: Write the failing registry test**

Create `hermes/unittests/VMRuntime/WasmUnitRegistryTest.cpp`:

```cpp
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/static_h.h"

#include "gtest/gtest.h"

#include <cstdlib>

#ifdef HERMES_ENABLE_WASM

namespace {

// None of these is ever called: the registry stores and compares creators,
// it never runs them. Distinct functions per test because the registry is a
// process-global list with no unregistration, so a creator one test
// registered stays registered for every test after it.
SHUnit *findCreator(void) {
  abort();
}
SHUnit *neverRegistered(void) {
  abort();
}
SHUnit *dupNameCreator(void) {
  abort();
}
SHUnit *sharedCreator(void) {
  abort();
}
SHUnit *nullConfigCreator(void) {
  abort();
}
SHUnit *conflictCreator(void) {
  abort();
}

TEST(WasmUnitRegistryTest, FindsTheRegistrationOfACreator) {
  static SHWasmUnitReg reg = {"regtest_find", findCreator, "cfg", nullptr};
  _sh_wasm_register_unit(&reg);
  EXPECT_EQ(&reg, _sh_wasm_find_unit_reg(findCreator));
  EXPECT_EQ(nullptr, _sh_wasm_find_unit_reg(neverRegistered));
}

TEST(WasmUnitRegistryDeathTest, DuplicateNameAlwaysAborts) {
  // The same creator and the same configuration: identical in every field
  // but the node itself, and still refused, because two units under one
  // name cannot both be found by fromNativeUnit().
  static SHWasmUnitReg first = {"regtest_dup", dupNameCreator, "cfg", nullptr};
  static SHWasmUnitReg second = {"regtest_dup", dupNameCreator, "cfg", nullptr};
  _sh_wasm_register_unit(&first);
  EXPECT_DEATH(
      _sh_wasm_register_unit(&second), "duplicate Wasm unit registration");
}

TEST(WasmUnitRegistryTest, OneCreatorUnderTwoNamesWithEqualConfigs) {
  static SHWasmUnitReg a = {"regtest_shared_a", sharedCreator, "cfg", nullptr};
  static SHWasmUnitReg b = {"regtest_shared_b", sharedCreator, "cfg", nullptr};
  _sh_wasm_register_unit(&a);
  _sh_wasm_register_unit(&b);
  const SHWasmUnitReg *found = _sh_wasm_find_unit_reg(sharedCreator);
  ASSERT_NE(nullptr, found);
  EXPECT_STREQ("cfg", found->codegen_config);
}

TEST(WasmUnitRegistryTest, NullConfigEqualsOnlyNull) {
  static SHWasmUnitReg a = {"regtest_null_a", nullConfigCreator, nullptr, nullptr};
  static SHWasmUnitReg b = {"regtest_null_b", nullConfigCreator, nullptr, nullptr};
  _sh_wasm_register_unit(&a);
  _sh_wasm_register_unit(&b);
  EXPECT_NE(nullptr, _sh_wasm_find_unit_reg(nullConfigCreator));
}

TEST(WasmUnitRegistryDeathTest, OneCreatorWithTwoConfigsAborts) {
  static SHWasmUnitReg first = {"regtest_conflict_a", conflictCreator, "cfg-1", nullptr};
  static SHWasmUnitReg differs = {"regtest_conflict_b", conflictCreator, "cfg-2", nullptr};
  static SHWasmUnitReg none = {"regtest_conflict_c", conflictCreator, nullptr, nullptr};
  _sh_wasm_register_unit(&first);
  EXPECT_DEATH(
      _sh_wasm_register_unit(&differs), "conflicting Wasm unit registrations");
  EXPECT_DEATH(
      _sh_wasm_register_unit(&none), "conflicting Wasm unit registrations");
}

} // namespace

#endif // HERMES_ENABLE_WASM
```

Add `WasmUnitRegistryTest.cpp` to the source list in `hermes/unittests/VMRuntime/CMakeLists.txt`, in alphabetical order.

- [ ] **Step 3: Confirm it fails**

```bash
cmake --build cmake-build-asan --target VMRuntimeTests 2>&1 | grep -m3 error
```
Expected: compile errors, either about too many initializers for `SHWasmUnitReg` or about `_sh_wasm_find_unit_reg` being undeclared.

- [ ] **Step 4: Add the shared configuration function**

Create `hermes/include/hermes/WasmFrontend/WasmCodegenConfig.h`:

```cpp
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_WASMFRONTEND_WASMCODEGENCONFIG_H
#define HERMES_WASMFRONTEND_WASMCODEGENCONFIG_H

#include "hermes/BCGen/HBC/BytecodeVersion.h"
#include "hermes/WasmFrontend/WasmCodegenVersion.h"

#include <string>

namespace hermes {

/// Everything about a compiler that changes the code the Wasm frontend
/// generates, as an opaque, self-describing string.
///
/// One function serves both sides of every comparison made with it. A
/// runtime describes itself with it -- the Wasm cache keys on the result, and
/// a natively compiled unit must match it before it runs -- and the SH
/// backend stamps it into every Wasm unit's registration from the emitting
/// compiler's own constants. Two copies of this format would be two things
/// that could disagree about a unit, which nothing could then load.
///
/// Legible on purpose: it ends up in cache keys and error messages, and "why
/// did this miss?" is a question someone will ask of a hexdump. Contains
/// only [A-Za-z0-9;=-], so it needs no escaping inside a C string literal.
///
/// Anything added here that affects codegen MUST also be set by every
/// caller, or a cache will serve code built under different rules.
inline std::string wasmCodegenConfigString(bool test262) {
  std::string out("hermes-wasm;bc=");
  out += std::to_string(hbc::BYTECODE_VERSION);
  out += ";cg=";
  out += std::to_string(WASM_CODEGEN_VERSION);
  out += ";t262=";
  out += test262 ? '1' : '0';
  return out;
}

} // namespace hermes

#endif // HERMES_WASMFRONTEND_WASMCODEGENCONFIG_H
```

In `WebAssembly.cpp`, add `#include "hermes/WasmFrontend/WasmCodegenConfig.h"` next to the `WasmCodegenVersion.h` include. Reduce the doc comment on `wasmCodegenConfig(Runtime &)` to one line pointing at `wasmCodegenConfigString`, and make its body:

```cpp
static std::string wasmCodegenConfig(Runtime &runtime) {
  return wasmCodegenConfigString(runtime.test262);
}
```

- [ ] **Step 5: Extend the registration and the registry**

In `static_h.h`, replace the `SHWasmUnitReg` struct and append the declaration after `_sh_wasm_find_unit`:

```c
typedef struct SHWasmUnitReg {
  /// The --exported-unit name. Not SHUnit::unit_name, which is a fixed
  /// string.
  const char *name;
  /// The unit creator, i.e. sh_export_<name>.
  SHUnitCreator creator;
  /// The codegen configuration the unit was compiled under -- the string
  /// hermes::wasmCodegenConfigString() produces, e.g.
  /// "hermes-wasm;bc=100;cg=1;t262=0" -- or NULL for a registration that
  /// states none. A runtime runs the unit only when this equals its own
  /// configuration; NULL never does.
  const char *codegen_config;
  /// Next entry. Set by _sh_wasm_register_unit; initialize to NULL.
  struct SHWasmUnitReg *next;
} SHWasmUnitReg;
```

```c
/// \return the registration whose creator is \p creator, or NULL. The node is
/// the registrant's own static storage and is never unregistered, so it
/// stays valid after this returns. Not a static-initialization-time
/// operation, for the reason _sh_wasm_find_unit gives.
SHERMES_EXPORT const SHWasmUnitReg *_sh_wasm_find_unit_reg(
    SHUnitCreator creator);
```

Update `_sh_wasm_register_unit`'s doc comment: it also aborts when another registration has the same creator and a different configuration (NULL equals only NULL). A creator then has exactly one configuration, so "the registration for this creator" is unambiguous. Two generated objects can never collide this way, because each defines its own `sh_export_<name>`.

In `StaticHWasm.cpp`, inside the anonymous namespace:

```cpp
/// NULL equals only NULL; otherwise by contents.
bool sameCodegenConfig(const char *a, const char *b) {
  if (!a || !b)
    return a == b;
  return std::strcmp(a, b) == 0;
}
```

Then change the registration loop and add the lookup:

```cpp
extern "C" void _sh_wasm_register_unit(SHWasmUnitReg *reg) {
  std::lock_guard<std::mutex> lock(wasmUnitsMutex());
  for (SHWasmUnitReg *p = s_wasmUnits; p; p = p->next) {
    if (std::strcmp(p->name, reg->name) == 0) {
      fprintf(
          stderr,
          "SH: duplicate Wasm unit registration for \"%s\"\n",
          reg->name);
      abort();
    }
    // One creator, one configuration: the native lookup finds a unit's
    // registration by its creator, and two answers would make the check it
    // performs depend on list order.
    if (p->creator == reg->creator &&
        !sameCodegenConfig(p->codegen_config, reg->codegen_config)) {
      fprintf(
          stderr,
          "SH: conflicting Wasm unit registrations \"%s\" and \"%s\": one "
          "creator, two codegen configurations\n",
          p->name,
          reg->name);
      abort();
    }
  }
  reg->next = s_wasmUnits;
  s_wasmUnits = reg;
}

extern "C" const SHWasmUnitReg *_sh_wasm_find_unit_reg(SHUnitCreator creator) {
  // Same lock discipline as _sh_wasm_find_unit: released before the caller
  // runs anything.
  std::lock_guard<std::mutex> lock(wasmUnitsMutex());
  for (SHWasmUnitReg *p = s_wasmUnits; p; p = p->next) {
    if (p->creator == creator)
      return p;
  }
  return nullptr;
}
```

- [ ] **Step 6: Run the registry test**

```bash
cmake --build cmake-build-asan --target VMRuntimeTests && \
  cmake-build-asan/hermes/unittests/VMRuntime/VMRuntimeTests \
  --gtest_filter='WasmUnitRegistry*'
```
Expected: 5 tests PASS.

- [ ] **Step 7: Stamp the configuration in emitted C**

In `SH.cpp`, inside the `#ifdef HERMES_ENABLE_WASM` block's `if (options.wasmUnit)`, replace the `s_wasm_reg` line with:

```cpp
      // The configuration this compiler generated the unit under, from its
      // own constants and this module's settings, so a runtime built
      // differently -- another bytecode or Wasm codegen version, or a
      // different -test262 -- refuses the unit rather than running it.
      std::string codegenConfig = wasmCodegenConfigString(
          M->getContext().getCodeGenerationSettings().test262);
      assert(
          codegenConfig.find_first_of("\"\\") == std::string::npos &&
          "the configuration is emitted inside a C string literal");
      OS << "\nstatic SHWasmUnitReg s_wasm_reg = {\"" << options.unitName
         << "\", sh_export_" << options.unitName << ", \"" << codegenConfig
         << "\", NULL};\n"
```
Keep the `<<` chain that follows (the MSVC `#error` and the constructor) unchanged. Add `#include "hermes/WasmFrontend/WasmCodegenConfig.h"` under `#ifdef HERMES_ENABLE_WASM` at the top of `SH.cpp`.

- [ ] **Step 8: Bump the unit ABI**

In `hermes/lib/CMakeLists.txt`, change `_u2` to `_u3`. Extend the comment's first sentence: generated C also initializes an `SHWasmUnitReg` by value, so its layout belongs to the same ABI. Bump this whenever either `SHUnit`'s or `SHWasmUnitReg`'s layout changes.

- [ ] **Step 9: Check `fromNativeUnit` against the registration**

In `wasmModuleFromNativeUnit` (`WebAssembly.cpp`), right after the `if (!creator) { ... }` block:

```cpp
  // A unit runs only under the configuration it was generated for: a
  // mismatched one never executes, by either entry point. There are no bytes
  // here to fall back to, so a mismatch is an error naming both sides.
  const SHWasmUnitReg *reg = _sh_wasm_find_unit_reg(creator);
  const std::string runtimeConfig = wasmCodegenConfig(runtime);
  const char *unitConfig = reg ? reg->codegen_config : nullptr;
  if (!unitConfig || runtimeConfig != unitConfig) {
    std::string msg = "WebAssembly.Module.fromNativeUnit(): unit '";
    msg += name;
    msg += "' was compiled for '";
    msg += unitConfig ? unitConfig : "(no configuration)";
    msg += "', but this runtime is '";
    msg += runtimeConfig;
    msg += '\'';
    return runtime.raiseTypeError(llvh::StringRef(msg));
  }
```

- [ ] **Step 10: Update the emission test**

In `hermes/test/wasm/native-register-emit.wat`, add a second RUN line and extend the checks:

```
;; RUN: %wat2wasm %s -o %t.wasm
;; RUN: %shermes -exported-unit=mymod -emit-c -o - %t.wasm | %FileCheck %s
;; RUN: %shermes -test262 -exported-unit=mymod -emit-c -o - %t.wasm \
;; RUN:     | %FileCheck --check-prefix=T262 %s

;; CHECK: static SHWasmUnitReg s_wasm_reg
;; CHECK-SAME: "mymod"
;; CHECK-SAME: sh_export_mymod
;; CHECK-SAME: "hermes-wasm;bc={{[0-9]+}};cg={{[0-9]+}};t262=0", NULL};
;; CHECK: _sh_wasm_register_unit(&s_wasm_reg)

;; T262: static SHWasmUnitReg s_wasm_reg
;; T262-SAME: "hermes-wasm;bc={{[0-9]+}};cg={{[0-9]+}};t262=1", NULL};
```

- [ ] **Step 11: Give the throwing shim a matching configuration**

`native-unit-throws.wat` must still reach the throw, now past the configuration check. Replace its RUN lines with:

```
;; RUN: %wat2wasm %s -o %t.wasm
;; RUN: %shermes -exported-unit=goodmod -emit-c -o %t-good.c %t.wasm
;; RUN: rm -rf %t.d && mkdir -p %t.d
;; RUN: sed -n 's/.*sh_export_goodmod, \("[^"]*"\).*/#define GOODMOD_CODEGEN_CONFIG \1/p' \
;; RUN:     %t-good.c > %t.d/goodmod-config.h
;; RUN: %shermes -exported-unit=goodmod -c -o %t-good.o %t.wasm
;; RUN: %shermes -exported-unit=throwmod -c -o %t-throw.o \
;; RUN:     %S/native-unit-throws-throwing.js_
;; RUN: %shermes -o %t.exe \
;; RUN:     -Wc,-I%t.d,%t-good.o,%t-throw.o,%S/native-unit-throws-shim.c \
;; RUN:     %S/native-unit-throws-driver.js_
;; RUN: %t.exe | %FileCheck --match-full-lines %s
```

In `native-unit-throws-shim.c`, replace the registration with:

```c
/* The configuration goodmod -- a real Wasm unit, compiled by this very
 * shermes -- carries, extracted by the RUN lines from its generated C. The
 * runtime runs a unit only when its configuration matches its own, so a
 * registration stating none, or a hard-coded string that drifted with the
 * next version bump, would be refused before the throwing top level ran:
 * the test would then report a TypeError instead of "boom". */
#include "goodmod-config.h"
#ifndef GOODMOD_CODEGEN_CONFIG
#error "goodmod-config.h did not define GOODMOD_CODEGEN_CONFIG"
#endif

extern SHUnit *sh_export_throwmod(void);

static SHWasmUnitReg reg = {
    "throwmod", sh_export_throwmod, GOODMOD_CODEGEN_CONFIG, 0};
```
Also extend the file's header comment by one sentence: the shim states the same configuration a real Wasm unit would.

- [ ] **Step 12: Add the mismatch test**

Create `hermes/test/wasm/native-config-mismatch.wat`:

```
;; Copyright (c) Meta Platforms, Inc. and affiliates.
;;
;; This source code is licensed under the MIT license found in the
;; LICENSE file in the root directory of this source tree.

;; A unit compiled with -test262 carries t262=1 in its registration, and a
;; runtime without -test262 refuses to run it: fromNativeUnit() raises a
;; TypeError naming both configurations instead of executing code generated
;; under rules this runtime does not follow.

;; REQUIRES: shermes, wasm
;; RUN: %wat2wasm %s -o %t.wasm
;; RUN: %shermes -test262 -exported-unit=t262mod -c -o %t-mod.o %t.wasm
;; RUN: %shermes -o %t.exe -Wc,%t-mod.o %S/native-config-mismatch-driver.js_
;; RUN: %t.exe | %FileCheck --match-full-lines %s

(module
  (func (export "nop")))

;; CHECK: TypeError: true
;; CHECK-NEXT: names the unit: true
;; CHECK-NEXT: names both configurations: true
```

Create `hermes/test/wasm/native-config-mismatch-driver.js_` (Meta header, as the other drivers):

```js
// Driver for native-config-mismatch.wat.
try {
  WebAssembly.Module.fromNativeUnit('t262mod');
  print('no throw');
} catch (e) {
  print('TypeError: ' + (e instanceof TypeError));
  print('names the unit: ' + (e.message.indexOf("'t262mod'") >= 0));
  print(
      'names both configurations: ' +
      (e.message.indexOf('t262=1') >= 0 && e.message.indexOf('t262=0') >= 0));
}
```

- [ ] **Step 13: Run the Hermes suites**

```bash
cmake --build cmake-build-asan --target VMRuntimeTests && \
  cmake-build-asan/hermes/unittests/VMRuntime/VMRuntimeTests \
  --gtest_filter='WasmUnitRegistry*:SHUnitABITest*'
LIT_FILTER='wasm/native-' cmake --build cmake-build-asan --target check-hermes
LIT_FILTER='shermes/no-wasm-register\.js$' cmake --build cmake-build-asan --target check-hermes
LIT_FILTER='wasm/' cmake --build cmake-build-asan --target check-hermes
```
Expected: all pass. `native-unit-throws.wat` still prints `message: boom`, and `native-config-mismatch.wat` passes. The full `wasm/` run matches Step 1's baseline plus the one new test; it must not add a failure.

- [ ] **Step 14: Commit (in the submodule)**

```bash
clang-format-18 -i hermes/include/hermes/WasmFrontend/WasmCodegenConfig.h \
  hermes/lib/VM/StaticHWasm.cpp hermes/lib/BCGen/SH/SH.cpp \
  hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp \
  hermes/unittests/VMRuntime/WasmUnitRegistryTest.cpp
git -C hermes add include/hermes/WasmFrontend/WasmCodegenConfig.h \
  include/hermes/VM/static_h.h lib/VM/StaticHWasm.cpp lib/BCGen/SH/SH.cpp \
  lib/VM/JSLib/WebAssembly/WebAssembly.cpp lib/CMakeLists.txt \
  unittests/VMRuntime/WasmUnitRegistryTest.cpp unittests/VMRuntime/CMakeLists.txt \
  test/wasm/native-register-emit.wat test/wasm/native-unit-throws.wat \
  test/wasm/native-unit-throws-shim.c test/wasm/native-config-mismatch.wat \
  test/wasm/native-config-mismatch-driver.js_
git -C hermes commit -m "Wasm: a native unit's registration states its codegen configuration

A natively compiled Wasm unit was generated under one bytecode version,
one Wasm codegen version and one -test262 setting, and nothing stopped a
runtime built under different ones from running it. The registration
shermes emits now carries the same configuration string the Wasm cache
keys on, built from the emitting compiler's own constants, and
fromNativeUnit() raises a TypeError naming both sides on a mismatch.

The string lives in the static registration, not in SHUnit: calling a
creator allocates a fresh UnitData, and _sh_unit_init calls the creator
again, so reading it from a created unit would leak the first allocation
on every refusal.

_sh_wasm_find_unit_reg() finds a registration by creator, which an
embedder holding only a creator needs. The registry now also refuses one
creator registered with two configurations, so that lookup has one answer.
SHWasmUnitReg's layout changed, so the model's unit-ABI component becomes
_u3.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 3: H1 -- `lookup_native` slot, with old callers still accepted (Hermes)

**Files:**
- Modify: `hermes/API/napi/hermes_napi.h:276-343`
- Modify: `hermes/API/napi/hermes_napi_wasm_cache.cpp`
- Modify: `hermes/include/hermes/VM/WasmCacheHooks.h`
- Test: `hermes/unittests/napi/NapiWasmCacheTest.cpp`

**Interfaces:**
- Consumes: nothing from Task 2.
- Produces:
  - `hermes_wasm_cache_callbacks::lookup_native`: `SHUnitCreator (*)(void *ctx, const uint8_t *wasm, size_t wasm_size)`, appended after `discard`.
  - The minimum accepted `struct_size` becomes `offsetof(hermes_wasm_cache_callbacks, lookup_native)`.
  - `hermes::vm::WasmCacheHooks::lookupNative` of type `NativeUnitCreator (*)(void *, const uint8_t *, size_t)`, where `using NativeUnitCreator = ::SHUnit *(*)();`.

- [ ] **Step 1: Write the failing old-caller test**

Append inside the `#ifdef HERMES_ENABLE_WASM` block of `NapiWasmCacheTest.cpp`, before `#else`:

```cpp
/// The callbacks struct exactly as a caller compiled before lookup_native
/// existed owns it: allocated to the old size and not one byte more, so a
/// read of any field appended since is a heap overflow AddressSanitizer
/// fails the test for. Installs, and serves a miss/store round trip and an
/// accepted-hit round trip, with the new slots never read.
TEST_F(NapiTestFixture, WasmCache_OldSizedStructInstallsAndRoundTrips) {
  FakeCache cache;
  hermes_wasm_cache_callbacks full = cache.callbacks();
  const size_t oldSize = offsetof(hermes_wasm_cache_callbacks, lookup_native);
  full.struct_size = oldSize;
  void *oldAlloc = ::malloc(oldSize);
  ASSERT_NE(nullptr, oldAlloc);
  std::memcpy(oldAlloc, &full, oldSize);
  auto *old = static_cast<const hermes_wasm_cache_callbacks *>(oldAlloc);

  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, old));
  EXPECT_TRUE(compileAdd(env_)); // miss, compile, store
  cache.serve = true;
  EXPECT_TRUE(compileAdd(env_)); // accepted hit, discard
  EXPECT_EQ(2, cache.lookups);
  EXPECT_EQ(1, cache.stores);
  EXPECT_EQ(1, cache.discards);
  EXPECT_EQ(0, cache.outstandingTokens);

  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, nullptr));
  ::free(oldAlloc);
}
```

- [ ] **Step 2: Confirm it fails**

```bash
cmake --build cmake-build-asan --target NapiTests 2>&1 | grep -m2 error
```
Expected: a compile error, because `lookup_native` is not a member yet. Once Step 3 adds the field, before Step 4 changes the setter, the test fails with `napi_invalid_arg`, because today's setter demands the full `sizeof`.

- [ ] **Step 3: Add the field, and move the creator typedefs above the struct**

In `hermes_napi.h`, move these lines and their doc comment from below `hermes_set_wasm_cache` to just above `struct hermes_wasm_cache_callbacks`:

```c
typedef struct SHUnit SHUnit;
typedef SHUnit *(*SHUnitCreator)(void);
```

Append to the struct, after `discard`:

```c
  /// Return the creator of a natively compiled unit for these bytes, or
  /// NULL. Consulted before `lookup`. Returning a creator does not commit
  /// Hermes to using it: the unit must be registered and built under this
  /// runtime's codegen configuration, and a creator that is not is followed
  /// by the ordinary `lookup`, exactly as if this had returned NULL.
  /// Optional: NULL here means "no native tier". No store token is involved:
  /// nothing is stored on this path, so "exactly one of `store` or
  /// `discard`" is untouched.
  SHUnitCreator (*lookup_native)(
      void *ctx, const uint8_t *wasm, size_t wasm_size);
```

Rewrite `hermes_set_wasm_cache`'s doc comment:

```c
/// Install \p callbacks on \p env's runtime. Passing NULL removes any
/// installed cache. Returns napi_invalid_arg if `struct_size` is smaller
/// than offsetof(hermes_wasm_cache_callbacks, lookup_native) -- the struct
/// as it was before its optional fields were appended -- or a required
/// callback (`lookup`, `store`, `discard`) is NULL. An optional field that
/// `struct_size` does not cover is treated as NULL and never read, so a
/// caller compiled against an older header keeps working; a larger
/// `struct_size` (a newer caller) is accepted too, and only the fields this
/// header knows about are read.
```

In `WasmCacheHooks.h`, forward-declare `struct SHUnit;` at global scope before `namespace hermes`, and add inside `struct WasmCacheHooks` after `discard`:

```cpp
  /// A natively compiled unit's creator, as hermes_napi.h spells
  /// SHUnitCreator.
  using NativeUnitCreator = ::SHUnit *(*)();

  /// Optional native tier, consulted before \c lookup: the creator of a
  /// natively compiled unit for \p wasm, or null. A creator whose unit is not
  /// registered under this runtime's codegen configuration is treated as
  /// null. Never involves a store token.
  NativeUnitCreator (*lookupNative)(
      void *ctx, const uint8_t *wasm, size_t wasmSize) = nullptr;
```

- [ ] **Step 4: Gate the setter on the old size**

In `hermes_napi_wasm_cache.cpp`, add `#include <cstddef>` and replace the size check and the copy:

```cpp
  // The struct as it was before its optional fields were appended. Demanding
  // the CURRENT sizeof would reject every caller compiled against an older
  // header -- appending a field would then be a breaking change.
  constexpr size_t kMinStructSize =
      offsetof(hermes_wasm_cache_callbacks, lookup_native);
  if (callbacks->struct_size < kMinStructSize)
    return napi_set_last_error(env, napi_invalid_arg);
  CHECK_ARG(env, callbacks->lookup);
  CHECK_ARG(env, callbacks->store);
  CHECK_ARG(env, callbacks->discard);

  hermes::vm::WasmCacheHooks hooks{};
  hooks.ctx = callbacks->ctx;
  hooks.lookup = callbacks->lookup;
  hooks.store = callbacks->store;
  hooks.discard = callbacks->discard;
  // Read only when the caller's struct reaches the whole field: a shorter
  // struct was compiled before the field existed, and the bytes past its
  // end are not the caller's.
  if (callbacks->struct_size >=
      offsetof(hermes_wasm_cache_callbacks, lookup_native) +
          sizeof(callbacks->lookup_native))
    hooks.lookupNative = callbacks->lookup_native;
  runtime.setWasmCacheHooks(hooks);
```

- [ ] **Step 5: Run the NAPI Wasm tests**

```bash
cmake --build cmake-build-asan --target NapiTests && \
  cmake-build-asan/hermes/unittests/napi/NapiTests --gtest_filter='*WasmCache*'
```
Expected: all pass, including the existing `TooSmallStructIsRejected` (its size, `offsetof(lookup)`, is still below the minimum) and `TrulyTruncatedStructIsNotReadPastItsEnd`.

- [ ] **Step 6: Commit (in the submodule)**

```bash
clang-format-18 -i hermes/API/napi/hermes_napi_wasm_cache.cpp \
  hermes/include/hermes/VM/WasmCacheHooks.h hermes/unittests/napi/NapiWasmCacheTest.cpp
git -C hermes add API/napi/hermes_napi.h API/napi/hermes_napi_wasm_cache.cpp \
  include/hermes/VM/WasmCacheHooks.h unittests/napi/NapiWasmCacheTest.cpp
git -C hermes commit -m "Wasm cache: an optional lookup_native slot

An embedder that links natively compiled Wasm units needs to hand Hermes
one for a given module's bytes, so that new WebAssembly.Module(bytes)
can take it without the program naming the unit. lookup_native is
appended to hermes_wasm_cache_callbacks for that; this change only adds
and copies it, and the next one consults it.

The setter used to demand struct_size >= sizeof(the current struct),
which turns any appended field into a break for every caller compiled
against the older header. The minimum is now the struct as it stood
before its optional fields, and an optional field is read only when
struct_size covers it whole. A test owns exactly an old caller's
allocation, so a read past it fails under AddressSanitizer.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 4: H3 -- `createModuleFromBytes` consults the native tier first (Hermes)

**Files:**
- Modify: `hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp:770-778` (the `.wasm` branch)
- Test: `hermes/unittests/napi/NapiWasmCacheTest.cpp`

**Interfaces:**
- Consumes: `WasmCacheHooks::lookupNative` (Task 3); `_sh_wasm_find_unit_reg` (Task 2); `wasmCodegenConfig(runtime)` (Task 2).
- Produces: for `new WebAssembly.Module`, `WebAssembly.compile` and `WebAssembly.instantiate`, the order native, then `lookup`, then compile. A unit whose top level fails is the module's error, not a fallback.

- [ ] **Step 1: Write failing tests**

Add to `FakeCache` in `NapiWasmCacheTest.cpp`:

```cpp
  int nativeCalls = 0;
  /// What lookup_native answers.
  SHUnitCreator nativeAnswer = nullptr;
  /// "native" and "lookup" in call order, to pin which tier is asked first.
  std::vector<std::string> order;

  static SHUnitCreator lookupNative(void *ctx, const uint8_t *, size_t) {
    auto *self = static_cast<FakeCache *>(ctx);
    self->nativeCalls++;
    self->order.push_back("native");
    return self->nativeAnswer;
  }
```
In `FakeCache::lookup`, add `self->order.push_back("lookup");` as its second statement. In `callbacks()`, add `cbs.lookup_native = &FakeCache::lookupNative;`. Add `#include "hermes/VM/static_h.h"` at the top.

Then the tests, inside the `#ifdef HERMES_ENABLE_WASM` block:

```cpp
/// Never called: a creator the native tier offers but Hermes must refuse. If
/// Hermes ever ran it, the test process aborts, which is the failure.
static SHUnit *creatorThatMustNotRun(void) {
  ::abort();
}

TEST_F(NapiTestFixture, WasmCache_NativeTierIsAskedFirst) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(1, cache.nativeCalls);
  EXPECT_EQ((std::vector<std::string>{"native", "lookup"}), cache.order);
  EXPECT_EQ(1, cache.stores);
}

TEST_F(NapiTestFixture, WasmCache_UnregisteredCreatorFallsThrough) {
  FakeCache cache;
  cache.nativeAnswer = &creatorThatMustNotRun;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(1, cache.nativeCalls);
  EXPECT_EQ(1, cache.lookups) << "a refused creator is a miss";
  EXPECT_EQ(1, cache.stores);
}

static SHUnit *mismatchedCreator(void) {
  ::abort();
}
static SHUnit *configlessCreator(void) {
  ::abort();
}

TEST_F(NapiTestFixture, WasmCache_MismatchedRegistrationFallsThrough) {
  // Registered once per process: the registry has no unregistration and
  // aborts on a duplicate name.
  static SHWasmUnitReg reg = {
      "napitest_mismatch", mismatchedCreator, "hermes-wasm;bc=0;cg=0;t262=0",
      nullptr};
  static bool registered = (_sh_wasm_register_unit(&reg), true);
  (void)registered;

  FakeCache cache;
  cache.nativeAnswer = &mismatchedCreator;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(1, cache.lookups);
  EXPECT_EQ(1, cache.stores);
}

TEST_F(NapiTestFixture, WasmCache_ConfiglessRegistrationFallsThrough) {
  static SHWasmUnitReg reg = {
      "napitest_configless", configlessCreator, nullptr, nullptr};
  static bool registered = (_sh_wasm_register_unit(&reg), true);
  (void)registered;

  FakeCache cache;
  cache.nativeAnswer = &configlessCreator;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(1, cache.lookups);
}

TEST_F(NapiTestFixture, WasmCache_NullNativeAnswerIsTheBytecodePath) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  cbs.lookup_native = nullptr; // no native tier at all
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(0, cache.nativeCalls);
  EXPECT_EQ(1, cache.lookups);
}
```

- [ ] **Step 2: Confirm they fail**

```bash
cmake --build cmake-build-asan --target NapiTests && \
  cmake-build-asan/hermes/unittests/napi/NapiTests \
  --gtest_filter='*NativeTierIsAskedFirst*:*FallsThrough*'
```
Expected: `NativeTierIsAskedFirst` and both `FallsThrough` tests FAIL with `nativeCalls` 0 (the slot is copied but never consulted).

- [ ] **Step 3: Consult the native tier**

In `createModuleFromBytes`, in the `.wasm` branch, insert right after `const std::string codegenConfig = wasmCodegenConfig(runtime);`:

```cpp
    // Native tier first. A unit the embedder offers is used only if it is
    // registered under exactly this runtime's codegen configuration; anything
    // else -- no registration, a registration stating none, or a different
    // one -- is a miss, and this module continues down the bytecode tiers as
    // if no native tier existed. Nothing has been allocated at that point,
    // and nothing is remembered: a unit whose registering constructor has not
    // run yet may simply not be found today.
    if (hooks.installed() && hooks.lookupNative) {
      if (SHUnitCreatorFn nativeCreator =
              hooks.lookupNative(hooks.ctx, data, size)) {
        const SHWasmUnitReg *reg = _sh_wasm_find_unit_reg(nativeCreator);
        if (reg && reg->codegen_config &&
            codegenConfig == reg->codegen_config) {
          // The unit is the artifact. If its top level fails, that is this
          // module's error, raised exactly as fromNativeUnit() raises it --
          // not a reason to compile: a broken artifact is not a slow one.
          // Validation is skipped, as on a bytecode cache hit; the key is
          // the embedder's, over the bytes the unit was compiled from.
          return buildModuleDataFromArtifact(
              runtime, nullptr, nativeCreator, errorMsg);
        }
      }
    }
```

- [ ] **Step 4: Run the NAPI tests**

```bash
cmake --build cmake-build-asan --target NapiTests && \
  cmake-build-asan/hermes/unittests/napi/NapiTests --gtest_filter='*WasmCache*'
```
Expected: all pass.

- [ ] **Step 5: Commit (in the submodule)**

```bash
clang-format-18 -i hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp \
  hermes/unittests/napi/NapiWasmCacheTest.cpp
git -C hermes add lib/VM/JSLib/WebAssembly/WebAssembly.cpp unittests/napi/NapiWasmCacheTest.cpp
git -C hermes commit -m "Wasm: consult the embedder's native tier before its bytecode cache

createModuleFromBytes now asks lookup_native first. A creator it
returns is used only when its registration states this runtime's
codegen configuration; otherwise the module takes exactly the path it
took before, so every entry point -- new WebAssembly.Module, compile and
instantiate -- reaches a linked unit with no change to the program.

A matched unit whose top level fails raises that failure as the
module's error, as fromNativeUnit() does, rather than compiling the
bytes instead.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 5: H4 -- the `accepted` notification (Hermes)

**Files:**
- Modify: `hermes/API/napi/hermes_napi.h` (struct field, after `lookup_native`)
- Modify: `hermes/API/napi/hermes_napi_wasm_cache.cpp`
- Modify: `hermes/include/hermes/VM/WasmCacheHooks.h`
- Modify: `hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp:796-812` (the good-hit branch)
- Test: `hermes/unittests/napi/NapiWasmCacheTest.cpp`

**Interfaces:**
- Consumes: Task 3's size gating.
- Produces: `void (*accepted)(void *ctx, void *store_token)` on the callbacks struct; `WasmCacheHooks::accepted`. On an accepted hit the order is finalizer, then `accepted`, then `discard`. `accepted` is never called for a rejected hit or a miss.

- [ ] **Step 1: Write failing tests**

Extend `FakeCache`:

```cpp
  int acceptedCalls = 0;
  /// When set, a hit hands Hermes a heap copy of `stored` with a finalizer
  /// that frees it -- a buffer that really is gone once finalized.
  bool useFinalizer = false;
  /// "lookup", "finalize", "accepted", "store", "discard", in call order.
  std::vector<std::string> events;

  static void finalize(const uint8_t *data, size_t, void *hint) {
    static_cast<FakeCache *>(hint)->events.push_back("finalize");
    delete[] data;
  }

  static void accepted(void *ctx, void *) {
    auto *self = static_cast<FakeCache *>(ctx);
    self->acceptedCalls++;
    self->events.push_back("accepted");
  }
```
In `lookup`: push `"lookup"` to `events`. When serving with `useFinalizer`, copy `stored` into `new uint8_t[size]`, return that, and set `*finalizeCb = &FakeCache::finalize; *finalizeHint = self;`. In `store` and `discard`, push `"store"` / `"discard"`. In `callbacks()`, set `cbs.accepted = &FakeCache::accepted;`.

Add a helper beside `compileAdd`:

```cpp
/// new WebAssembly.Module over \p bytes; true if it compiled. Leaves no
/// exception pending.
static bool compileBytes(napi_env env, const uint8_t *bytes, size_t size) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env, &scope));
  std::string src = "(function(){var b=new Uint8Array([";
  for (size_t i = 0; i < size; ++i)
    src += std::to_string(bytes[i]) + ",";
  src += "]); new WebAssembly.Module(b); return true;})()";
  napi_value script = nullptr, result = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_string_utf8(env, src.c_str(), NAPI_AUTO_LENGTH, &script));
  bool ok = napi_run_script(env, script, &result) == napi_ok;
  if (!ok) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
  }
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env, scope));
  return ok;
}

/// The right magic, a version Hermes does not know, nothing after it.
static const uint8_t kGarbage[] = {0x00, 0x61, 0x73, 0x6d, 0x09, 0x00, 0x00, 0x00};
```

Tests:

```cpp
TEST_F(NapiTestFixture, WasmCache_AcceptedHitOrderIsFinalizeAcceptedDiscard) {
  FakeCache cache;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_)); // populates
  cache.serve = true;
  cache.useFinalizer = true;
  cache.events.clear();
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(
      (std::vector<std::string>{"lookup", "finalize", "accepted", "discard"}),
      cache.events);
}

TEST_F(NapiTestFixture, WasmCache_RejectedHitIsNeverAccepted) {
  FakeCache cache;
  cache.stored = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33};
  cache.serve = true;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_TRUE(compileAdd(env_)); // rejected hit, recompiled, stored
  EXPECT_EQ(0, cache.acceptedCalls);
  EXPECT_EQ(1, cache.stores);
}

TEST_F(NapiTestFixture, WasmCache_RejectedHitThenFailedCompileIsABareDiscard) {
  // Bytes that do not compile, and a cache entry for them that Hermes
  // refuses: no store, no accepted, one discard.
  FakeCache cache;
  cache.stored = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33};
  cache.serve = true;
  auto cbs = cache.callbacks();
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, &cbs));
  EXPECT_FALSE(compileBytes(env_, kGarbage, sizeof(kGarbage)));
  EXPECT_EQ(0, cache.acceptedCalls);
  EXPECT_EQ(0, cache.stores);
  EXPECT_EQ(1, cache.discards);
  EXPECT_EQ(0, cache.outstandingTokens);
}

/// A caller whose struct reaches lookup_native but stops before accepted.
TEST_F(NapiTestFixture, WasmCache_StructWithoutAcceptedIsNotReadPastItsEnd) {
  FakeCache cache;
  hermes_wasm_cache_callbacks full = cache.callbacks();
  const size_t size = offsetof(hermes_wasm_cache_callbacks, accepted);
  full.struct_size = size;
  void *alloc = ::malloc(size);
  ASSERT_NE(nullptr, alloc);
  std::memcpy(alloc, &full, size);
  ASSERT_EQ(
      napi_ok,
      hermes_set_wasm_cache(
          env_, static_cast<const hermes_wasm_cache_callbacks *>(alloc)));
  EXPECT_TRUE(compileAdd(env_));
  cache.serve = true;
  EXPECT_TRUE(compileAdd(env_));
  EXPECT_EQ(0, cache.acceptedCalls);
  EXPECT_EQ(1, cache.discards);
  ASSERT_EQ(napi_ok, hermes_set_wasm_cache(env_, nullptr));
  ::free(alloc);
}
```

- [ ] **Step 2: Confirm they fail**

```bash
cmake --build cmake-build-asan --target NapiTests 2>&1 | grep -m2 error
```
Expected: a compile error (`accepted` is not a member). After Step 3 adds the field, `AcceptedHitOrder...` fails until Step 4 is done.

- [ ] **Step 3: Add the field**

In `hermes_napi.h`, append after `lookup_native`:

```c
  /// Called when Hermes has accepted the bytecode a `lookup` returned,
  /// immediately before the `discard` for that token. Optional.
  ///
  /// The order on an accepted hit is fixed: the buffer's finalizer, then
  /// `accepted`, then `discard`. So the bytes `lookup` returned may already
  /// be released when this runs; an embedder that wants them here must copy
  /// them during `lookup`. Never called for a miss, or for a hit Hermes
  /// rejected -- that token goes on to `store` (the recompile succeeded) or
  /// `discard` (it failed). It reports that a bytecode provider was
  /// accepted, not that the bytecode corresponds to the bytes.
  void (*accepted)(void *ctx, void *store_token);
```

In `WasmCacheHooks.h`, add after `lookupNative`:

```cpp
  /// Optional: Hermes accepted the bytes \c lookup returned for
  /// \p storeToken. Called after the finalizer and before \c discard.
  void (*accepted)(void *ctx, void *storeToken) = nullptr;
```

In `hermes_napi_wasm_cache.cpp`, after the `lookupNative` copy:

```cpp
  if (callbacks->struct_size >=
      offsetof(hermes_wasm_cache_callbacks, accepted) +
          sizeof(callbacks->accepted))
    hooks.accepted = callbacks->accepted;
```

- [ ] **Step 4: Call it**

In `createModuleFromBytes`, in the `if (ret.first) {` branch of the cache hit, before `hooks.discard(...)`:

```cpp
          // The finalizer has already run, just above; the order is part of
          // the contract (hermes_napi.h).
          if (hooks.accepted)
            hooks.accepted(hooks.ctx, storeToken);
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-asan --target NapiTests && \
  cmake-build-asan/hermes/unittests/napi/NapiTests --gtest_filter='*WasmCache*'
```
Expected: all pass, including Task 3's `OldSizedStructInstallsAndRoundTrips`. That test's accepted hit must not read `accepted`; ASAN fails it if it does.

- [ ] **Step 6: Commit (in the submodule)**

```bash
clang-format-18 -i hermes/API/napi/hermes_napi_wasm_cache.cpp \
  hermes/include/hermes/VM/WasmCacheHooks.h \
  hermes/lib/VM/JSLib/WebAssembly/WebAssembly.cpp \
  hermes/unittests/napi/NapiWasmCacheTest.cpp
git -C hermes add API/napi/hermes_napi.h API/napi/hermes_napi_wasm_cache.cpp \
  include/hermes/VM/WasmCacheHooks.h lib/VM/JSLib/WebAssembly/WebAssembly.cpp \
  unittests/napi/NapiWasmCacheTest.cpp
git -C hermes commit -m "Wasm cache: tell the embedder when a hit was accepted

An accepted hit and a rejected hit whose recompile then failed both end
in discard, so an embedder cannot tell whether the bytes it served were
used. Both outcomes are reachable without running out of memory: IR
verification is outside the cache key and can reject a recompile, and a
cache entry proves nothing about the bytes it is filed under.

The optional accepted callback reports the first outcome. On an
accepted hit the order is fixed: the buffer's finalizer, then accepted,
then discard, so an embedder that wants the bytes at that point copies
them during lookup. The store-or-discard contract is unchanged.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 6: The native tier through a real unit (Hermes NAPI harness)

The tests so far reach H3 only with creators Hermes must refuse. This one links real units into an addon and drives `new WebAssembly.Module(bytes)` from the `hermes` binary, so a matching unit is actually run.

**Files:**
- Create: `hermes/test/wasm/native-lookup/addon.c`
- Create: `hermes/test/wasm/native-lookup/lkmod.wat_`
- Create: `hermes/test/wasm/native-lookup.js`
- Modify: `hermes/unittests/napi/CMakeLists.txt` (the addon target)
- Modify: `hermes/CMakeLists.txt:~851` (`HERMES_TEST_DEPS`)

**Interfaces:**
- Consumes: Tasks 2-5.
- Produces: the `napi_wasm_lookup_addon.node` test addon in `%napi_addon_dir`. Its exports: `install(mode)`, where mode is `"lkmod"`, `"lkmod262"`, `"throwmod"` or `"none"`, returning whether `hermes_set_wasm_cache` succeeded; and `counts()`, returning `{native, lookup, store, discard, accepted}` since the last `install`.

- [ ] **Step 1: The module**

Create `hermes/test/wasm/native-lookup/lkmod.wat_`. The trailing underscore keeps lit from collecting it, as the `.js_` drivers do:

```
;; Copyright (c) Meta Platforms, Inc. and affiliates.
;;
;; This source code is licensed under the MIT license found in the
;; LICENSE file in the root directory of this source tree.

;; Compiled into napi_wasm_lookup_addon twice, with and without -test262
;; (unittests/napi/CMakeLists.txt). Same module as native-lookup.js's bytes.
(module
  (func (export "add") (param i32 i32) (result i32)
    (i32.add (local.get 0) (local.get 1))))
```

- [ ] **Step 2: The addon**

Create `hermes/test/wasm/native-lookup/addon.c`:

```c
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A Wasm cache whose native tier answers with units linked into this addon,
 * so that new WebAssembly.Module(bytes) goes through createModuleFromBytes's
 * native tier -- the path WebAssembly.Module.fromNativeUnit() never takes.
 * Loaded by the hermes binary through loadNativeModule(), which gives it a
 * napi_env on the runtime the test script runs on.
 *
 * lkmod and lkmod262 are lkmod.wat_ compiled without and with -test262;
 * throwmod is native-unit-throws-throwing.js_, an ordinary JS unit whose top
 * level throws. The build generates all three (unittests/napi/
 * CMakeLists.txt).
 */

#include <hermes/VM/static_h.h>
#include <hermes_napi.h>
#include <node_api.h>

#include <stdbool.h>
#include <string.h>

extern SHUnit *sh_export_lkmod(void);
extern SHUnit *sh_export_lkmod262(void);
extern SHUnit *sh_export_throwmod(void);

static SHUnitCreator s_answer;
static int s_native, s_lookup, s_store, s_discard, s_accepted;

static SHUnitCreator lookupNative(void *ctx, const uint8_t *wasm, size_t size) {
  (void)ctx;
  (void)wasm;
  (void)size;
  ++s_native;
  return s_answer;
}

static bool lookup(
    void *ctx,
    const uint8_t *wasm,
    size_t wasmSize,
    const uint8_t *config,
    size_t configSize,
    const uint8_t **hbc,
    size_t *hbcSize,
    void (**finalizeCb)(const uint8_t *, size_t, void *),
    void **finalizeHint,
    void **storeToken) {
  (void)ctx;
  (void)wasm;
  (void)wasmSize;
  (void)config;
  (void)configSize;
  (void)hbc;
  (void)hbcSize;
  (void)finalizeCb;
  (void)finalizeHint;
  ++s_lookup;
  *storeToken = NULL;
  return false;
}

static void store(void *ctx, void *token, const uint8_t *hbc, size_t size) {
  (void)ctx;
  (void)token;
  (void)hbc;
  (void)size;
  ++s_store;
}

static void discard(void *ctx, void *token) {
  (void)ctx;
  (void)token;
  ++s_discard;
}

static void accepted(void *ctx, void *token) {
  (void)ctx;
  (void)token;
  ++s_accepted;
}

/* throwmod is a JS unit, and a JS unit does not register itself. Registered
 * here, at module init -- every constructor in this addon has run by then, so
 * lkmod's registration exists -- with lkmod's configuration, which is what a
 * real Wasm unit compiled by this shermes carries. Stating none, or a
 * different one, would get it refused before its top level threw. */
static SHWasmUnitReg s_throwReg = {"throwmod", sh_export_throwmod, NULL, NULL};

static napi_value install(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  char mode[32] = {0};
  size_t len = 0;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_get_value_string_utf8(env, argv[0], mode, sizeof(mode), &len);
  if (strcmp(mode, "lkmod") == 0)
    s_answer = sh_export_lkmod;
  else if (strcmp(mode, "lkmod262") == 0)
    s_answer = sh_export_lkmod262;
  else if (strcmp(mode, "throwmod") == 0)
    s_answer = sh_export_throwmod;
  else
    s_answer = NULL;
  s_native = s_lookup = s_store = s_discard = s_accepted = 0;

  hermes_wasm_cache_callbacks cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.struct_size = sizeof(cbs);
  cbs.lookup = lookup;
  cbs.store = store;
  cbs.discard = discard;
  cbs.lookup_native = lookupNative;
  cbs.accepted = accepted;
  napi_value ok;
  napi_get_boolean(env, hermes_set_wasm_cache(env, &cbs) == napi_ok, &ok);
  return ok;
}

static void setInt(napi_env env, napi_value obj, const char *name, int v) {
  napi_value n;
  napi_create_int32(env, v, &n);
  napi_set_named_property(env, obj, name, n);
}

static napi_value counts(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value obj;
  napi_create_object(env, &obj);
  setInt(env, obj, "native", s_native);
  setInt(env, obj, "lookup", s_lookup);
  setInt(env, obj, "store", s_store);
  setInt(env, obj, "discard", s_discard);
  setInt(env, obj, "accepted", s_accepted);
  return obj;
}

napi_value napi_register_module_v1(napi_env env, napi_value exports) {
  const SHWasmUnitReg *lk = _sh_wasm_find_unit_reg(sh_export_lkmod);
  if (lk && !s_throwReg.codegen_config) {
    s_throwReg.codegen_config = lk->codegen_config;
    _sh_wasm_register_unit(&s_throwReg);
  }
  napi_value fn;
  napi_create_function(env, "install", NAPI_AUTO_LENGTH, install, NULL, &fn);
  napi_set_named_property(env, exports, "install", fn);
  napi_create_function(env, "counts", NAPI_AUTO_LENGTH, counts, NULL, &fn);
  napi_set_named_property(env, exports, "counts", fn);
  return exports;
}
```

- [ ] **Step 3: Build it**

In `hermes/unittests/napi/CMakeLists.txt`, inside `if(NOT WIN32)` after the last existing addon:

```cmake
  # Wasm units linked into an addon rather than an executable, so that
  # test/wasm/native-lookup.js can drive createModuleFromBytes's native tier
  # from the hermes binary through hermes_set_wasm_cache. The units' C is
  # generated at build time: it is a function of this very compiler, and a
  # checked-in copy would go stale with every codegen change.
  if(HERMES_ENABLE_WASM AND TARGET shermes AND TARGET wat2wasm)
    set(NL_SRC ${PROJECT_SOURCE_DIR}/test/wasm/native-lookup)
    set(NL_OUT ${CMAKE_CURRENT_BINARY_DIR}/native-lookup)
    set(NL_THROW ${PROJECT_SOURCE_DIR}/test/wasm/native-unit-throws-throwing.js_)
    add_custom_command(
      OUTPUT ${NL_OUT}/lkmod.wasm
      COMMAND ${CMAKE_COMMAND} -E make_directory ${NL_OUT}
      COMMAND $<TARGET_FILE:wat2wasm> ${NL_SRC}/lkmod.wat_ -o ${NL_OUT}/lkmod.wasm
      DEPENDS wat2wasm ${NL_SRC}/lkmod.wat_
      VERBATIM)
    add_custom_command(
      OUTPUT ${NL_OUT}/lkmod.c
      COMMAND $<TARGET_FILE:shermes> -emit-c -exported-unit=lkmod
              -o ${NL_OUT}/lkmod.c ${NL_OUT}/lkmod.wasm
      DEPENDS shermes ${NL_OUT}/lkmod.wasm
      VERBATIM)
    add_custom_command(
      OUTPUT ${NL_OUT}/lkmod262.c
      COMMAND $<TARGET_FILE:shermes> -emit-c -test262 -exported-unit=lkmod262
              -o ${NL_OUT}/lkmod262.c ${NL_OUT}/lkmod.wasm
      DEPENDS shermes ${NL_OUT}/lkmod.wasm
      VERBATIM)
    add_custom_command(
      OUTPUT ${NL_OUT}/throwmod.c
      COMMAND $<TARGET_FILE:shermes> -emit-c -exported-unit=throwmod
              -o ${NL_OUT}/throwmod.c ${NL_THROW}
      DEPENDS shermes ${NL_THROW}
      VERBATIM)
    add_library(napi_wasm_lookup_addon SHARED
      ${NL_SRC}/addon.c
      ${NL_OUT}/lkmod.c
      ${NL_OUT}/lkmod262.c
      ${NL_OUT}/throwmod.c
    )
    # Headers only, like every other test addon: the VM and NAPI symbols
    # resolve from the host binary at load time.
    target_include_directories(napi_wasm_lookup_addon PRIVATE
      ${PROJECT_SOURCE_DIR}/include
      ${PROJECT_SOURCE_DIR}/include/hermes/napi
      ${PROJECT_SOURCE_DIR}/API/napi
      ${PROJECT_BINARY_DIR}/lib/config
    )
    # What shermes passes when it compiles generated C itself
    # (tools/shermes/compile.cpp).
    target_compile_options(napi_wasm_lookup_addon PRIVATE
      -fno-strict-aliasing -fno-strict-overflow)
    set_target_properties(napi_wasm_lookup_addon PROPERTIES
      PREFIX ""
      SUFFIX ".node"
      C_STANDARD 11
      C_EXTENSIONS ON
    )
    if(APPLE)
      target_link_options(napi_wasm_lookup_addon PRIVATE
        -undefined dynamic_lookup)
    endif()
  endif()
```
Before relying on `${PROJECT_BINARY_DIR}/lib/config`, confirm that this is where `libhermesvm-config.h` is generated: `ls cmake-build-asan/hermes/lib/config/libhermesvm-config.h`. `hermes/lib/CMakeLists.txt` configures it as `config/libhermesvm-config.h` relative to its own binary dir.

In `hermes/CMakeLists.txt`, after the `napi_async_work_addon` entry in the `HERMES_TEST_DEPS` list (inside `if(NOT WIN32)`), add:

```cmake
      # Defined only in a Wasm build with shermes (unittests/napi), which
      # is added above this point.
      if(TARGET napi_wasm_lookup_addon)
        list(APPEND HERMES_TEST_DEPS napi_wasm_lookup_addon)
      endif()
```

```bash
cmake --build cmake-build-asan --target napi_wasm_lookup_addon
nm -D cmake-build-asan/hermes/unittests/napi/napi_wasm_lookup_addon.node | grep -E 'sh_export_(lkmod|lkmod262|throwmod)$'
```
Expected: three `T` lines.

- [ ] **Step 4: Write the lit test**

Create `hermes/test/wasm/native-lookup.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// new WebAssembly.Module(bytes) reaching a linked native unit through the
// embedder's lookup_native, rather than through fromNativeUnit(). The addon
// answers with the unit named by install(); counts() reports which tiers
// Hermes then asked. A unit runs only under its own codegen configuration:
// lkmod is t262=0 and lkmod262 is t262=1, so each run accepts exactly one,
// and the other falls through to lookup and a compile. throwmod's top level
// throws, which must be the module's error -- not a fallback.

// REQUIRES: napi, wasm, shermes
// UNSUPPORTED: windows
// RUN: sed 's#@ADDON@#%napi_addon_dir/napi_wasm_lookup_addon.node#' %s > %t.js
// RUN: %hermes %t.js | %FileCheck --match-full-lines --check-prefix=PLAIN %s
// RUN: %hermes -test262 %t.js | %FileCheck --match-full-lines --check-prefix=T262 %s

var addon = loadNativeModule('@ADDON@');
// lkmod.wat_, assembled.
var bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
  0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
  0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20,
  0x00, 0x20, 0x01, 0x6a, 0x0b]);

function run(mode) {
  var installed = addon.install(mode);
  var result;
  try {
    result = new WebAssembly.Instance(new WebAssembly.Module(bytes), {})
        .exports.add(2, 3);
  } catch (e) {
    result = 'threw ' + e.message;
  }
  var c = addon.counts();
  print(mode + ' (' + installed + '): ' + result + ' native=' + c.native +
      ' lookup=' + c.lookup + ' store=' + c.store + ' accepted=' + c.accepted);
}

run('lkmod');
run('lkmod262');
run('none');
run('throwmod');

// PLAIN: lkmod (true): 5 native=1 lookup=0 store=0 accepted=0
// PLAIN-NEXT: lkmod262 (true): 5 native=1 lookup=1 store=1 accepted=0
// PLAIN-NEXT: none (true): 5 native=1 lookup=1 store=1 accepted=0
// PLAIN-NEXT: throwmod (true): threw boom native=1 lookup=0 store=0 accepted=0

// T262: lkmod (true): 5 native=1 lookup=1 store=1 accepted=0
// T262-NEXT: lkmod262 (true): 5 native=1 lookup=0 store=0 accepted=0
// T262-NEXT: none (true): 5 native=1 lookup=1 store=1 accepted=0
// T262-NEXT: throwmod (true): 5 native=1 lookup=1 store=1 accepted=0
```
(`throwmod` under `-test262`: its registration carries lkmod's `t262=0`, so it is refused and the bytes compile.)

- [ ] **Step 5: Run it, then the whole Hermes Wasm and NAPI set**

```bash
LIT_FILTER='wasm/native-lookup\.js$' cmake --build cmake-build-asan --target check-hermes
LIT_FILTER='wasm/|napi/' cmake --build cmake-build-asan --target check-hermes
cmake-build-asan/hermes/unittests/napi/NapiTests
cmake-build-asan/hermes/unittests/VMRuntime/VMRuntimeTests
```
Expected: all pass. If `native-lookup.js` is reported UNSUPPORTED, the lit run lacks the `napi` feature (`napi_enabled` param). Check `hermes/CMakeLists.txt:~903`; the feature must be on in this tree for the test to mean anything.

- [ ] **Step 6: Commit (in the submodule)**

```bash
git -C hermes add test/wasm/native-lookup/addon.c test/wasm/native-lookup/lkmod.wat_ \
  test/wasm/native-lookup.js unittests/napi/CMakeLists.txt CMakeLists.txt
git -C hermes commit -m "Test the Wasm native tier through a real linked unit

The unit tests reach lookup_native only with creators Hermes has to
refuse. A NAPI addon now carries real units -- one module compiled with
and without -test262, and a unit whose top level throws -- and answers
new WebAssembly.Module(bytes) with whichever the test selects, loaded
into the hermes binary through loadNativeModule.

Each runtime accepts exactly the unit built under its own -test262 and
compiles the bytes for the other, and the throwing unit's error reaches
the program instead of a fallback compile.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 7: Record format v2 (hermes-node)

**Files:**
- Modify: `include/hermes/node-compat/bundle/wasm_record.h`, `lib/bundle/wasm_record.cpp`
- Modify: `include/hermes/node-compat/bundle/native_digest.h`, `lib/bundle/native_digest.cpp` (`sha256Digest`)
- Modify: `lib/bundle/bundle_tools.cpp:618-665` (`dumpWasmRecord`)
- Test: `unittests/WasmRecordTest.cpp`, plus any test that matches `format v1` (`grep -rn 'format v1' test unittests`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  constexpr uint32_t kWasmRecordFormatVersion = 2;   // what the writer writes
  struct WasmRecordHeader {   // v2; v1 is its first kWasmRecordHeaderV1Size bytes
    char magic[8];
    uint32_t formatVersion, count, versionOffset, versionLength,
        recordTableOffset, payloadOffset, payloadSize;
    uint32_t sourceTableOffset, sourceCount;   // v2 only
  };
  constexpr size_t kWasmRecordHeaderV1Size = offsetof(WasmRecordHeader, sourceTableOffset); // 36
  struct WasmSourceRecord { uint8_t digest[kNativeDigestBytes]; uint32_t payloadOffset; uint32_t payloadSize; };
  struct WasmRecordCommit {
    const uint8_t *configDigest; const uint8_t *bytecode; size_t bytecodeSize;
    const uint8_t *sourceDigest; const uint8_t *source; size_t sourceSize;
  };
  bool WasmRecordWriter::record(const WasmRecordCommit &commit);
  uint32_t WasmRecordReader::formatVersion() const;
  uint32_t WasmRecordReader::sourceCount() const;            // 0 for v1
  std::string_view WasmRecordReader::sourceDigest(uint32_t i) const;
  std::string_view WasmRecordReader::source(uint32_t i) const;
  std::string sha256Digest(const uint8_t *bytes, size_t size); // raw 32 bytes
  ```
  The three-argument `record(digest, bytecode, size)` stays for now and writes a v2 file with no source for that entry. Task 8 removes it.

- [ ] **Step 1: Write failing tests**

Append to `unittests/WasmRecordTest.cpp`. Reuse its existing helpers for temp files and reading a file back, if it has them; otherwise use `TempTree` and an `std::ifstream` as its other tests do.

```cpp
namespace {

WasmRecordCommit commitOf(
    const std::string &cfg,
    const std::string &bc,
    const std::string &srcDigest,
    const std::string &src) {
  return WasmRecordCommit{
      reinterpret_cast<const uint8_t *>(cfg.data()),
      reinterpret_cast<const uint8_t *>(bc.data()),
      bc.size(),
      reinterpret_cast<const uint8_t *>(srcDigest.data()),
      reinterpret_cast<const uint8_t *>(src.data()),
      src.size()};
}

/// A v1 file exactly as every hermes-node before format v2 wrote it.
std::vector<uint8_t> makeV1File(
    const std::string &version,
    const std::vector<std::pair<std::string, std::string>> &entries) {
  auto align = [](size_t n, size_t a) { return (n + a - 1) & ~(a - 1); };
  size_t tableOff = align(kWasmRecordHeaderV1Size + version.size(), 4);
  size_t payloadOff =
      align(tableOff + entries.size() * sizeof(BundleWasmRecord), 8);
  std::vector<uint8_t> payload;
  std::vector<BundleWasmRecord> table;
  for (const auto &[digest, bc] : entries) {
    BundleWasmRecord r{};
    std::memcpy(r.digest, digest.data(), kNativeDigestBytes);
    r.payloadOffset = static_cast<uint32_t>(payload.size());
    r.payloadSize = static_cast<uint32_t>(bc.size());
    table.push_back(r);
    payload.insert(payload.end(), bc.begin(), bc.end());
    payload.resize(align(payload.size(), 8), 0);
  }
  std::vector<uint8_t> out(payloadOff + payload.size(), 0);
  std::memcpy(out.data(), kWasmRecordMagic, 8);
  uint32_t fields[7] = {
      1,
      static_cast<uint32_t>(entries.size()),
      static_cast<uint32_t>(kWasmRecordHeaderV1Size),
      static_cast<uint32_t>(version.size()),
      static_cast<uint32_t>(tableOff),
      static_cast<uint32_t>(payloadOff),
      static_cast<uint32_t>(payload.size())};
  std::memcpy(out.data() + 8, fields, sizeof(fields));
  std::memcpy(out.data() + kWasmRecordHeaderV1Size, version.data(), version.size());
  std::memcpy(out.data() + tableOff, table.data(), table.size() * sizeof(BundleWasmRecord));
  std::memcpy(out.data() + payloadOff, payload.data(), payload.size());
  return out;
}

} // namespace

TEST(WasmRecordTest, Sha256DigestMatchesTheStandardVector) {
  const uint8_t abc[] = {'a', 'b', 'c'};
  EXPECT_EQ(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      nativeDigestToHex(sha256Digest(abc, sizeof(abc))));
}

TEST(WasmRecordTest, V2RoundTripsBytecodeAndSource) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "BYTECODE", makeDigest(9), "WASMSRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  std::string err;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(2u, r->formatVersion());
  ASSERT_EQ(1u, r->count());
  EXPECT_EQ(makeDigest(1), r->digest(0));
  EXPECT_EQ("BYTECODE", r->payload(0));
  ASSERT_EQ(1u, r->sourceCount());
  EXPECT_EQ(makeDigest(9), r->sourceDigest(0));
  EXPECT_EQ("WASMSRC", r->source(0));
}

TEST(WasmRecordTest, OneSourceForTwoConfigurations) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "BC-T262-0", makeDigest(9), "SRC")));
  ASSERT_TRUE(w.record(commitOf(makeDigest(2), "BC-T262-1", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), nullptr);
  ASSERT_TRUE(r);
  EXPECT_EQ(2u, r->count());
  EXPECT_EQ(1u, r->sourceCount());
}

TEST(WasmRecordTest, ReplacingBytecodeKeepsTheSource) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "OLD", makeDigest(9), "SRC")));
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "NEW", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), nullptr);
  ASSERT_TRUE(r);
  ASSERT_EQ(1u, r->count());
  EXPECT_EQ("NEW", r->payload(0));
  ASSERT_EQ(1u, r->sourceCount());
  EXPECT_EQ("SRC", r->source(0));
}

TEST(WasmRecordTest, ReadsAV1FileForItsBytecode) {
  std::vector<uint8_t> v1 = makeV1File("ver", {{makeDigest(3), "BC3"}});
  std::string err;
  auto r = WasmRecordReader::open(v1.data(), v1.size(), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(1u, r->formatVersion());
  ASSERT_EQ(1u, r->count());
  EXPECT_EQ("BC3", r->payload(0));
  EXPECT_EQ(0u, r->sourceCount());
}

TEST(WasmRecordTest, AnEmptyV1FileIsNeverReadPastItsEnd) {
  // Exactly the v1 file's bytes and no more: the v2 fields would sit past
  // its end, so reading them is a heap overflow under AddressSanitizer.
  std::vector<uint8_t> v1 = makeV1File("", {});
  ASSERT_LT(v1.size(), sizeof(WasmRecordHeader));
  auto *exact = static_cast<uint8_t *>(::malloc(v1.size()));
  ASSERT_NE(nullptr, exact);
  std::memcpy(exact, v1.data(), v1.size());
  std::string err;
  auto r = WasmRecordReader::open(exact, v1.size(), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(1u, r->formatVersion());
  EXPECT_EQ(0u, r->count());
  EXPECT_EQ(0u, r->sourceCount());
  ::free(exact);
}

TEST(WasmRecordTest, RejectsAV2FileTruncatedToTheV1HeaderSize) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "");
  ASSERT_TRUE(w.flush());
  std::vector<uint8_t> bytes = readFileBytes(path);
  bytes.resize(kWasmRecordHeaderV1Size + 4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(bytes.data(), bytes.size(), &err));
  EXPECT_NE(std::string::npos, err.find("truncated"));
}

TEST(WasmRecordTest, RejectsAnUnknownLaterVersion) {
  std::vector<uint8_t> v1 = makeV1File("", {});
  uint32_t three = 3;
  std::memcpy(v1.data() + offsetof(WasmRecordHeader, formatVersion), &three, 4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(v1.data(), v1.size(), &err));
  EXPECT_NE(std::string::npos, err.find("version"));
}

TEST(WasmRecordTest, RejectsAMisalignedSourceTable) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "BC", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  uint32_t off;
  std::memcpy(&off, bytes.data() + offsetof(WasmRecordHeader, sourceTableOffset), 4);
  off += 1;
  std::memcpy(bytes.data() + offsetof(WasmRecordHeader, sourceTableOffset), &off, 4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(bytes.data(), bytes.size(), &err));
}

TEST(WasmRecordTest, RejectsAnOutOfRangeSourcePayload) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "BC", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  uint32_t tableOff;
  std::memcpy(&tableOff, bytes.data() + offsetof(WasmRecordHeader, sourceTableOffset), 4);
  uint32_t huge = 0x7fffffff;
  std::memcpy(
      bytes.data() + tableOff + offsetof(WasmSourceRecord, payloadSize), &huge, 4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(bytes.data(), bytes.size(), &err));
  EXPECT_NE(std::string::npos, err.find("source"));
}
```
`makeDigest` and `readFileBytes` are the file's own helpers. Add `#include <hermes/node-compat/bundle/native_digest.h>`, `<cstddef>` and `<cstdlib>` if missing.

- [ ] **Step 2: Confirm they fail**

```bash
cmake --build cmake-build-asan --target WasmRecordTest 2>&1 | grep -m3 error
```
Expected: compile errors (`WasmRecordCommit`, `sha256Digest`, `kWasmRecordHeaderV1Size` and friends do not exist).

- [ ] **Step 3: `sha256Digest`**

In `native_digest.h`:

```cpp
/// SHA-256 of \p size bytes at \p bytes, as kNativeDigestBytes raw bytes.
/// What a WebAssembly module's source entry and its native unit are keyed
/// on: the module alone, with no codegen configuration in it.
std::string sha256Digest(const uint8_t *bytes, size_t size);
```
In `native_digest.cpp`, following `nativeFileDigest`'s use of picohash:

```cpp
std::string sha256Digest(const uint8_t *bytes, size_t size) {
  picohash_ctx_t ctx;
  picohash_init_sha256(&ctx);
  picohash_update(&ctx, bytes, size);
  std::string raw(kNativeDigestBytes, '\0');
  picohash_final(&ctx, raw.data());
  return raw;
}
```

- [ ] **Step 4: Header, source record, commit**

In `wasm_record.h`: set `kWasmRecordFormatVersion = 2`. Append `sourceTableOffset` and `sourceCount` to `WasmRecordHeader`, with a comment saying they are v2-only and must never be read from a v1 file. After the struct:

```cpp
/// A v1 header is the first this many bytes of a v2 one.
constexpr size_t kWasmRecordHeaderV1Size =
    offsetof(WasmRecordHeader, sourceTableOffset);
static_assert(kWasmRecordHeaderV1Size == 36, "v1 header layout is frozen");

/// One distinct WebAssembly module's bytes, keyed by SHA-256 over those
/// bytes alone. A separate table rather than a wider BundleWasmRecord,
/// because that struct is shared verbatim with the container's Wasm table,
/// which does not change.
struct WasmSourceRecord {
  uint8_t digest[kNativeDigestBytes];
  uint32_t payloadOffset; // relative to the header's payloadOffset
  uint32_t payloadSize;
};
static_assert(
    sizeof(WasmSourceRecord) == kNativeDigestBytes + 8,
    "WasmSourceRecord is written to disk verbatim");

/// A module Hermes accepted: the bytecode it compiled to (or was served)
/// under one codegen configuration, and the module bytes it came from.
/// Every pointer is borrowed for the call.
struct WasmRecordCommit {
  const uint8_t *configDigest; // kNativeDigestBytes; the cache's digest
  const uint8_t *bytecode;
  size_t bytecodeSize;
  const uint8_t *sourceDigest; // kNativeDigestBytes; sha256Digest(source)
  const uint8_t *source;
  size_t sourceSize;
};
```

`WasmRecordWriter`: add

```cpp
  /// Add or replace the bytecode for commit.configDigest -- replacing for
  /// the reason the three-argument record() gives -- and add the module
  /// bytes under commit.sourceDigest unless that digest is already held,
  /// then rewrite the file once. One source entry serves every
  /// configuration the same module was recorded under.
  bool record(const WasmRecordCommit &commit);
```
Add a private `struct PendingSource { std::string digest; std::string bytes; };` and `std::vector<PendingSource> sources_;`.

`WasmRecordReader`: add the accessors from the Interfaces block. Replace `const WasmRecordHeader *header_` with a by-value `WasmRecordHeader header_{}`, and add `const WasmSourceRecord *sources_ = nullptr;`. Document on the class that a v1 file's v2-only fields stay zero and are never read from the buffer.

- [ ] **Step 5: Writer and reader**

`record(const WasmRecordCommit &)`:

```cpp
bool WasmRecordWriter::record(const WasmRecordCommit &c) {
  std::string digest(
      reinterpret_cast<const char *>(c.configDigest), kNativeDigestBytes);
  std::string bytecode(
      reinterpret_cast<const char *>(c.bytecode), c.bytecodeSize);
  std::string srcDigest(
      reinterpret_cast<const char *>(c.sourceDigest), kNativeDigestBytes);
  bool changed = false;

  auto rec = std::find_if(records_.begin(), records_.end(), [&](auto &r) {
    return r.digest == digest;
  });
  if (rec == records_.end()) {
    records_.push_back(PendingRecord{std::move(digest), std::move(bytecode)});
    changed = true;
  } else if (rec->bytecode != bytecode) {
    rec->bytecode = std::move(bytecode);
    changed = true;
  }

  // Same digest, same bytes: SHA-256 over the module is the module, so a
  // second sighting adds nothing.
  bool haveSource = std::any_of(sources_.begin(), sources_.end(), [&](auto &s) {
    return s.digest == srcDigest;
  });
  if (!haveSource) {
    sources_.push_back(PendingSource{
        std::move(srcDigest),
        std::string(reinterpret_cast<const char *>(c.source), c.sourceSize)});
    changed = true;
  }
  return changed ? writeFile() : true;
}
```

`writeFile()`: sort sources by digest as records are sorted. Layout: header (`sizeof(WasmRecordHeader)`), version bytes, pad to `alignof(BundleWasmRecord)` -> record table, pad to `alignof(WasmSourceRecord)` -> source table, pad to `kBundlePayloadAlign` -> payload. The payload area holds every bytecode payload, then every source payload, each padded to `kBundlePayloadAlign`. Source offsets are relative to `payloadOffset`, exactly as bytecode offsets are. Fill `header.sourceTableOffset` and `header.sourceCount`. Update the layout comment.

`open()`:

```cpp
  if (size < kWasmRecordHeaderV1Size)
    return fail("wasm record file: truncated (shorter than the header)");
  if (std::memcmp(data, kWasmRecordMagic, sizeof(kWasmRecordMagic)) != 0)
    return fail(
        "wasm record file: not a hermes-node wasm record file (bad magic)");
  uint32_t version;
  std::memcpy(
      &version, data + offsetof(WasmRecordHeader, formatVersion), sizeof(version));
  // An unknown version is refused before anything past the common prefix is
  // interpreted: its layout is not known here.
  if (version != 1 && version != kWasmRecordFormatVersion)
    return fail(
        "wasm record file: format version mismatch (file was written by "
        "an incompatible hermes-node)");
  WasmRecordHeader h{};
  if (version == 1) {
    std::memcpy(&h, data, kWasmRecordHeaderV1Size); // v2 fields stay zero
  } else {
    if (size < sizeof(WasmRecordHeader))
      return fail("wasm record file: truncated (shorter than the header)");
    std::memcpy(&h, data, sizeof(WasmRecordHeader));
  }
```
Then keep the existing range, alignment and per-record checks, reading `h.` instead of `header->`. For version 2 only, add:

```cpp
  if (version != 1) {
    if (!tableInRange(size, h.sourceTableOffset, h.sourceCount, sizeof(WasmSourceRecord)))
      return fail("wasm record file: source table out of range");
    if (h.sourceTableOffset % alignof(WasmSourceRecord) != 0)
      return fail("wasm record file: source table offset is misaligned");
    const auto *src = reinterpret_cast<const WasmSourceRecord *>(data + h.sourceTableOffset);
    for (uint32_t i = 0; i < h.sourceCount; ++i)
      if (!inRange(h.payloadSize, src[i].payloadOffset, src[i].payloadSize))
        return fail("wasm record file: source payload out of range");
    reader.sources_ = src;
  }
```
`formatVersion()` returns `header_.formatVersion`. `sourceCount()` returns `header_.sourceCount`, which is 0 for v1. `sourceDigest(i)` and `source(i)` mirror `digest(i)` and `payload(i)`.

- [ ] **Step 6: `--dump-wasm` prints the version it read, and the sources**

In `dumpWasmRecord`, print `reader->formatVersion()` instead of the constant, and drop the comment that says only one version can open. After the `WASM` loop:

```cpp
  if (reader->formatVersion() < 2) {
    out << "\nSOURCES: none (format v1 records no module bytes)\n";
  } else {
    const uint32_t sources = reader->sourceCount();
    out << "\nSOURCES (" << sources << ")\n";
    for (uint32_t i = 0; i < sources; ++i) {
      std::string digestHex = nativeDigestToHex(reader->sourceDigest(i));
      if (!verbose)
        digestHex.resize(16);
      out << "  [" << i << "] " << reader->source(i).size()
          << " bytes  sha256:" << digestHex << "\n";
    }
  }
```

- [ ] **Step 7: Run the tests, then update every `format v1` expectation**

```bash
cmake --build cmake-build-asan --target WasmRecordTest BundleToolsTest && \
  cmake-build-asan/unittests/WasmRecordTest && cmake-build-asan/unittests/BundleToolsTest
grep -rn 'format v1' test unittests
```
Change each hit that describes a file this binary just wrote to `format v2`. Each `--dump-wasm` check also gains a `SOURCES (0)` line, since the three-argument `record()` the runtime still calls writes no source. Then run the full suite:

```bash
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```
Expected: green, with the exceptions listed in Global Constraints.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/bundle/wasm_record.h lib/bundle/wasm_record.cpp \
  include/hermes/node-compat/bundle/native_digest.h lib/bundle/native_digest.cpp \
  lib/bundle/bundle_tools.cpp unittests/WasmRecordTest.cpp <each test file you edited>
git commit -m "Wasm record format v2: carry each module's source bytes

A native build has to compile the modules a recording run saw, and v1
kept only their bytecode. v2 adds a second table, keyed by SHA-256 over
the module bytes alone, so one source entry serves every configuration
the module was recorded under. BundleWasmRecord is untouched, because
it is shared verbatim with the container's Wasm table.

Readers take v1 and v2. The parse is version-dependent: a v1 file is
never read past its own header, which a test pins by handing the reader
exactly a v1 file's bytes. --dump-wasm prints the source table.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 8: Commit only what Hermes accepted (hermes-node)

**Files:**
- Modify: `lib/compile-cache/wasm_cache_hooks.cpp`
- Modify: `include/hermes/node-compat/bundle/wasm_record.h`, `lib/bundle/wasm_record.cpp` (remove the three-argument `record`)
- Modify: `unittests/WasmRecordTest.cpp` (move the remaining three-argument callers to `WasmRecordCommit`)
- Modify: `unittests/CompileCacheRunTest.cpp`, `unittests/CMakeLists.txt`

**Interfaces:**
- Consumes: Hermes `accepted` (Task 5); `WasmRecordWriter::record(const WasmRecordCommit &)` and `sha256Digest` (Task 7).
- Produces: when recording, `lookup` copies the module bytes into the store token, plus the returned bytecode on a hit. Commits happen at `store` and at `accepted`; a bare `discard` commits nothing. Trace lines `wasm record container|disk|compile` are now printed at commit time.

- [ ] **Step 1: Write failing tests**

In `unittests/CMakeLists.txt`, after `CompileCacheRunTest`'s `target_link_libraries`:

```cmake
# The Wasm cases drive a real WebAssembly.Module compile.
if(HERMES_ENABLE_WASM)
  target_compile_definitions(CompileCacheRunTest PRIVATE HERMES_ENABLE_WASM)
endif()
```

In `CompileCacheRunTest.cpp`, add includes for `wasm_cache_hooks.h`, `wasm_record.h` and `native_digest.h`, and `#include <hermes/node-compat/compile-cache/compile_cache.h>` if it is not already reached. Add to the fixture:

```cpp
  /// new WebAssembly.Module over \p bytes; true if it compiled. Leaves no
  /// exception pending.
  bool compileWasm(const std::vector<uint8_t> &bytes) {
    std::string src = "(function(){var b=new Uint8Array([";
    for (uint8_t b : bytes)
      src += std::to_string(b) + ",";
    src += "]); new WebAssembly.Module(b); return true;})()";
    napi_value script = nullptr, result = nullptr;
    EXPECT_EQ(
        napi_ok,
        napi_create_string_utf8(env_, src.c_str(), NAPI_AUTO_LENGTH, &script));
    bool ok = napi_run_script(env_, script, &result) == napi_ok;
    clearException();
    return ok;
  }

  /// The codegen configuration Hermes hands a cache, learned the way an
  /// embedder learns it: from a lookup. Leaves no cache installed.
  std::string codegenConfig() {
    static std::string seen;
    hermes_wasm_cache_callbacks probe{};
    probe.struct_size = sizeof(probe);
    probe.lookup = [](void *, const uint8_t *, size_t, const uint8_t *cfg,
                      size_t cfgSize, const uint8_t **, size_t *,
                      void (**)(const uint8_t *, size_t, void *), void **,
                      void **token) {
      seen.assign(reinterpret_cast<const char *>(cfg), cfgSize);
      *token = nullptr;
      return false;
    };
    probe.store = [](void *, void *, const uint8_t *, size_t) {};
    probe.discard = [](void *, void *) {};
    EXPECT_EQ(napi_ok, hermes_set_wasm_cache(env_, &probe));
    EXPECT_TRUE(compileWasm(kAddWasm));
    EXPECT_EQ(napi_ok, hermes_set_wasm_cache(env_, nullptr));
    return seen;
  }
```

At file scope, inside `#ifdef HERMES_ENABLE_WASM` and **above the fixture class** (its new methods use these). Wrap the two new fixture methods in `#ifdef HERMES_ENABLE_WASM` as well:

```cpp
/// (module (func (export "add") (param i32 i32) (result i32)
///   (i32.add (local.get 0) (local.get 1))))
const std::vector<uint8_t> kAddWasm = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
    0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20,
    0x00, 0x20, 0x01, 0x6a, 0x0b};
/// The right magic and a version Hermes does not know: never compiles.
const std::vector<uint8_t> kGarbageWasm = {0x00, 0x61, 0x73, 0x6d, 0x09, 0x00, 0x00, 0x00};

struct RecordView {
  uint32_t count = 0, sourceCount = 0;
  std::string bytecode, source, sourceDigest;
};

/// What the record file at \p path holds now.
RecordView readRecord(const std::string &path) {
  std::string bytes = readFile(path);
  RecordView v;
  auto r = WasmRecordReader::open(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), nullptr);
  if (!r)
    return v;
  v.count = r->count();
  v.sourceCount = r->sourceCount();
  if (v.count)
    v.bytecode = std::string(r->payload(0));
  if (v.sourceCount) {
    v.source = std::string(r->source(0));
    v.sourceDigest = std::string(r->sourceDigest(0));
  }
  return v;
}
```

Tests:

```cpp
TEST_F(CompileCacheRunTest, RecordCommitsAtStore) {
  TempTree tree;
  WasmRecordWriter w(tree.path() + "/rec.bin", "test");
  WasmCacheContext ctx;
  ctx.recorder = &w;
  installWasmCacheHooks(env_, &ctx, /*bundleMode*/ false);
  ASSERT_TRUE(compileWasm(kAddWasm));
  RecordView v = readRecord(tree.path() + "/rec.bin");
  EXPECT_EQ(1u, v.count);
  EXPECT_FALSE(v.bytecode.empty());
  ASSERT_EQ(1u, v.sourceCount);
  EXPECT_EQ(std::string(kAddWasm.begin(), kAddWasm.end()), v.source);
  EXPECT_EQ(sha256Digest(kAddWasm.data(), kAddWasm.size()), v.sourceDigest);
}

TEST_F(CompileCacheRunTest, RecordCommitsAtAcceptedFromAReleasedMapping) {
  TempTree tree;
  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path() + "/cc", "g"));

  WasmRecordWriter cold(tree.path() + "/cold.bin", "test");
  WasmCacheContext coldCtx;
  coldCtx.cache = &cache;
  coldCtx.recorder = &cold;
  installWasmCacheHooks(env_, &coldCtx, false);
  ASSERT_TRUE(compileWasm(kAddWasm)); // miss: compiled, stored, recorded

  // Warm: a disk hit. The disk adapter hands Hermes its mapping with a
  // finalizer, and Hermes finalizes before it calls accepted -- so what is
  // recorded must have been copied at lookup.
  WasmRecordWriter warm(tree.path() + "/warm.bin", "test");
  WasmCacheContext warmCtx;
  warmCtx.cache = &cache;
  warmCtx.recorder = &warm;
  installWasmCacheHooks(env_, &warmCtx, false);
  ASSERT_TRUE(compileWasm(kAddWasm));

  RecordView c = readRecord(tree.path() + "/cold.bin");
  RecordView h = readRecord(tree.path() + "/warm.bin");
  ASSERT_EQ(1u, h.count);
  EXPECT_EQ(c.bytecode, h.bytecode);
  ASSERT_EQ(1u, h.sourceCount);
  EXPECT_EQ(std::string(kAddWasm.begin(), kAddWasm.end()), h.source);
}

TEST_F(CompileCacheRunTest, NothingCommittedWhenAMissFailsToCompile) {
  TempTree tree;
  WasmRecordWriter w(tree.path() + "/rec.bin", "test");
  WasmCacheContext ctx;
  ctx.recorder = &w;
  installWasmCacheHooks(env_, &ctx, false);
  EXPECT_FALSE(compileWasm(kGarbageWasm));
  RecordView v = readRecord(tree.path() + "/rec.bin");
  EXPECT_EQ(0u, v.count);
  EXPECT_EQ(0u, v.sourceCount);
}

TEST_F(CompileCacheRunTest, NothingCommittedWhenARejectedHitsRecompileFails) {
  TempTree tree;
  std::string cfg = codegenConfig();
  ASSERT_FALSE(cfg.empty());

  // Real bytecode for kAddWasm, cut in half, is not bytecode Hermes will
  // load. Filed under kGarbageWasm's digest, it is a disk hit Hermes
  // rejects, for bytes whose recompile then fails.
  WasmRecordWriter seed(tree.path() + "/seed.bin", "test");
  WasmCacheContext seedCtx;
  seedCtx.recorder = &seed;
  installWasmCacheHooks(env_, &seedCtx, false);
  ASSERT_TRUE(compileWasm(kAddWasm));
  std::string bc = readRecord(tree.path() + "/seed.bin").bytecode;
  ASSERT_GT(bc.size(), 16u);
  std::string half = bc.substr(0, bc.size() / 2);

  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path() + "/cc", "g"));
  cache.setTracing(true);
  CompileCacheEntry entry;
  std::string digest = compileCacheWasmDigest(
      reinterpret_cast<const uint8_t *>(cfg.data()), cfg.size(),
      kGarbageWasm.data(), kGarbageWasm.size());
  ASSERT_FALSE(cache.lookupWasm(entry, digest, kGarbageWasm.data(), kGarbageWasm.size()));
  cache.saveWasm(entry, reinterpret_cast<const uint8_t *>(half.data()), half.size());

  WasmRecordWriter w(tree.path() + "/rec.bin", "test");
  WasmCacheContext ctx;
  ctx.cache = &cache;
  ctx.recorder = &w;
  installWasmCacheHooks(env_, &ctx, false);
  testing::internal::CaptureStderr();
  EXPECT_FALSE(compileWasm(kGarbageWasm));
  std::string trace = testing::internal::GetCapturedStderr();
  EXPECT_NE(std::string::npos, trace.find("wasm hit"))
      << "the planted entry must be a hit, or this tests nothing:\n" << trace;
  EXPECT_EQ(std::string::npos, trace.find("wasm store"));
  RecordView v = readRecord(tree.path() + "/rec.bin");
  EXPECT_EQ(0u, v.count);
  EXPECT_EQ(0u, v.sourceCount);
}

TEST_F(CompileCacheRunTest, ARejectedHitRecordsWhatWasRecompiled) {
  TempTree tree;
  std::string cfg = codegenConfig();
  WasmRecordWriter seed(tree.path() + "/seed.bin", "test");
  WasmCacheContext seedCtx;
  seedCtx.recorder = &seed;
  installWasmCacheHooks(env_, &seedCtx, false);
  ASSERT_TRUE(compileWasm(kAddWasm));
  std::string good = readRecord(tree.path() + "/seed.bin").bytecode;
  std::string half = good.substr(0, good.size() / 2);

  CompileCache cache;
  ASSERT_TRUE(cache.enable(tree.path() + "/cc", "g"));
  CompileCacheEntry entry;
  std::string digest = compileCacheWasmDigest(
      reinterpret_cast<const uint8_t *>(cfg.data()), cfg.size(),
      kAddWasm.data(), kAddWasm.size());
  ASSERT_FALSE(cache.lookupWasm(entry, digest, kAddWasm.data(), kAddWasm.size()));
  cache.saveWasm(entry, reinterpret_cast<const uint8_t *>(half.data()), half.size());

  WasmRecordWriter w(tree.path() + "/rec.bin", "test");
  WasmCacheContext ctx;
  ctx.cache = &cache;
  ctx.recorder = &w;
  installWasmCacheHooks(env_, &ctx, false);
  ASSERT_TRUE(compileWasm(kAddWasm));
  EXPECT_EQ(good, readRecord(tree.path() + "/rec.bin").bytecode);
}
```
Wrap all of the above in `#ifdef HERMES_ENABLE_WASM`.

- [ ] **Step 2: Confirm they fail**

```bash
cmake --build cmake-build-asan --target CompileCacheRunTest && \
  cmake-build-asan/unittests/CompileCacheRunTest --gtest_filter='*Record*:*Rejected*:*Committed*'
```
Expected: `RecordCommitsAtStore` and `RecordCommitsAtAccepted...` FAIL with `sourceCount` 0, because the runtime still uses the three-argument `record()`. `NothingCommittedWhenARejectedHitsRecompileFails` FAILs because today the disk hit is recorded at lookup, so `count` is 1. (`NothingCommittedWhenAMissFailsToCompile` and `ARejectedHitRecordsWhatWasRecompiled` may already pass. That is fine: they pin behaviour the rework must keep.)

- [ ] **Step 3: Move recording to the commit points**

In `wasm_cache_hooks.cpp`:

1. Add to `StoreToken`:

```cpp
  /// --record-wasm only. The module bytes are valid only during lookup, and
  /// a hit's bytecode may be unmapped by its finalizer before accepted runs
  /// (the order is finalizer, accepted, discard), so both are copied here,
  /// at lookup, and committed only once Hermes has taken the module: at
  /// store (a compile succeeded) or accepted (the returned bytecode was
  /// loaded). A bare discard commits nothing. Without a recorder nothing is
  /// copied, and a hit costs what it always did.
  bool recording = false;
  std::string sourceDigest; // sha256Digest(module bytes), raw
  std::string source;
  std::string hitBytecode;
```

2. Replace `recordWasm(token, event, bytes, size)` with `commitRecord(token, event, bytes, size)`. It has the same failure handling (`recordingFailed`, once per run) and returns immediately unless `token->recording`. It calls

```cpp
  WasmRecordCommit commit{
      token->digest.data(), bytes, size,
      reinterpret_cast<const uint8_t *>(token->sourceDigest.data()),
      reinterpret_cast<const uint8_t *>(token->source.data()),
      token->source.size()};
  if (ctx->recorder->record(commit))
    return;
```

3. In `cacheLookup`, after `token->digest = ...`:

```cpp
  if (cacheCtx->recorder != nullptr) {
    token->recording = true;
    token->sourceDigest = sha256Digest(wasm, wasmSize);
    token->source.assign(reinterpret_cast<const char *>(wasm), wasmSize);
  }
```
On the container hit, replace the `recordWasm(...)` call with `if (token->recording) token->hitBytecode.assign(reinterpret_cast<const char *>(baked), bakedSize);`. On the disk hit, likewise copy from `token->entry.bytecode`, before the mapping is handed off.

4. Add the callback:

```cpp
void cacheAccepted(void *ctx, void *storeToken) {
  (void)ctx;
  auto *token = static_cast<StoreToken *>(storeToken);
  if (token == nullptr || !token->recording)
    return;
  commitRecord(
      token,
      token->origin == StoreToken::Origin::kContainer ? "wasm record container"
                                                      : "wasm record disk",
      reinterpret_cast<const uint8_t *>(token->hitBytecode.data()),
      token->hitBytecode.size());
}
```

5. In `cacheStore`, replace `recordWasm(token, "wasm record compile", hbc, hbcSize)` with `commitRecord(...)`, same arguments.

6. In `installWasmCacheHooks`, add `callbacks.accepted = &cacheAccepted;`.

7. Update the comments that describe recording at lookup ("replacing whatever the lookup recorded" in `cacheStore`, and `recordWasm`'s doc) so they describe the commit points.

8. Delete the three-argument `WasmRecordWriter::record` from the header and the source. Move its remaining callers in `WasmRecordTest.cpp` to `WasmRecordCommit` (any digest and source will do). Keep the replace-on-same-digest test: it now covers the commit overload.

- [ ] **Step 4: Run the tests**

```bash
cmake --build cmake-build-asan --target CompileCacheRunTest WasmRecordTest && \
  cmake-build-asan/unittests/CompileCacheRunTest && cmake-build-asan/unittests/WasmRecordTest
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```
Expected: all pass. If a `test/test-wasm-record*.js` check fails on ordering, it is because `wasm record container|disk` now prints after Hermes accepts the bytecode, not right after the hit line. Relax only that check (`CHECK` rather than `CHECK-NEXT`) and say why in a comment.

- [ ] **Step 5: Commit**

```bash
./utils/format.sh -f
git add lib/compile-cache/wasm_cache_hooks.cpp include/hermes/node-compat/bundle/wasm_record.h \
  lib/bundle/wasm_record.cpp unittests/WasmRecordTest.cpp unittests/CompileCacheRunTest.cpp \
  unittests/CMakeLists.txt <any test/*.js you relaxed>
git commit -m "--record-wasm: record a module only once Hermes accepted it

The recorder used to record a cache hit at lookup, before Hermes had
decided whether it could load the bytes, so a hit Hermes rejected --
followed by a recompile that also failed -- left an entry for a module
the program never got. Recording now happens at store, when a compile
succeeded, or at Hermes's new accepted notification, when served
bytecode was loaded; a bare discard records nothing.

The module bytes are copied into the store token at lookup, and so is a
hit's bytecode: Hermes runs the hit's finalizer, which unmaps a disk
entry, before it reports acceptance. Every entry now carries its source
bytes too, in record format v2.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 9: The native tier at run time (hermes-node)

**Files:**
- Modify: `include/hermes/node-compat/compile-cache/wasm_cache_hooks.h`, `lib/compile-cache/wasm_cache_hooks.cpp`
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h:160-168`, `lib/runtime/hermes_node_runtime.cpp:~875`
- Modify: `include/hermes/node-compat/build-exe/build_exe.h:350-377`, `lib/build-exe/build_exe.cpp:429-505`
- Modify: `tools/hermes-node/bundle_main.cpp`
- Test: `unittests/BuildExeTest.cpp`, `unittests/CompileCacheRunTest.cpp`

**Interfaces:**
- Consumes: Hermes `lookup_native` (Task 3), `sha256Digest` (Task 7).
- Produces:
  ```cpp
  // wasm_cache_hooks.h
  struct NativeWasmUnitEntry { uint8_t digest[kNativeDigestBytes]; const void *creator; }; // 40 bytes
  const NativeWasmUnitEntry *findNativeWasmUnit(const NativeWasmUnitEntry *table, size_t count, const uint8_t *digest);
  // WasmCacheContext gains:
  const NativeWasmUnitEntry *nativeWasmUnits = nullptr; size_t nativeWasmUnitCount = 0;
  bool warnOnNativeFallback = false;
  std::unordered_map<std::string, bool> nativeAnswered; std::unordered_set<std::string> nativeWarned;
  // hermes_node_runtime.h, HermesNodeConfig gains:
  const void *nativeWasmUnits = nullptr; size_t nativeWasmUnitCount = 0; bool nativeWasmWarnOnFallback = false;
  // build_exe.h
  struct NativeWasmUnitSymbol { std::string digest; std::string unitName; };
  std::string payloadAssembly(const std::string &bundlePath,
      const std::vector<std::string> &unitSymbols, ObjectFormat format = hostObjectFormat(),
      const std::vector<NativeWasmUnitSymbol> &wasmUnits = {}, bool wasmWarnOnFallback = false);
  ```

- [ ] **Step 1: Failing payload tests**

In `unittests/BuildExeTest.cpp`, extend the exact-text expectation of `PayloadAssemblyElf`: insert these lines just before `"\t.section .note.GNU-stack,\"\",@progbits\n"`:

```cpp
      "\t.p2align 3\n"
      "\t.globl hermesNodeNativeWasmUnits\n"
      "hermesNodeNativeWasmUnits:\n"
      "\t.globl hermesNodeNativeWasmUnitCount\n"
      "hermesNodeNativeWasmUnitCount:\n"
      "\t.quad 0\n"
      "\t.globl hermesNodeNativeWasmPolicy\n"
      "hermesNodeNativeWasmPolicy:\n"
      "\t.quad 0\n"
```
Make the same insertion, with leading underscores, at the end of `PayloadAssemblyMachO`'s expectation. Then add:

```cpp
namespace {
NativeWasmUnitSymbol wasmUnit(uint8_t fill) {
  NativeWasmUnitSymbol u;
  u.digest = std::string(32, static_cast<char>(fill));
  u.unitName = "w" + std::string(64, fill == 1 ? '1' : '2');
  return u;
}
} // namespace

TEST(BuildExeTest, PayloadAssemblyWasmTableIsSortedByDigest) {
  std::string s = payloadAssembly(
      "/tmp/app.hbb", {}, ObjectFormat::ELF, {wasmUnit(2), wasmUnit(1)}, true);
  size_t first = s.find("sh_export_w" + std::string(64, '1'));
  size_t second = s.find("sh_export_w" + std::string(64, '2'));
  ASSERT_NE(std::string::npos, first);
  ASSERT_NE(std::string::npos, second);
  EXPECT_LT(first, second);
  // 32 digest bytes, then the 8-byte creator: one 40-byte entry.
  std::string ones = "\t.byte 1";
  for (int i = 1; i < 32; ++i)
    ones += ",1";
  EXPECT_NE(std::string::npos, s.find(ones + "\n\t.quad sh_export_w"));
  EXPECT_NE(
      std::string::npos,
      s.find("hermesNodeNativeWasmUnitCount:\n\t.quad 2\n"));
  EXPECT_NE(
      std::string::npos, s.find("hermesNodeNativeWasmPolicy:\n\t.quad 1\n"));
}

TEST(BuildExeTest, PayloadAssemblyWasmTableIsAlignedInRelRo) {
  std::string s = payloadAssembly(
      "/tmp/app.hbb", {}, ObjectFormat::ELF, {wasmUnit(1)}, false);
  size_t table = s.find("hermesNodeNativeWasmUnits:");
  ASSERT_NE(std::string::npos, table);
  size_t align = s.rfind(".p2align 3", table);
  size_t relro = s.rfind(".section .data.rel.ro", table);
  EXPECT_NE(std::string::npos, align);
  EXPECT_NE(std::string::npos, relro);
  EXPECT_LT(relro, align);
  // The GNU-stack note stays last.
  EXPECT_LT(table, s.find(".note.GNU-stack"));
}

TEST(BuildExeTest, PayloadAssemblyWasmTableUnderscoresForMachO) {
  std::string s = payloadAssembly(
      "/tmp/app.hbb", {}, ObjectFormat::MachO, {wasmUnit(1)}, false);
  EXPECT_NE(std::string::npos, s.find("_hermesNodeNativeWasmUnits:"));
  EXPECT_NE(std::string::npos, s.find(".quad _sh_export_w"));
  EXPECT_NE(std::string::npos, s.find("_hermesNodeNativeWasmPolicy:"));
}
```

- [ ] **Step 2: Failing hook tests**

In `CompileCacheRunTest.cpp`, inside `#ifdef HERMES_ENABLE_WASM`:

```cpp
namespace {
/// Offered by the native table but never registered with Hermes, so Hermes
/// refuses it and asks the bytecode tiers. If Hermes ever ran it, the test
/// process aborts.
SHUnit *unregisteredCreator() {
  ::abort();
}
} // namespace

TEST(NativeWasmTableTest, BinarySearchFindsEveryEntryAndNothingElse) {
  NativeWasmUnitEntry table[3] = {};
  for (int i = 0; i < 3; ++i) {
    std::memset(table[i].digest, 0x10 * (i + 1), kNativeDigestBytes);
    table[i].creator = &table[i];
  }
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(&table[i], findNativeWasmUnit(table, 3, table[i].digest));
  uint8_t absent[kNativeDigestBytes];
  std::memset(absent, 0x18, sizeof(absent));
  EXPECT_EQ(nullptr, findNativeWasmUnit(table, 3, absent));
  EXPECT_EQ(nullptr, findNativeWasmUnit(table, 0, table[0].digest));
}

TEST_F(CompileCacheRunTest, NativeFallbackWarnsOncePerModuleWithTheRightReason) {
  // kAddWasm is "in the table" with a creator Hermes refuses: the second
  // message. A module not in the table: the first. Each compiled twice and
  // each reported once.
  std::vector<uint8_t> other = kAddWasm;
  other[other.size() - 2] = 0x6b; // i32.add -> i32.sub: a different module
  NativeWasmUnitEntry table[1] = {};
  std::string d = sha256Digest(kAddWasm.data(), kAddWasm.size());
  std::memcpy(table[0].digest, d.data(), kNativeDigestBytes);
  table[0].creator = reinterpret_cast<const void *>(&unregisteredCreator);

  WasmCacheContext ctx;
  ctx.nativeWasmUnits = table;
  ctx.nativeWasmUnitCount = 1;
  ctx.warnOnNativeFallback = true;
  ctx.tracing = true;
  installWasmCacheHooks(env_, &ctx, false);
  testing::internal::CaptureStderr();
  EXPECT_TRUE(compileWasm(kAddWasm));
  EXPECT_TRUE(compileWasm(kAddWasm));
  EXPECT_TRUE(compileWasm(other));
  EXPECT_TRUE(compileWasm(other));
  std::string err = testing::internal::GetCapturedStderr();

  std::string addHex = nativeDigestToHex(d).substr(0, 16);
  std::string otherHex =
      nativeDigestToHex(sha256Digest(other.data(), other.size())).substr(0, 16);
  std::string unavailable = "warning: WebAssembly module " + addHex +
      ": its native unit is unavailable or incompatible with this run "
      "(e.g. -test262); falling back to bytecode\n";
  std::string notRecorded = "warning: WebAssembly module " + otherHex +
      " was not compiled natively (it is not in the recording); falling back "
      "to bytecode\n";
  auto count = [&](const std::string &needle) {
    size_t n = 0;
    for (size_t p = err.find(needle); p != std::string::npos;
         p = err.find(needle, p + 1))
      ++n;
    return n;
  };
  EXPECT_EQ(1u, count(unavailable)) << err;
  EXPECT_EQ(1u, count(notRecorded)) << err;
  EXPECT_EQ(2u, count("wasm native hit")) << err;
  EXPECT_EQ(2u, count("wasm native miss")) << err;
}

TEST_F(CompileCacheRunTest, NoNativeTierNoWarning) {
  WasmCacheContext ctx; // empty table, policy 0: lookup_native not installed
  ctx.tracing = true;
  installWasmCacheHooks(env_, &ctx, /*bundleMode*/ true);
  testing::internal::CaptureStderr();
  EXPECT_TRUE(compileWasm(kAddWasm));
  std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(std::string::npos, err.find("warning:"));
  EXPECT_EQ(std::string::npos, err.find("wasm native"));
}
```
Include `hermes/VM/static_h.h` only if `SHUnit` is not already visible through `napi/hermes_napi.h`, which after Task 3 declares it.

- [ ] **Step 3: Confirm they fail**

```bash
cmake --build cmake-build-asan --target BuildExeTest CompileCacheRunTest 2>&1 | grep -m3 error
```
Expected: compile errors (`NativeWasmUnitSymbol`, `NativeWasmUnitEntry`, `findNativeWasmUnit` and the new context fields do not exist).

- [ ] **Step 4: Emit the table**

In `build_exe.h`, before `payloadAssembly`:

```cpp
/// One natively compiled WebAssembly module, as the payload's Wasm table
/// names it.
struct NativeWasmUnitSymbol {
  /// kNativeDigestBytes raw bytes: SHA-256 over the module bytes alone.
  std::string digest;
  /// "w" + 64 hex digits; the unit's creator is sh_export_<unitName>.
  std::string unitName;
};
```
Add the two defaulted parameters to the declaration, and extend its doc comment with what the Wasm table is (40-byte entries, sorted, three symbols always present, the policy word).

In `build_exe.cpp`, inside `payloadAssembly` after the `hermesNodeNativeUnitCount` lines and before the GNU-stack note:

```cpp
  // The native WebAssembly table: per natively compiled module, its 32-byte
  // SHA-256 then the address of its creator -- a 40-byte entry -- sorted by
  // digest so the run time can binary-search it with memcmp. Same section as
  // the unit table and for the same reason (an address is a load-time
  // relocation); .p2align 3 because each entry's pointer must be 8-aligned,
  // which 32 + 8 keeps true for every entry after the first.
  //
  // All three symbols are emitted unconditionally, empty for a bytecode
  // --build-exe, so one bundle_main.cpp links against a single definition
  // whichever producer generated this file. The policy word says whether a
  // module the table cannot serve is reported on stderr.
  std::vector<const NativeWasmUnitSymbol *> wasmSorted;
  for (const NativeWasmUnitSymbol &u : wasmUnits)
    wasmSorted.push_back(&u);
  std::sort(wasmSorted.begin(), wasmSorted.end(), [](auto *a, auto *b) {
    return a->digest < b->digest;
  });
  os << "\t.p2align 3\n"
     << "\t.globl " << prefix << "hermesNodeNativeWasmUnits\n"
     << prefix << "hermesNodeNativeWasmUnits:\n";
  for (const NativeWasmUnitSymbol *u : wasmSorted) {
    assert(u->digest.size() == 32 && "a Wasm table digest is a raw SHA-256");
    os << "\t.byte ";
    for (size_t i = 0; i < u->digest.size(); ++i)
      os << (i ? "," : "") << unsigned(static_cast<uint8_t>(u->digest[i]));
    os << "\n\t.quad " << prefix << "sh_export_" << u->unitName << "\n";
  }
  os << "\t.globl " << prefix << "hermesNodeNativeWasmUnitCount\n"
     << prefix << "hermesNodeNativeWasmUnitCount:\n"
     << "\t.quad " << wasmSorted.size() << "\n"
     << "\t.globl " << prefix << "hermesNodeNativeWasmPolicy\n"
     << prefix << "hermesNodeNativeWasmPolicy:\n"
     << "\t.quad " << (wasmWarnOnFallback ? 1 : 0) << "\n";
```
Add `#include <algorithm>` and `#include <cassert>` if they are missing. `std::string`'s `operator<` compares as `char`, which may be signed. Sort with `std::lexicographical_compare` over `unsigned char` (or `memcmp`) so the order matches the run time's `memcmp`:

```cpp
  std::sort(wasmSorted.begin(), wasmSorted.end(), [](auto *a, auto *b) {
    return std::memcmp(a->digest.data(), b->digest.data(), 32) < 0;
  });
```

- [ ] **Step 5: The hook**

In `wasm_cache_hooks.h`:

```cpp
/// One entry of the payload's native WebAssembly table (payloadAssembly()):
/// SHA-256 over a module's bytes, and the creator of the unit compiled from
/// them. 40 bytes, sorted by digest. The creator is opaque here, as it is in
/// HermesNodeConfig, so this header stays free of Hermes VM headers.
struct NativeWasmUnitEntry {
  uint8_t digest[kNativeDigestBytes];
  const void *creator;
};
static_assert(
    sizeof(NativeWasmUnitEntry) == 40,
    "payloadAssembly() emits 32 digest bytes then an 8-byte .quad");

/// The entry of \p table (\p count entries, sorted by digest) whose digest is
/// \p digest (kNativeDigestBytes raw bytes), or null.
const NativeWasmUnitEntry *findNativeWasmUnit(
    const NativeWasmUnitEntry *table,
    size_t count,
    const uint8_t *digest);
```
Add to `WasmCacheContext`, with comments:

```cpp
  /// The native WebAssembly table a build-native artifact links, or null.
  const NativeWasmUnitEntry *nativeWasmUnits = nullptr;
  size_t nativeWasmUnitCount = 0;

  /// Report on stderr, once per module, a module the native tier did not
  /// serve. The artifact's policy word, not a run-time choice.
  bool warnOnNativeFallback = false;

  /// Per raw sha256Digest() of a module: whether the native tier offered a
  /// creator for it. Read by the bytecode-tier lookup that follows a native
  /// miss, to tell "not in the table" from "offered but refused by Hermes".
  /// One runtime, one thread: no lock.
  std::unordered_map<std::string, bool> nativeAnswered;
  /// Modules already warned about.
  std::unordered_set<std::string> nativeWarned;
```
Add `<string>`, `<unordered_map>` and `<unordered_set>`.

In `wasm_cache_hooks.cpp`:

```cpp
const NativeWasmUnitEntry *findNativeWasmUnit(
    const NativeWasmUnitEntry *table,
    size_t count,
    const uint8_t *digest) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = std::memcmp(table[mid].digest, digest, kNativeDigestBytes);
    if (c == 0)
      return &table[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return nullptr;
}
```
Define it once, before the file's `#ifdef HERMES_ENABLE_WASM` line, in its own `namespace hermes { namespace node_compat {` block with `#include <cstring>`: it uses nothing of Hermes's, and a build without Wasm still links it. Then, inside the anonymous namespace of the Wasm branch:

```cpp
SHUnitCreator cacheLookupNative(void *ctx, const uint8_t *wasm, size_t size) {
  auto *c = static_cast<WasmCacheContext *>(ctx);
  std::string digest = sha256Digest(wasm, size);
  const NativeWasmUnitEntry *e = findNativeWasmUnit(
      c->nativeWasmUnits,
      c->nativeWasmUnitCount,
      reinterpret_cast<const uint8_t *>(digest.data()));
  c->nativeAnswered[digest] = e != nullptr;
  // "hit" means the table offered a unit. Hermes may still refuse it, in
  // which case the bytecode lookup that follows says so.
  wasmTrace(
      c,
      e ? "wasm native hit" : "wasm native miss",
      reinterpret_cast<const uint8_t *>(digest.data()));
  return e ? reinterpret_cast<SHUnitCreator>(const_cast<void *>(e->creator))
           : nullptr;
}

/// Called at the top of the bytecode-tier lookup, before anything there can
/// fail: reaching that lookup means the native tier did not serve this
/// module.
void warnNativeFallback(
    WasmCacheContext *c,
    const uint8_t *wasm,
    size_t size) {
  if (!c->warnOnNativeFallback)
    return;
  std::string digest = sha256Digest(wasm, size);
  if (!c->nativeWarned.insert(digest).second)
    return;
  auto it = c->nativeAnswered.find(digest);
  bool offered = it != c->nativeAnswered.end() && it->second;
  std::string hex = nativeDigestToHex(digest).substr(0, 16);
  if (offered)
    std::fprintf(
        stderr,
        "warning: WebAssembly module %s: its native unit is unavailable or "
        "incompatible with this run (e.g. -test262); falling back to "
        "bytecode\n",
        hex.c_str());
  else
    std::fprintf(
        stderr,
        "warning: WebAssembly module %s was not compiled natively (it is not "
        "in the recording); falling back to bytecode\n",
        hex.c_str());
}
```
At the very top of `cacheLookup`, before the `new (std::nothrow) StoreToken`, add `warnNativeFallback(static_cast<WasmCacheContext *>(ctx), wasm, wasmSize);`. Reuse its digest for `token->sourceDigest` only if that stays simple. Hashing twice when both warning and recording are on is acceptable; they never coincide in a real run, because a produced executable cannot record.

In `installWasmCacheHooks`:

```cpp
  const bool nativeTier =
      ctx->nativeWasmUnitCount != 0 || ctx->warnOnNativeFallback;
  if (!cacheCanAnswer && ctx->recorder == nullptr && !bundleMode && !nativeTier)
    return;
  ...
  // Only an artifact with a table or a warning policy pays the per-compile
  // SHA-256 the native lookup costs.
  callbacks.lookup_native = nativeTier ? &cacheLookupNative : nullptr;
```

- [ ] **Step 6: Wire the config through**

`hermes_node_runtime.h`, after `nativeUnitCount`:

```cpp
  /// The native WebAssembly table the payload object defines (an array of
  /// NativeWasmUnitEntry, see wasm_cache_hooks.h), its entry count, and its
  /// policy word. Null/0/false everywhere but a build-native artifact. Opaque
  /// for the reason nativeUnits is.
  const void *nativeWasmUnits = nullptr;
  size_t nativeWasmUnitCount = 0;
  bool nativeWasmWarnOnFallback = false;
```
`hermes_node_runtime.cpp`, just before `installWasmCacheHooks(`:

```cpp
  wasmCacheContext.nativeWasmUnits =
      static_cast<const NativeWasmUnitEntry *>(config.nativeWasmUnits);
  wasmCacheContext.nativeWasmUnitCount = config.nativeWasmUnitCount;
  wasmCacheContext.warnOnNativeFallback = config.nativeWasmWarnOnFallback;
```
`bundle_main.cpp`, in the `extern "C"` block:

```cpp
/// The native WebAssembly table, likewise emitted by both producers (empty,
/// with a zero policy, for a bytecode --build-exe). 40-byte entries; see
/// NativeWasmUnitEntry.
extern const uint8_t hermesNodeNativeWasmUnits[];
extern const uint64_t hermesNodeNativeWasmUnitCount;
extern const uint64_t hermesNodeNativeWasmPolicy;
```
and in `main` after `nativeUnitCount`:

```cpp
  config.nativeWasmUnits = hermesNodeNativeWasmUnits;
  config.nativeWasmUnitCount = static_cast<size_t>(hermesNodeNativeWasmUnitCount);
  config.nativeWasmWarnOnFallback = hermesNodeNativeWasmPolicy != 0;
```

- [ ] **Step 7: Run everything**

```bash
cmake --build cmake-build-asan --target BuildExeTest CompileCacheRunTest && \
  cmake-build-asan/unittests/BuildExeTest && cmake-build-asan/unittests/CompileCacheRunTest
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```
Expected: all pass. The `build-exe*` and `build-native*` lit tests prove the three new symbols link in both producers, since `check-hermes-node-js` re-cuts the kit.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/compile-cache/wasm_cache_hooks.h \
  lib/compile-cache/wasm_cache_hooks.cpp include/hermes/node-compat/runtime/hermes_node_runtime.h \
  lib/runtime/hermes_node_runtime.cpp include/hermes/node-compat/build-exe/build_exe.h \
  lib/build-exe/build_exe.cpp tools/hermes-node/bundle_main.cpp \
  unittests/BuildExeTest.cpp unittests/CompileCacheRunTest.cpp
git commit -m "Answer Hermes's native Wasm lookup from a linked table

The payload object gains a table of natively compiled WebAssembly
units -- SHA-256 of the module bytes, then the unit's creator, sorted
by digest -- with a count and a policy word. The runtime answers
Hermes's lookup_native with a binary search over it, before the
container, disk and compile tiers.

When the policy is set, a module the native tier did not serve is
reported on stderr once, saying whether it was missing from the table
or offered and refused (a different -test262, for instance). Every
producer emits the three symbols, empty for --build-exe, so one
bundle_main.cpp links against both. Nothing fills the table yet.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 10: build-native toolchain pieces (VM-free) and the effective `-test262`

**Files:**
- Modify: `include/hermes/node-compat/build-native/build_native.h`
- Modify: `lib/build-native/staging.cpp`, `lib/build-native/native_compile.cpp`, `lib/build-native/job_pool.cpp`, `lib/build-native/unit_table.cpp`
- Modify: `include/hermes/node-compat/vm-options/vm_options.h`, `lib/vm-options/vm_options.cpp`
- Test: `unittests/BuildNativeTest.cpp`, `unittests/VmOptionsTest.cpp`

**Interfaces:**
- Consumes: `NativeWasmUnitSymbol` and the extended `payloadAssembly` (Task 9).
- Produces:
  ```cpp
  std::string nativeWasmUnitName(std::string_view rawDigest);   // "w" + 64 lowercase hex
  std::string stagedWasmPath(const std::string &tempDir, const std::string &unitName);   // <dir>/<unit>.wasm
  std::string stagedWasmCPath(const std::string &tempDir, const std::string &unitName);  // <dir>/<unit>.c
  std::string stagedWasmObjectPath(const std::string &tempDir, const std::string &unitName); // <dir>/<unit>.o
  std::vector<std::string> buildShermesWasmCommand(const std::string &shermesPath,
      const std::string &wasmPath, const std::string &outCPath, const std::string &unitName,
      bool test262, OptLevel opt);
  struct NativeWasmJob { std::string digest; std::string bytes; };  // digest: raw sha256Digest(bytes)
  struct NativeBatchResults { std::vector<NativeModuleResult> modules; std::vector<NativeModuleResult> wasm; };
  NativeBatchResults compileNativeBatch(const std::vector<NativeModuleJob> &modules,
      const std::vector<NativeWasmJob> &wasm, const std::string &tempDir,
      const std::string &shermesPath, const KitManifest &manifest, const std::string &driver,
      bool driverIsClang, OptLevel opt, bool wasmTest262, unsigned parallelism,
      const CommandRunner &run);
  std::vector<NativeWasmUnitSymbol> nativeWasmUnitSymbols(const std::vector<NativeWasmJob> &wasm);
  std::string nativePayloadAssembly(const std::string &containerPath,
      const std::vector<std::string> &unitSymbols,
      const std::vector<NativeWasmUnitSymbol> &wasmUnits, bool wasmWarnOnFallback);
  bool effectiveVmTest262(const std::vector<std::string> &options, bool *test262, std::string *error);
  ```
  `compileModules(...)` keeps its signature and returns `compileNativeBatch(jobs, {}, ..., false, ...).modules`.

- [ ] **Step 1: Failing tests**

`unittests/VmOptionsTest.cpp`:

```cpp
TEST(VmOptionsTest, EffectiveTest262FollowsTheBakedList) {
  bool t = true;
  std::string err;
  ASSERT_TRUE(effectiveVmTest262({}, &t, &err)) << err;
  EXPECT_FALSE(t);
  ASSERT_TRUE(effectiveVmTest262({"-test262"}, &t, &err)) << err;
  EXPECT_TRUE(t);
  ASSERT_TRUE(effectiveVmTest262({"-test262=false"}, &t, &err)) << err;
  EXPECT_FALSE(t);
  // Last occurrence wins, through the same dedupe a run uses.
  ASSERT_TRUE(effectiveVmTest262({"-test262", "-test262=false"}, &t, &err)) << err;
  EXPECT_FALSE(t);
  ASSERT_TRUE(effectiveVmTest262({"-test262=false", "-test262"}, &t, &err)) << err;
  EXPECT_TRUE(t);
  // An unrelated option is still parsed and still has to be valid.
  EXPECT_FALSE(effectiveVmTest262({"-no-such-flag"}, &t, &err));
  EXPECT_NE(std::string::npos, err.find("no-such-flag"));
}
```

`unittests/BuildNativeTest.cpp`:

```cpp
TEST(BuildNativeTest, WasmUnitNameIsWAndTheLowercaseHexDigest) {
  std::string raw;
  for (int i = 0; i < 32; ++i)
    raw.push_back(static_cast<char>(i == 0 ? 0xab : i));
  std::string name = nativeWasmUnitName(raw);
  ASSERT_EQ(65u, name.size());
  EXPECT_EQ("wab01", name.substr(0, 5));
  EXPECT_EQ(std::string::npos, name.find_first_not_of("w0123456789abcdef"));
}

TEST(BuildNativeTest, ShermesWasmCommandShape) {
  auto argv = buildShermesWasmCommand(
      "/kit/shermes", "/t/wX.wasm", "/t/wX.c", "wX", /*test262=*/false, OptLevel::O3);
  EXPECT_EQ("/kit/shermes", argv[0]);
  EXPECT_TRUE(has(argv, "-emit-c"));
  EXPECT_TRUE(has(argv, "-O"));
  EXPECT_TRUE(has(argv, "-exported-unit=wX"));
  EXPECT_FALSE(has(argv, "-test262"));
  // A Wasm module has no JavaScript: none of the JS parity flags, and no
  // -source-name.
  EXPECT_FALSE(has(argv, "-Xes6-block-scoping"));
  for (const std::string &a : argv)
    EXPECT_NE(0u, a.rfind("-source-name", 0)) << a;
  ASSERT_GE(argv.size(), 3u);
  EXPECT_EQ("-o", argv[argv.size() - 3]);
  EXPECT_EQ("/t/wX.c", argv[argv.size() - 2]);
  EXPECT_EQ("/t/wX.wasm", argv.back());

  auto t262 = buildShermesWasmCommand(
      "/kit/shermes", "/t/wX.wasm", "/t/wX.c", "wX", /*test262=*/true, OptLevel::O0);
  EXPECT_TRUE(has(t262, "-test262"));
  EXPECT_TRUE(has(t262, "-O0"));
}

TEST(BuildNativeTest, CompileNativeBatchRunsBothKindsInOnePool) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  std::vector<NativeWasmJob> wasm = {
      {std::string(32, '\x01'), "WASM-ONE"}, {std::string(32, '\x02'), "WASM-TWO"}};
  NativeBatchResults r = compileNativeBatch(
      {threeJobs()[0]}, wasm, dir, "/kit/shermes", m, "cc", true, OptLevel::O3,
      /*wasmTest262=*/true, 4, std::ref(fake));
  ASSERT_EQ(1u, r.modules.size());
  ASSERT_EQ(2u, r.wasm.size());
  EXPECT_TRUE(r.modules[0].ok) << r.modules[0].message;
  EXPECT_TRUE(r.wasm[0].ok) << r.wasm[0].message;
  EXPECT_TRUE(r.wasm[1].ok) << r.wasm[1].message;
  EXPECT_EQ(6u, fake.calls.size()); // shermes + cc for each of three units
  // The staged module is the bytes, verbatim.
  std::ifstream in(stagedWasmPath(dir, nativeWasmUnitName(wasm[1].digest)), std::ios::binary);
  std::ostringstream body;
  body << in.rdbuf();
  EXPECT_EQ("WASM-TWO", body.str());
  // -test262 reaches the Wasm units and only them.
  size_t t262 = 0;
  for (const auto &argv : fake.calls)
    if (has(argv, "-test262")) {
      ++t262;
      EXPECT_NE(std::string::npos, argv.back().find(".wasm"));
    }
  EXPECT_EQ(2u, t262);
}

TEST(BuildNativeTest, CompileNativeBatchReportsAWasmShermesFailure) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &argv) {
    CommandResult r;
    if (argv[0].find("shermes") != std::string::npos &&
        argv.back().find(".wasm") != std::string::npos) {
      r.outcome = CommandResult::Outcome::Exited;
      r.status = 1;
      r.output = "Error: unsupported\n";
    }
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  NativeBatchResults r = compileNativeBatch(
      {}, {{std::string(32, '\x03'), "X"}}, dir, "/kit/shermes", m, "cc", true,
      OptLevel::O3, false, 1, std::ref(fake));
  ASSERT_EQ(1u, r.wasm.size());
  EXPECT_FALSE(r.wasm[0].ok);
  EXPECT_EQ(NativeStage::Shermes, r.wasm[0].failedStage);
  EXPECT_NE(std::string::npos, r.wasm[0].diagnostics.find("unsupported"));
  EXPECT_EQ(1u, fake.calls.size()) << "cc must not run after shermes failed";
}

TEST(BuildNativeTest, NativeWasmUnitSymbolsCarryDigestAndName) {
  std::vector<NativeWasmJob> wasm = {{std::string(32, '\x07'), "X"}};
  auto syms = nativeWasmUnitSymbols(wasm);
  ASSERT_EQ(1u, syms.size());
  EXPECT_EQ(wasm[0].digest, syms[0].digest);
  EXPECT_EQ(nativeWasmUnitName(wasm[0].digest), syms[0].unitName);
}
```

```bash
cmake --build cmake-build-asan --target BuildNativeTest VmOptionsTest 2>&1 | grep -m3 error
```
Expected: compile errors for every name above.

- [ ] **Step 2: `effectiveVmTest262`**

`vm_options.h`:

```cpp
/// The -test262 setting a runtime built from \p options ALONE would have,
/// through the same classification, dedupe and last-occurrence-wins parse a
/// run uses (buildVmRuntimeConfig). What build-native compiles WebAssembly
/// units under, so that an artifact's baked options and its units agree.
/// The environment is deliberately not consulted. Parsing the whole list
/// means an unrelated invalid option fails here too, with buildVmRuntime-
/// Config's message in \p error.
bool effectiveVmTest262(
    const std::vector<std::string> &options,
    bool *test262,
    std::string *error);
```
`vm_options.cpp`:

```cpp
bool effectiveVmTest262(
    const std::vector<std::string> &options,
    bool *test262,
    std::string *error) {
  hermes::vm::RuntimeConfig config;
  if (!buildVmRuntimeConfig(options, &config, error))
    return false;
  *test262 = config.getTest262();
  return true;
}
```

- [ ] **Step 3: Names, paths, the shermes argv**

In `staging.cpp`:

```cpp
std::string nativeWasmUnitName(std::string_view rawDigest) {
  static const char kHex[] = "0123456789abcdef";
  std::string name = "w";
  for (unsigned char c : rawDigest) {
    name.push_back(kHex[c >> 4]);
    name.push_back(kHex[c & 15]);
  }
  return name;
}

std::string stagedWasmPath(const std::string &tempDir, const std::string &unit) {
  return tempDir + "/" + unit + ".wasm";
}
std::string stagedWasmCPath(const std::string &tempDir, const std::string &unit) {
  return tempDir + "/" + unit + ".c";
}
std::string stagedWasmObjectPath(const std::string &tempDir, const std::string &unit) {
  return tempDir + "/" + unit + ".o";
}
```
(JavaScript modules stage as `<6+ digits>.js/.c/.o`, so a `w`-prefixed name cannot collide with one.)

In `native_compile.cpp`:

```cpp
std::vector<std::string> buildShermesWasmCommand(
    const std::string &shermesPath,
    const std::string &wasmPath,
    const std::string &outCPath,
    const std::string &unitName,
    bool test262,
    OptLevel opt) {
  // None of the JavaScript flags: a Wasm module has no source text, no
  // block scoping to get wrong and no source map comment. shermes takes
  // .wasm input by extension; one built without Wasm refuses it, and that
  // failure is reported as a build error naming --bytecode-wasm.
  std::vector<std::string> argv{
      shermesPath, "-emit-c", shermesOptFlag(opt), "-exported-unit=" + unitName};
  // The unit's codegen configuration records this, and a runtime runs the
  // unit only under the same setting -- see the effective baked value in
  // effectiveVmTest262().
  if (test262)
    argv.push_back("-test262");
  argv.push_back("-o");
  argv.push_back(outCPath);
  argv.push_back(wasmPath);
  return argv;
}
```

Add the declarations above to `build_native.h`, each with a doc comment, near their JavaScript counterparts.

- [ ] **Step 4: One pool for both kinds**

In `job_pool.cpp`, add `runOneWasm` beside `runOne`. It has the same shape: stage, then shermes, then check the C file is non-empty, then cc via `buildCompileCommand`, then check the object is non-empty. Staging writes `job.bytes` to `stagedWasmPath` in binary mode and reports `NativeStage::Stage` with "cannot write <path>" on failure. Use `describeCommandResult(sh, "shermes")` and `describeCommandResult(cc, "the C compiler")` exactly as `runOne` does. `result.sourceBytes = job.bytes.size()`.

Then generalize the pool:

```cpp
NativeBatchResults compileNativeBatch(
    const std::vector<NativeModuleJob> &modules,
    const std::vector<NativeWasmJob> &wasm,
    const std::string &tempDir,
    const std::string &shermesPath,
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    OptLevel opt,
    bool wasmTest262,
    unsigned parallelism,
    const CommandRunner &run) {
  NativeBatchResults out;
  out.modules.resize(modules.size());
  out.wasm.resize(wasm.size());
  const size_t total = modules.size() + wasm.size();
  if (parallelism == 0)
    parallelism = 1;
  unsigned workers =
      (unsigned)std::min<size_t>(parallelism, std::max<size_t>(total, 1));

  // One index space over both lists, so --jobs bounds the two kinds
  // together; results are still written by index, in job order.
  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= total)
        return;
      if (i < modules.size())
        out.modules[i] = runOne(
            modules[i], tempDir, shermesPath, manifest, driver, driverIsClang,
            opt, run);
      else
        out.wasm[i - modules.size()] = runOneWasm(
            wasm[i - modules.size()], tempDir, shermesPath, manifest, driver,
            driverIsClang, opt, wasmTest262, run);
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
  return out;
}

std::vector<NativeModuleResult> compileModules(/* unchanged parameters */) {
  return compileNativeBatch(
             jobs, {}, tempDir, shermesPath, manifest, driver, driverIsClang,
             opt, /*wasmTest262=*/false, parallelism, run)
      .modules;
}
```

- [ ] **Step 5: Symbols and the payload**

In `unit_table.cpp`:

```cpp
std::vector<NativeWasmUnitSymbol> nativeWasmUnitSymbols(
    const std::vector<NativeWasmJob> &wasm) {
  std::vector<NativeWasmUnitSymbol> out;
  out.reserve(wasm.size());
  for (const NativeWasmJob &job : wasm)
    out.push_back({job.digest, nativeWasmUnitName(job.digest)});
  return out;
}

std::string nativePayloadAssembly(
    const std::string &containerPath,
    const std::vector<std::string> &unitSymbols,
    const std::vector<NativeWasmUnitSymbol> &wasmUnits,
    bool wasmWarnOnFallback) {
  return payloadAssembly(
      containerPath, unitSymbols, hostObjectFormat(), wasmUnits, wasmWarnOnFallback);
}
```
Update `nativePayloadAssembly`'s declaration and doc comment. Update its one caller in `bundle_build_native.cpp` to pass `{}` and `false` for now; Task 11 passes the real values.

- [ ] **Step 6: Run the tests**

```bash
cmake --build cmake-build-asan --target BuildNativeTest VmOptionsTest && \
  cmake-build-asan/unittests/BuildNativeTest && cmake-build-asan/unittests/VmOptionsTest
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/build-native/build_native.h lib/build-native/staging.cpp \
  lib/build-native/native_compile.cpp lib/build-native/job_pool.cpp lib/build-native/unit_table.cpp \
  include/hermes/node-compat/vm-options/vm_options.h lib/vm-options/vm_options.cpp \
  lib/bundle/bundle_build_native.cpp unittests/BuildNativeTest.cpp unittests/VmOptionsTest.cpp
git commit -m "build-native: compile WebAssembly units in the same job pool

The pieces a native Wasm build needs, unit-tested with no toolchain: a
unit is named w plus the SHA-256 of its module bytes, staged as
<unit>.wasm, turned into C by shermes -emit-c --exported-unit (with
-test262 when asked) and compiled by the same cc command a JavaScript
module gets. compileNativeBatch runs JavaScript and Wasm jobs in one
pool, so --jobs bounds both; compileModules is now a wrapper over it.

effectiveVmTest262 reports the -test262 a runtime built from a baked
--vm= list alone would have, through the same parse a run uses. Nothing
calls the new code yet.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 11: build-native compiles recorded modules natively

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_build.h` (`NativeBuildOptions`)
- Modify: `lib/bundle/bundle_build_internal.h` (`WasmBakeMode`, `PendingWasmSource`, `BuildProducts::wasmSources`, `buildBundleImpl` parameter)
- Modify: `lib/bundle/bundle_build.cpp:~744-760` (forwarder) and `:1788-1849` (bake loop)
- Modify: `lib/bundle/bundle_build_native.cpp`
- Modify: `tools/hermes-node/hermes-node.cpp:945-1150` (help, `--bytecode-wasm`, effective `-test262`)
- Create: `test/fixtures/wasm/make-v1-record.py`, `test/build-native-wasm-native.js`
- Modify: `test/build-native-errors.js`, `test/build-native-wasm.js`

**Interfaces:**
- Consumes: Tasks 7, 9 and 10.
- Produces:
  - `NativeBuildOptions::bytecodeWasm` (bool, default false) and `NativeBuildOptions::wasmTest262` (bool, set by the CLI).
  - `enum class WasmBakeMode { Bytecode, NativeSources };`
  - `struct PendingWasmSource { std::string digest; std::string bytes; };`
  - `BuildProducts::wasmSources` (`std::vector<PendingWasmSource>`).
  - `buildBundleImpl(..., PayloadMode mode, WasmBakeMode wasmMode, BuildProducts *products)`.
  - `build-native --bytecode-wasm`.

- [ ] **Step 1: The v1 fixture generator**

Create `test/fixtures/wasm/make-v1-record.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Write an empty format-v1 --record-wasm file to argv[1].

What every hermes-node before record format v2 wrote, and what the current
one no longer can: v1 carries bytecode only, with no module bytes for a
native build to compile. Laid out exactly as the v1 writer did
(lib/bundle/wasm_record.cpp at format v1): a 36-byte header, the build
version string, the record table at a 4-byte boundary, the payload at an
8-byte one.
"""

import struct
import sys

VERSION = b"v1-fixture"
HEADER = 36
table = (HEADER + len(VERSION) + 3) & ~3
payload = (table + 7) & ~7
data = b"HNWASMRC" + struct.pack(
    "<7I", 1, 0, HEADER, len(VERSION), table, payload, 0)
data += VERSION
data += b"\0" * (payload - len(data))
with open(sys.argv[1], "wb") as f:
    f.write(data)
```

- [ ] **Step 2: Failing error tests (no toolchain)**

Append to `test/build-native-errors.js`, before its `--help` case. `--kit=%t/nokit` makes the ordering observable: had the refusal come later, the message would be the missing kit's.

```
// A v1 recording carries no module bytes, so native mode refuses it before
// anything else -- before the kit is even read.
// RUN: python3 %source_dir/test/fixtures/wasm/make-v1-record.py %t/v1.rec
// RUN: %not %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/v1.rec --kit=%t/nokit 2>&1 | %FileCheck --check-prefix=V1REC %s
// V1REC: v1.rec
// V1REC-SAME: format v1
// V1REC: --bytecode-wasm
// V1REC-NOT: kit.manifest

// Baked --vm= options are parsed at build time now, the whole list, before
// the kit is read.
// RUN: %not %hermes-node build-native %t/src/app.js -o %t/app --vm=-no-such-flag --kit=%t/nokit 2>&1 | %FileCheck --check-prefix=BADVM %s
// BADVM: no-such-flag
// BADVM-NOT: kit.manifest
```
Add a help check: `// HELP: --bytecode-wasm`.

```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/build-native-errors.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test
```
Expected: FAIL. The v1 case reaches the kit check and prints `kit.manifest`, and the bad `--vm=` likewise.

- [ ] **Step 3: Options and the CLI**

`NativeBuildOptions` gains:

```cpp
  /// Bake each recorded module's bytecode into the container, as
  /// --build-bundle --bake-wasm does, instead of compiling it natively; also
  /// turns the run-time fallback warning off. The lever that separates a
  /// fault in native Wasm codegen from anything else.
  bool bytecodeWasm = false;
  /// The -test262 a runtime built from vmOptions alone would have
  /// (effectiveVmTest262), which every native Wasm unit is compiled under.
  /// Filled in by the CLI, which links the VM-options parser; this library
  /// stays VM-free.
  bool wasmTest262 = false;
```
In `runBuildNativeSubcommand`, add the flag beside `--bytecode-builtins`:

```cpp
    } else if (std::strcmp(arg, "--bytecode-wasm") == 0) {
      options.bytecodeWasm = true;
```
and the usage line:

```
"  --bytecode-wasm           Bake recorded WebAssembly as bytecode instead\n"
"                            of compiling it natively\n"
```
After the `haveOut` check and before `realpath`:

```cpp
  // Parsed here, not in the producer: the parser is the runtime's own
  // (llvh::cl, a process-global registry), which the producer library does
  // not link, and it must run before the parallel compile starts. The whole
  // list is parsed, so an invalid baked option is now a build error rather
  // than a failure at the artifact's first launch.
  std::string vmError;
  if (!hermes::node_compat::effectiveVmTest262(
          options.vmOptions, &options.wasmTest262, &vmError)) {
    std::fprintf(stderr, "Error: %s\n", vmError.c_str());
    return 1;
  }
```
Before writing that `fprintf`, check how the run path reports a `buildVmRuntimeConfig` failure (`grep -n 'buildVmRuntimeConfig(' lib/runtime/hermes_node_runtime.cpp`) and match its prefix exactly. Add `#include <hermes/node-compat/vm-options/vm_options.h>` if `hermes-node.cpp` lacks it.

- [ ] **Step 4: The bake step takes a mode**

In `bundle_build_internal.h`:

```cpp
/// What --bake-wasm does with a recording.
enum class WasmBakeMode {
  /// Copy each recorded module's bytecode into the container's Wasm table.
  Bytecode,
  /// Add no bytecode -- a native hit never reads it -- and return each
  /// distinct module's bytes in BuildProducts::wasmSources, for the native
  /// producer to compile. Requires format v2 recordings.
  NativeSources,
};

/// One distinct WebAssembly module a native build still has to compile.
/// Owned copies: the recording's mapping is closed before the bake step
/// returns.
struct PendingWasmSource {
  std::string digest; // kNativeDigestBytes raw: SHA-256 of `bytes`
  std::string bytes;
};
```
Add `std::vector<PendingWasmSource> wasmSources;` to `BuildProducts`, with a doc comment ("Empty unless the bake step ran in NativeSources mode; de-duplicated across every --bake-wasm file"). Add the parameter `WasmBakeMode wasmMode` to `buildBundleImpl` after `PayloadMode mode`, and document it. `buildBundle`'s forwarder passes `WasmBakeMode::Bytecode`.

In the bake loop (`bundle_build.cpp`), after the build-version check and before the `record->count() == 0` warning:

```cpp
    if (wasmMode == WasmBakeMode::NativeSources) {
      // buildNativeExecutable refused a v1 file before the kit was read;
      // this keeps the step correct on its own.
      if (record->formatVersion() < 2) {
        std::fprintf(
            stderr,
            "error: %s is a format v1 WebAssembly recording, which carries "
            "no module bytes to compile natively\n",
            recordPath.c_str());
        return 1;
      }
      if (record->sourceCount() == 0) {
        std::fprintf(
            stderr,
            "warning: %s records no WebAssembly modules\n",
            recordPath.c_str());
        continue;
      }
      for (uint32_t i = 0; i < record->sourceCount(); ++i) {
        std::string_view digest = record->sourceDigest(i);
        if (!bakedWasmDigests.insert(std::string(digest)).second) {
          reporter.wasmSkippedDuplicate(
              nativeDigestToHex(digest).substr(0, 16), recordPath);
          continue;
        }
        products->wasmSources.push_back(
            {std::string(digest), std::string(record->source(i))});
      }
      continue;
    }
```
Gate the closing `reporter.wasmBakeSummary(...)` on `wasmMode == WasmBakeMode::Bytecode`.

- [ ] **Step 5: The native producer**

In `bundle_build_native.cpp`:

1. A helper in the anonymous namespace:

```cpp
/// Native mode compiles each recorded module from its bytes, which a v1
/// recording does not carry. Refused here, before the kit is read and long
/// before anything is compiled, naming both ways out. A file that does not
/// open is left for the bake step, which reports it in full.
bool refuseV1Recordings(const std::vector<std::string> &paths) {
  for (const std::string &path : paths) {
    std::optional<MappedFile> file = MappedFile::open(path, nullptr);
    if (!file)
      continue;
    std::optional<WasmRecordReader> r =
        WasmRecordReader::open(file->data(), file->size(), nullptr);
    if (r && r->formatVersion() < 2) {
      std::fprintf(
          stderr,
          "error: %s is a format v1 WebAssembly recording, which carries no "
          "module bytes to compile natively\n"
          "       Re-record it with --record-wasm, or pass --bytecode-wasm to "
          "bake its bytecode instead.\n",
          path.c_str());
      return true;
    }
  }
  return false;
}
```
Check `MappedFile::open`'s signature in `bundle_build.cpp`'s bake loop; if it does not accept a null error pointer, pass a local string.

2. In `buildNativeExecutable`, right after the step-0 same-file check:

```cpp
  if (!options.bytecodeWasm && refuseV1Recordings(options.bakeWasmPaths))
    return 1;
```

3. In the `buildBundleImpl` call, pass `options.bytecodeWasm ? WasmBakeMode::Bytecode : WasmBakeMode::NativeSources` after `PayloadMode::NativeSources`.

4. Build the Wasm jobs and compile everything in one batch. Replace the `compileModules(jobs, ...)` call:

```cpp
    std::vector<NativeWasmJob> wasmJobs;
    wasmJobs.reserve(products.wasmSources.size());
    for (const PendingWasmSource &s : products.wasmSources)
      wasmJobs.push_back({s.digest, s.bytes});
    if (options.verbose && !wasmJobs.empty())
      std::fprintf(
          stderr,
          "wasm: %zu module%s, compiled%s\n",
          wasmJobs.size(),
          wasmJobs.size() == 1 ? "" : "s",
          options.wasmTest262 ? " with -test262 (baked --vm=)" : "");

    NativeBatchResults batch = compileNativeBatch(
        jobs,
        wasmJobs,
        tempDir,
        shermesPath,
        *manifest,
        driver->driver,
        driverIsClang,
        options.opt,
        options.wasmTest262,
        parallelism,
        runCommandCaptured);
    std::vector<NativeModuleResult> &results = batch.modules;
```

5. After the JavaScript results loop, before `if (anyFailed) return 1;`:

```cpp
    // Any Wasm failure is a hard error. A recorded module is one the
    // recording run actually used, so a throwing stub would hide a module
    // this program certainly loads. Not diagnosed further: the cause may be
    // shermes, the runtime's compiler, or a recording whose upstream cache
    // filed bytecode under bytes it was never compiled from.
    bool wasmFailed = false;
    for (size_t i = 0; i < batch.wasm.size(); ++i) {
      const NativeModuleResult &r = batch.wasm[i];
      std::string hex16 = nativeWasmUnitName(wasmJobs[i].digest).substr(1, 16);
      if (r.ok) {
        std::printf(
            "wasm: native %s (%zu bytes)\n", hex16.c_str(), wasmJobs[i].bytes.size());
        continue;
      }
      wasmFailed = true;
      std::fprintf(
          stderr, "error: WebAssembly module %s: %s\n", hex16.c_str(), r.message.c_str());
      if (!r.diagnostics.empty())
        std::fprintf(stderr, "%s", r.diagnostics.c_str());
      if (!r.failedCommand.empty())
        std::fprintf(
            stderr, "  command: %s\n", formatCommandLine(r.failedCommand).c_str());
    }
    if (wasmFailed) {
      std::fprintf(
          stderr,
          "note: --bytecode-wasm bakes the recorded bytecode instead of "
          "compiling these modules natively.\n");
      anyFailed = true;
    }
```

6. Step 8's payload: `nativePayloadAssembly(containerPath, unitSymbols, nativeWasmUnitSymbols(wasmJobs), /*wasmWarnOnFallback=*/!options.bytecodeWasm)`.

7. Step 9's objects: after the JavaScript objects,

```cpp
    // Directly on the link line, never in an archive: the table references
    // each unit's creator, and the unit's registering constructor must run.
    for (const NativeWasmJob &job : wasmJobs)
      objects.push_back(stagedWasmObjectPath(tempDir, nativeWasmUnitName(job.digest)));
```

8. Step 11's summary: after the `modules:` line's stub suffix, before its newline:

```cpp
    if (!wasmJobs.empty())
      std::printf(
          ", %zu WebAssembly module%s",
          wasmJobs.size(),
          wasmJobs.size() == 1 ? "" : "s");
```

Update the step comments' numbering and prose wherever they now say something false (e.g. "Every pending module becomes a compile job").

- [ ] **Step 6: Keep the old Wasm test on its old meaning**

In `test/build-native-wasm.js`, add `--bytecode-wasm` to both `build-native` invocations. Extend the header comment: what this file pins (the disk tier and baked bytecode in a native artifact) is now the opt-out's behaviour, and the default is pinned by `build-native-wasm-native.js` and the files beside it.

- [ ] **Step 7: The main end-to-end test**

Create `test/build-native-wasm-native.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The default: a module the recording run saw is compiled natively by
// build-native and served by the native tier at run time -- no Wasm compile,
// no bytecode, no warning -- with the source tree gone and the disk cache
// off (the suite disables it), so nothing else could have answered.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: echo "var m = require('./modules.js'); var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD)); console.log('add', i.exports.add(19, 23));" > %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir > %t/build.out
// RUN: %FileCheck --check-prefix=BUILD %s < %t/build.out
// BUILD: wasm: native {{[0-9a-f]+}} ({{[0-9]+}} bytes)
// BUILD: modules: {{.*}}, 1 WebAssembly module

// RUN: mv %t/src %t/src.away
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app 2> %t/run.err | %FileCheck --check-prefix=ADD %s
// RUN: %FileCheck --check-prefix=NATIVE %s < %t/run.err
// ADD: add 42
// NATIVE: wasm native hit
// NATIVE-NOT: wasm store
// NATIVE-NOT: wasm container
// NATIVE-NOT: warning:

// Last on purpose: lit stops at the first failing line, so only a passing run
// deletes the (large, under ASAN) artifact.
// RUN: rm -f %t/app
```

- [ ] **Step 8: Run**

```bash
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```
Expected: green, including `build-native-errors.js`, `build-native-wasm.js` and `build-native-wasm-native.js`. If `build-native-wasm-native.js` fails at the build step with a `shermes` error about `.wasm` input, the kit's `shermes` was built without Wasm. That contradicts this tree's configuration; stop and investigate rather than working around it.

- [ ] **Step 9: Commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/bundle/bundle_build.h lib/bundle/bundle_build_internal.h \
  lib/bundle/bundle_build.cpp lib/bundle/bundle_build_native.cpp tools/hermes-node/hermes-node.cpp \
  test/fixtures/wasm/make-v1-record.py test/build-native-errors.js test/build-native-wasm.js \
  test/build-native-wasm-native.js
git commit -m "build-native: compile recorded WebAssembly modules natively

--bake-wasm in build-native now compiles every module the recording
saw: each is written out, turned into C by shermes and compiled by cc
in the same job pool as the JavaScript, and linked with a payload table
mapping its SHA-256 to the unit. The container gets no bytecode entry
for it, since a native hit never reads one, and the artifact warns on
stderr when a module falls back to bytecode at run time.

--bytecode-wasm restores the bytecode bake and turns the warning off.
A format v1 recording has no module bytes and is refused before the kit
is read, naming both ways out. Units are compiled under the -test262 the
baked --vm= list produces, parsed with the runtime's own parser, so an
invalid baked option is now a build error. Any failure compiling a Wasm
unit fails the build: a recorded module is one the program loads.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 12: End-to-end coverage

Every build-native link is slow under ASAN, so the cases share builds and sit in three files that lit can run in parallel. One more file covers the bytecode producers.

**Files:**
- Create: `test/build-native-wasm-fallback.js`, `test/build-native-wasm-entry.js`, `test/build-native-wasm-test262.js`, `test/test-wasm-record-v2.js`

**Interfaces:**
- Consumes: everything above. Produces: tests only.

- [ ] **Step 1: Fallbacks, probes, and the opt-out**

Create `test/build-native-wasm-fallback.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// What a native-Wasm artifact does with a module the native tier cannot
// serve, and what --bytecode-wasm changes.
//
// The program compiles ADD (recorded, so native), GARBAGE inside a
// try/catch -- a feature-detection probe whose bytes never compile, so the
// recording never committed it -- and, only when COMPILE_FIB is set at run
// time, FIB, which the recording therefore never saw. Each twice.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: cp %s %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --dump-wasm=%t/rec.bin | %FileCheck --check-prefix=REC %s
// REC: SOURCES (1)

// The probe's failure was never committed, so the build succeeds with one
// unit.
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir --keep-temp > %t/build.out 2> %t/build.err
// RUN: grep -c '^wasm: native ' %t/build.out | %FileCheck --check-prefix=ONEUNIT %s
// ONEUNIT: {{^1$}}

// The intermediate container carries no bytecode for the native module.
// RUN: sed -n 's/^build-native: temp directory: //p' %t/build.err > %t/dir.txt
// RUN: %hermes-node --bundle="$(cat %t/dir.txt)/container.hbb" --dump | %FileCheck --check-prefix=NOWASM %s
// NOWASM: EDGES (
// NOWASM-NOT: WASM (
// NOWASM: SECTIONS
// RUN: rm -rf "$(cat %t/dir.txt)"

// Cold: FIB falls back with the first warning, once; the probe warns once
// and still throws its CompileError; ADD is native.
// RUN: COMPILE_FIB=1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app > %t/cold.out 2> %t/cold.err
// RUN: %FileCheck --check-prefix=OUT %s < %t/cold.out
// RUN: grep -c 'was not compiled natively (it is not in the recording); falling back to bytecode' %t/cold.err | %FileCheck --check-prefix=TWO %s
// RUN: grep -c 'wasm native hit' %t/cold.err | %FileCheck --check-prefix=TWO %s
// OUT: add 42
// OUT-NEXT: add 42
// OUT-NEXT: probe: CompileError
// OUT-NEXT: probe: CompileError
// OUT-NEXT: fib 55
// OUT-NEXT: fib 55
// TWO: {{^2$}}

// Warm: the same with a live disk cache, run twice so the second is a disk
// hit for FIB. The warning does not depend on where the bytecode came from.
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t/cc COMPILE_FIB=1 %t/app > /dev/null 2> /dev/null
// RUN: env -u HERMES_NODE_DISABLE_COMPILE_CACHE HERMES_NODE_COMPILE_CACHE=%t/cc COMPILE_FIB=1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app > /dev/null 2> %t/warm.err
// RUN: %FileCheck --check-prefix=WARM %s < %t/warm.err
// RUN: grep -c 'was not compiled natively' %t/warm.err | %FileCheck --check-prefix=TWO %s
// WARM: wasm hit

// --bytecode-wasm: the recorded bytecode is baked and served from the
// container, nothing warns, and the container carries the entry.
// RUN: %hermes-node build-native %t/src/app.js -o %t/appbc --bake-wasm=%t/rec.bin --bytecode-wasm --kit=%kit_dir --keep-temp > /dev/null 2> %t/buildbc.err
// RUN: sed -n 's/^build-native: temp directory: //p' %t/buildbc.err > %t/dirbc.txt
// RUN: %hermes-node --bundle="$(cat %t/dirbc.txt)/container.hbb" --dump | %FileCheck --check-prefix=BCWASM %s
// BCWASM: WASM (1)
// RUN: rm -rf "$(cat %t/dirbc.txt)"
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/appbc > /dev/null 2> %t/bc.err
// RUN: %FileCheck --check-prefix=BC %s < %t/bc.err
// BC: wasm container hit
// BC-NOT: warning:
// BC-NOT: wasm native

// RUN: rm -f %t/app %t/appbc

var m = require('./modules.js');
function add() {
  var i = new WebAssembly.Instance(new WebAssembly.Module(m.ADD));
  console.log('add', i.exports.add(19, 23));
}
function probe() {
  try {
    new WebAssembly.Module(m.GARBAGE);
    console.log('probe: compiled');
  } catch (e) {
    console.log('probe: ' + (e instanceof WebAssembly.CompileError ? 'CompileError' : e));
  }
}
function fib() {
  var i = new WebAssembly.Instance(new WebAssembly.Module(m.FIB), {env: {log: function () {}}});
  console.log('fib', i.exports.fib(10));
}
add();
add();
probe();
probe();
if (process.env.COMPILE_FIB) {
  fib();
  fib();
}
```
Note on `TWO` for the warnings: FIB warns once and the probe warns once, so the count of lines matching "was not compiled natively" is 2, which pins both "once per module" and "the probe warns". `wasm native hit` is 2 because ADD is compiled twice. FIB is 55 at `fib(10)`. If `GARBAGE` reaches the program as a different error type, the fixture is wrong, not the feature: `test/test-wasm-api.js` already pins `GARBAGE` as a `CompileError`.

- [ ] **Step 2: Every entry point, and a recording made from a warm cache**

Create `test/build-native-wasm-entry.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The native tier sits under Hermes's module construction, so every entry
// point reaches it with no change to the program: new WebAssembly.Module,
// WebAssembly.compile and WebAssembly.instantiate over bytes. And a
// recording made with every module served from a warm disk cache -- no
// compile at all during the run -- still carries the module bytes, and
// builds natively.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: cp %s %t/src/app.js
// RUN: %hermes-node-cc --compile-cache=%t/cc %t/src/app.js > /dev/null
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %hermes-node-cc --compile-cache=%t/cc --record-wasm=%t/rec.bin %t/src/app.js 2> %t/rec.err > /dev/null
// RUN: %FileCheck --check-prefix=WARMREC %s < %t/rec.err
// WARMREC: wasm hit
// WARMREC-NOT: wasm store
// RUN: %hermes-node --dump-wasm=%t/rec.bin | %FileCheck --check-prefix=REC %s
// REC: SOURCES (1)

// RUN: %hermes-node build-native %t/src/app.js -o %t/app --bake-wasm=%t/rec.bin --kit=%kit_dir > /dev/null
// RUN: mv %t/src %t/src.away
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app > %t/out 2> %t/err
// RUN: %FileCheck --check-prefix=OUT %s < %t/out
// RUN: grep -c 'wasm native hit' %t/err | %FileCheck --check-prefix=THREE %s
// RUN: %FileCheck --check-prefix=QUIET %s < %t/err
// OUT: module 42
// OUT-DAG: compile 42
// OUT-DAG: instantiate 42
// THREE: {{^3$}}
// QUIET-NOT: warning:
// QUIET-NOT: wasm store

// RUN: rm -f %t/app

var m = require('./modules.js');
var mod = new WebAssembly.Module(m.ADD);
console.log('module', new WebAssembly.Instance(mod).exports.add(19, 23));
WebAssembly.compile(m.ADD).then(function (mod2) {
  console.log('compile', new WebAssembly.Instance(mod2).exports.add(19, 23));
});
WebAssembly.instantiate(m.ADD).then(function (r) {
  console.log('instantiate', r.instance.exports.add(19, 23));
});
```

- [ ] **Step 3: `-test262` both ways, and the same module recorded twice**

Create `test/build-native-wasm-test262.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A unit is compiled under the -test262 its artifact's baked --vm= list
// produces, and a run under a different setting refuses it, falls back with
// the second warning, and still gets the right semantics. The observable is
// an out-of-bounds store: dropped without test262, trapped with it, caught
// inside the program so output, warning and exit status are asserted
// together.
//
// REQUIRES: wasm, linker-available, shermes-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: cp %s %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --vm=-test262 --record-wasm=%t/rec262.bin %t/src/app.js > /dev/null

// Baked -test262, unlocked. Two recordings name the same module: one unit.
// RUN: %hermes-node build-native %t/src/app.js -o %t/baked --vm=-test262 --allow-vm-options-override --bake-wasm=%t/rec.bin --bake-wasm=%t/rec262.bin --kit=%kit_dir > %t/baked.build
// RUN: grep -c '^wasm: native ' %t/baked.build | %FileCheck --check-prefix=ONE %s
// ONE: {{^1$}}
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/baked > %t/b1.out 2> %t/b1.err
// RUN: %FileCheck --check-prefix=TRAP %s < %t/b1.out
// RUN: %FileCheck --check-prefix=HIT %s < %t/b1.err
// RUN: HERMES_NODE_VM_OPTIONS=-test262=false %t/baked > %t/b2.out 2> %t/b2.err
// RUN: %FileCheck --check-prefix=DROP %s < %t/b2.out
// RUN: %FileCheck --check-prefix=INCOMPATIBLE %s < %t/b2.err

// Nothing baked, unlocked: the inverse.
// RUN: %hermes-node build-native %t/src/app.js -o %t/plain --allow-vm-options-override --bake-wasm=%t/rec.bin --kit=%kit_dir > /dev/null
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/plain > %t/p1.out 2> %t/p1.err
// RUN: %FileCheck --check-prefix=DROP %s < %t/p1.out
// RUN: %FileCheck --check-prefix=HIT %s < %t/p1.err
// RUN: HERMES_NODE_VM_OPTIONS=-test262 %t/plain > %t/p2.out 2> %t/p2.err
// RUN: %FileCheck --check-prefix=TRAP %s < %t/p2.out
// RUN: %FileCheck --check-prefix=INCOMPATIBLE %s < %t/p2.err

// TRAP: oob store: trapped
// DROP: oob store: dropped
// HIT: wasm native hit
// HIT-NOT: warning:
// INCOMPATIBLE: warning: WebAssembly module {{[0-9a-f]+}}: its native unit is unavailable or incompatible with this run (e.g. -test262); falling back to bytecode

// RUN: rm -f %t/baked %t/plain

var m = require('./modules.js');
var i = new WebAssembly.Instance(new WebAssembly.Module(m.FIB), {env: {log: function () {}}});
try {
  i.exports.poke(70000, 7);
  console.log('oob store: dropped');
} catch (e) {
  console.log('oob store: trapped');
}
```
(Measured on 2026-10-08 with this tree's ASAN `hermes-node`: this fixture prints `dropped` plain and `trapped` under `--vm=-test262`.)

- [ ] **Step 4: The bytecode producers still read v2**

Create `test/test-wasm-record-v2.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Record format v2 changes nothing for the bytecode producers: --build-bundle
// --bake-wasm reads only its bytecode table, and --build-exe consumes the
// container, never a recording.
//
// REQUIRES: wasm, linker-available

// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %source_dir/test/fixtures/wasm/modules.js %t/src/modules.js
// RUN: echo "var m = require('./modules.js'); console.log('add', new WebAssembly.Instance(new WebAssembly.Module(m.ADD)).exports.add(19, 23));" > %t/src/app.js
// RUN: %hermes-node --record-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --dump-wasm=%t/rec.bin | %FileCheck --check-prefix=DUMP %s
// DUMP: format v2
// DUMP: WASM (1)
// DUMP: SOURCES (1)
// RUN: %hermes-node --build-bundle=%t/app.hbb --bake-wasm=%t/rec.bin %t/src/app.js > /dev/null
// RUN: %hermes-node --build-exe=%t/app --kit=%kit_dir %t/app.hbb > /dev/null
// RUN: HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE %t/app 2> %t/err | %FileCheck --check-prefix=ADD %s
// RUN: %FileCheck --check-prefix=HIT %s < %t/err
// ADD: add 42
// HIT: wasm container hit
// HIT-NOT: warning:
// RUN: rm -f %t/app
```
Check `test/test-wasm-bake-build-exe.js` for the exact `--build-exe` spelling this suite uses (positional container, `--kit=`) and match it.

- [ ] **Step 5: Run**

```bash
for t in build-native-wasm-fallback build-native-wasm-entry build-native-wasm-test262 test-wasm-record-v2; do
  python3 cmake-build-asan/bin/hermes-lit -v $(pwd)/test/$t.js \
    --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
    --param not=$(pwd)/cmake-build-asan/bin/not \
    --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test \
    --param kit_dir=$(pwd)/cmake-build-asan/kit 2>&1 | tail -3
done
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
```
`linker-available` and `shermes-available` come from lit params. If a hand run reports UNSUPPORTED, copy the params `check-hermes-node-js` passes (`grep -n 'param' CMakeLists.txt test/CMakeLists.txt`); the full target run is authoritative. Expected: all pass.

A failure here is a finding about Tasks 7-11, not about the test, unless the test contradicts the spec. Fix the code in a commit of its own, test first, before committing these tests.

- [ ] **Step 6: Commit**

```bash
git add test/build-native-wasm-fallback.js test/build-native-wasm-entry.js \
  test/build-native-wasm-test262.js test/test-wasm-record-v2.js
git commit -m "Test native Wasm end to end: fallbacks, entry points, -test262

Four lit files covering what a native-Wasm artifact meets in practice:
a module the recording never saw and a feature-detection probe that
never compiles (each warned about once, cold and with a warm disk
cache), every WebAssembly entry point taking the native tier, a
recording made entirely from a warm cache, the same module recorded
twice under different -test262 settings compiled once, -test262
overridden both ways at run time, --bytecode-wasm, and the bytecode
producers reading a v2 recording.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

---

### Task 13: The example, the measurements, the docs

**Files:**
- Modify: `examples/hermes-parser-ast-wasm/run.sh`, `examples/hermes-parser-ast-wasm/README.md`
- Modify: `CLAUDE.md`
- Modify: `docs/superpowers/plans/progress-native-wasm.md`

**Interfaces:** none.

- [ ] **Step 1: A `build-native` arm for the example**

Read `examples/tetris/run.sh`'s `build-native` arm first, and copy its conventions: how it finds the kit, how it moves `node_modules` aside and restores it on any exit, and how it asserts the producer printed no warning. Then add, after the bundle checks and before the cross-check:

1. `echo "hermes-node build-native:"`.
2. Record: `"$HERMES_NODE" --no-compile-cache --record-wasm="$OUT_NATIVE/ast.wasmrec" "$HERE/ast.js" "$HERE/sample.js" > /dev/null`.
3. Build: `"$HERMES_NODE" build-native "$HERE/ast.js" -o "$OUT_NATIVE/ast" --bake-wasm="$OUT_NATIVE/ast.wasmrec" --kit="$BUILD_DIR/kit"`. Capture stdout and stderr. Fail if the output contains `warning:`, and fail unless it contains exactly one `wasm: native` line.
4. Run with `node_modules` moved away and `HERMES_NODE_DISABLE_COMPILE_CACHE=1 HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`. Diff stdout against `$UNBUNDLED_OUT`. Require `wasm native hit` and no `warning:` on stderr.
5. `$OUT_NATIVE` is `"$HERE/out-native"`; extend `cleanup()` to remove it.

Update the script's header comment and the README: the example now covers four shapes. Run it against Release:

```bash
cmake --build cmake-build-release --target hermes-node hermes-node-kit shermes
examples/hermes-parser-ast-wasm/run.sh cmake-build-release
```
Expected: `PASS: hermes-parser-ast-wasm`.

- [ ] **Step 2: Measure**

On `cmake-build-release`, with the recording from Step 1:

- the artifact size of the native build against the same build with `--bytecode-wasm`;
- the build wall time of each;
- a cold run of each over `sample.js`, with `HERMES_NODE_DISABLE_COMPILE_CACHE=1`, best of 5 (`/usr/bin/time -f %e`).

Record them in the progress file under "Task 13: artifact and run time", quoting each measurement whole (both absolutes and the difference, from the same run). The spec says a speed comparison is recorded, not required: report whatever it shows.

- [ ] **Step 3: CLAUDE.md**

1. Under "Native Compilation", add a subsection "### Native Wasm", after "Native built-ins". It covers: what `--bake-wasm` means in `build-native` (compile natively, no bytecode entry, the `wasm: native` line); `--bytecode-wasm`; the policy bit and the two warnings, emitted once per module and unconditionally, with the feature-detection-probe consequence; `-test262` (the effective baked value, parsed with the runtime's parser, so an invalid baked option is now a build error); the hard-error failure policy and why it differs from JavaScript's two tiers; the payload table; the Hermes side (`lookup_native`, configuration-stamped registrations, `accepted`) and that it must land upstream before the gitlink can pin it; MSVC is unsupported; the source bytes still ship. Link the design, plan and progress files, and name the tests.
2. In "Baked Wasm entries": the record file is format v2 (source table; readers take v1 and v2), and recording now commits at `store` or `accepted`, never at lookup. Update "Tracing distinguishes the tiers" accordingly (the `wasm record ...` lines are printed at commit time).
3. In the Native Compilation bullet that lists `build-native`'s options, add `--bytecode-wasm`, and add the new test files to its Tests line.
4. In "Wasm entries", the bullet "The Hermes side is an embedder hook": mention the two optional callbacks, and that `struct_size`'s minimum is the pre-extension size.

Keep the existing voice: state how it works now, with the reason, and no history.

- [ ] **Step 4: Full verification**

```bash
cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -6
LIT_FILTER='wasm/|napi/' cmake --build cmake-build-asan --target check-hermes 2>&1 | tail -6
cmake-build-asan/hermes/unittests/napi/NapiTests 2>&1 | tail -3
cmake-build-asan/hermes/unittests/VMRuntime/VMRuntimeTests 2>&1 | tail -3
cmake --build cmake-build-release --target check-hermes-node-native 2>&1 | tail -6
```
Expected: green, with only the exceptions in Global Constraints. `check-hermes-node-native` runs `test-wasm-api.js` and `test-wasm-exec.js` as native artifacts with no recording. They now print first warnings on stderr, which neither test reads; if either fails, that is a finding.

- [ ] **Step 5: Progress file and commit**

Mark every task done in the progress file, and add a short "State at the end" section: the submodule is on `native-wasm-lookup` with five commits (Tasks 2-6) over `80ea8dc99`; the gitlink is dirty and unstaged; those commits must land upstream on `hermes-node` before a gitlink commit can pin them.

```bash
./utils/format.sh -f
git add examples/hermes-parser-ast-wasm/run.sh examples/hermes-parser-ast-wasm/README.md \
  CLAUDE.md docs/superpowers/plans/progress-native-wasm.md
git commit -m "Native Wasm: example, measurements, documentation

examples/hermes-parser-ast-wasm gains a build-native arm: record, build
with the recording, and run with node_modules moved away -- the AST is
byte-identical to the unbundled run, served by a native unit, with no
warning. The progress file records artifact size and run time against
--bytecode-wasm, and CLAUDE.md describes native Wasm, record format v2
and the commit boundary.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014G1ZvumxkNPNrtXx3pzzJZ"
```

- [ ] **Step 6: Report**

Tell the user: what was built; the Task 1 and Task 13 numbers; the submodule branch and its five commits; and that the gitlink is deliberately uncommitted. Do not push, and do not offer to.
