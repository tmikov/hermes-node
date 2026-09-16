# Native Built-ins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pre-compile hermes-node's 187 built-in JavaScript modules to Static
Hermes native units, ship them in the kit, and have `hermes-node build-native`
link those instead of the embedded bytecode registry.

**Architecture:** `EmbeddedModule` grows an `SHUnitCreator creator` field and
the one dispatch function branches on it. Two registry objects define the same
symbol, `findEmbeddedModule`; a `build-native` link names the native archive
ahead of the merged kit archive so the bytecode member is never pulled, and
roots a marker symbol with `-Wl,-u` so a misconfiguration fails the link
instead of silently selecting bytecode. Everything else -- the loader, the
container, the closed world -- is untouched.

**Tech Stack:** CMake + Ninja, Static Hermes (`shermes -emit-c`), Clang,
Python 3 (code generators, lit configs), GTest, LLVM lit.

**Spec:** `docs/superpowers/specs/2026-09-14-native-builtins-design.md`. Read
it before Task 1. Every claim in it was verified against the code across eight
external review rounds; where this plan and the spec disagree, the spec is
right and the plan has a bug.

## Global Constraints

- **Build directory for all work: `cmake-build-release`.** The ASAN kit is
  ~755 MB and links far slower; the native corpus is Release-only.
- **Formatting before every commit:** `./utils/format.sh -f`.
- **Commit messages: ASCII only, no emojis, subject <= 50 chars, body hard
  wrapped at 72 columns.** Say only what the diff cannot.
- **`clang-format 18` specifically.** `format.sh` refuses another major
  version and says how to get the right one.
- The Hermes bytecode magic is `0x1F1903C103BC1FC6`
  (`hermes/include/hermes/BCGen/HBC/BytecodeFileFormat.h:30`), little-endian
  on disk.
- There are **187** embedded modules: 180 CommonJS-wrapped (161 manifest + 19
  vendored) and 7 unwrapped (6 `@bootstrap` + generated `vendored-packages`).
- **Built-in unit names are `hn_b_<safe_id>`.** The `hn_b_` prefix is
  mandatory: user modules under `build-native` are already named
  `hn_m<index>` by `nativeUnitName()`, both become `sh_export_<name>` symbols
  in one binary, and a collision is a duplicate-symbol link failure.
- `safe_id` is the existing transform in `tools/gen-embedded-registry.py`:
  `id.replace("/", "__").replace("-", "_")`.
- **shermes requires an exported-unit name to match `^[A-Za-z0-9_]+$`**
  (`hermes/include/hermes/Utils/Options.h:34`).
- The generated C **must** be compiled with `-fno-strict-aliasing`,
  `-fno-strict-overflow`, `-w` and `gnu11`. The first two are correctness
  requirements, not tuning: Hermes appends them to `CMAKE_C_FLAGS` only inside
  its own subdirectory scope (`hermes/CMakeLists.txt:609-617`), which a
  top-level hermes-node target does not inherit. `gnu` rather than `c` because
  `static_h.h` uses a zero-length array.
- **shermes defaults to `-g0`.** Pass `-g2`, matching
  `lib/build-native/native_compile.cpp:66`. Absent, every built-in stack frame
  loses its location.
- **The marker must be declared `extern "C"`.** The registry is generated
  inside `namespace hermes::node_compat`; without it the definition is mangled
  while `-u` asks for the unmangled name, and every native link fails.
- Existing lit invocation for one test (paths must be absolute):

  ```bash
  python3 cmake-build-release/bin/hermes-lit $(pwd)/test/test-foo.js \
    --param hermes_node=$(pwd)/cmake-build-release/bin/hermes-node \
    --param hermes=$(pwd)/cmake-build-release/bin/hermes \
    --param FileCheck=$(pwd)/cmake-build-release/bin/FileCheck \
    --param not=$(pwd)/cmake-build-release/bin/not \
    --param source_dir=$(pwd) \
    --param test_exec_root=$(pwd)/cmake-build-release/test \
    --param hello_addon=$(pwd)/cmake-build-release/hello_addon.node \
    --param kit_dir=$(pwd)/cmake-build-release/kit
  ```

  **All eight params are required.** Omitting `hello_addon` or `kit_dir` makes
  tests FAIL rather than skip.
- Full suites: `cmake --build cmake-build-release --target check-hermes-node`.

---

## File Structure

**New files**

| Path | Responsibility |
|---|---|
| `tools/gen-native-registry.py` | Emits `embedded_modules_registry_native.cpp`: `extern "C"` unit-creator declarations, the sorted table, `findEmbeddedModule`, and the `extern "C"` marker. |
| `test/litnative.cfg` | Second lit suite over `test/`, selected with lit's `--config-prefix=litnative`: loads `test/lit.cfg`, replaces `%hermes-node`, gates on features, installs the corpus-filtering format. |
| `test/native/corpus_util.py` | The wrappable `RUN:` shapes, the tree scan, and the work-directory slug -- shared by the wrapper and the checker so the invariant one relies on is enforced by the other. |
| `test/native/run-native.py` | The wrapper `%hermes-node` becomes: `build-native` the script, symlink fixtures, run the artifact, forward stdout/stderr/status. |
| `test/native/corpus.txt` | The tests the native suite runs. |
| `test/native/excluded.txt` | Every test containing a wrappable `RUN:` line that is nevertheless not run, with a reason. |
| `test/native/check-corpus.py` | Reconciles both lists against the tree. |
| `test/build-native-builtins.js` | Magic-count and `--bytecode-builtins` lit test. |

**Modified files**

| Path | Change |
|---|---|
| `CMakeLists.txt` | `HERMESVM_INTERNAL_JAVASCRIPT_NATIVE ON ... FORCE`; `check-hermes-node-native` target. |
| `include/hermes/node-compat/embedded-modules/embedded_modules.h` | `SHUnitCreator creator` field. |
| `lib/embedded-modules/embedded_modules.cpp` | Branch on `creator`. |
| `lib/embedded-modules/CMakeLists.txt` | Shared language-flag variable; native compile rules; `hermesNodeBuiltinsNative`. |
| `tools/gen-embedded-registry.py` | Emit the null `creator`; validate unit names. |
| `utils/make-kit.py` | `--native-builtins-name`, `nativebuiltins:` line. |
| `include/hermes/node-compat/build-exe/kit_manifest.h`, `lib/build-exe/kit_manifest.cpp` | `nativeBuiltinsArchive` field and key. |
| `include/hermes/node-compat/build-exe/build_exe.h`, `lib/build-exe/build_exe.cpp` | `symbolPrefix()` helper. |
| `include/hermes/node-compat/build-native/build_native.h`, `lib/build-native/native_compile.cpp` | `nativeBuiltinsLinkArgs()`. |
| `include/hermes/node-compat/bundle/bundle_build.h` | `NativeBuildOptions::bytecodeBuiltins`. |
| `lib/bundle/bundle_build_native.cpp` | Append the marker root and the archive to the link. |
| `tools/hermes-node/hermes-node.cpp` | `--bytecode-builtins` parsing and usage text. |
| `tools/hermes-node/CMakeLists.txt` | Kit copy rule; pass the name to make-kit. |
| `unittests/BuildExeTest.cpp`, `unittests/BuildNativeTest.cpp` | Manifest key and link-argv cases. |
| `CLAUDE.md` | A subsection under Native Compilation. |
| `docs/superpowers/plans/progress-native-builtins.md` | New progress file. |

---

## Task 1: Turn on native Hermes InternalJavaScript

**Files:**
- Modify: `CMakeLists.txt` (after the `HERMES_ENABLE_WASM` block, which ends
  at line 31)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing in code. Later tasks rely on the *fact* that
  `InternalJavaScript` no longer contributes a bytecode blob, which Task 8's
  exact magic count depends on.

**Background.** Hermes compiles its own bootstrap JavaScript two ways and
picks one with an alias (`hermes/lib/InternalJavaScript/CMakeLists.txt:98-103`).
`hermesInternalUnit` -- the native form -- is **already built** in every
non-MSVC configuration "to prevent bitrot" (same file, :55-59), so this is an
option flip over an archive that already exists, not new compilation work.
Measured: `hermes-node` grows 12,503,912 -> 12,842,872 bytes and the bytecode
magic count drops 189 -> 188.

- [ ] **Step 1: Record the baseline**

```bash
cmake --build cmake-build-release --target hermes-node
python3 -c "
import struct
m = struct.pack('<Q', 0x1F1903C103BC1FC6)
d = open('cmake-build-release/bin/hermes-node','rb').read()
print('magic:', d.count(m), 'bytes:', len(d))
assert d.count(m) == 189, 'expected 189 before the flip'
"
```

Expected: `magic: 189 bytes: 12503912` (the byte count will differ on another
host; the magic count must be 189).

- [ ] **Step 2: Add the option**

Insert after the `set(HERMES_ENABLE_WASM ...)` line:

```cmake
# Compile Hermes's own InternalJavaScript to a native Static Hermes unit
# instead of running it as bytecode at every runtime creation. Without this a
# "fully native" build-native artifact still interprets Hermes's bootstrap JS,
# which makes the feature's headline claim false and its residual-bytecode
# test impossible to write.
#
# Nearly free, because the archive already exists: Hermes builds
# hermesInternalUnit in every non-MSVC configuration to keep it from
# bitrotting, and only the alias selection is gated. Measured on macOS arm64:
# hermes-node grows 338,960 bytes and the embedded bytecode magic count drops
# by exactly one. A universal binary pays the size per slice.
#
# FORCE, unlike HERMES_ENABLE_WASM above and like the debugger: a non-FORCE
# `set(... CACHE ...)` leaves an existing cache entry alone, so every build
# directory configured before this change would silently keep Hermes's OFF and
# report the feature as not working -- the exact trap CLAUDE.md records for
# the Wasm option. Forcing costs nothing here because there is no build
# configuration that wants the bytecode form.
set(HERMESVM_INTERNAL_JAVASCRIPT_NATIVE ON CACHE BOOL
    "Use natively compiled InternalJavaScript" FORCE)
```

- [ ] **Step 3: Reconfigure, rebuild, and verify the drop**

```bash
cmake -B cmake-build-release
cmake --build cmake-build-release --target hermes-node
python3 -c "
import struct
m = struct.pack('<Q', 0x1F1903C103BC1FC6)
d = open('cmake-build-release/bin/hermes-node','rb').read()
n = d.count(m)
print('magic:', n, 'bytes:', len(d))
assert n == 188, f'expected 188 after the flip, got {n}'
"
```

Expected: `magic: 188`. If it is still 189, the cache kept `OFF` -- check that
`FORCE` is present and that the `set()` is **before**
`add_subdirectory(hermes)`.

- [ ] **Step 4: Run the full suite**

```bash
cmake --build cmake-build-release --target check-hermes-node
```

Expected: all tests pass. This is the real gate -- the flip changes how every
`Array.prototype` helper Hermes implements in JavaScript reaches the runtime.

- [ ] **Step 5: Format and commit**

```bash
./utils/format.sh -f
git commit -am "Compile Hermes's own bootstrap JavaScript natively"
```

Suggested message body: that the archive already exists so this is an option
flip rather than new work; the measured 338,960 bytes; and why `FORCE` rather
than the Wasm option's non-forced form.

---

## Task 2: Give EmbeddedModule a unit creator

**Files:**
- Modify: `include/hermes/node-compat/embedded-modules/embedded_modules.h`
- Modify: `lib/embedded-modules/embedded_modules.cpp`
- Modify: `tools/gen-embedded-registry.py:84-91` (the table emitter)

**Interfaces:**
- Consumes: nothing.
- Produces: `struct EmbeddedModule { const char *id; const uint8_t *data;
  size_t size; SHUnitCreator creator; bool isBootstrap; }`. Task 4's generator
  emits this exact field order.

**This task changes no behaviour.** Every existing entry gets a null `creator`
and takes the bytecode path exactly as before. Its test is the existing suite:
if `check-hermes-node` is green, the refactor is correct. Do not invent a new
test for it -- there is nothing new to observe until Task 4 produces a registry
with non-null creators.

- [ ] **Step 1: Add the field**

In `embedded_modules.h`, `SHUnit` and `SHUnitCreator` come from
`napi/hermes_napi.h`, which `embedded_modules.cpp` already includes; the header
itself includes only `<node_api_types.h>`, so add the two typedefs' source.
Replace the struct with:

```cpp
/// Describes a single embedded JS module. Exactly one of \c data and
/// \c creator is set, and which one says how the module was compiled: a
/// bytecode registry fills \c data / \c size, a native one fills \c creator.
/// Both registries define findEmbeddedModule(); a build-native link resolves
/// it from the native archive, which is why this one struct has to describe
/// both (see the design doc, "Two registries, chosen by the linker").
struct EmbeddedModule {
  const char *id;
  const uint8_t *data;
  size_t size;
  SHUnitCreator creator;
  bool isBootstrap;
};
```

and add above it, after the existing includes:

```cpp
#include <napi/hermes_napi.h>
```

- [ ] **Step 2: Branch in the dispatch**

`embedded_modules.cpp` has two call sites that run a module:
`runEmbeddedModule()` and `loadBytecodeModuleCallback()`. Give them one shared
helper rather than two copies of the branch. Insert before
`runEmbeddedModule`:

```cpp
namespace {

/// Runs \p mod, whichever form it was compiled to, and reports exactly what
/// hermes_run_bytecode reports: napi_ok with the module's top-level
/// completion value, or napi_pending_exception with the thrown value pending.
///
/// The two paths return the same thing, which is the whole reason this is a
/// branch rather than a second loader. A CommonJS module is compiled wrapped
/// in `(function (exports, require, module, __filename, __dirname) { ... });`
/// either way, so its completion value is the wrapper closure in both. A
/// bootstrap module's is its file's completion value, again in both.
napi_status runModule(
    napi_env env,
    const EmbeddedModule *mod,
    napi_value *result) {
  if (mod->creator)
    return hermes_init_sh_unit(env, mod->creator, result);

  // Static data embedded in the binary -- no finalize callback needed.
  // Persistent: internal modules live for the lifetime of the process.
  hermes_bytecode_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.persistent = true;
  return hermes_run_bytecode(
      env, mod->data, mod->size, nullptr, nullptr, mod->id, &flags, result);
}

} // namespace
```

Then `runEmbeddedModule` becomes:

```cpp
napi_status
runEmbeddedModule(napi_env env, const char *id, napi_value *result) {
  const EmbeddedModule *mod = findEmbeddedModule(id);
  if (!mod) {
    return napi_generic_failure;
  }
  return runModule(env, mod, result);
}
```

and in `loadBytecodeModuleCallback`, replace the `hermes_bytecode_flags` block
and the `hermes_run_bytecode` call with:

```cpp
  napi_value result;
  if (runModule(env, mod, &result) != napi_ok) {
    // Exception is pending; return nullptr to propagate it.
    return nullptr;
  }
  return result;
```

Note the source-URL argument moves from the local `idBuf` to `mod->id`. They
are equal -- `mod` was found by `idBuf` -- and `mod->id` is the pointer that
outlives the call.

- [ ] **Step 3: Emit the null creator**

In `tools/gen-embedded-registry.py`, the table emitter currently writes four
fields. Change it to five:

```python
        lines.append(
            '    {"%s", data_%s, sizeof(data_%s), nullptr, %s},'
            % (mod_id, sid, sid, bootstrap_str)
        )
```

- [ ] **Step 4: Build and run the full suite**

```bash
cmake --build cmake-build-release --target check-hermes-node
```

Expected: all tests pass, unchanged from Task 1.

- [ ] **Step 5: Format and commit**

```bash
./utils/format.sh -f
git commit -am "Let an embedded module be a native unit"
```

---

## Task 3: Share the id transform and the language flags

**Files:**
- Create: `tools/embedded_module_ids.py`
- Modify: `tools/gen-embedded-registry.py` (replace its local `safe_id`)
- Modify: `lib/embedded-modules/CMakeLists.txt:30-36` (the `JS_COMPILER_FLAGS`
  block)

**Interfaces:**
- Consumes: nothing.
- Produces, for Task 4 and Task 5:
  - `embedded_module_ids.safe_id(module_id) -> str`
  - `embedded_module_ids.unit_name(module_id) -> str` (`"hn_b_" + safe_id`)
  - `embedded_module_ids.parse_manifest(path) -> list[(module_id, is_bootstrap)]`
  - `embedded_module_ids.validate(modules) -> None`, raising `SystemExit` with
    a message naming the offender
  - CMake variable `HERMES_NODE_JS_LANGUAGE_FLAGS`

**Why a shared module rather than a copied function.** Both generators derive
a unit name from a module id and must derive the *same* one: the native
registry declares `sh_export_<unit_name>` while CMake passes
`-exported-unit=<unit_name>` to shermes, and a disagreement fails the link with
an undefined symbol naming a unit nobody wrote.

**This does not make the transform single-sourced, and the plan should not
claim it does.** CMake needs a unit name inside its loops, before the combined
manifest exists, so it derives `hn_b_${SAFE_ID}` itself -- a *third*
derivation. What Task 5 adds is not a shared implementation but a build-time
cross-check: CMake records every pair it used and
`gen-native-registry.py` refuses to emit if any disagrees with this module's
answer. Two derivations, checked against each other, rather than one.

**Why validation is not optional.** `safe_id` replaces only `/` and `-`. A
future vendored package path containing `.` or `@` would produce a name
shermes rejects (`^[A-Za-z0-9_]+$`), and `a-b` and `a_b` both map to `a_b`, so
two distinct modules can collide onto one unit. Today's 187 ids are clean;
nothing currently stops the 188th from not being. Failing the build with the
offending id beats a duplicate-symbol error from the linker.

- [ ] **Step 1: Write the shared module**

Create `tools/embedded_module_ids.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Module-id helpers shared by the two embedded-registry generators.

gen-embedded-registry.py names byte arrays after a module;
gen-native-registry.py names Static Hermes units after the same module. Both
read this, so those two cannot drift.

CMake is the third, and it is NOT single-sourced here: it needs a unit name
inside its own loops, before the combined manifest exists, so it derives
hn_b_<safe_id> itself. That derivation is cross-checked against this one at
build time by gen-native-registry.py, which refuses to emit on a
disagreement -- see lib/embedded-modules/CMakeLists.txt. Two implementations
checked against each other, not one.
"""

import re
import sys

# shermes accepts an exported-unit name of ASCII alphanumerics and underscore
# only, and nothing else (hermes/include/hermes/Utils/Options.h).
_VALID_UNIT = re.compile(r"^[A-Za-z0-9_]+$")


def safe_id(module_id):
    """A C identifier fragment for a module id.

    e.g. 'internal/errors' -> 'internal__errors'
         'internal/streams/add-abort-signal' ->
             'internal__streams__add_abort_signal'
    """
    return module_id.replace("/", "__").replace("-", "_")


def unit_name(module_id):
    """The Static Hermes unit name for a built-in module.

    The hn_b_ prefix is load bearing. build-native names a *user* module's
    unit hn_m<index> (nativeUnitName(), lib/build-native/staging.cpp), both
    become sh_export_<name> symbols in one linked binary, and a collision is a
    duplicate-symbol failure at the customer's link rather than at ours.
    """
    return "hn_b_" + safe_id(module_id)


def parse_manifest(path):
    """[(module_id, is_bootstrap)] from a combined manifest file."""
    modules = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("@bootstrap "):
                modules.append((line[len("@bootstrap "):].strip(), True))
            else:
                modules.append((line, False))
    return modules


def validate(modules):
    """Refuse a manifest whose ids do not map to distinct, legal unit names.

    Raises SystemExit naming the offender. Both checks are real: safe_id
    replaces only '/' and '-', so an id containing '.' or '@' yields a name
    shermes rejects outright, and 'a-b' and 'a_b' both yield 'a_b'.
    """
    seen = {}
    for module_id, _ in modules:
        name = unit_name(module_id)
        if not _VALID_UNIT.match(name):
            sys.exit(
                "embedded module id %r maps to unit name %r, which shermes "
                "will reject: only ASCII letters, digits and underscore are "
                "allowed. Extend safe_id() to cover the new character."
                % (module_id, name)
            )
        if name in seen:
            sys.exit(
                "embedded module ids %r and %r both map to unit name %r. "
                "Unit names must be distinct; extend safe_id() so they are."
                % (seen[name], module_id, name)
            )
        seen[name] = module_id
```

- [ ] **Step 2: Prove validation catches both failures**

```bash
cd tools && python3 -c "
import embedded_module_ids as e
print(e.safe_id('internal/streams/add-abort-signal'))
print(e.unit_name('internal/errors'))
for bad, why in [([('a.b', False)], 'illegal character'),
                 ([('a-b', False), ('a_b', False)], 'collision')]:
    try:
        e.validate(bad); raise AssertionError('accepted ' + why)
    except SystemExit as ex:
        print('rejected', why + ':', ex)
e.validate([('fs', False), ('internal/errors', False)])
print('clean manifest accepted')
"
```

Expected:

```
internal__streams__add_abort_signal
hn_b_internal__errors
rejected illegal character: embedded module id 'a.b' maps to unit name ...
rejected collision: embedded module ids 'a-b' and 'a_b' both map to ...
clean manifest accepted
```

- [ ] **Step 3: Make the existing generator use it**

In `tools/gen-embedded-registry.py`, delete the local `safe_id` function and
the manifest-parsing loop inside `main`, and use the shared module. Add near
the top:

```python
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embedded_module_ids import parse_manifest, safe_id, validate
```

and in `main`, replace the manual parse with:

```python
    modules = parse_manifest(manifest_path)
    validate(modules)
```

- [ ] **Step 4: Split the language flags**

In `lib/embedded-modules/CMakeLists.txt`, replace the `JS_COMPILER_FLAGS`
block with:

```cmake
# The language features every built-in module is compiled with, and the ONLY
# flags the bytecode and native pipelines share. Everything else below --
# output format, debug level, warning control -- differs per tool, and sharing
# the whole list would hand shermes the bytecode-only -emit-binary.
#
# A third copy of these values lives in C++, as kJSLanguageFlags
# (include/hermes/node-compat/bundle/cjs_wrapper.h), which drives the require()
# scanner and the shermes argv for a user's own modules. Nothing mechanically
# forces that copy and this one to agree. Changing one means changing the
# other: block scoping off makes every let-in-loop closure capture the wrong
# binding, with no diagnostic at build time or run time.
set(HERMES_NODE_JS_LANGUAGE_FLAGS
  -Xes6-block-scoping
  -Xasync-generators
)

set(JS_COMPILER_FLAGS
  ${HERMES_NODE_JS_LANGUAGE_FLAGS}
  -Wno-undefined-variable
  -emit-binary
  -g
)
```

- [ ] **Step 5: Rebuild and run the full suite**

```bash
cmake --build cmake-build-release --target check-hermes-node
```

Expected: all tests pass. Nothing observable changed -- the same flags reach
`hermesc` and the same registry is generated.

- [ ] **Step 6: Format and commit**

```bash
./utils/format.sh -f
git add tools/embedded_module_ids.py
git commit -am "Share the built-in module id transform"
```

---

## Task 4: Generate the native registry

**Files:**
- Create: `tools/gen-native-registry.py`

**Interfaces:**
- Consumes: `embedded_module_ids.{parse_manifest,unit_name,validate}` (Task 3).
- Produces: a C++ source file defining, in `namespace hermes::node_compat`,
  `const EmbeddedModule *findEmbeddedModule(const char *id)`, and at global
  scope `extern "C" const char hermesNodeNativeBuiltinsMarker`.

**The marker's two properties are both load bearing.** It must be `extern "C"`
(the file is inside a C++ namespace, `-u` asks for an unmangled name, and a
mangled definition means every native-builtins link fails), and it must live in
**the same translation unit** as `findEmbeddedModule` (rooting it is what pulls
that archive member in, and a marker in a different member would pull the
wrong one).

- [ ] **Step 1: Write the generator**

Create `tools/gen-native-registry.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Generate the NATIVE embedded module registry.

Usage: gen-native-registry.py <manifest> <output.cpp>

Emits the same findEmbeddedModule() symbol gen-embedded-registry.py emits,
over a table of Static Hermes unit creators instead of bytecode arrays. A
build-native link names this archive ahead of the merged kit archive, so the
linker resolves findEmbeddedModule here and never pulls the bytecode member --
which is what keeps 2.1 MB of built-in bytecode out of a native artifact.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embedded_module_ids import parse_manifest, unit_name, validate


def main():
    if len(sys.argv) != 3:
        print(
            "Usage: gen-native-registry.py <manifest> <output.cpp>",
            file=sys.stderr,
        )
        sys.exit(1)

    modules = parse_manifest(sys.argv[1])
    validate(modules)

    out = []
    w = out.append
    w("// AUTO-GENERATED by gen-native-registry.py. DO NOT EDIT.")
    w("")
    w('#include "hermes/node-compat/embedded-modules/embedded_modules.h"')
    w("")
    w("#include <algorithm>")
    w("#include <cstring>")
    w("")
    # Global scope, C linkage: `shermes -exported-unit=X` emits
    # `SHUnit *sh_export_X(void)` from a C file.
    w('extern "C" {')
    for module_id, _ in modules:
        w("SHUnit *sh_export_%s(void);" % unit_name(module_id))
    w("}")
    w("")
    # Also global scope and also C linkage, and both matter. build-native
    # passes -Wl,-u,<prefix>hermesNodeNativeBuiltinsMarker, which names an
    # UNMANGLED symbol: a C++ definition would be mangled and every native
    # link would fail. It sits in this translation unit on purpose -- rooting
    # it is what extracts this archive member, and therefore what makes this
    # file's findEmbeddedModule the one that wins.
    w("/// Rooted with -Wl,-u by a build-native link. Its only job is to")
    w("/// exist: if this archive is missing from the link line the marker")
    w("/// is undefined and the link fails by name, instead of silently")
    w("/// falling through to the bytecode registry in the merged kit")
    w("/// archive and producing a working, interpreted binary.")
    w('extern "C" const char hermesNodeNativeBuiltinsMarker = 1;')
    w("")
    w("namespace hermes {")
    w("namespace node_compat {")
    w("")
    w("static const EmbeddedModule kModules[] = {")
    for module_id, is_bootstrap in sorted(modules, key=lambda m: m[0]):
        w(
            '    {"%s", nullptr, 0, sh_export_%s, %s},'
            % (module_id, unit_name(module_id),
               "true" if is_bootstrap else "false")
        )
    w("};")
    w("")
    w("static const size_t kModuleCount = "
      "sizeof(kModules) / sizeof(kModules[0]);")
    w("")
    w("const EmbeddedModule *findEmbeddedModule(const char *id) {")
    w("  auto it = std::lower_bound(")
    w("      kModules, kModules + kModuleCount, id,")
    w("      [](const EmbeddedModule &m, const char *key) {")
    w("        return std::strcmp(m.id, key) < 0;")
    w("      });")
    w("  if (it != kModules + kModuleCount && "
      "std::strcmp(it->id, id) == 0)")
    w("    return it;")
    w("  return nullptr;")
    w("}")
    w("")
    w("} // namespace node_compat")
    w("} // namespace hermes")
    w("")

    with open(sys.argv[2], "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it against the real manifest and check the output**

The combined manifest only exists after a configure; use the one in the build
directory.

```bash
python3 tools/gen-native-registry.py \
  cmake-build-release/lib/embedded-modules/combined-manifest.txt \
  /tmp/reg-native.cpp
