# Single-file executables implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `hermes-node --build-exe=<out> <bundle.hbb>` produces a standalone
executable that runs the bundled program with no `hermes-node` and no
container present.

**Architecture:** We link rather than inject. The container becomes an
ordinary object file through `.incbin`; the system linker produces the
executable, which arrives already ad-hoc linker-signed on macOS. A build-time
"kit" -- one merged static archive, the force-loaded NAPI archive, an entry
object and a manifest -- is what `--build-exe` links against. Two link
configurations exist and share nothing at run time: `hermes-node` links the
CLI entry, an app links the bundle entry plus the payload object.

**Tech Stack:** C++17, CMake + Ninja, Python 3 (build-time launcher only),
LLVM lit + FileCheck, GTest.

**Spec:** `history/plans/2026-08-23-single-executable-design.md`

## Global Constraints

- **Never modify anything under `hermes/`.** It is a submodule. Never
  `git add hermes`, never `git submodule update`.
- **Never push.**
- **No LTO.** Bitcode objects would require a matching linker plugin on the
  end user's machine. Explicitly rejected in the spec.
- **The merged archive must never be `-force_load`ed.** It preserves
  duplicate symbols the real link never pulls both of
  (`hermes::vm::matchTypeOfIs` in two `Operations.cpp.o` members,
  `llvh::DisplayGraph` in two `GraphWriter.cpp.o`). `libhermesNapi.a` stays a
  separate file for exactly this reason.
- **Entry objects stay out of the merged archive.** If `hermes-node.cpp.o`
  were inside it, `main` would be pulled from the archive and the two link
  configurations would collapse into one.
- **These invariants hold for every produced binary**, and every task that
  produces one must check them: `nm` reports the same `napi_`-prefixed
  exported symbols as `bin/hermes-node` (145 on both platforms today), and a
  `.node` addon loads through `dlopen`.
- **The run path must stay free of the parser and compiler.**
  `hermesNodeBundleRun` links `hermesNodeBundle` and `hermesNapi` and must not
  gain `hermesNapiCompile` or `hermesvm_a` beyond what it already has.
- **`--build-exe` needs no runtime** and is dispatched from `runToolVerb()`
  before `runHermesNode`, alongside the four read-only verbs.
- ASCII-only commit messages, no emojis. `Copyright (c) Tzvetan Mikov.` on
  new files.
- Before any commit: `./utils/format.sh -f` and
  `cmake --build cmake-build-asan --target check-hermes-node`.

---

## File Structure

**Created:**

| Path | Responsibility |
|---|---|
| `utils/make-kit.py` | Build-time launcher. Intercepts CMake's link line for a probe target; merges the archives into the kit, copies the force-loaded archive, writes `kit.manifest`. The only thing that ever sees the real link line. |
| `tools/hermes-node/bundle_main.cpp` | `main()` for a produced app. Declares the payload symbols, fills a `HermesNodeConfig`, calls `runHermesNode`. Tiny and deliberately so. |
| `include/hermes/node-compat/build-exe/kit_manifest.h` | `KitManifest` struct + `readKitManifest()`. Pure parsing, unit-tested. |
| `lib/build-exe/kit_manifest.cpp` | Its implementation. |
| `include/hermes/node-compat/build-exe/build_exe.h` | `buildExecutable()` -- the whole verb behind one function. |
| `lib/build-exe/build_exe.cpp` | Container validation, `.S` generation, assemble, link, reporting. |
| `lib/build-exe/CMakeLists.txt` | `hermesNodeBuildExe`, VM-free, links `hermesNodeBundle` only. |
| `unittests/BuildExeTest.cpp` | Manifest parsing and link-line construction, with no filesystem and no linker. |
| `test/build-exe.js` | End-to-end: build a container, build an executable, run it. |
| `test/build-exe-errors.js` | Every failure mode of the verb. |
| `test/build-exe-tool-errors.js` | The flag-conflict matrix rows, both orders. |
| `test/build-exe-natives.js` | Sidecar beside the executable; missing sidecar. |
| `test/build-exe-escapes.js` | The closed world holds inside an executable. |

**Modified:**

| Path | Change |
|---|---|
| `tools/hermes-node/CMakeLists.txt` | Factor the link setup so `hermes-node` and the kit probe share it; add the `hermes-node-kit` target; add the bundle-entry object. |
| `include/hermes/node-compat/bundle/bundle_run.h` | `openEmbeddedBundle()`. |
| `lib/bundle/bundle_run.cpp` | Share the validation/indexing tail between the two open paths; root from the executable's directory. |
| `include/hermes/node-compat/runtime/hermes_node_runtime.h` | `embeddedBundleData` / `embeddedBundleSize` on `HermesNodeConfig`. |
| `lib/runtime/hermes_node_runtime.cpp` | Split `runBundle` into open + run; dispatch the embedded case. |
| `tools/hermes-node/hermes-node.cpp` | `--build-exe`, `--kit`, the conflict rows, usage text. |
| `test/lit.cfg` | `linker-available` feature and `%kit_dir` substitution. |
| `CLAUDE.md` | A "Single-file executables" section. |
| `history/plans/progress-single-executable.md` | Created and updated per task. |

---

## Task 1: The kit

Produces the artifact everything else links against. Nothing downstream can
be built or tested until this exists, so it is first.

**Files:**
- Create: `utils/make-kit.py`
- Modify: `tools/hermes-node/CMakeLists.txt`

**Interfaces:**
- Produces: a directory `${CMAKE_BINARY_DIR}/kit/` containing
  `libhermes-node-kit.a`, `libhermesNapi.a`, `kit.manifest`. (The entry object
  is added in Task 2; this task must not fail if it is absent.)