grep -c '^SHUnit \*sh_export_hn_b_' /tmp/reg-native.cpp
grep -c '^    {"' /tmp/reg-native.cpp
grep -n 'hermesNodeNativeBuiltinsMarker' /tmp/reg-native.cpp
python3 -c "
s = open('/tmp/reg-native.cpp').read()
i_marker = s.index('hermesNodeNativeBuiltinsMarker = 1')
i_ns = s.index('namespace hermes {')
assert i_marker < i_ns, 'marker must be at global scope, before the namespace'
assert 'extern \"C\" const char hermesNodeNativeBuiltinsMarker' in s
print('marker OK: extern C, global scope')
"
```

Expected: `187` declarations, `187` table rows, the marker found, and
`marker OK`.

- [ ] **Step 3: Check the table is sorted, since the lookup binary-searches it**

```bash
python3 -c "
import re
rows = re.findall(r'^    \{\"([^\"]+)\"', open('/tmp/reg-native.cpp').read(), re.M)
assert rows == sorted(rows), 'table is not sorted by id'
assert len(rows) == len(set(rows)), 'duplicate ids in the table'
print('sorted and unique:', len(rows))
"
```

Expected: `sorted and unique: 187`.

- [ ] **Step 4: Commit**

Nothing is wired up yet, so there is nothing to build. The generator is
exercised by the checks above and will be exercised by CMake in Task 5.

```bash
git add tools/gen-native-registry.py
git commit -m "Generate a native embedded module registry"
```

---
## Task 5: Compile the built-ins to a native archive

**Files:**
- Modify: `lib/embedded-modules/CMakeLists.txt`
- Modify: `tools/gen-native-registry.py` (add the cross-check, Step 4)

**Interfaces:**
- Consumes: `HERMES_NODE_JS_LANGUAGE_FLAGS`, `tools/embedded_module_ids.py`
  (Task 3), `tools/gen-native-registry.py` (Task 4).
- Produces: CMake target `hermesNodeBuiltinsNative`, a `STATIC`
  `EXCLUDE_FROM_ALL` library at `$<TARGET_FILE:hermesNodeBuiltinsNative>`.
  Task 6 copies that file into the kit.

**Where the rules go.** Inside the two existing `foreach` loops, right after
each computes `INPUT_FOR_HERMESC` and `SAFE_ID`, plus one more for the
generated `vendored-packages`. Not in a third loop: the question "which file is
this module's compiler input" already has an answer at that point, and asking
it twice is how the two pipelines would come to disagree about a module.

**The unit name has two derivations and they are cross-checked, not trusted.**
CMake needs a unit name inside the loops, before `COMBINED_MANIFEST` exists, so
it cannot ask the Python helper for one. It therefore derives `hn_b_${SAFE_ID}`
itself, records every `<module_id> <unit_name>` pair it used, and
`gen-native-registry.py` hard-fails if any pair disagrees with its own
derivation. Without that check, extending `safe_id()` in Python -- which is
exactly what its own diagnostic tells you to do -- would make the registry
declare `sh_export_<new>` while shermes emitted `sh_export_<old>`, and the link
would fail naming a unit nobody wrote.

**Include directories.** `${PROJECT_SOURCE_DIR}/hermes/API` is **required** and
easy to miss: `embedded_modules.h` includes `napi/hermes_napi.h` (Task 2) and
that header lives at `hermes/API/napi/hermes_napi.h`. Every existing includer
gets the directory transitively from `hermesNapi`; this target links nothing,
so it must name it, the way `lib/event-loop/CMakeLists.txt:13` does. The two
`--sh-include` directories `make-kit.py` stages into the kit
(`tools/hermes-node/CMakeLists.txt:135-136`) are what the generated C needs.

- [ ] **Step 1: Declare the shermes binary, output directory, and mapping file**

Near the top of `lib/embedded-modules/CMakeLists.txt`, beside `hermesc_EXE`:

```cmake
set(shermes_EXE ${HERMES_TOOLS_OUTPUT_DIR}/shermes${CMAKE_EXECUTABLE_SUFFIX})
set(NATIVE_C_DIR ${CMAKE_CURRENT_BINARY_DIR}/native-c)
file(MAKE_DIRECTORY ${NATIVE_C_DIR})
set(ALL_NATIVE_C_FILES "")

# Every <module id> <unit name> pair CMake derived, cross-checked by
# gen-native-registry.py against its own derivation. Truncated here, before
# either loop appends to it, so a reconfigure does not accumulate duplicates.
set(NATIVE_UNIT_MAP ${CMAKE_CURRENT_BINARY_DIR}/native-unit-names.txt)
file(WRITE ${NATIVE_UNIT_MAP} "")
```

- [ ] **Step 2: Add the macro**

After those variables. A macro rather than three copies, because the third copy
is where a flag goes missing. `UNIT_NAME` is computed **once** and used for
both the recorded pair and `-exported-unit=`, so the cross-check cannot be
comparing an expression against itself.

```cmake
# Emits the rule that turns one built-in module's compiler input into
# generated C, records the unit name it used, and appends the C file to
# ALL_NATIVE_C_FILES.
#
# The input is the SAME file the bytecode pipeline compiles -- already
# CJS-wrapped for an ordinary module, raw for a bootstrap one -- which is what
# makes line and column parity with the bytecode build structural rather than
# something a test has to assert. wrap-cjs.py puts the wrapper header on the
# same line as source line 1 for exactly that reason.
macro(add_native_builtin MODULE_ID SAFE_ID INPUT_FILE)
  set(_unit_name "hn_b_${SAFE_ID}")
  file(APPEND ${NATIVE_UNIT_MAP} "${MODULE_ID} ${_unit_name}\n")
  set(_native_c "${NATIVE_C_DIR}/${SAFE_ID}.c")
  get_filename_component(_native_subdir "${_native_c}" DIRECTORY)
  file(MAKE_DIRECTORY "${_native_subdir}")
  add_custom_command(
    OUTPUT "${_native_c}"
    COMMAND ${shermes_EXE}
            -emit-c
            # shermes defaults to -g0, which emits no source locations at all.
            # Without this every built-in frame in a stack trace prints
            # "(native)" with no file or line. Matches what build-native
            # passes for a user's modules (native_compile.cpp).
            -g2
            -O
            # A built-in module reads primordials, internalBinding and process
            # as undeclared globals. The bytecode pipeline silences the same
            # diagnostics with -Wno-undefined-variable.
            -w
            # The bytecode pipeline ignores a //# sourceMappingURL comment;
            # shermes would honour one and rewrite the location table.
            -sm-comment=off
            ${HERMES_NODE_JS_LANGUAGE_FLAGS}
            -source-name=${MODULE_ID}
            -exported-unit=${_unit_name}
            -o ${_native_c}
            "${INPUT_FILE}"
    DEPENDS shermes "${INPUT_FILE}"
    COMMENT "Compiling ${MODULE_ID} to native C"
    VERBATIM
  )
  list(APPEND ALL_NATIVE_C_FILES "${_native_c}")
endmacro()
```

`list(APPEND)` inside a `macro` is correct here and a `function` would not be:
a macro runs in the caller's scope, so the append lands in the directory-level
variable the target below reads.

- [ ] **Step 3: Call it from the three places**

In the manifest `foreach`, after `set(INPUT_FOR_HERMESC ...)` (both branches
have run by then) and before the `HBC_FILE` block:

```cmake
  add_native_builtin("${MODULE_ID}" "${SAFE_ID}" "${INPUT_FOR_HERMESC}")
```

In the vendored `foreach`, after the `WRAPPED_FILE` `add_custom_command` and
before its `HBC_FILE` block:

```cmake
    add_native_builtin("${MODULE_ID}" "${SAFE_ID}" "${WRAPPED_FILE}")
```

After `configure_file(... VENDORED_PACKAGES_JS)` and before `VP_HBC_FILE`:

```cmake
add_native_builtin("vendored-packages" "${VP_SAFE_ID}" "${VENDORED_PACKAGES_JS}")
```

`VP_SAFE_ID` is already `vendored_packages`, which is
`safe_id("vendored-packages")`.

- [ ] **Step 4: Teach the generator to cross-check**

In `tools/gen-native-registry.py`, take a third argument and validate before
emitting anything:

```python
def check_cmake_names(modules, map_path):
    """Refuse to emit if CMake and Python disagree about any unit name.

    CMake derives a unit name inside its own loops, before the combined
    manifest exists, so it cannot ask this module for one. It records what it
    used instead, and this compares the two sets. Without the check, extending
    safe_id() here -- which is what its own diagnostic tells you to do --
    would have the registry declare sh_export_<new> while shermes emitted
    sh_export_<old>, and the link would fail naming a unit nobody wrote.
    """
    recorded = {}
    with open(map_path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            module_id, _, name = line.partition(" ")
            if not name:
                sys.exit("%s:%d: expected '<module id> <unit name>'"
                         % (map_path, n))
            if module_id in recorded:
                sys.exit("%s:%d: %r recorded twice" % (map_path, n, module_id))
            recorded[module_id] = name

    ours = {module_id: unit_name(module_id) for module_id, _ in modules}
    missing = sorted(set(ours) - set(recorded))
    extra = sorted(set(recorded) - set(ours))
    if missing:
        sys.exit("CMake compiled no unit for these manifest modules: %s"
                 % ", ".join(missing))
    if extra:
        sys.exit("CMake compiled units for modules the manifest does not "
                 "list: %s" % ", ".join(extra))
    for module_id in sorted(ours):
        if ours[module_id] != recorded[module_id]:
            sys.exit(
                "unit name disagreement for %r: CMake used %r, "
                "embedded_module_ids.unit_name() gives %r. Both derivations "
                "must match or the registry declares a symbol shermes never "
                "emitted." % (module_id, recorded[module_id], ours[module_id]))
```

Change `main()` to require three arguments (`<manifest> <unit-map> <output>`)
and call `validate(modules)` then `check_cmake_names(modules, unit_map)` before
emitting.

- [ ] **Step 5: Generate the registry and build the archive**

At the end of `lib/embedded-modules/CMakeLists.txt`:

```cmake
# ============================================================
# Native built-ins (EXCLUDE_FROM_ALL; cut into the kit)
# ============================================================
# The same 187 modules, compiled to Static Hermes units instead of bytecode,
# in an archive of their own. A build-native link names this archive ahead of
# the merged kit archive, so the linker resolves findEmbeddedModule() here and
# never pulls the bytecode member -- which is what keeps 2.1 MB of built-in
# bytecode out of a native artifact rather than merely unused inside it.
#
# EXCLUDE_FROM_ALL because a plain hermes-node build has no use for it and
# should not pay: measured 5.4 s wall at -j16, dominated by cc. hermes-node-kit
# depends on it, and check-hermes-node-js already depends on the kit.
set(NATIVE_REGISTRY_CPP
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_modules_registry_native.cpp")

# COMBINED_MANIFEST, not MANIFEST_FILE: the combined one also carries the
# vendored modules, which come from a CONFIGURE_DEPENDS glob, so adding or
# removing a vendored package must rebuild this table.
add_custom_command(
  OUTPUT "${NATIVE_REGISTRY_CPP}"
  COMMAND ${Python_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tools/gen-native-registry.py
          "${COMBINED_MANIFEST}" "${NATIVE_UNIT_MAP}" "${NATIVE_REGISTRY_CPP}"
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/gen-native-registry.py
          ${PROJECT_SOURCE_DIR}/tools/embedded_module_ids.py
          "${COMBINED_MANIFEST}"
          "${NATIVE_UNIT_MAP}"
  COMMENT "Generating native embedded modules registry"
  VERBATIM
)

add_library(hermesNodeBuiltinsNative STATIC EXCLUDE_FROM_ALL
  "${NATIVE_REGISTRY_CPP}"
  ${ALL_NATIVE_C_FILES}
)

target_include_directories(hermesNodeBuiltinsNative PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  # REQUIRED, and the one easy to miss: embedded_modules.h includes
  # napi/hermes_napi.h, which lives under hermes/API. Every other includer
  # gets this transitively from hermesNapi; this target links nothing, so it
  # has to name it. lib/event-loop/CMakeLists.txt does the same.
  ${PROJECT_SOURCE_DIR}/hermes/API
  # What the generated C includes: the SH headers and the generated
  # libhermesvm-config.h -- the same two directories make-kit.py stages into
  # the kit as include/ (--sh-include), so an in-tree compile and a kit
  # compile see the same headers.
  ${PROJECT_SOURCE_DIR}/hermes/include
  ${CMAKE_BINARY_DIR}/hermes/lib/config
)

# Applied to the C sources only; the registry is C++ and needs none of it.
#
# -fno-strict-aliasing and -fno-strict-overflow are REQUIRED, not tuning:
# Static Hermes's generated C reads and writes C++ objects through mirroring C
# structs, so unrelated types alias by construction. Hermes appends both to
# CMAKE_C_FLAGS inside its own subdirectory scope only
# (hermes/CMakeLists.txt), which a top-level target like this one does not
# inherit -- so they have to be named here.
#
# -std=gnu11 as a compile option rather than the C_STANDARD/C_EXTENSIONS
# target properties, which do not exist as SOURCE properties and would have
# silently done nothing. gnu rather than c11 because static_h.h uses a
# zero-length array. This is the same flag native_compile.cpp passes for the
# same generated C.
#
# NDEBUG is deliberately NOT set here. The generated C calls _SH_MODEL(),
# whose symbol is suffixed _dbg or _rel depending on whether NDEBUG was
# defined, and hermesvm_a -- which this links against -- got its answer from
# CMake's own per-config flags. Being a target in the same build is what makes
# the two agree by construction; utils/make-kit.py has to transport that
# decision by hand for the kit's own compiles, and got it wrong once.
set_source_files_properties(${ALL_NATIVE_C_FILES} PROPERTIES
  COMPILE_OPTIONS
    "-std=gnu11;-fno-strict-aliasing;-fno-strict-overflow;-w"
)

add_dependencies(hermesNodeBuiltinsNative hermes-node-version)
```

Also add `${PROJECT_SOURCE_DIR}/hermes/API` to `hermesNodeEmbeddedModules`'s
PUBLIC include directories: its public header now requires it, and relying on
the `hermesNapi` link to propagate it makes that a coincidence rather than a
declaration.

- [ ] **Step 6: Add the helper's dependency to the bytecode registry rule**

Task 3 made `gen-embedded-registry.py` import `embedded_module_ids.py`, so add
it to that rule's `DEPENDS` (`lib/embedded-modules/CMakeLists.txt`, the
`REGISTRY_CPP` command). Without it, editing the shared helper does not
regenerate the bytecode registry, and the two registries can be built from
different transforms.

- [ ] **Step 7: Configure and build the archive alone**

```bash
cmake -B cmake-build-release
time cmake --build cmake-build-release --target hermesNodeBuiltinsNative
```

Expected: builds clean, roughly 5-10 s wall on 16 cores. A module that fails is
a real finding -- the spec's central claim is that all 187 compile, verified by
spike, so a failure means a flag differs from the spike's.

- [ ] **Step 8: Prove the cross-check actually fires**

A check nobody has seen fail is a check nobody should trust.

```bash
cp cmake-build-release/lib/embedded-modules/native-unit-names.txt /tmp/unit-map.bak
sed -i.bak '1s/ hn_b_/ hn_b_WRONG_/' \
  cmake-build-release/lib/embedded-modules/native-unit-names.txt
python3 tools/gen-native-registry.py \
  cmake-build-release/lib/embedded-modules/combined-manifest.txt \
  cmake-build-release/lib/embedded-modules/native-unit-names.txt \
  /tmp/should-not-exist.cpp ; echo "exit=$?"
cp /tmp/unit-map.bak cmake-build-release/lib/embedded-modules/native-unit-names.txt
```

Expected: exit 1 with `unit name disagreement for ...: CMake used
'hn_b_WRONG_...'`.

- [ ] **Step 9: Check the archive holds what it should**

```bash
A=cmake-build-release/lib/embedded-modules/libhermesNodeBuiltinsNative.a
ar t "$A" | wc -l
nm -g "$A" 2>/dev/null | grep -c 'T _\{0,1\}sh_export_hn_b_'
nm -g "$A" 2>/dev/null | grep 'findEmbeddedModule' | wc -l
```

Expected: 188 members (187 units + the registry), 187 `sh_export_hn_b_*`, and
exactly one `findEmbeddedModule`.

- [ ] **Step 10: Prove the marker is unmangled**

This catches a missing `extern "C"`, which would otherwise surface as an
undefined symbol at every future native link.

```bash
nm -g cmake-build-release/lib/embedded-modules/libhermesNodeBuiltinsNative.a \
  2>/dev/null | grep hermesNodeNativeBuiltinsMarker
```

Expected: one line, a data symbol, with **no** `_ZN` in the name.

- [ ] **Step 11: Confirm a plain build still does not pay**

```bash
rm -rf cmake-build-release/lib/embedded-modules/native-c
cmake --build cmake-build-release --target hermes-node >/dev/null
test -d cmake-build-release/lib/embedded-modules/native-c \
  && echo "FAIL: a plain build generated native C" \
  || echo "OK: EXCLUDE_FROM_ALL holds"
```

Expected: `OK: EXCLUDE_FROM_ALL holds`.

- [ ] **Step 12: Format and commit**

```bash
./utils/format.sh -f
git commit -am "Build the built-in modules as native units"
```

Record the measured compile cost in the message, and that the two unit-name
derivations are cross-checked rather than assumed to agree.

---
## Task 6: Ship the archive in the kit

**Files:**
- Modify: `include/hermes/node-compat/build-exe/kit_manifest.h`
- Modify: `lib/build-exe/kit_manifest.cpp` (format comment at the top; the key
  dispatch at :139)
- Modify: `utils/make-kit.py` (argument parsing ~:353, manifest write ~:415)
- Modify: `tools/hermes-node/CMakeLists.txt` (the make-kit invocation ~:134,
  and a new copy rule beside `hermes-node-kit-entry` ~:176)
- Modify: `unittests/BuildExeTest.cpp`

**Interfaces:**
- Consumes: `hermesNodeBuiltinsNative` (Task 5).
- Produces: `KitManifest::nativeBuiltinsArchive` -- a `std::string`, empty when
  the kit records no such archive, `{kit}` already substituted. Task 7 reads
  it.

**Exactly one writer to the kit destination.** The copy is a tracked
`add_custom_command(OUTPUT ...)`, modelled on `hermes-node-kit-entry`
(`tools/hermes-node/CMakeLists.txt:164-188`) and for the reason recorded
there: the kit's real outputs are invisible to the build system, so an
`add_dependencies` order-only edge builds the archive but leaves a stale copy
in the kit. `make-kit.py` must **not** also copy it -- two writers to one
destination race -- so it is given only the file *name*, and writes the
manifest line.

- [ ] **Step 1: Write the failing manifest test**

Add to `unittests/BuildExeTest.cpp`, after `ParsesKeysInOrder`:

```cpp
TEST(KitManifestTest, ReadsTheNativeBuiltinsArchive) {
  TempTree tree;
  tree.write(
      "kit/kit.manifest",
      "version: 1.2.3\n"
      "cc: /usr/bin/clang\n"
      "nativebuiltins: {kit}/libhermes-node-builtins-native.a\n");
  std::string error;
  auto m = readKitManifest(tree.path("kit"), &error);
  ASSERT_TRUE(m.has_value()) << error;
  EXPECT_EQ(
      m->nativeBuiltinsArchive,
      tree.path("kit") + "/libhermes-node-builtins-native.a");
}

TEST(KitManifestTest, NativeBuiltinsArchiveIsOptional) {
  // A kit cut before this key existed still parses. The consumer decides
  // what to do about an empty value; the reader does not require it.
  TempTree tree;
  tree.write("kit/kit.manifest", "version: 1.2.3\ncc: /usr/bin/clang\n");
  std::string error;
  auto m = readKitManifest(tree.path("kit"), &error);
  ASSERT_TRUE(m.has_value()) << error;
  EXPECT_TRUE(m->nativeBuiltinsArchive.empty());
}

TEST(KitManifestTest, DuplicateNativeBuiltinsIsAnError) {
  TempTree tree;
  tree.write(
      "kit/kit.manifest",
      "version: 1.2.3\n"
      "cc: /usr/bin/clang\n"
      "nativebuiltins: {kit}/a.a\n"
      "nativebuiltins: {kit}/b.a\n");
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("kit"), &error).has_value());
  EXPECT_NE(error.find("nativebuiltins"), std::string::npos);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build cmake-build-release --target BuildExeTest
cmake-build-release/unittests/BuildExeTest --gtest_filter='KitManifestTest.*'
```

Expected: the three new cases fail -- the first two because
`nativeBuiltinsArchive` does not compile, so in practice this is a build
failure naming the member. That is the failing state.

- [ ] **Step 3: Add the field**

In `kit_manifest.h`, after `ccFlags`:

```cpp
  /// The kit's archive of natively compiled built-in modules, or empty if the
  /// kit records none (a kit cut before this key existed, or one cut by a
  /// build that did not produce it). `{kit}` already substituted.
  ///
  /// Optional on purpose, unlike `version` and `cc`: a kit without it can
  /// still link a --build-exe artifact and can still run build-native with
  /// --bytecode-builtins. The consumer reports the absence with an error that
  /// says which kit and what to rebuild; the reader does not.
  std::string nativeBuiltinsArchive; // {kit} already substituted
```

- [ ] **Step 4: Parse the key**

In `kit_manifest.cpp`, extend the format comment at the top:

```
//   nativebuiltins -- at most once, the archive of natively compiled
//                 built-in modules. `{kit}` substituted. Optional: a kit
//                 without it cannot serve `build-native` default built-ins.
```

and add a branch beside `ccflag`, before the unknown-key `else`:

```cpp
    } else if (key == "nativebuiltins") {
      if (!manifest.nativeBuiltinsArchive.empty()) {
        if (error)
          *error = manifestPath + ": duplicate key 'nativebuiltins'";
        return std::nullopt;
      }
      manifest.nativeBuiltinsArchive = substituteKitDir(value, kitDir);
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build cmake-build-release --target BuildExeTest
cmake-build-release/unittests/BuildExeTest --gtest_filter='KitManifestTest.*'
```

Expected: all pass.

- [ ] **Step 6: Teach make-kit.py to record the name**

In `utils/make-kit.py`, beside the `--shermes` argument:

```python
    ap.add_argument("--native-builtins-name", default="",
                    help="file name (not path) of the native built-ins "
                         "archive inside the kit, recorded in the manifest")
```

and where the manifest is written, after the `ccflag` loop:

```python
        # Recorded, not copied. The archive is placed in the kit by a tracked
        # CMake custom command (tools/hermes-node/CMakeLists.txt) so the build
        # system can tell when it is stale -- the kit's real outputs are
        # invisible to CMake, which is the same reason the entry object needs
        # a real OUTPUT rule. Copying it here as well would give one
        # destination two writers.
        if args.native_builtins_name:
            f.write("nativebuiltins: {kit}/%s\n" % args.native_builtins_name)
```

- [ ] **Step 7: Copy it into the kit, and pass the name**

In `tools/hermes-node/CMakeLists.txt`, add to the make-kit command line
(beside `--shermes`):

```
     --native-builtins-name libhermes-node-builtins-native.a \
```

and after the `hermes-node-kit-entry` block:

```cmake
# The native built-ins archive, copied with a real OUTPUT rule for the same
# reason the entry object above is: the kit's own outputs are written behind
# the build system's back, so a POST_BUILD step or a bare add_dependencies
# would leave a stale archive in the kit whenever the sources changed without
# the kit's stamp being invalidated. make-kit.py records the manifest line but
# deliberately does not copy the file -- one destination, one writer.
add_custom_command(
  OUTPUT ${HERMES_NODE_KIT_DIR}/libhermes-node-builtins-native.a
  COMMAND ${CMAKE_COMMAND} -E make_directory ${HERMES_NODE_KIT_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:hermesNodeBuiltinsNative>
          ${HERMES_NODE_KIT_DIR}/libhermes-node-builtins-native.a
  DEPENDS hermesNodeBuiltinsNative
  COMMENT "Copying the native built-ins archive into the kit"
  VERBATIM)
add_custom_target(hermes-node-kit-builtins
  DEPENDS ${HERMES_NODE_KIT_DIR}/libhermes-node-builtins-native.a)
add_dependencies(hermes-node-kit hermes-node-kit-builtins)
```

- [ ] **Step 8: Cut the kit and check it**

```bash
rm -rf cmake-build-release/kit
cmake -B cmake-build-release
cmake --build cmake-build-release --target hermes-node-kit
ls -la cmake-build-release/kit/
grep nativebuiltins cmake-build-release/kit/kit.manifest
```

Expected: `libhermes-node-builtins-native.a` present in the kit, and the
manifest line `nativebuiltins: <kitdir>/libhermes-node-builtins-native.a`.

- [ ] **Step 9: Check the copy is tracked, not stale**

This is the property the `OUTPUT` rule exists for, and it is worth proving
rather than assuming.

```bash
touch libjs/primordials.js
cmake --build cmake-build-release --target hermes-node-kit 2>&1 | \
  grep -c 'Copying the native built-ins archive'
```

Expected: `1` -- the source change rebuilt the archive and re-copied it. A `0`
means the rule is order-only and the kit will serve stale built-ins.

- [ ] **Step 10: Run the full suite and commit**

```bash
cmake --build cmake-build-release --target check-hermes-node
./utils/format.sh -f
git commit -am "Ship the native built-ins archive in the kit"
```

---

## Task 7: Select the native registry at link time

**Files:**
- Modify: `include/hermes/node-compat/build-exe/build_exe.h`,
  `lib/build-exe/build_exe.cpp` (extract `symbolPrefix()`; give
  `buildLinkCommand()` a `beforeLinkArgs` parameter)
- Modify: `include/hermes/node-compat/build-native/build_native.h`,
  `lib/build-native/native_compile.cpp` (`nativeBuiltinsLinkArgs()`,
  `buildNativeLinkCommand()`)
- Modify: `include/hermes/node-compat/bundle/bundle_build.h:159-166`
- Modify: `lib/bundle/bundle_build_native.cpp` (the manifest check, and the
  link at :555)
- Modify: `tools/hermes-node/hermes-node.cpp`
- Modify: `unittests/BuildNativeTest.cpp`

**Interfaces:**
- Consumes: `KitManifest::nativeBuiltinsArchive` (Task 6).
- Produces:
  - `const char *symbolPrefix(ObjectFormat)` -- `"_"` Mach-O, `""` ELF
  - `buildLinkCommand(manifest, driver, blobObject, outPath, beforeLinkArgs
    = {})` -- the existing function, with a new trailing parameter inserted
    between the objects and `manifest.linkArgs`
  - `std::vector<std::string> nativeBuiltinsLinkArgs(const std::string
    &archivePath, ObjectFormat)`
  - `std::vector<std::string> buildNativeLinkCommand(const KitManifest &,
    const std::string &driver, const std::string &blobArg, const std::string
    &outPath, bool bytecodeBuiltins, ObjectFormat)`
  - `NativeBuildOptions::bytecodeBuiltins` (`bool`, default `false`)

**Why `buildLinkCommand` grows a parameter instead of the caller splicing.** An
earlier draft had the producer call `buildLinkCommand` and then `std::find` its
way to the insertion point. That reconstructs, by search, a boundary
`buildLinkCommand` already owns (`lib/build-exe/build_exe.cpp:619`) -- and the
search is not even sound, since a `driverflag` could equal the first `linkarg`.
The function knows where the objects end; it takes the arguments.

**Why the test must cover the producer's command, not the helper.** The spec
names one misconfiguration the linker cannot catch: correct archive order with
`-u` omitted. A test over `nativeBuiltinsLinkArgs()` alone passes in exactly
that state, because the helper is not what the producer calls. So the seam is
`buildNativeLinkCommand()` -- one function the producer uses and the test
exercises.

**Argument order.** `-u` must precede the archive (GNU `ld` scans archives in
order; an undefined symbol introduced later does not go back for it), and both
must precede the merged kit archive, which arrives in `manifest.linkArgs`.

- [ ] **Step 1: Write the failing tests**

Add to `unittests/BuildNativeTest.cpp` (and the matching `using` declarations:
`buildNativeLinkCommand`, `nativeBuiltinsLinkArgs`, `symbolPrefix`,
`ObjectFormat`, `KitManifest`):

```cpp
namespace {
KitManifest nbManifest() {
  KitManifest m;
  m.kitDir = "/k";
  m.version = "0.0.0";
  m.cc = "/usr/bin/c++";
  m.driverFlags = {"-O3"};
  m.linkArgs = {"/k/libhermes-node-kit.a", "-lm"};
  m.nativeBuiltinsArchive = "/k/libhermes-node-builtins-native.a";
  return m;
}
} // namespace

TEST(NativeBuiltinsTest, RootsTheMarkerBeforeTheArchive) {
  std::vector<std::string> args =
      nativeBuiltinsLinkArgs("/k/libnb.a", ObjectFormat::MachO);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "-Wl,-u,_hermesNodeNativeBuiltinsMarker");
  EXPECT_EQ(args[1], "/k/libnb.a");
}

TEST(NativeBuiltinsTest, ElfHasNoSymbolPrefix) {
  std::vector<std::string> args =
      nativeBuiltinsLinkArgs("/k/libnb.a", ObjectFormat::ELF);
  EXPECT_EQ(args[0], "-Wl,-u,hermesNodeNativeBuiltinsMarker");
}

// The property the linker cannot check for us: the producer's OWN command
// carries the -u root, in front of the archive, in front of the merged kit
// archive. A test over nativeBuiltinsLinkArgs() alone would pass with the
// producer never calling it -- which is the exact silent regression here,
// since the artifact would still work and would simply be interpreted.
TEST(NativeBuiltinsTest, ProducerCommandCarriesTheRootInOrder) {
  std::vector<std::string> cmd = buildNativeLinkCommand(
      nbManifest(), "/usr/bin/c++", "@/tmp/objects.rsp", "/tmp/app",
      /*bytecodeBuiltins=*/false, ObjectFormat::MachO);
  auto idx = [&cmd](const std::string &s) {
    return std::find(cmd.begin(), cmd.end(), s) - cmd.begin();
  };
  auto marker = idx("-Wl,-u,_hermesNodeNativeBuiltinsMarker");
  auto archive = idx("/k/libhermes-node-builtins-native.a");
  auto kit = idx("/k/libhermes-node-kit.a");
  auto blob = idx("@/tmp/objects.rsp");
  ASSERT_LT(marker, (long)cmd.size()) << "the -u root is missing entirely";
  EXPECT_LT(blob, marker) << "objects must precede the archives";
  EXPECT_LT(marker, archive) << "-u must precede the archive it extracts";
  EXPECT_LT(archive, kit) << "the native registry must win over the merged "
                             "archive's bytecode one";
}

TEST(NativeBuiltinsTest, BytecodeModeAddsNeither) {
  std::vector<std::string> cmd = buildNativeLinkCommand(
      nbManifest(), "/usr/bin/c++", "@/tmp/objects.rsp", "/tmp/app",
      /*bytecodeBuiltins=*/true, ObjectFormat::MachO);
  for (const std::string &a : cmd) {
    EXPECT_EQ(a.find("hermesNodeNativeBuiltinsMarker"), std::string::npos);
    EXPECT_NE(a, "/k/libhermes-node-builtins-native.a");
  }
}

TEST(NativeBuiltinsTest, SymbolPrefixMatchesPayloadAssembly) {
  // payloadAssembly() computes the same prefix for its own symbols. If the
  // two disagreed, the -u root would name a symbol the archive does not
  // define and every native-builtins link would fail.
  EXPECT_EQ(std::string(symbolPrefix(ObjectFormat::MachO)), "_");
  EXPECT_EQ(std::string(symbolPrefix(ObjectFormat::ELF)), "");
  EXPECT_NE(
      payloadAssembly("/tmp/x.hbb", {}, ObjectFormat::MachO)
          .find("_hermesNodeNativeUnits"),
      std::string::npos);
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
cmake --build cmake-build-release --target BuildNativeTest
```

Expected: a build failure naming `buildNativeLinkCommand`,
`nativeBuiltinsLinkArgs` and `symbolPrefix`.

- [ ] **Step 3: Extract the prefix helper**

In `build_exe.h`, beside `hostObjectFormat()`:

```cpp
/// The prefix a linker symbol carries in \p format's object files: "_" on
/// Mach-O, nothing on ELF.
///
/// Shared rather than recomputed because two callers must agree. Emitting
/// `_hermesNodeNativeUnits` in payloadAssembly() and rooting
/// `_hermesNodeNativeBuiltinsMarker` at the link are the same question, and a
/// disagreement fails the link naming a symbol nobody wrote.
const char *symbolPrefix(ObjectFormat format);
```

In `build_exe.cpp`, define it above `payloadAssembly` and use it there:

```cpp
const char *symbolPrefix(ObjectFormat format) {
  return format == ObjectFormat::MachO ? "_" : "";
}
```

- [ ] **Step 4: Give buildLinkCommand the parameter**

In `build_exe.h`, extend the declaration with a defaulted trailing parameter so
every existing caller is unaffected:

```cpp
/// \p beforeLinkArgs go after the objects and before the manifest's own
/// linkArgs. That boundary is this function's to know -- a caller splicing
/// them in afterwards would have to rediscover it by searching the vector,
/// which is not even sound, since a driverflag can equal the first linkarg.
std::vector<std::string> buildLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobObject,
    const std::string &outPath,
    const std::vector<std::string> &beforeLinkArgs = {});
```

and in `build_exe.cpp`, update the **definition's** signature to match --
the default argument belongs on the declaration only -- and insert the loop
after the two objects and before the `manifest.linkArgs` loop:

```cpp
std::vector<std::string> buildLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobObject,
    const std::string &outPath,
    const std::vector<std::string> &beforeLinkArgs) {
  std::vector<std::string> cmd;
  cmd.push_back(driver);
  for (const std::string &flag : manifest.driverFlags)
    cmd.push_back(flag);
  cmd.push_back(blobObject);
  cmd.push_back((fs::path(manifest.kitDir) / kEntryObjectName).string());
  // After every object, before the manifest's archives: an archive
  // contributes only what is already undefined, and -u must reach the
  // native-builtins archive before the merged kit archive defines the same
  // findEmbeddedModule().
  for (const std::string &arg : beforeLinkArgs)
    cmd.push_back(arg);
  for (const std::string &arg : manifest.linkArgs)
    cmd.push_back(arg);
  cmd.push_back("-o");
  cmd.push_back(outPath);
  return cmd;
}
```

The existing comments in that function are unchanged and are not repeated
here; keep them.

- [ ] **Step 5: Add the two native helpers**

In `build_native.h`:

```cpp
/// The two link arguments that select the kit's NATIVE built-in registry, in
/// the order they must appear.
///
/// Both registries define findEmbeddedModule(). The linker takes the first
/// definition it needs, so placing \p archivePath ahead of the merged kit
/// archive resolves it here and leaves the bytecode member unpulled -- which
/// keeps 2.1 MB of built-in bytecode out of the artifact rather than merely
/// unused inside it.
///
/// Order alone would be silent when wrong: a link that picked the bytecode
/// registry would succeed and produce a working, interpreted binary. So the
/// marker is rooted with -Wl,-u, which does three jobs -- it extracts the
/// archive member, it keeps that member from -dead_strip / --gc-sections, and
/// it fails the link by name if the archive is absent. The -u MUST precede the
/// archive: GNU ld scans archives in order and does not go back.
std::vector<std::string> nativeBuiltinsLinkArgs(
    const std::string &archivePath,
    ObjectFormat format);

/// The complete link command for a native build.
///
/// This exists so the producer and its test call the SAME construction. The
/// one misconfiguration the linker cannot catch is a correct archive order
/// with the -u root missing, and a test over nativeBuiltinsLinkArgs() alone
/// would pass in exactly that state, because the producer would not be calling
/// it. \p bytecodeBuiltins omits both arguments, which is what
/// --bytecode-builtins does.
std::vector<std::string> buildNativeLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobArg,
    const std::string &outPath,
    bool bytecodeBuiltins,
    ObjectFormat format);