- Manifest grammar, consumed by Task 3: UTF-8 text, one `key: value` per
  line, `#` comments and blank lines ignored, repeated keys accumulate **in
  order**. Keys: `version` (once), `cc` (once), `driverflag` (repeated,
  ordered), `linkarg` (repeated, ordered). `{kit}` inside a `linkarg` value is
  replaced by the kit directory at read time.

- [ ] **Step 1: Factor hermes-node's link setup so two targets can share it**

In `tools/hermes-node/CMakeLists.txt`, replace the body with a function
applied to both targets. The existing content (whole-archive `hermesNapi`,
`-rdynamic` / `-Wl,-export_dynamic`, `--gc-sections` / `-dead_strip`, the
version dependency, the generated include dir) moves inside it verbatim --
including every comment, which explains why each line exists.

```cmake
# Everything that makes a hermes-node-shaped link: the libraries, the
# whole-archive NAPI dance, dynamic export, and dead-stripping. Applied to
# the real binary and to the kit probe below, so that the kit is cut from
# exactly the closure the binary links and the two cannot drift.
function(hermes_node_link_setup target)
  target_link_libraries(${target}
    hermesNodeRuntime
    hermesNodeBundleTools
    hermesNodeBytecodeDump
  )
  target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
  add_dependencies(${target} hermes-node-version)
  # ... the three existing if(APPLE)/elseif blocks, unchanged ...
endfunction()

add_hermes_tool(hermes-node hermes-node.cpp)
hermes_node_link_setup(hermes-node)
```

- [ ] **Step 2: Verify the refactor changed nothing**

```bash
cmake --build cmake-build-release --target hermes-node
nm -D --defined-only cmake-build-release/bin/hermes-node | grep -c ' napi_'
```
Expected: `145`, and the binary size unchanged from before the edit. Record
both numbers; if either moved, the refactor dropped something.

- [ ] **Step 3: Write `utils/make-kit.py`**

```python
#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
"""RULE_LAUNCH_LINK launcher that cuts the kit instead of linking a binary.

CMake hands us the complete link command it generated for the probe target.
That command is the only place the real closure and the real system-library
list exist -- deriving them any other way means maintaining a second copy
that drifts.  So we take it apart here:

  *.a  ........... merged into one archive, except the force-loaded one
  the force-loaded archive ... copied out whole, kept separate, because
                   merging it would make -force_load pull duplicate symbols
                   the real link never pulls both of
  *.o  ........... dropped; entry objects must not live in the archive or
                   main would be pulled from it
  everything else . recorded in kit.manifest, in order
"""
import argparse, os, re, shutil, subprocess, sys

FORCE_FLAGS_PRE = ("-force_load", "-Wl,--whole-archive")
FORCE_FLAGS_POST = ("-Wl,--no-whole-archive",)


def parse_version(header_path):
    """Read HERMES_NODE_VERSION_STRING out of the generated version header.

    The version is derived at build time into a header, not available as a
    CMake variable, which is why this reads the header rather than taking
    the value as an argument.
    """
    text = open(header_path).read()
    m = re.search(r'#define\s+HERMES_NODE_VERSION_STRING\s+"([^"]*)"', text)
    if not m:
        sys.exit(f"make-kit: no HERMES_NODE_VERSION_STRING in {header_path}")
    return m.group(1)


def merge_archives(archives, out_path):
    """One archive from many, without linking.

    libtool/ar concatenate members; they do not resolve symbols and do not
    coalesce atoms, so MH_SUBSECTIONS_VIA_SYMBOLS survives on every member
    and the final link folds and dead-strips normally.  This is the whole
    reason the kit is an archive rather than an `ld -r` object.
    """
    if os.path.exists(out_path):
        os.remove(out_path)
    if sys.platform == "darwin":
        subprocess.check_call(["libtool", "-static", "-o", out_path] + archives)
    else:
        script = "create %s\n" % out_path
        script += "".join("addlib %s\n" % a for a in archives)
        script += "save\nend\n"
        subprocess.run(["ar", "-M"], input=script, text=True, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kit-dir", required=True)
    ap.add_argument("--version-header", required=True)
    ap.add_argument("argv", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    argv = args.argv
    cc, rest = argv[0], argv[1:]

    archives, linkargs, syslibs, driverflags = [], [], [], []
    force_loaded = None
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "-o":
            i += 2
            continue
        if a in FORCE_FLAGS_PRE:
            # The next .a is the force-loaded one.  Record the flag spelling
            # verbatim so the consumer needs no platform knowledge.
            linkargs.append(a)
            j = i + 1
            while j < len(rest) and not rest[j].endswith(".a"):
                j += 1
            if j >= len(rest):
                sys.exit("make-kit: %s with no archive after it" % a)
            force_loaded = rest[j]
            linkargs.append("{kit}/" + os.path.basename(force_loaded))
            i = j + 1
            continue
        if a in FORCE_FLAGS_POST:
            linkargs.append(a)
            i += 1
            continue
        if a.endswith(".a"):
            archives.append(a)
        elif a.endswith(".o"):
            pass  # entry object -- see the module docstring
        elif a.startswith("-l") or a.endswith(".so") or a == "-framework":
            # A list of their own, deliberately. CMake emits system
            # libraries interleaved with archives, but every one of them
            # must end up AFTER every archive, or lazy resolution finds
            # nothing and the final link fails with undefined symbols that
            # look like a broken kit.
            syslibs.append(a)
            if a == "-framework":
                i += 1
                syslibs.append(rest[i])
        else:
            driverflags.append(a)
        i += 1

    if force_loaded is None:
        sys.exit("make-kit: no force-loaded archive found in the link line")
    # The force-loaded archive is named separately above; it must not also be
    # merged, or its members would appear twice.
    archives = [a for a in archives if a != force_loaded]

    os.makedirs(args.kit_dir, exist_ok=True)
    kit_archive = os.path.join(args.kit_dir, "libhermes-node-kit.a")
    merge_archives(archives, kit_archive)
    shutil.copy2(force_loaded,
                 os.path.join(args.kit_dir, os.path.basename(force_loaded)))
    # Final order: the force-load flag and its archive (appended in the
    # loop), then the merged archive, then every system library in the order
    # CMake emitted them.
    linkargs.append("{kit}/libhermes-node-kit.a")
    linkargs.extend(syslibs)

    with open(os.path.join(args.kit_dir, "kit.manifest"), "w") as f:
        f.write("# hermes-node kit manifest -- generated by "
                "utils/make-kit.py, do not edit\n")
        f.write("version: %s\n" % parse_version(args.version_header))
        f.write("cc: %s\n" % cc)
        for d in driverflags:
            f.write("driverflag: %s\n" % d)
        for a in linkargs:
            f.write("linkarg: %s\n" % a)
    print("make-kit: %d archives merged into %s" % (len(archives), kit_archive),
          file=sys.stderr)


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Verify the manifest against the real link line**

Before wiring anything into CMake, run the script by hand on the line Ninja
actually uses:

```bash
cd cmake-build-release
ninja -t commands bin/hermes-node | tail -1 | sed 's/^: && //; s/ && :$//' \
  > /tmp/orig-link.sh