```

In `native_compile.cpp`:

```cpp
std::vector<std::string> nativeBuiltinsLinkArgs(
    const std::string &archivePath,
    ObjectFormat format) {
  return {
      std::string("-Wl,-u,") + symbolPrefix(format) +
          "hermesNodeNativeBuiltinsMarker",
      archivePath};
}

std::vector<std::string> buildNativeLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobArg,
    const std::string &outPath,
    bool bytecodeBuiltins,
    ObjectFormat format) {
  std::vector<std::string> before;
  if (!bytecodeBuiltins && !manifest.nativeBuiltinsArchive.empty())
    before = nativeBuiltinsLinkArgs(manifest.nativeBuiltinsArchive, format);
  return buildLinkCommand(manifest, driver, blobArg, outPath, before);
}
```

The empty-archive case returns a command with neither argument rather than
failing here: a pure command builder should not decide policy, and the caller
refuses the build earlier and with a better message (Step 7).

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build cmake-build-release --target BuildNativeTest
cmake-build-release/unittests/BuildNativeTest --gtest_filter='NativeBuiltinsTest.*'
```

Expected: all five pass.

- [ ] **Step 7: Add the option, the flag, and the early refusal**

In `bundle_build.h`, in `NativeBuildOptions`:

```cpp
  /// Link the kit's bytecode built-in registry instead of its native one, as
  /// --build-exe does. Off by default: a fully native binary is the point.
  bool bytecodeBuiltins = false;
```

In `hermes-node.cpp`'s `runBuildNativeSubcommand`, beside `--keep-temp`:

```cpp
    } else if (std::strcmp(arg, "--bytecode-builtins") == 0) {
      options.bytecodeBuiltins = true;
```

and in `printBuildNativeUsage`:

```
  --bytecode-builtins   Link interpreted built-in modules (smaller binary)
```

In `bundle_build_native.cpp`, add `#include <algorithm>` (needed by the link
code and not currently present), and refuse a kit that cannot serve the request
**immediately after the manifest is read**, before any module is compiled --
failing after a multi-minute compile for a reason known at the start is the
worst available ordering:

```cpp
  if (!options.bytecodeBuiltins && manifest->nativeBuiltinsArchive.empty()) {
    std::fprintf(
        stderr,
        "error: this kit records no native built-ins archive.\n"
        "       Rebuild it with: cmake --build <build dir> --target "
        "hermes-node-kit\n"
        "       or pass --bytecode-builtins to link the interpreted "
        "built-ins.\n");
    return 1;
  }
```

- [ ] **Step 8: Use the new command builder**

Replace the `buildLinkCommand` call at `bundle_build_native.cpp:555` with:

```cpp
    std::vector<std::string> linkCmd = buildNativeLinkCommand(
        *manifest,
        driver->driver,
        "@" + responsePath,
        options.outPath,
        options.bytecodeBuiltins,
        hostObjectFormat());
```

- [ ] **Step 9: Build and try it end to end**

```bash
cmake --build cmake-build-release --target hermes-node hermes-node-kit
cd /tmp && cat > nb-hello.js <<'EOF'
const path = require('path');
console.log('PASS', path.join('a', 'b'));
EOF
HN=/Users/tmikov/prog/hermes-node/cmake-build-release/bin/hermes-node
KIT=/Users/tmikov/prog/hermes-node/cmake-build-release/kit
$HN build-native nb-hello.js -o nb-hello --kit=$KIT --verbose 2>&1 | grep '^link:'
./nb-hello
```

Expected: `PASS a/b`, and the `link:` line shows
`-Wl,-u,_hermesNodeNativeBuiltinsMarker` immediately followed by the archive,
both before `libhermes-node-kit.a`.

- [ ] **Step 10: Check the refusal fires on a kit without the archive**

```bash
rm -rf /tmp/kit-noarchive && cp -R $KIT /tmp/kit-noarchive
rm -f /tmp/kit-noarchive/libhermes-node-builtins-native.a
grep -v '^nativebuiltins:' $KIT/kit.manifest > /tmp/kit-noarchive/kit.manifest
$HN build-native nb-hello.js -o /tmp/nb-nokit --kit=/tmp/kit-noarchive 2>&1 | head -4
$HN build-native nb-hello.js -o /tmp/nb-nokit --kit=/tmp/kit-noarchive \
  --bytecode-builtins >/dev/null && /tmp/nb-nokit
```

Expected: the first refuses with the "rebuild it with" message and compiles
nothing; the second succeeds and prints `PASS a/b`.

- [ ] **Step 11: Run the full suite and commit**

```bash
cmake --build cmake-build-release --target check-hermes-node
./utils/format.sh -f
git commit -am "Link the native built-ins by default"
```

---
## Task 8: Pin the claim, and measure the real cost

**Files:**
- Create: `test/fixtures/native/count-magic.py`
- Create: `test/build-native-builtins.js`
- Create: `docs/superpowers/plans/progress-native-builtins.md`

**Interfaces:**
- Consumes: everything through Task 7.
- Produces: the measured per-artifact size delta, recorded in the progress
  file. The spec deliberately leaves that number open; this task closes it.

**Per slice, and per slice independently.** A universal Mach-O carries a
complete linked image per architecture and therefore one `ExtensionsBytecode`
blob each; release CI builds `x86_64;arm64`. Dividing a total by the slice
count is **not** the same check -- across two slices, 0 and 2 average to 1 and
would pass while one slice was entirely wrong. So the counter parses the fat
header itself and requires **every** slice to be in range. Doing it in Python
rather than shelling out to `lipo` also removes a tool dependency and the shell
quoting around it.

**Why exactly one and not "few".** 189 = 187 embedded modules +
`InternalJavaScript` + `ExtensionsBytecode`; with Task 1 it is 188; with Tasks
2-7 an artifact should hold only `ExtensionsBytecode`. An exact number means an
accidentally-bytecoded `InternalJavaScript` fails the test instead of hiding
under a threshold.

- [ ] **Step 1: Write the counter**

Create `test/fixtures/native/count-magic.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Count Hermes bytecode blobs in a binary, independently per architecture.

Usage: count-magic.py <binary> <min> [<max>]

Prints one "MAGIC <arch-index> <n>" line per slice and exits non-zero if any
slice falls outside [min, max]. max defaults to min.

EVERY slice, not the average over slices: a universal Mach-O carries a
complete linked image per architecture, and 0 blobs in one slice with 2 in the
other averages to 1, which is exactly the wrong answer passing.

The fat header is parsed here rather than shelling out to `lipo` so the test
needs no extra tool and no shell quoting. A thin file is treated as one slice
covering the whole file.
"""

import struct
import sys

MAGIC = struct.pack("<Q", 0x1F1903C103BC1FC6)

FAT_MAGIC = 0xCAFEBABE      # fat header, 32-bit entries, big-endian fields
FAT_MAGIC_64 = 0xCAFEBABF   # fat header, 64-bit entries
FAT_CIGAM = 0xBEBAFECA      # byte-swapped forms; see slices()
FAT_CIGAM_64 = 0xBFBAFECA


def slices(data):
    """[(offset, size)] per architecture, or one entry for a thin file."""
    if len(data) < 8:
        return [(0, len(data))]
    magic = struct.unpack(">I", data[0:4])[0]
    if magic in (FAT_CIGAM, FAT_CIGAM_64):
        # A little-endian fat header. Apple does not produce these, so rather
        # than write a byte-swapping path nothing here can exercise, refuse:
        # silently treating it as a thin file would count every slice at once
        # and report a number this test would misread as a pass.
        sys.exit("count-magic: byte-swapped fat header is not supported")
    if magic not in (FAT_MAGIC, FAT_MAGIC_64):
        return [(0, len(data))]

    wide = magic == FAT_MAGIC_64
    nfat = struct.unpack(">I", data[4:8])[0]
    entry = 32 if wide else 20
    out = []
    for i in range(nfat):
        at = 8 + i * entry
        if at + entry > len(data):
            sys.exit("count-magic: fat header runs past the end of the file")
        if wide:
            off, size = struct.unpack(">QQ", data[at + 8:at + 24])
        else:
            off, size = struct.unpack(">II", data[at + 8:at + 16])
        if off + size > len(data):
            sys.exit("count-magic: slice %d runs past the end of the file" % i)
        out.append((off, size))
    if not out:
        sys.exit("count-magic: fat header declares no architectures")
    return out


def count(buf):
    n, at = 0, buf.find(MAGIC)
    while at != -1:
        n += 1
        at = buf.find(MAGIC, at + 1)
    return n


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    data = open(sys.argv[1], "rb").read()
    lo = int(sys.argv[2])
    hi = int(sys.argv[3]) if len(sys.argv) == 4 else lo

    bad = False
    for i, (off, size) in enumerate(slices(data)):
        n = count(data[off:off + size])
        print("MAGIC %d %d" % (i, n))
        if n < lo or n > hi:
            print(
                "count-magic: %s slice %d has %d, expected %d..%d"
                % (sys.argv[1], i, n, lo, hi),
                file=sys.stderr,
            )
            bad = True
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Check the counter against binaries that exist now**

Before trusting it in a test, point it at known quantities.

```bash
python3 test/fixtures/native/count-magic.py cmake-build-release/bin/hermes-node 188
echo "exit=$? (expect 0: 187 modules + ExtensionsBytecode, InternalJavaScript native)"
python3 test/fixtures/native/count-magic.py cmake-build-release/bin/hermes-node 1
echo "exit=$? (expect 1: the wrong expectation must fail)"
```

Expected: the first prints `MAGIC 0 188` and exits 0; the second exits 1 with
the diagnostic. If the first reports 189, Task 1's option did not take in this
build directory.

- [ ] **Step 3: Write the lit test**

Create `test/build-native-builtins.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The built-in modules are natively compiled by default, and the artifact
// says so in the one way that cannot be faked: it carries no Hermes bytecode
// beyond the single blob Hermes itself runs at runtime creation.
//
// REQUIRES: linker-available, shermes-available

'use strict';
var path = require('path');
var assert = require('assert');
assert.strictEqual(path.join('a', 'b'), 'a/b');
console.log('PASS');

// Built twice from this same file: once with the default (native built-ins)
// and once declining them. No --bake-wasm anywhere -- a baked Wasm entry is
// Hermes bytecode inside the container and would break the exact count below.
// RUN: rm -rf %t.nb && mkdir -p %t.nb
// RUN: %hermes-node build-native %s -o %t.nb/native --kit=%kit_dir
// RUN: %hermes-node build-native %s -o %t.nb/bytecode --kit=%kit_dir --bytecode-builtins

// Both must run, and run identically. A binary that links the wrong registry
// still works -- that is exactly why the counts below exist -- so this check
// alone would prove nothing about which one was linked.
// RUN: %t.nb/native | %FileCheck %s
// RUN: %t.nb/bytecode | %FileCheck %s

// One bytecode blob per architecture slice in the default artifact: Hermes's
// own ExtensionsBytecode, which loadAndInstallExtensions() runs at every
// runtime creation and which nothing in this repo can reach. The 187 embedded
// modules and Hermes's InternalJavaScript are both native, so neither
// contributes. An EXACT number, not a bound: a threshold would let an
// accidentally-bytecoded InternalJavaScript unit pass.
// RUN: python3 %S/fixtures/native/count-magic.py %t.nb/native 1

// And --bytecode-builtins must carry the built-ins it asked for. A floor
// rather than an exact number: the point of this line is only that the flag
// did something, and a floor cannot be broken by an unrelated blob appearing.
// RUN: python3 %S/fixtures/native/count-magic.py %t.nb/bytecode 180 9999

// CHECK: PASS
```

- [ ] **Step 4: Run it**

```bash
python3 cmake-build-release/bin/hermes-lit \
  $(pwd)/test/build-native-builtins.js \
  --param hermes_node=$(pwd)/cmake-build-release/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-release/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-release/bin/FileCheck \
  --param not=$(pwd)/cmake-build-release/bin/not \
  --param source_dir=$(pwd) \
  --param test_exec_root=$(pwd)/cmake-build-release/test \
  --param hello_addon=$(pwd)/cmake-build-release/hello_addon.node \
  --param kit_dir=$(pwd)/cmake-build-release/kit
```

Expected: PASS. If the default artifact reports more than 1, the native
registry was not selected -- re-run `build-native --verbose` and check the
`link:` line for the `-u` flag and the archive's position relative to
`libhermes-node-kit.a`.

- [ ] **Step 5: Measure the real per-artifact delta**

The spec states a range and defers the number to here.

```bash
cd /tmp
HN=/Users/tmikov/prog/hermes-node/cmake-build-release/bin/hermes-node
KIT=/Users/tmikov/prog/hermes-node/cmake-build-release/kit
$HN build-native nb-hello.js -o nb-native   --kit=$KIT
$HN build-native nb-hello.js -o nb-bytecode --kit=$KIT --bytecode-builtins
a=$(stat -f%z nb-bytecode 2>/dev/null || stat -c%s nb-bytecode)
b=$(stat -f%z nb-native   2>/dev/null || stat -c%s nb-native)
echo "bytecode built-ins: $a"; echo "native built-ins: $b"; echo "delta: $((b-a))"
```

Then a real program, where relative growth is what matters:

```bash
cd /Users/tmikov/prog/hermes-node/examples/ditz2
if [ -d node_modules ]; then
  ./build-cjs.sh >/dev/null
  $HN build-native dist-cjs/cli/main.js -o /tmp/dz-native   --kit=$KIT
  $HN build-native dist-cjs/cli/main.js -o /tmp/dz-bytecode --kit=$KIT --bytecode-builtins
  ls -la /tmp/dz-native /tmp/dz-bytecode
else
  echo "examples/ditz2 not installed -- record this in the progress file"
fi
```

If ditz2 is not installed, say so in the progress file rather than skipping
silently: the trivial-program number alone overstates the cost of the feature.

- [ ] **Step 6: Start the progress file**

Create `docs/superpowers/plans/progress-native-builtins.md`, naming the plan it
tracks. Record the deltas from Step 5 with the machine and build configuration
they were taken on, and which tasks are complete.

- [ ] **Step 7: Run the full suite and commit**

```bash
cmake --build cmake-build-release --target check-hermes-node
./utils/format.sh -f
git add test/fixtures/native/count-magic.py test/build-native-builtins.js \
        docs/superpowers/plans/progress-native-builtins.md
git commit -m "Assert a native artifact carries one bytecode blob"
```

The message should carry the measured delta -- that number cannot be derived
from the diff, and it is the one the spec left open.

---
## Task 9: Build, seed and green the native test corpus

**Files:**
- Create: `test/litnative.cfg`
- Create: `test/native/corpus_util.py`
- Create: `test/native/run-native.py`
- Create: `test/native/check-corpus.py`
- Create: `test/native/corpus.txt`, `test/native/excluded.txt`
- Modify: `CMakeLists.txt` (two targets, after `check-hermes-node-examples`)

**Interfaces:**
- Consumes: `build-native` with native built-ins (Task 7).
- Produces: `check-hermes-node-native`, green, plus the executed corpus size
  and the exclusion classes recorded in the progress file. The spec commits
  only to "at most 117 of 123 candidates" and leaves the real number here.

**The discovery mechanism, which does not work naively.** A lit config cannot
rewrite a `RUN:` line, so this is a second suite over the *same* source files.
Pointing lit at a `test/native/` directory whose config declares
`test_source_root = test/` does not work -- lit computes the path-in-suite by
relative path and would walk only `test/native/`. Instead the config lives at
`test/litnative.cfg` and lit is invoked over `test/` with
`--config-prefix=litnative` (`.../lit/main.py:215`), so it finds that config
instead of `lit.cfg` and walks the tree normally. `add_lit_testsuite` forwards
`ARGS` ahead of the test path (`hermes/cmake/modules/Lit.cmake:17,27,56`),
which is where it needs to be.

Three consequences, and all three are why `config.excludes` is not used for
selection: lit omits an excluded name **silently**
(`.../lit/formats/base.py`), `test/node-tests/lit.local.cfg` **assigns**
`config.excludes` and would discard anything inherited, and with a different
config prefix that local config is not loaded at all. Selection is a custom
format reading `corpus.txt`.

**This task seeds the manifests, rather than leaving them empty.** The checker
gates the suite, so empty manifests would make the target fail before lit ran
and there would be no "discovers nothing" state to observe. Seeding here also
means the correction steps that follow start from a suite that runs.

- [ ] **Step 1: Write the shared helper**

Create `test/native/corpus_util.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Shared by run-native.py and check-corpus.py.