python3 ../utils/make-kit.py --kit-dir /tmp/kit \
  --version-header generated/hermes/node-compat/version.h \
  -- $(cat /tmp/orig-link.sh)
cat /tmp/kit/kit.manifest
```

Expected on Linux: `linkarg:` lines reading `-Wl,--whole-archive`,
`{kit}/libhermesNapi.a`, `-Wl,--no-whole-archive`,
`{kit}/libhermes-node-kit.a`, then `-lpthread -ldl -lrt -lm` and the three ICU
`.so` paths. On macOS: `-force_load`, `{kit}/libhermesNapi.a`,
`{kit}/libhermes-node-kit.a`, `-lresolv -lpthread -lm -framework
CoreFoundation`.

(The exact version-header path may differ; find it with
`find . -name version.h -path '*generated*'`.)

- [ ] **Step 5: Prove the kit relinks a working hermes-node**

This is the task's real test. Do it by hand before adding the CMake target.

```bash
cd cmake-build-release
CC=$(sed -n 's/^cc: //p' /tmp/kit/kit.manifest)
DRIVER=$(sed -n 's/^driverflag: //p' /tmp/kit/kit.manifest | tr '\n' ' ')
LINKARGS=$(sed -n 's/^linkarg: //p' /tmp/kit/kit.manifest \
           | sed "s|{kit}|/tmp/kit|" | tr '\n' ' ')
$CC $DRIVER tools/hermes-node/CMakeFiles/hermes-node.dir/hermes-node.cpp.o \
   -o /tmp/hn-from-kit $LINKARGS
/tmp/hn-from-kit --version
nm -D --defined-only /tmp/hn-from-kit | grep -c ' napi_'      # expect 145
diff <(nm -D --defined-only bin/hermes-node | sort) \
     <(nm -D --defined-only /tmp/hn-from-kit | sort) && echo "exports identical"
ls -la bin/hermes-node /tmp/hn-from-kit
```

Expected: runs, 145, `exports identical`, and a size within a few bytes of
`bin/hermes-node` (the macOS spike measured 8 bytes). **A size several percent
larger means folding was lost and something merged wrong** -- stop and
investigate rather than proceeding.

- [ ] **Step 6: Wire the kit into CMake**

```cmake
# A target that exists only to make CMake compute hermes-node's exact link
# closure and system-library list.  The launcher intercepts the link and
# cuts the kit from it instead of producing a binary, so the kit can never
# disagree with what actually links.
set(HERMES_NODE_KIT_DIR ${CMAKE_BINARY_DIR}/kit)
add_executable(hermes-node-kit hermes-node.cpp)
hermes_node_link_setup(hermes-node-kit)
set_target_properties(hermes-node-kit PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kit-stamp
  RULE_LAUNCH_LINK
    "${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/make-kit.py \
     --kit-dir ${HERMES_NODE_KIT_DIR} \
     --version-header ${HERMES_NODE_VERSION_HEADER} --")
```

If `Python3_EXECUTABLE` is not already found in this project, add
`find_package(Python3 REQUIRED COMPONENTS Interpreter)` at top level. Check
first -- do not add a duplicate.

- [ ] **Step 7: Build the target and re-verify**

```bash
cmake --build cmake-build-release --target hermes-node-kit
ls -la cmake-build-release/kit/
```
Expected: `libhermes-node-kit.a`, `libhermesNapi.a`, `kit.manifest`. Repeat
Step 5 against `cmake-build-release/kit` and confirm the same three results.

- [ ] **Step 8: Commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add utils/make-kit.py tools/hermes-node/CMakeLists.txt
git commit
```

---

## Task 2: The embedded-bundle run path

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_run.h`,
  `lib/bundle/bundle_run.cpp`,
  `include/hermes/node-compat/runtime/hermes_node_runtime.h`,
  `lib/runtime/hermes_node_runtime.cpp`
- Create: `tools/hermes-node/bundle_main.cpp`
- Modify: `tools/hermes-node/CMakeLists.txt`

**Interfaces:**
- Consumes: the kit directory from Task 1.
- Produces, for Task 4: an entry object at `<kit>/hermes-node-bundle-main.o`
  defining `main`, and the two symbols it expects the payload object to
  define -- `hermesNodeBundleStart` and `hermesNodeBundleEnd`, both
  `extern "C"`, ELF spelling (the assembler adds the leading underscore on
  Mach-O).

- [ ] **Step 1: Add `openEmbeddedBundle` to the header**

In `include/hermes/node-compat/bundle/bundle_run.h`, after `openBundle`:

```cpp
/// Opens a bundle that was linked into this executable rather than mapped
/// from a file. \p data and \p size name the payload object's contents; the
/// linker computed the size from the two symbols, so it cannot disagree with
/// the bytes. Validation is identical to openBundle(), generation tag
/// included -- a container linked into a binary built from a different kit
/// is refused exactly as a mismatched file would be.
///
/// \p exePath is the path of the running executable, used only to derive the
/// bundle root: identities resolve against the executable's own directory,
/// and native addon sidecars sit beside it. It is passed in rather than
/// discovered here so this library keeps needing nothing but the format
/// layer -- the caller already links libuv, which answers the question
/// portably (uv_exepath).
///
/// Only one bundle can be open at a time; a second call to either open
/// function fails.
bool openEmbeddedBundle(
    const uint8_t *data,
    size_t size,
    const std::string &exePath,
    std::string *error);
```

- [ ] **Step 2: Implement it by factoring `openBundle`**

In `lib/bundle/bundle_run.cpp`, extract everything after the successful
`BundleReader::open` into a shared tail that takes an already-computed root,
so the two entry points cannot diverge on how state is published:

```cpp
/// Publishes a validated reader plus its root into the process-wide state.
/// Shared by both open paths: everything below this point is identical
/// whether the bytes came from a mapping or from the executable's own
/// __const/.rodata, which is the point.
static void publishBundle(
    OpenBundle &state,
    BundleReader reader,
    std::string rootPath) {
  state.reader = std::move(reader);
  state.root = std::move(rootPath);
  state.byIdentity.reserve(state.reader->moduleCount());
  for (uint32_t i = 0; i < state.reader->moduleCount(); ++i)
    state.byIdentity.emplace(state.reader->identity(i), i);
  state.fileSource.emplace(*state.reader, state.root);
}

/// realpath'd parent directory of \p path, falling back to the absolute
/// parent when the path cannot be canonicalized. One copy, because the file
/// case and the embedded case must agree on what "the root" means.
static std::string rootDirectoryFor(const std::string &path) {
  std::error_code ec;
  fs::path canonical = fs::canonical(fs::path(path), ec);
  return ec ? fs::absolute(fs::path(path)).parent_path().string()
            : canonical.parent_path().string();
}
```

`openBundle` keeps its mapping, its `file->release()` and every existing
comment, and ends with
`publishBundle(state, std::move(*reader), rootDirectoryFor(path));`.

The new function:

```cpp
bool openEmbeddedBundle(
    const uint8_t *data,
    size_t size,
    const std::string &exePath,
    std::string *error) {
  OpenBundle &state = openBundleState();
  if (state.reader) {
    *error = "a bundle is already open";
    return false;
  }
  std::optional<BundleReader> reader =
      BundleReader::open(data, size, bundleGenerationTag(), error);
  if (!reader)
    return false;
  // No mapping to release: the payload is a read-only section of this
  // executable, mapped by the loader and live for as long as the process.
  publishBundle(state, std::move(*reader), rootDirectoryFor(exePath));
  return true;
}
```

- [ ] **Step 3: Add the config fields**

In `include/hermes/node-compat/runtime/hermes_node_runtime.h`, on
`HermesNodeConfig`:

```cpp
  /// Non-null when this binary was linked with a bundle rather than given
  /// one. Set only by tools/hermes-node/bundle_main.cpp, which is the one
  /// translation unit that knows the payload symbols exist; the CLI never
  /// sets it, and the runtime never looks for the symbols itself. That is
  /// what makes "am I an app?" a link-time fact rather than a run-time
  /// question, with no fuse and no weak symbols.
  const uint8_t *embeddedBundleData = nullptr;
  size_t embeddedBundleSize = 0;