The wrapper derives a work directory per test and deletes it before building;
the checker enforces that those directories are distinct. Both must agree on
how the name is derived, so it lives here once -- a collision would have two
parallel wrappers rmtree each other's build.
"""

import hashlib
import os

# A test whose SOLE RUN line is one of these is mechanically wrappable.
# ShTest executes EVERY RUN line, so a file with a second one runs a command
# the wrapper cannot reproduce -- which is why the criterion is "sole", not
# "contains".
RUN_TOP = "// RUN: %hermes-node %s | %FileCheck %s"
RUN_NODE = "// RUN: TEST_THREAD_ID=$$ %hermes-node %s"


def slug(rel):
    """A unique, readable directory name for a test path relative to test/.

    The readable part alone is not injective -- a/b_c.js and a_b/c.js both
    flatten to a_b_c_js -- so a digest of the real path is appended.
    """
    rel = rel.replace(os.sep, "/")
    flat = rel.replace("/", "_").replace(".", "_")
    return "%s-%s" % (flat, hashlib.sha1(rel.encode()).hexdigest()[:8])


def run_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [l.strip() for l in f if l.strip().startswith("// RUN:")]


def shape_for(rel):
    return RUN_NODE if rel.replace(os.sep, "/").startswith("node-tests/") \
        else RUN_TOP


def scan(test_dir):
    """(contains, candidates) as sets of paths relative to test_dir.

    contains  -- has a wrappable RUN line anywhere
    candidates -- its SOLE RUN line is the wrappable one
    """
    contains, candidates = set(), set()
    for root, dirs, files in os.walk(test_dir):
        dirs[:] = [d for d in dirs
                   if d not in ("native", "fixtures", "common", "Output")]
        for name in files:
            if not name.endswith((".js", ".ts")):
                continue
            rel = os.path.relpath(os.path.join(root, name), test_dir)
            rel = rel.replace(os.sep, "/")
            lines = run_lines(os.path.join(root, name))
            shape = shape_for(rel)
            if shape in lines:
                contains.add(rel)
                if lines == [shape]:
                    candidates.add(rel)
    return contains, candidates
```

- [ ] **Step 2: Write the wrapper**

Create `test/native/run-native.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Stand in for %hermes-node in the native corpus suite.

Builds the named script into a standalone executable with `hermes-node
build-native`, runs it, and forwards stdout, stderr and the exit status, so a
test written as `%hermes-node %s | %FileCheck %s` exercises natively compiled
built-in modules without its RUN line changing.

Usage:
  run-native.py --hermes-node H --kit K --work W --source-root S -- SCRIPT [ARG...]
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from corpus_util import slug


def fixtures_for(rel):
    """Which fixtures tree belongs beside the artifact for this test.

    A bundled module's __dirname roots at the EXECUTABLE's directory
    (rootDirectoryFor(), lib/bundle/bundle_run.cpp), and the producer packages
    code, not assets -- so a test that reads a data file finds nothing unless
    the right tree sits beside the artifact. Which tree differs by suite,
    because the bundle root does: a top-level test roots at test/, while a
    node-ported test at node-tests/parallel/x.js requires ../common and so
    roots at test/node-tests/, where common/fixtures.js resolves
    common/../fixtures.
    """
    if rel.startswith("node-tests/"):
        return os.path.join("test", "node-tests", "fixtures")
    return os.path.join("test", "fixtures")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hermes-node", required=True)
    ap.add_argument("--kit", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--source-root", required=True,
                    help="the repository root, not the test directory")
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    rest = args.rest
    if rest and rest[0] == "--":
        rest = rest[1:]
    if not rest:
        print("run-native: no script given", file=sys.stderr)
        return 2
    script = os.path.abspath(rest[0])
    script_args = rest[1:]

    test_dir = os.path.join(args.source_root, "test")
    rel = os.path.relpath(script, test_dir).replace(os.sep, "/")
    work = os.path.join(args.work, slug(rel))
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)

    exe = os.path.join(work, "app")
    # --kit= with an equals sign: build-native's parser matches "--kit=" by
    # prefix and sends a bare "--kit" to its unknown-option branch.
    #
    # --jobs=1 because lit already runs one worker per CPU. Without it every
    # wrapper would start hardware_concurrency() compiler jobs of its own and
    # the suite would oversubscribe the machine quadratically. These are one-
    # and two-module programs, so serial compilation costs almost nothing.
    build = subprocess.run(
        [args.hermes_node, "build-native", script, "-o", exe,
         "--kit=" + args.kit, "--jobs=1"],
        capture_output=True, text=True)
    if build.returncode != 0:
        # To stderr, both streams: the test's own stdout is what FileCheck
        # reads, and build chatter there would be checked as program output.
        sys.stderr.write(build.stdout)
        sys.stderr.write(build.stderr)
        print("run-native: build-native failed for %s" % script,
              file=sys.stderr)
        return 1

    src = os.path.join(args.source_root, fixtures_for(rel))
    dst = os.path.join(work, "fixtures")
    if os.path.isdir(src) and not os.path.exists(dst):
        os.symlink(src, dst)

    return subprocess.run([exe] + script_args).returncode


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Write the checker**

Create `test/native/check-corpus.py`:

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Reconcile the native corpus manifests against the test tree.

Usage: check-corpus.py <test dir>

Fails if a test with a wrappable RUN line is in neither manifest, if an entry
is in both, if an entry is stale, if a corpus entry is not a candidate, if an
exclusion carries no reason, if a line is duplicated, or if two corpus entries
would share a work directory.

Run by check-hermes-node-native before the suite, so the corpus cannot shrink
when someone edits a RUN line, or grow when someone adds a test.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from corpus_util import scan, slug


def read_list(path, want_reason):
    entries, problems, seen = {}, [], set()
    if not os.path.exists(path):
        return entries, ["missing manifest: " + path]
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if want_reason:
                name, sep, reason = line.partition(":")
                name = name.strip()
                if not sep or not reason.strip():
                    problems.append("%s:%d: expected '<path>: <reason>'"
                                    % (path, n))
                    continue
                value = reason.strip()
            else:
                name, value = line.strip(), ""
            if name in seen:
                problems.append("%s:%d: %s listed twice" % (path, n, name))
                continue
            seen.add(name)
            entries[name] = value
    return entries, problems


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    test_dir = os.path.abspath(sys.argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    contains, candidates = scan(test_dir)
    corpus, problems = read_list(os.path.join(here, "corpus.txt"), False)
    excluded, p2 = read_list(os.path.join(here, "excluded.txt"), True)
    problems += p2

    for rel in sorted(contains - set(corpus) - set(excluded)):
        problems.append(
            "%s has a wrappable RUN line but is in neither corpus.txt nor "
            "excluded.txt" % rel)
    for rel in sorted(set(corpus) & set(excluded)):
        problems.append("%s is in both corpus.txt and excluded.txt" % rel)
    for rel in sorted(set(corpus) | set(excluded)):
        if rel not in contains:
            problems.append(
                "%s is listed but no longer has a wrappable RUN line (stale "
                "entry, or the test was edited or deleted)" % rel)
    for rel in sorted(set(corpus) - candidates):
        if rel in contains:
            problems.append(
                "%s is in corpus.txt but is not a candidate: ShTest runs all "
                "its RUN lines and the wrapper cannot reproduce the others"
                % rel)

    # run-native.py derives a work directory per test and deletes it before
    # building. Two tests sharing one would race.
    slugs = {}
    for rel in sorted(corpus):
        s = slug(rel)
        if s in slugs:
            problems.append("%s and %s share the work directory %s"
                            % (slugs[s], rel, s))
        slugs[s] = rel

    if problems:
        for p in problems:
            print("check-corpus: " + p, file=sys.stderr)
        return 1
    print("check-corpus: %d candidates, %d in corpus, %d excluded"
          % (len(candidates), len(corpus), len(excluded)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Seed the two manifests**

Deterministic rewrite, not append: re-running this step must produce the same
two files rather than duplicating every line.

Create `test/native/corpus.txt` and `test/native/excluded.txt` with headers,
then fill them:

```bash
python3 - <<'EOF'
import os, sys
sys.path.insert(0, 'test/native')
from corpus_util import scan

CORPUS_HEADER = """\
# Tests the native-built-ins suite runs. One path per line, relative to
# test/. Reconciled against the tree by check-corpus.py, which runs before
# the suite: a test with a wrappable RUN line must appear here or in
# excluded.txt, so the corpus cannot shrink or grow without someone saying so.
"""

EXCLUDED_HEADER = """\
# Tests that have a wrappable RUN line and are nevertheless not run, one
# `<path>: <reason>` per line, relative to test/.
#
# The rule is deliberately wider than "candidate": it holds both tests that
# fail the sole-RUN criterion and tests that pass it and then fail when run.
# One list with one rule beats two lists that each need explaining.
#
# Not XFAIL. An XFAILed test stops reporting the day it becomes a real
# failure, which is why CLAUDE.md declines to quarantine the two known flaky
# tests either.
"""

EXCLUDE = {
  "test-process-version.js":
    "second RUN line passes --node-version, which a produced executable "
    "cannot accept: every argument belongs to the program",
}
for t in ["test-process-exit-code.js", "test-process-exit-event.js",
          "test-uncaught-exception-async.js", "test-uncaught-exception-io.js",
          "node-tests/parallel/test-child-process-exit-code.js",
          "node-tests/parallel/test-child-process-kill.js"]:
    EXCLUDE[t] = ("re-spawns through process.execPath, which under the "
                  "wrapper is the artifact rather than hermes-node")

contains, candidates = scan(os.path.abspath('test'))
corpus = sorted(candidates - set(EXCLUDE))
with open('test/native/corpus.txt', 'w') as f:
    f.write(CORPUS_HEADER)
    for t in corpus:
        f.write(t + "\n")
with open('test/native/excluded.txt', 'w') as f:
    f.write(EXCLUDED_HEADER)
    for t in sorted(EXCLUDE):
        f.write("%s: %s\n" % (t, EXCLUDE[t]))
print("contains", len(contains), "candidates", len(candidates),
      "corpus", len(corpus), "excluded", len(EXCLUDE))
EOF
python3 test/native/check-corpus.py test
```

Expected: `check-corpus: 123 candidates, 117 in corpus, 7 excluded`, exit 0. If
the candidate count is not 123 the tree changed since the spec was written --
record the new number in the progress file rather than forcing it. If the
checker reports an unlisted test, the seeding script and `scan()` disagree,
which is a bug in this task.

- [ ] **Step 5: Write the suite config**

Create `test/litnative.cfg`:

```python
import os
import sys
import lit.formats

# The same tests as test/lit.cfg, run through build-native so that natively
# compiled built-in modules are exercised by real programs rather than by a
# smoke test. Selected with lit's --config-prefix=litnative, which is what
# lets one source tree carry two suites: lit looks for this file instead of
# lit.cfg and then walks test/ exactly as it normally would.

source_dir = lit_config.params['source_dir']
lit_config.load_config(config, os.path.join(source_dir, 'test', 'lit.cfg'))

config.name = 'hermes-node-native'
# A tree of its own: the wrapper writes an executable per test, and sharing an
# exec root with the ordinary suite would have the two overwrite each other's
# Output directories.
config.test_exec_root = os.path.join(
    lit_config.params['test_exec_root'], 'native')

# build-native needs both halves of the kit AND the native built-ins archive.
# A kit cut before that archive existed must report UNSUPPORTED, not fail.
kit_dir = lit_config.params.get('kit_dir', '')
have_kit = (
    'linker-available' in config.available_features and
    'shermes-available' in config.available_features and
    bool(kit_dir) and
    os.path.exists(
        os.path.join(kit_dir, 'libhermes-node-builtins-native.a')))
# Marked unsupported rather than emptying the corpus: an empty corpus is a
# green run of zero tests, which reports success for a suite that never ran.
config.unsupported = not have_kit

# REPLACE the inherited %hermes-node rather than appending a second entry:
# substitutions are an ordered list and only the first match for a pattern
# ever fires. %hermes-node-cc keeps its place ahead of it, as in the parent.
wrapper = os.path.join(source_dir, 'test', 'native', 'run-native.py')
native_cmd = (
    '"%s" "%s" --hermes-node "%s" --kit "%s" --work "%s" '
    '--source-root "%s" --' % (
        sys.executable,
        wrapper,
        lit_config.params['hermes_node'],
        kit_dir,
        os.path.join(config.test_exec_root, 'build'),
        source_dir,
    ))
config.substitutions = [
    (pattern, native_cmd if pattern == '%hermes-node' else value)
    for pattern, value in config.substitutions
]

# Selection is a manifest, not config.excludes. lit omits an excluded name
# silently, so a RUN line someone edited would drop out of the corpus with
# nothing said; and test/node-tests/lit.local.cfg ASSIGNS config.excludes,
# discarding anything inherited -- and is not even loaded under this config
# prefix.
corpus = set()
with open(os.path.join(source_dir, 'test', 'native', 'corpus.txt')) as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith('#'):
            corpus.add(line.replace(os.sep, '/'))


class CorpusShTest(lit.formats.ShTest):
    """ShTest restricted to the paths named in corpus.txt."""

    def __init__(self, execute_external, allowed):
        super().__init__(execute_external)
        self.allowed = allowed

    def getTestsInDirectory(self, testSuite, path_in_suite, litConfig,
                            localConfig):
        for test in super().getTestsInDirectory(
                testSuite, path_in_suite, litConfig, localConfig):
            if '/'.join(test.path_in_suite) in self.allowed:
                yield test


config.test_format = CorpusShTest(True, corpus)
```

- [ ] **Step 6: Add the two CMake targets**

In `CMakeLists.txt`, after `check-hermes-node-examples`:

```cmake
# Reconciles test/native/corpus.txt and test/native/excluded.txt against the
# tree. A gate rather than a convention: without it the corpus shrinks
# silently the first time someone edits a RUN line, which is the whole failure
# mode the two manifests exist to prevent.
add_custom_target(check-hermes-node-native-corpus
  COMMAND ${Python_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/test/native/check-corpus.py
          ${CMAKE_CURRENT_SOURCE_DIR}/test
  COMMENT "Reconciling the native test corpus"
  VERBATIM
)

# The same JS tests again, each built into a standalone executable with
# build-native so that natively compiled built-in modules are exercised by
# real programs. Deliberately NOT part of check-hermes-node, for the reason
# check-hermes-node-examples is not: cost. Each test is a link, and the ASAN
# kit this project develops against is ~755 MB and links far slower than the
# Release one -- folding these into the primary suite would make the
# configuration used for everyday work the slowest one to test. Run it
# against cmake-build-release.
#
# --config-prefix=litnative is what lets one source tree carry two suites:
# lit reads test/litnative.cfg instead of test/lit.cfg and walks test/
# normally. Selection is test/native/corpus.txt, applied by a custom format.
add_lit_testsuite(check-hermes-node-native
  "Running hermes-node JS tests with native built-ins"
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  PARAMS ${HERMES_NODE_LIT_PARAMS}
  ARGS --config-prefix=litnative
  DEPENDS hermes hermes-node hello_addon hermes-node-kit
          check-hermes-node-native-corpus
)
```

- [ ] **Step 7: Prove the checker is a real gate**

```bash
cp test/native/corpus.txt /tmp/corpus.bak
echo "test-does-not-exist.js" >> test/native/corpus.txt
( set -o pipefail
  cmake --build cmake-build-release --target check-hermes-node-native-corpus 2>&1 | tail -3 )
echo "exit=$? (expect non-zero)"
cp /tmp/corpus.bak test/native/corpus.txt
```

Expected: a non-zero exit naming the stale entry. A zero exit means the target
is not actually running the checker.

Two details that cost a round when they were missing. `corpus.txt` is created
by this task and is **not yet tracked**, so `git checkout --` cannot restore it
-- it reports an unknown pathspec and leaves the bogus entry in place, after
which the gate blocks the suite. Copy it aside instead. And without
`pipefail`, `$?` after a pipeline is `tail`'s status, which is zero whatever
the checker did, so the check would pass while proving nothing.

- [ ] **Step 8: Run the suite and check what it discovered**

```bash
( set -o pipefail
  cmake --build cmake-build-release --target check-hermes-node-native 2>&1 \
    | tee /tmp/native-run.log | tail -40 )
echo "exit=$? (non-zero is expected here -- see below)"
```

Expected: the checker reports `123 candidates, 117 in corpus, 7 excluded`,
then lit discovers **117** tests. Some will fail; correcting that is the
rest of this task. What this step must establish is that the config loads,
the substitution fires, and the filter selects 117 rather than 0 or the
whole tree. A discovery count of 0 or ~220 is a failure of the machinery,
not of the corpus.

- [ ] **Step 9: Classify each failure against these three rules, in order**

Read the actual output -- lit prints the command and the diff -- and decide:

1. **A real defect in native built-ins.** The test fails because a module
   behaves differently compiled natively: a wrong value, a missing method, a
   changed error message that is not path-related. **Stop and report this.**
   It is what the corpus exists to find and it must not be excluded. A
   block-scoping or async-generator divergence looks like this, and the first
   thing to check is that Task 5's `shermes` command carries
   `${HERMES_NODE_JS_LANGUAGE_FLAGS}`.
2. **A missing data file.** The test reads something relative to `__dirname`
   that the producer did not package. If it lives under a `fixtures`
   directory, Task 9's symlink should already cover it -- if not, find out why
   before excluding (wrong branch in `fixtures_for()`? a different directory
   name?). If it reads something else entirely, exclude it naming the path it
   wanted.
3. **It observes the running binary.** `process.execPath`, `process.argv[0]`,
   a path in an error message that names the artifact. Exclude it, naming what
   it observed.

Move each exclusion from `corpus.txt` to `excluded.txt` as `<path>: <reason>`,
writing the reason you actually saw rather than the category.

- [ ] **Step 10: Re-run until green**

In a subshell with `pipefail` again, for the same reason as Step 7: `$?`
after a pipeline is `tail`'s status, so a red suite piped into `tail` reports
success.

```bash
( set -o pipefail
  cmake --build cmake-build-release --target check-hermes-node-native 2>&1 | tail -15 )
echo "exit=$? (expect 0)"
```

Expected: the checker reconciles, the suite is green, and the exit is 0.

- [ ] **Step 11: Confirm the corpus is not hollow**

A suite that excluded its way to green proves nothing. Check what is left.

```bash
grep -cv '^#\|^$' test/native/corpus.txt
grep -c 'node-tests/' test/native/corpus.txt
```

Expected: a corpus still holding most of the 117, and a substantial share of
the 45 node-ported tests -- those are the ones that exercise Node's own lib
code directly and are the reason the corpus is worth having. If node-ported
coverage collapsed, say so plainly in the progress file: it means the
verification is much weaker than the spec assumed.

- [ ] **Step 12: Record what the corpus turned out to be**

In the progress file: the final counts, the wall-clock time of a full
`check-hermes-node-native` run, and a one-line summary per *class* of exclusion
with how many fell into it. If anything landed in class 1, that is the
feature's most important finding and belongs at the top of the file whether or
not it was fixed.

- [ ] **Step 13: Confirm the default suite is untouched**

```bash
( set -o pipefail
  cmake --build cmake-build-release --target check-hermes-node 2>&1 | tail -8 )
echo "exit=$? (expect 0)"
```

Expected: unchanged results and exit 0. `check-hermes-node-native` is a
separate target and must not have been pulled into it.

- [ ] **Step 14: Commit**

One commit, not two. The machinery and the corpus are one deliverable: a
reviewer cannot accept the suite while rejecting the manifests it selects
from, and committing them apart would leave a revision at which
`check-hermes-node-native` is red.

```bash
./utils/format.sh -f
git add test/litnative.cfg test/native/ \
        docs/superpowers/plans/progress-native-builtins.md
git commit -am "Add the native-built-ins test suite"
```

The message should carry the executed corpus size and what the exclusions
turned out to be -- neither is derivable from a diff of manifest lines.

---

## Task 10: Document it

**Files:**
- Modify: `CLAUDE.md` (a subsection at the end of "Native Compilation")
- Modify: `docs/superpowers/plans/progress-native-builtins.md`
- Create: one `dz` issue for the Hermes extensions unit

**Interfaces:**
- Consumes: the measurements from Tasks 8 and 9.
- Produces: nothing in code.

**Read `CLAUDE.md`'s own rules first.** Its Issue Tracking section is explicit
that known defects and unfixed limitations live in `dz/`, **not** in
`CLAUDE.md`, and that duplicating an issue's content into that file makes two
records that drift. So the `ExtensionsBytecode` remainder gets an issue, and
`CLAUDE.md` gets at most a pointer.

- [ ] **Step 1: Check which `dz` to run**

```bash
grep -m1 '"version"' examples/ditz2/ditz2/package.json
dz --version 2>/dev/null || echo "no dz on PATH"
```

A `dz` on `PATH` may be used only if its version matches the pinned one to the
patch series. Otherwise build and run the bundled one **with Node**:

```bash
git submodule update --init examples/ditz2/ditz2
npm --prefix examples/ditz2/ditz2 install
npm --prefix examples/ditz2/ditz2 run build
node examples/ditz2/ditz2/dist/cli/main.js list
```

- [ ] **Step 2: File the extensions issue**

Title: `Hermes extensions bytecode is interpreted in a native build`.

Body must record: that `ExtensionsBytecode.hbc` is 1,680 bytes; that
`lib/runtime/hermes_node_runtime.cpp:813` reaches it through
`facebook::hermes::makeHermesRuntime` ->`loadAndInstallExtensions`
(`hermes/API/hermes/hermes.cpp:1557`) under `HERMES_ENABLE_CORE_EXTENSIONS`;
that it is the sole reason `test/build-native-builtins.js` asserts one blob
per slice rather than zero; that turning the option off would remove
`TextEncoder` along with the interpretation; and that the fix is a Hermes
change shaped exactly like `hermesInternalUnit`.

- [ ] **Step 3: Write the CLAUDE.md subsection**

Add at the end of the "Native Compilation" section. It must state, in the
file's existing voice, only what the code cannot:

- Built-in JavaScript is native in a `build-native` artifact, and the phase 1
  section's "Built-in JavaScript stays interpreted bytecode" line must be
  **corrected**, not merely supplemented -- leaving a contradiction in the
  file is worse than saying nothing.
- Two registries define `findEmbeddedModule`; the native archive goes ahead of
  the merged kit archive and the marker is rooted with `-Wl,-u`, which is what
  makes a wrong link loud instead of silently interpreted. The marker is
  `extern "C"` and that is load bearing.
- `HERMESVM_INTERNAL_JAVASCRIPT_NATIVE` is `FORCE`d, and why `FORCE` where
  `HERMES_ENABLE_WASM` is not.
- The measured per-artifact cost from Task 8, and `--bytecode-builtins`.
- The corpus: what it is, why it is a separate target, that selection is a
  manifest rather than `config.excludes` and why, and the two files.
- What remains interpreted, pointing at the tracker rather than restating it.
- That the JS language flags now have three copies, one of which is
  `kJSLanguageFlags` in C++, and that the corpus is the forcing function.

- [ ] **Step 4: Verify the file has no contradiction left**

```bash
grep -n 'stays embedded bytecode\|stays interpreted\|Built-in JavaScript' CLAUDE.md
```

Expected: every hit either describes the `--build-exe` path (still true) or
has been updated. A line still claiming built-ins are interpreted under
`build-native` is a defect in this task.

- [ ] **Step 5: Finish the progress file**

Mark every task complete, and make sure it carries the three numbers a future
reader will want and cannot recompute cheaply: the per-artifact size delta
(Task 8), the executed corpus size and exclusion classes (Task 9), and the
wall-clock cost of `check-hermes-node-native`.

- [ ] **Step 6: Final full run, both suites**

```bash
cmake --build cmake-build-release --target check-hermes-node
cmake --build cmake-build-release --target check-hermes-node-native
```

Expected: both green.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add dz/ CLAUDE.md docs/superpowers/plans/progress-native-builtins.md
git commit -m "Document native built-ins"
```

---

## Plan self-review

Run against the spec after Task 10, before handing off.

**Spec coverage.** Every section maps to a task: the seam -> 2; the two
registries and the marker -> 4, 7; the kit -> 5, 6; the Hermes option -> 1;
the CLI -> 7; verification -> 8, 9; costs -> 8; interactions -> nothing to
do (`--record-wasm` is already refused, baked Wasm is only a fixture
constraint, honoured in Task 8's test). The two measurements the spec
deliberately leaves open -- the real size delta and the executed corpus size
-- are Task 8 Step 5 and Task 9.

**Names used consistently across tasks.** `hn_b_<safe_id>` (constraints, 3, 4,
5); `hermesNodeNativeBuiltinsMarker` (4, 7); `nativeBuiltinsArchive` (6, 7);
`nativebuiltins:` (6); `hermesNodeBuiltinsNative` (5, 6);
`HERMES_NODE_JS_LANGUAGE_FLAGS` (3, 5); `symbolPrefix` and
`nativeBuiltinsLinkArgs` (7); `bytecodeBuiltins` (7); `check-hermes-node-native`
(9, 10).

**What external review changed, and what it validated.** Three rounds, sixteen
findings. Two were blocking: `build-native` accepts only `--kit=<dir>` and the wrapper
passed a bare `--kit`, and `hermesNodeBuiltinsNative` could not have compiled
the registry at all, because `napi/hermes_napi.h` lives under `hermes/API`,
which every existing includer gets transitively from `hermesNapi` and this
target -- linking nothing -- would not have.

Three findings changed the shape of a task rather than a line of it. The link
test exercised only the helper, so the one misconfiguration the linker cannot
catch (correct archive order, `-u` omitted) would have passed it; the seam is
now `buildNativeLinkCommand()`, which the producer and the test both call, and
`buildLinkCommand()` grew the parameter rather than having the caller
rediscover its boundary by searching the vector. The magic count divided a
total by the slice count, which is an average and lets 0 and 2 across two
slices pass as 1; it now parses the fat header and requires every slice
independently. And the corpus checker was documented as a gate while being
only a manual step, which the spec had already asked for as a target.

Review also **validated** the four mechanisms Task 9 rests on, which the plan
had flagged as reasoned-from-source rather than executed: `add_lit_testsuite`
does forward `ARGS`, ahead of the test path
(`hermes/cmake/modules/Lit.cmake:17,27,56`); `load_config` leaves
`test_source_root` at the parent's directory and later assignments stick
(`.../lit/LitConfig.py:102`, `.../lit/TestingConfig.py:83`); the
`getTestsInDirectory` signature matches and the base is a generator
(`.../lit/formats/base.py:13,26`); and `%S` is built in
(`.../lit/TestRunner.py:1274`).

**Tasks 9 and 10 were merged.** They were split as machinery and population,
but the machinery's own acceptance test is a green suite, and the corpus
cannot be populated without it -- so the split left a commit at which
`check-hermes-node-native` was red and neither half was independently
reviewable. That fails this skill's own boundary rule: split only where a
reviewer could reject one task while approving its neighbour.

**Where this plan is still most likely to be wrong.** Task 5's CMake -- the
macro appending into the caller's scope from inside two `foreach` loops, and
`set_source_files_properties` on generated sources -- is reasoned about rather
than executed. Steps 7 through 11 of that task exist to catch it early, and
Step 8 deliberately breaks the unit-name cross-check to prove the check fires
rather than assuming it would.