```

- [ ] **Step 4: Split `runBundle` and dispatch the embedded case**

In `lib/runtime/hermes_node_runtime.cpp`, rename the body after the
`openBundle` call to `runOpenBundle(napi_env, ModuleLoader &)` -- everything
from `installBundleGlobals` onward moves there unchanged. Then:

```cpp
int runBundle(napi_env env, ModuleLoader &loader, const std::string &path) {
  std::string error;
  if (!openBundle(path, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  return runOpenBundle(env, loader);
}

/// The same, for a bundle that arrived as a section of this executable.
/// uv_exepath rather than argv[0]: argv[0] is whatever the caller chose and
/// need not be a path at all, and the root decides where sidecars are found.
int runEmbeddedBundle(
    napi_env env,
    ModuleLoader &loader,
    const uint8_t *data,
    size_t size) {
  char exePath[4096];
  size_t len = sizeof(exePath);
  if (uv_exepath(exePath, &len) != 0) {
    std::fprintf(stderr, "Error: cannot determine the executable path\n");
    return 1;
  }
  std::string error;
  if (!openEmbeddedBundle(data, size, std::string(exePath, len), &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  return runOpenBundle(env, loader);
}
```

At the step-13 dispatch (around line 1342), the embedded case comes first,
because a binary carrying a payload has no other program to run:

```cpp
    if (config.embeddedBundleData != nullptr) {
      exitCode = runEmbeddedBundle(
          env, loader, config.embeddedBundleData, config.embeddedBundleSize);
    } else if (!config.bundlePath.empty()) {
```

- [ ] **Step 5: Write the app entry**

`tools/hermes-node/bundle_main.cpp`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// main() for an executable produced by --build-exe.
///
/// This is the whole difference between hermes-node and an app built from
/// it. There is no flag parsing: the bundle is the program, so every
/// argument belongs to it, exactly as a positional argument does under
/// --bundle. And there is no fuse -- the payload symbols below are resolved
/// by the object --build-exe generates, so a binary that links this file
/// always has a bundle and a binary that does not never sees these symbols.

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

#include <cstdint>

using hermes::node_compat::HermesNodeConfig;
using hermes::node_compat::runHermesNode;

extern "C" {
/// Defined by the generated payload object. Arrays rather than pointers:
/// the symbols mark positions in a read-only section, and the linker
/// computes the length as their difference, so no stored size can disagree
/// with the bytes.
extern const uint8_t hermesNodeBundleStart[];
extern const uint8_t hermesNodeBundleEnd[];
}

int main(int argc, char **argv) {
  HermesNodeConfig config;
  config.embeddedBundleData = hermesNodeBundleStart;
  config.embeddedBundleSize =
      static_cast<size_t>(hermesNodeBundleEnd - hermesNodeBundleStart);
  for (int i = 0; i < argc; ++i)
    config.argv.push_back(argv[i]);
  return runHermesNode(config);
}
```

- [ ] **Step 6: Add the entry object to the kit**

```cmake
# The app entry, compiled but never linked here: --build-exe links it
# against the kit together with the payload object it generates. An OBJECT
# library rather than a static one, because it must stay a loose .o -- see
# the entry-object constraint in the design.
add_library(hermesNodeBundleMain OBJECT bundle_main.cpp)
target_include_directories(hermesNodeBundleMain PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi)
add_custom_command(TARGET hermes-node-kit POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_OBJECTS:hermesNodeBundleMain>
          ${HERMES_NODE_KIT_DIR}/hermes-node-bundle-main.o
  COMMAND_EXPAND_LISTS)
add_dependencies(hermes-node-kit hermesNodeBundleMain)
```

- [ ] **Step 7: Prove an app runs, by hand**

```bash
cd cmake-build-release
cmake --build . --target hermes-node-kit
echo 'console.log("embedded bundle runs", 40 + 2);' > /tmp/app.js
./bin/hermes-node --build-bundle=/tmp/app.hbb /tmp/app.js
cat > /tmp/blob.s <<'EOS'
        .globl hermesNodeBundleStart
        .globl hermesNodeBundleEnd
        .p2align 4
hermesNodeBundleStart:
        .incbin "/tmp/app.hbb"
hermesNodeBundleEnd:
EOS
# Mach-O needs the section directive and leading underscores; Task 4 emits
# the right one per platform. On Linux prepend `.section .rodata`.
K=$(pwd)/kit
CC=$(sed -n 's/^cc: //p' $K/kit.manifest)
$CC -c /tmp/blob.s -o /tmp/blob.o
$CC $(sed -n 's/^driverflag: //p' $K/kit.manifest | tr '\n' ' ') \
    /tmp/blob.o $K/hermes-node-bundle-main.o -o /tmp/myapp \
    $(sed -n 's/^linkarg: //p' $K/kit.manifest | sed "s|{kit}|$K|" | tr '\n' ' ')
/tmp/myapp
```
Expected: `embedded bundle runs 42`. Then confirm the app is smaller than
`bin/hermes-node` (it links neither the CLI nor the tool verbs) and that
`nm -D --defined-only /tmp/myapp | grep -c napi_` is still 145.

- [ ] **Step 8: Commit**

Format, run `check-hermes-node`, commit.

---

## Task 3: The kit manifest reader

Small, pure, and unit-testable with no filesystem and no linker. Separated
from Task 4 so the parsing rules get their own gate.

**Files:**
- Create: `include/hermes/node-compat/build-exe/kit_manifest.h`,
  `lib/build-exe/kit_manifest.cpp`, `lib/build-exe/CMakeLists.txt`,
  `unittests/BuildExeTest.cpp`
- Modify: `CMakeLists.txt` (add_subdirectory), `unittests/CMakeLists.txt`

**Interfaces:**
- Produces, for Task 4:

```cpp
struct KitManifest {
  /// The directory this was read from. Carried on the struct because every
  /// consumer needs it -- {kit} substitution above, and the entry object at
  /// <kitDir>/hermes-node-bundle-main.o -- and threading it separately
  /// through each call is one more chance for the two to disagree.
  std::string kitDir;
  std::string version;
  std::string cc;
  std::vector<std::string> driverFlags;
  std::vector<std::string> linkArgs;  // {kit} already substituted
};
std::optional<KitManifest> readKitManifest(
    const std::string &kitDir, std::string *error);
```

- [ ] **Step 1: Write the failing test**

`unittests/BuildExeTest.cpp`:

```cpp
TEST(KitManifestTest, ParsesKeysInOrder) {
  TempTree tree;
  tree.write("kit/kit.manifest",
             "# comment\n"
             "\n"
             "version: 1.2.3\n"
             "cc: /usr/bin/clang\n"
             "driverflag: -O3\n"
             "driverflag: -rdynamic\n"
             "linkarg: -Wl,--whole-archive\n"
             "linkarg: {kit}/libhermesNapi.a\n"
             "linkarg: -Wl,--no-whole-archive\n"
             "linkarg: {kit}/libhermes-node-kit.a\n"
             "linkarg: -lpthread\n");
  std::string error;
  auto m = readKitManifest(tree.path("kit"), &error);
  ASSERT_TRUE(m.has_value()) << error;
  EXPECT_EQ(m->version, "1.2.3");
  EXPECT_EQ(m->cc, "/usr/bin/clang");
  ASSERT_EQ(m->driverFlags.size(), 2u);
  EXPECT_EQ(m->driverFlags[0], "-O3");        // order is load-bearing
  EXPECT_EQ(m->driverFlags[1], "-rdynamic");
  ASSERT_EQ(m->linkArgs.size(), 5u);
  EXPECT_EQ(m->linkArgs[1], tree.path("kit") + "/libhermesNapi.a");
  EXPECT_EQ(m->linkArgs[4], "-lpthread");     // system libs stay last
}

TEST(KitManifestTest, MissingFileIsAnError) {
  TempTree tree;
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("nope"), &error).has_value());
  EXPECT_NE(error.find("kit.manifest"), std::string::npos);
}

TEST(KitManifestTest, MissingRequiredKeyIsAnError) {
  TempTree tree;
  tree.write("kit/kit.manifest", "version: 1.2.3\nlinkarg: -lm\n");
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("kit"), &error).has_value());
  EXPECT_NE(error.find("cc"), std::string::npos);
}

TEST(KitManifestTest, UnknownKeyIsAnError) {
  // Not ignored: an unknown key means this kit was cut by a newer
  // make-kit.py that records something we would silently drop.
  TempTree tree;
  tree.write("kit/kit.manifest",
             "version: 1\ncc: cc\nlinkarg: -lm\nsysroot: /x\n");
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("kit"), &error).has_value());
  EXPECT_NE(error.find("sysroot"), std::string::npos);
}
```

`TempTree` already exists (`unittests/TempTree.h`); check its exact API and
match it rather than assuming these method names.

- [ ] **Step 2: Run it and watch it fail to compile**

Expected: no such header `kit_manifest.h`.

- [ ] **Step 3: Implement**

Parsing rules, all four exercised above: `#` and blank lines skipped; split on
the first `": "`; `version` and `cc` required and single; `driverflag` and
`linkarg` repeated and order-preserving; `{kit}` in a `linkarg` replaced with
the kit directory; any other key is an error naming the key.

- [ ] **Step 4: Wire the library and the test**

`lib/build-exe/CMakeLists.txt`:

```cmake
# Producer for --build-exe. VM-free by construction -- it reads an
# already-compiled container and runs the toolchain, so it links the format
# layer and nothing else. That is what lets BuildExeTest run with no
# runtime, the same property BundleFormatTest and BundleToolsTest rely on.
add_hermes_library(hermesNodeBuildExe STATIC
  kit_manifest.cpp
  build_exe.cpp          # added in Task 4; create it empty for now
)
target_include_directories(hermesNodeBuildExe PUBLIC
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hermesNodeBuildExe PUBLIC hermesNodeBundle)
```

and in `unittests/CMakeLists.txt`:
`add_node_compat_unittest(BuildExeTest BuildExeTest.cpp)` +
`target_link_libraries(BuildExeTest hermesNodeBuildExe)`.

- [ ] **Step 5: Run the tests, then commit**

```bash
cmake-build-asan/unittests/BuildExeTest
```
All four pass. Format, `check-hermes-node`, commit.

---

## Task 4: The producer

**Files:**
- Create: `include/hermes/node-compat/build-exe/build_exe.h`,
  `lib/build-exe/build_exe.cpp`
- Modify: `unittests/BuildExeTest.cpp`

**Interfaces:**
- Consumes: `readKitManifest` (Task 3), `BundleReader::open` and
  `bundleGenerationTag()`.
- Produces, for Task 5:

```cpp
/// Builds a standalone executable from an already-built container.
/// Returns a process exit code; every failure is reported on \p err.
int buildExecutable(
    const std::string &bundlePath,
    const std::string &outPath,
    const std::string &kitDir,
    bool verbose,
    std::ostream &out,
    std::ostream &err);
```

- [ ] **Step 1: Write the failing test for link-line construction**

The subprocess-driving part is covered end-to-end in Task 6; what deserves a
unit test here is the argument vector, which has an ordering rule that is easy
to break and invisible until a link fails. Expose it:

```cpp
/// The exact argv for the final link, given a manifest and the two objects.
/// Separated from the running of it so the ordering rule -- objects first,
/// then everything the manifest names, system libraries last -- is testable
/// without a toolchain.
std::vector<std::string> buildLinkCommand(
    const KitManifest &manifest,
    const std::string &blobObject,
    const std::string &outPath);
```

```cpp
TEST(BuildExeTest, LinkCommandPutsObjectsBeforeArchives) {
  KitManifest m;
  m.cc = "/usr/bin/clang";
  m.driverFlags = {"-O3"};
  m.linkArgs = {"-Wl,--whole-archive", "/k/libhermesNapi.a",
                "-Wl,--no-whole-archive", "/k/libhermes-node-kit.a",
                "-lpthread"};
  m.kitDir = "/k";
  auto cmd = buildLinkCommand(m, "/tmp/blob.o", "/tmp/app");
  ASSERT_GE(cmd.size(), 8u);
  EXPECT_EQ(cmd[0], "/usr/bin/clang");
  EXPECT_EQ(cmd[1], "-O3");
  // Both objects precede every archive, or lazy resolution finds nothing.
  auto idx = [&](const std::string &s) {
    return std::find(cmd.begin(), cmd.end(), s) - cmd.begin();
  };
  EXPECT_LT(idx("/tmp/blob.o"), idx("/k/libhermes-node-kit.a"));
  EXPECT_LT(idx("/k/hermes-node-bundle-main.o"), idx("/k/libhermes-node-kit.a"));
  EXPECT_LT(idx("/k/libhermes-node-kit.a"), idx("-lpthread"));
  EXPECT_LT(idx("-o"), (long)cmd.size() - 1);
}
```

The entry object is derived inside `buildLinkCommand` as
`<manifest.kitDir>/hermes-node-bundle-main.o`, which is why `kitDir` is a
field on `KitManifest` (Task 3) rather than a second parameter here.

- [ ] **Step 2: Implement `buildExecutable`**

Sequence, each step reporting its own failure and returning 1:

1. `MappedFile::open(bundlePath)`. Missing file -> name it.
2. `BundleReader::open(data, size, bundleGenerationTag(), &error)`. This is
   where a container from another hermes-node is refused, with the tag
   mismatch the reader already words.
3. `readKitManifest(kitDir)`. Missing -> say where it looked and that
   `--kit=<dir>` overrides it.
4. Compare `manifest.version` against this binary's
   `HERMES_NODE_VERSION_STRING` (from `<hermes/node-compat/version.h>`).
   Mismatch is an error naming both versions -- a kit from a different build
   than the producer.
5. Write the payload assembly to a temp file beside the output, then assemble:
   `<cc> -c <blob.s> -o <blob.o>`.
6. Link with `buildLinkCommand`.
7. Remove the temporaries. Report `wrote <out> (<n> bytes)`.

The assembly, per platform -- Mach-O needs a section directive and leading
underscores, ELF does not:

```cpp
  // .incbin rather than a generated C array: a multi-megabyte initializer
  // would explode compile time for nothing. .p2align 4 is 16 bytes,
  // comfortably above the format's kBundlePayloadAlign of 8; payload
  // offsets inside the container are relative to its start, so aligning
  // the start is what lets bytecode be executed in place.
#ifdef __APPLE__
  os << "\t.section __DATA,__const\n"
     << "\t.p2align 4\n"
     << "\t.globl _hermesNodeBundleStart\n"
     << "_hermesNodeBundleStart:\n"
     << "\t.incbin \"" << bundlePath << "\"\n"
     << "\t.globl _hermesNodeBundleEnd\n"
     << "_hermesNodeBundleEnd:\n";
#else
  os << "\t.section .rodata\n"
     << "\t.p2align 4\n"
     << "\t.globl hermesNodeBundleStart\n"
     << "hermesNodeBundleStart:\n"
     << "\t.incbin \"" << bundlePath << "\"\n"
     << "\t.globl hermesNodeBundleEnd\n"
     << "hermesNodeBundleEnd:\n";
#endif
```

Note that `bundlePath` is interpolated into a quoted assembler string. Use an
absolute path, and reject a path containing `"` or a newline with a clear
error rather than emitting broken assembly.

Run subprocesses with `posix_spawnp`/`fork`+`execvp` and wait for the exit
status -- **not** `system()`, which would re-expose the same quoting problem
through the shell. On a non-zero status, report the failing command's argv
joined by spaces so the user can rerun it.

- [ ] **Step 3: `--verbose` output**

To `err`, matching the other verbose paths: kit directory and manifest
version, container path and size, the generated assembly, the assemble and
link command lines verbatim, and the output size.

- [ ] **Step 4: Run tests, commit**

---

## Task 5: The flag surface

**Files:** Modify `tools/hermes-node/hermes-node.cpp`

- [ ] **Step 1: Add the options**

On `ToolOptions`, with a comment in the style of its neighbours:

```cpp
  /// --build-exe=<output>: link a standalone executable from the container
  /// named by the positional argument. std::nullopt when the verb was not
  /// requested; an empty value ("--build-exe=") is still a request, for an
  /// output path that cannot be written, and the two must stay
  /// distinguishable.
  std::optional<std::string> buildExe;
  /// --kit=<dir>: where to find the kit. std::nullopt means "beside this
  /// binary", resolved at use.
  std::optional<std::string> kitDir;
```

Parse alongside the others (`--build-exe=` is 12 chars, `--kit=` is 6).

- [ ] **Step 2: Add the conflict rows to `checkToolOptions`**

All after the parse loop, so flag order never matters. Each names both flags:

- `--build-exe` with `--dump`, `--extract-module`, `--dump-bytecode`,
  `--verify-natives` (two verbs, one invocation)
- `--build-exe` with `--bundle` (consuming and producing at once)
- `--build-exe` with `--build-bundle` (two producers)
- `--build-exe` with `-e`/`--eval`
- `--build-exe` with `--inspect`/`--inspect-brk`
- `--kit` without `--build-exe`
- `--build-exe=` empty -- name the flag, do not report a missing file with no
  filename in it. (`--kit=` empty is the same shape; include it.)
- `--build-exe` with no positional argument -- it needs a container.

- [ ] **Step 3: Add the dispatch branch**

In `runToolVerb`, alongside the other four:

```cpp
  if (tools.buildExe.has_value()) {
    exitCode = hermes::node_compat::buildExecutable(
        config.scriptPath, *tools.buildExe, resolveKitDir(tools.kitDir),
        config.verbose, std::cout, std::cerr);
    return true;
  }
```

`resolveKitDir` defaults to the directory of the running binary plus `kit`,
found with `uv_exepath`. **Check whether `runToolVerb` currently sets
`config.scriptPath`** -- the positional is assigned near the end of `main`,
after `runToolVerb` is called, so the container path may need to be read
directly from `argv[scriptArgIndex]` instead. Resolve this while implementing;
do not assume.

- [ ] **Step 4: Update `printUsage`**

- [ ] **Step 5: Extend the verbose consumers check**

`--verbose` currently errors when none of its four consumers is present.
`--build-exe` is a fifth; update the condition and its message.

- [ ] **Step 6: Commit**

---

## Task 6: Tests

**Files:** Create `test/build-exe.js`, `test/build-exe-errors.js`,
`test/build-exe-tool-errors.js`, `test/build-exe-natives.js`,
`test/build-exe-escapes.js`; modify `test/lit.cfg`.

- [ ] **Step 1: Add the lit feature and substitution**

A produced executable needs a linker and a kit; a source checkout without
either must report UNSUPPORTED rather than FAIL, exactly as `bundle-yargs.js`
does with `examples-installed`:

```python
# --build-exe needs a linker and a built kit. Tests that produce an
# executable opt in with `REQUIRES: linker-available` so an environment
# without a toolchain reports UNSUPPORTED rather than failing.
kit_dir = lit_config.params.get('kit_dir', '')
if kit_dir and os.path.exists(os.path.join(kit_dir, 'kit.manifest')):
    config.available_features.add('linker-available')
    config.substitutions.append(('%kit_dir', kit_dir))
```

and pass `--param kit_dir=${HERMES_NODE_KIT_DIR}` from the lit invocation in
the test CMake. Find where the other params are passed and add it there.

- [ ] **Step 2: `test/build-exe.js` -- the end-to-end case**

```js
// REQUIRES: linker-available
// RUN: %hermes-node --build-bundle=%t.hbb %s
// RUN: %hermes-node --build-exe=%t.exe --kit=%kit_dir %t.hbb
// RUN: %t.exe | %FileCheck %s
// RUN: rm -f %t.hbb && %t.exe | %FileCheck %s
console.log('PASS', 40 + 2);
// CHECK: PASS 42
```

The second run is the point: deleting the container proves the executable does
not read it.

Add cases in the same file or siblings for: arguments reaching the program
(`process.argv`), a non-zero `process.exitCode`, a multi-module bundle, and a
`--preload` recorded in the container still running before the entry.

- [ ] **Step 3: `test/build-exe-errors.js`**

Each with `%not` and a `CHECK` on the message: container does not exist;
container is not a container (feed it a `.js`); a corrupt container; kit
directory missing; `kit.manifest` missing from an otherwise-present directory;
output path in a directory that does not exist. A generation-tag mismatch
cannot be produced from a test without a second build -- assert instead that
the reader's existing mismatch message is reachable, or leave it to
`BundleFormatTest`, which already covers tag mismatch directly.

- [ ] **Step 4: `test/build-exe-tool-errors.js`**

Every row from Task 5 Step 2, **each in both flag orders**, asserting that the
message names both flags.

- [ ] **Step 5: `test/build-exe-natives.js`**

Build a bundle that records an addon, build an executable, place the sidecar
beside the executable, run it. Then remove the sidecar and assert
`MODULE_NOT_FOUND` naming the file to ship -- not `ERR_DLOPEN_FAILED`.
Model it on `test/bundle-natives.js`. `REQUIRES: linker-available` and
whatever the addon tests already require.

- [ ] **Step 6: `test/build-exe-escapes.js`**

The closed world must hold in an executable exactly as in a container. Port
the cases from `test/bundle-escapes.js`: `(0, eval)('require')`,
`global.require`, `new Function('return require')()`,
`Module.createRequire()`, and a computed specifier. Plant a `node_modules`
decoy beside the produced executable and assert it is not loaded -- the root
is the executable's directory now, so this is the case most likely to regress.

- [ ] **Step 7: Run the suite and commit**

```bash
cmake --build cmake-build-release --target hermes-node-kit
cmake --build cmake-build-asan --target check-hermes-node
```

Run the new tests explicitly against a release tree too, since that is where a
kit exists. Confirm they report UNSUPPORTED, not FAIL, when `--param kit_dir`
is absent.

---

## Task 7: Documentation

**Files:** Modify `CLAUDE.md`; create
`history/plans/progress-single-executable.md`.

- [ ] **Step 1: Add a "Single-file executables" section to `CLAUDE.md`**

After the "Bundle tooling" section, in the same voice as its neighbours --
what it does, and the reasoning a future reader could not reconstruct:

- `--build-exe=<out> <bundle.hbb>` takes a container, not an entry script,
  and why (`--build-bundle` already makes containers, and the container going
  in is inspectable with the five existing verbs first).
- We link rather than inject, and the macOS payoff: `ld64` ad-hoc signs, so
  the CodeDirectory workaround sui hand-writes is not ours to write.
- The kit is a merged archive, not an `ld -r` object, because partial linking
  drops `MH_SUBSECTIONS_VIA_SYMBOLS` -- costing folding and dead-stripping,
  ~12% of the produced binary on macOS and nothing on Linux, where sections
  already carry the granularity.
- The two constraints from the Global Constraints above: nothing may
  `-force_load` the merged archive, and entry objects stay out of it.
- The root is the executable's own directory, so sidecars sit beside the
  executable.
- What does not work: Windows, cross-compilation, universal macOS unproven
  end to end, and the Linux ICU/`libstdc++` dependency.
- The test list.

- [ ] **Step 2: Write the progress file**

Naming the plan it tracks, per the convention in `history/README.md` and the
other `progress-*.md` files.

- [ ] **Step 3: Commit**

---

## Open items deliberately left out

Named here so a reviewer does not read them as omissions:

- **Windows.** Untestable for us. Nothing in this design is Windows-hostile.
- **Universal macOS binaries.** The mechanism works on a toy case; nobody has
  cut a two-slice kit. `libtool` and `lipo` both need the same per-slice
  assertion `ld -r` does -- a missing architecture yields a valid-looking
  empty slice and exit 0.
- **The Linux ICU / `libstdc++` / `libgcc_s` dependency.** A produced Linux
  executable is not self-contained; a macOS one is. Deferred deliberately, to
  be taken after this works.
- **Teaching the read-only verbs to open a produced executable.**
- **Accepting a `.js` entry directly.** Strictly additive later, dispatched on
  the magic bytes.
