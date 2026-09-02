# Hermes VM Options Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a hermes-node command line configure the Hermes VM (`--vm=<flag>`), let an AOT container carry its program's VM options, and lock those options unless the build opted into overrides.

**Architecture:** hermes-node's hand-written parse loop collects `--vm=` values verbatim and hands only those to Hermes's own `llvh::cl` flag structs (`hermes::cli::RuntimeFlags`), which produce a `vm::RuntimeConfig` using the same mapping the `hermes` binary uses. An allowlist rejects the flags Hermes implements through `ConsoleHost`, which this runtime cannot reach. The resolved option strings ride in `HermesNodeProcessConfig` so both runtimes (user and inspector) are configured identically. Bundle format v5 adds a VM-options section and an override-allowed flag bit.

**Tech Stack:** C++17, CMake + Ninja, Hermes `llvh::cl`, GTest (`unittests/`), LLVM Lit (`test/`).

**Spec:** `docs/superpowers/specs/2026-09-02-vm-options-design.md`

## Global Constraints

- **Build with `cmake-build-asan`** (Debug, Clang, ASAN) for all development. Always Clang, never GCC.
- **Before every commit:** `./utils/format.sh -f` then `cmake --build cmake-build-asan --target check-hermes-node`. `format.sh` requires **clang-format 18 specifically** and refuses to run on another major version.
- **Copyright header on new files:** `Copyright (c) Tzvetan Mikov.` — NOT Meta Platforms. Copy the five-line header verbatim from any existing file in the same directory.
- **Commit messages: ASCII only, no emojis.**
- **Never `git add hermes`.** The submodule pointer is not ours to move. Stage named files, never `git add -A` or `git add .`.
- **hermes-node must not link `hermesCompilerDriver`.** That library defines the `hermes::cl::compilerRuntimeFlags` global (`hermes/lib/CompilerDriver/CompilerDriver.cpp:351`), which registers the same `llvh::cl` option names this feature registers. Both present means a duplicate-registration abort at static-initialisation time. Do not add it to any `target_link_libraries` reachable from `hermes-node`.
- **The two flags hermes-node forces on are `-Xes6-block-scoping` and `-Xasync-generators`.** Hermes defaults both to `false` (`hermes/include/hermes/Utils/CompilerRuntimeFlags.h:46,53`); hermes-node needs both `true`. Every task that touches config construction must preserve that.
- **Known flaky tests:** `test-inspect.js` and `test-repl-history.js` fail intermittently under the suite's parallel load and pass in isolation. A single red run naming one of these is not a regression — re-run it alone six times before investigating.

---

## File Structure

**New files:**

| File | Responsibility |
| --- | --- |
| `include/hermes/node-compat/vm-options/vm_options.h` | Public interface: the classifier (`isHonouredVmOption`), the parse-and-build entry point, and the help printer. |
| `lib/vm-options/vm_options.cpp` | The allowlist table, the `llvh::cl` parse against a synthesized argv, and the `RuntimeConfig` mapping. |
| `lib/vm-options/CMakeLists.txt` | `hermesNodeVmOptions` library. Links `hermesvm_a` (needs `RuntimeFlags.h` and `RuntimeConfig`). |
| `unittests/VmOptionsTest.cpp` | GTest for the classifier, the parse, and default preservation. |
| `test/vm-options.js` | Lit: honoured flags take effect; defaults survive; last occurrence wins. |
| `test/vm-options-errors.js` | Lit: refused flags, unknown flags, empty value, the whole conflict matrix. |
| `test/bundle-vm-options.js` | Lit: bake, dump, run, lock, override. |
| `test/build-exe-vm-options.js` | Lit: an executable inherits and locks. `REQUIRES: linker-available`. |

**Modified files:**

| File | Change |
| --- | --- |
| `include/hermes/node-compat/bundle/bundle_format.h` | Version 4 -> 5; two header fields; one flag bit; `BundleVmOptions`. |
| `include/hermes/node-compat/bundle/bundle_writer.h` + `lib/bundle/bundle_writer.cpp` | `addVmOption()`, `setAllowVmOptionsOverride()`, section layout. |
| `include/hermes/node-compat/bundle/bundle_reader.h` + `lib/bundle/bundle_reader.cpp` | `vmOptionCount()`, `vmOption(i)`, `allowsVmOptionsOverride()`, `vmOptionsTableSize()`, validation. |
| `include/hermes/node-compat/bundle/bundle_run.h` + `lib/bundle/bundle_run.cpp` | `readBundleVmOptions()`, `readEmbeddedBundleVmOptions()`. |
| `include/hermes/node-compat/bundle/bundle_build.h` + `lib/bundle/bundle_build.cpp` | `buildBundle()` gains `vmOptions` and `allowVmOptionsOverride`. |
| `lib/bundle/bundle_tools.cpp` | `VM_OPTIONS` dump section and the `vmopts` size row. |
| `include/hermes/node-compat/runtime/hermes_node_runtime.h` | `HermesNodeProcessConfig::vmOptions`. |
| `lib/runtime/hermes_node_runtime.cpp` | Build the `RuntimeConfig` from `config.process.vmOptions`. |
| `tools/hermes-node/hermes-node.cpp` | `--vm=`, `--vm-help`, `--allow-vm-options-override`, conflict matrix, usage text, container option read. |
| `tools/hermes-node/bundle_main.cpp` | Read the embedded container's options before `runHermesNode`. |
| `lib/runtime/CMakeLists.txt`, `unittests/CMakeLists.txt` | Link the new library / add the new test. |
| `CLAUDE.md` | A section documenting the feature. |

**Task order rationale:** Tasks 1-3 build the VM-options library bottom-up and wire it to a plain script, which is independently useful and testable with no format work. Tasks 4-7 add the container side. Task 8 does the executable. Task 9 documents.

---

### Task 1: The flag classifier

Standalone table of which Hermes flags hermes-node honours. No parsing yet — this task exists on its own because the allowlist is the piece with the most facts in it and it deserves its own review gate.

**Files:**
- Create: `include/hermes/node-compat/vm-options/vm_options.h`
- Create: `lib/vm-options/vm_options.cpp`
- Create: `lib/vm-options/CMakeLists.txt`
- Create: `unittests/VmOptionsTest.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(lib/vm-options)` beside the other `lib/` entries)
- Modify: `unittests/CMakeLists.txt`

**Interfaces:**
- Produces: `hermes::node_compat::VmOptionStatus`, `classifyVmOption(std::string_view flag)`, `vmOptionFlagName(std::string_view arg)`, `honouredVmOptionNames()`.

- [x] **Step 1: Write the failing test**

Create `unittests/VmOptionsTest.cpp`. Copy the copyright header from `unittests/BundleFormatTest.cpp`.

```cpp
#include "hermes/node-compat/vm-options/vm_options.h"

#include "gtest/gtest.h"

using namespace hermes::node_compat;

namespace {

TEST(VmOptionsTest, FlagNameStripsDashesAndValue) {
  EXPECT_EQ(vmOptionFlagName("-gc-max-heap=1g"), "gc-max-heap");
  EXPECT_EQ(vmOptionFlagName("--gc-max-heap=1g"), "gc-max-heap");
  EXPECT_EQ(vmOptionFlagName("-Xjit"), "Xjit");
  EXPECT_EQ(vmOptionFlagName("-Xjit=force"), "Xjit");
  // No leading dash at all is still a name; llvh::cl would reject it as a
  // positional, but classification happens first and must not crash.
  EXPECT_EQ(vmOptionFlagName("gc-max-heap=1g"), "gc-max-heap");
}

TEST(VmOptionsTest, HonouredFlagsAreHonoured) {
  // One from each family in the spec's table.
  EXPECT_EQ(classifyVmOption("gc-max-heap"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("gc-init-heap"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("gc-sanitize-handles"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("max-register-stack"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("Xjit"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("Xes6-block-scoping"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("Xasync-generators"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("enable-eval"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("test262"), VmOptionStatus::kHonoured);
}

TEST(VmOptionsTest, ConsoleHostFlagsAreRefused) {
  EXPECT_EQ(classifyVmOption("Xdump-jitcode"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xjit-crash-on-error"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xjit-emit-asserts"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xjit-emit-counters"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xjit-hc-id-limit"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("stop-after-module-init"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xheap-timeline"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xperf-prof"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xperf-prof-dir"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("sample-profiling"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("sample-profiling-freq"), VmOptionStatus::kConsoleHost);
}

TEST(VmOptionsTest, UnknownFlagsAreUnknown) {
  EXPECT_EQ(classifyVmOption("gc-max-hep"), VmOptionStatus::kUnknown);
  EXPECT_EQ(classifyVmOption(""), VmOptionStatus::kUnknown);
  // llvh::cl's own help and version printers must never be reachable: they
  // would dump the whole option registry and exit the process.
  EXPECT_EQ(classifyVmOption("help"), VmOptionStatus::kUnknown);
  EXPECT_EQ(classifyVmOption("version"), VmOptionStatus::kUnknown);
}

TEST(VmOptionsTest, HonouredListMatchesClassifier) {
  auto names = honouredVmOptionNames();
  EXPECT_EQ(names.size(), 27u);
  for (const auto &n : names)
    EXPECT_EQ(classifyVmOption(n), VmOptionStatus::kHonoured) << n;
}

} // namespace
```

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-asan --target VmOptionsTest`
Expected: FAIL — `vm_options.h` does not exist.

- [x] **Step 3: Write the header**

Create `include/hermes/node-compat/vm-options/vm_options.h`:

```cpp
/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_VM_OPTIONS_VM_OPTIONS_H
#define HERMES_NODE_COMPAT_VM_OPTIONS_VM_OPTIONS_H

#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// What hermes-node does with a Hermes VM flag named on --vm=.
enum class VmOptionStatus {
  /// Reaches vm::RuntimeConfig or vm::GCConfig. Accepted.
  kHonoured,
  /// A real Hermes flag, but one the hermes binary implements through
  /// ConsoleHost -- by calling methods on a live vm::Runtime, which this
  /// codebase deliberately never touches (see the comment at
  /// lib/runtime/hermes_node_runtime.cpp:718). Accepting it would parse
  /// cleanly and then do nothing, which is the silent no-op this project
  /// has spent several rounds removing. Refused by name.
  kConsoleHost,
  /// Not a flag we recognise. Includes llvh::cl's own -help and -version,
  /// which are deliberately not reachable: they print the entire option
  /// registry, LLVM's internal options included, and exit the process.
  kUnknown,
};

/// The bare flag name of \p arg: leading dashes and any "=value" removed.
/// "-gc-max-heap=1g" -> "gc-max-heap". Never throws; an argument with no
/// name at all yields an empty string, which classifies as kUnknown.
std::string_view vmOptionFlagName(std::string_view arg);

/// Classifies a bare flag name, as returned by vmOptionFlagName().
VmOptionStatus classifyVmOption(std::string_view flagName);

/// Every honoured flag name, in the order --vm-help prints them. Exactly
/// the names classifyVmOption() answers kHonoured for; VmOptionsTest pins
/// the two to each other.
std::vector<std::string_view> honouredVmOptionNames();

} // namespace node_compat
} // namespace hermes

#endif
```

- [x] **Step 4: Write the implementation**

Create `lib/vm-options/vm_options.cpp` with the same copyright header:

```cpp
#include "hermes/node-compat/vm-options/vm_options.h"

#include <algorithm>
#include <array>

namespace hermes {
namespace node_compat {

namespace {

/// The flags that reach vm::RuntimeConfig or vm::GCConfig, mirroring the
/// mapping the hermes binary itself uses at
/// hermes/tools/hermes/hermes.cpp:110-141. When Hermes moves, diff against
/// that function: it is the authority, not
/// hermes::cli::buildRuntimeConfig(), which omits ES6BlockScoping,
/// EnableAsyncGenerators, Test262 and every JIT field.
constexpr std::array<std::string_view, 27> kHonoured = {
    // GCConfig.
    "gc-init-heap",
    "gc-max-heap",
    "occupancy-target",
    "gc-sanitize-handles",
    "gc-sanitize-handles-random-seed",
    "gc-print-stats",
    "gc-alloc-young",
    "gc-revert-to-yg-at-tti",
    // RuntimeConfig.
    "max-register-stack",
    "Xjit",
    "Xjit-threshold",
    "Xjit-memory-limit",
    "Xes6-proxy",
    "Xes6-block-scoping",
    "Xasync-generators",
    "Xintl",
    "Xmicrotask-queue",
    "Xvm-experiment-flags",
    "Xrandomize-memory-layout",
    "track-io",
    "enable-hermes-internal",
    "Xhermes-internal-test-methods",
    "enable-eval",
    "verify-ir",
    "optimized-eval",
    "emit-async-break-check",
    "test262",
};

/// Real Hermes flags that only reach ExecuteOptions, a ConsoleHost concept.
/// Listed rather than lumped into kUnknown so the error can say why the
/// flag is refused instead of implying the user misspelled it.
constexpr std::array<std::string_view, 11> kConsoleHostOnly = {
    "sample-profiling",
    "sample-profiling-freq",
    "stop-after-module-init",
    "Xheap-timeline",
    "Xdump-jitcode",
    "Xperf-prof",
    "Xperf-prof-dir",
    "Xjit-crash-on-error",
    "Xjit-emit-asserts",
    "Xjit-emit-counters",
    "Xjit-hc-id-limit",
};

} // namespace

std::string_view vmOptionFlagName(std::string_view arg) {
  while (!arg.empty() && arg.front() == '-')
    arg.remove_prefix(1);
  size_t eq = arg.find('=');
  if (eq != std::string_view::npos)
    arg = arg.substr(0, eq);
  return arg;
}

VmOptionStatus classifyVmOption(std::string_view flagName) {
  if (std::find(kHonoured.begin(), kHonoured.end(), flagName) !=
      kHonoured.end())
    return VmOptionStatus::kHonoured;
  if (std::find(kConsoleHostOnly.begin(), kConsoleHostOnly.end(), flagName) !=
      kConsoleHostOnly.end())
    return VmOptionStatus::kConsoleHost;
  return VmOptionStatus::kUnknown;
}

std::vector<std::string_view> honouredVmOptionNames() {
  return {kHonoured.begin(), kHonoured.end()};
}

} // namespace node_compat
} // namespace hermes
```

- [x] **Step 5: Write the CMake files**

Create `lib/vm-options/CMakeLists.txt`, modelled on `lib/bundle/CMakeLists.txt`:

```cmake
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# The classifier and the RuntimeConfig mapping. Links hermesvm_a because
# hermes/VM/RuntimeFlags.h instantiates llvh::cl options and
# hermes/Public/RuntimeConfig.h defines the config being built.
#
# It must NOT link hermesCompilerDriver: that library defines the
# hermes::cl::compilerRuntimeFlags global, which registers the same option
# names this library's RuntimeFlags instance does, and llvh::cl aborts on a
# duplicate registration.
add_library(hermesNodeVmOptions STATIC
  vm_options.cpp
)

target_include_directories(hermesNodeVmOptions PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hermesNodeVmOptions PUBLIC hermesvm_a)
```

Add `add_subdirectory(lib/vm-options)` to the top-level `CMakeLists.txt` beside the other `lib/` subdirectories.

Add to `unittests/CMakeLists.txt`, following exactly how `BundleFormatTest` is declared there (read that block first and mirror it), a `VmOptionsTest` target built from `VmOptionsTest.cpp` and linked against `hermesNodeVmOptions`.

- [x] **Step 6: Run the test to verify it passes**

Run: `cmake --build cmake-build-asan --target VmOptionsTest && cmake-build-asan/unittests/VmOptionsTest`
Expected: PASS, 5 tests.

- [x] **Step 7: Format and commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/vm-options lib/vm-options unittests/VmOptionsTest.cpp unittests/CMakeLists.txt CMakeLists.txt
git commit -m "vm-options: classify which Hermes VM flags hermes-node honours"
```

---

### Task 2: Parse the flags into a RuntimeConfig

Turn a list of `--vm=` strings into a `vm::RuntimeConfig`, preserving hermes-node's own defaults. Still nothing on the command line.

**Files:**
- Modify: `include/hermes/node-compat/vm-options/vm_options.h`
- Modify: `lib/vm-options/vm_options.cpp`
- Modify: `unittests/VmOptionsTest.cpp`

**Interfaces:**
- Consumes: `classifyVmOption`, `vmOptionFlagName` (Task 1).
- Produces: `bool buildVmRuntimeConfig(const std::vector<std::string> &options, hermes::vm::RuntimeConfig *out, std::string *error)` and `std::string vmOptionsHelpText()`.

- [x] **Step 1: Write the failing test**

Append to `unittests/VmOptionsTest.cpp` (and add `#include "hermes/Public/RuntimeConfig.h"` at the top):

```cpp
TEST(VmOptionsTest, EmptyOptionsPreserveHermesNodeDefaults) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({}, &config, &error)) << error;
  // The three hermes-node forces on. Hermes defaults the first two to
  // false; regressing them is the failure mode this whole test exists for.
  EXPECT_TRUE(config.getES6BlockScoping());
  EXPECT_TRUE(config.getEnableAsyncGenerators());
  EXPECT_TRUE(config.getMicrotaskQueue());
}

TEST(VmOptionsTest, HonouredFlagChangesTheConfig) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({"-Xes6-proxy=false"}, &config, &error))
      << error;
  EXPECT_FALSE(config.getES6Proxy());
  // Untouched fields keep hermes-node's defaults.
  EXPECT_TRUE(config.getES6BlockScoping());
}

TEST(VmOptionsTest, HermesNodeDefaultsCanBeOverridden) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(
      buildVmRuntimeConfig({"-Xasync-generators=false"}, &config, &error))
      << error;
  EXPECT_FALSE(config.getEnableAsyncGenerators());
}

TEST(VmOptionsTest, LastOccurrenceWins) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig(
      {"-Xes6-proxy=false", "-Xes6-proxy=true"}, &config, &error))
      << error;
  EXPECT_TRUE(config.getES6Proxy());
}

TEST(VmOptionsTest, MemorySizeSuffixesParse) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({"-gc-max-heap=64m"}, &config, &error))
      << error;
  EXPECT_EQ(config.getGCConfig().getMaxHeapSize(), 64u * 1024 * 1024);
}

TEST(VmOptionsTest, RefusedFlagReportsWhy) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  EXPECT_FALSE(buildVmRuntimeConfig({"-Xdump-jitcode=1"}, &config, &error));
  EXPECT_NE(error.find("Xdump-jitcode"), std::string::npos);
  EXPECT_NE(error.find("ConsoleHost"), std::string::npos);
}

TEST(VmOptionsTest, UnknownFlagIsAnError) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  EXPECT_FALSE(buildVmRuntimeConfig({"-gc-max-hep=1g"}, &config, &error));
  EXPECT_NE(error.find("gc-max-hep"), std::string::npos);
}

TEST(VmOptionsTest, BadValueIsAnErrorAndDoesNotExit) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  // The point of this case is as much that the process survives as that it
  // returns false: llvh::cl calls exit() unless an Errs stream is passed.
  EXPECT_FALSE(buildVmRuntimeConfig({"-gc-max-heap=banana"}, &config, &error));
  EXPECT_FALSE(error.empty());
}

TEST(VmOptionsTest, HelpTextNamesEveryHonouredFlag) {
  std::string help = vmOptionsHelpText();
  for (const auto &n : honouredVmOptionNames())
    EXPECT_NE(help.find(std::string(n)), std::string::npos) << n;
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-asan --target VmOptionsTest`
Expected: FAIL — `buildVmRuntimeConfig` not declared.

- [x] **Step 3: Declare the new entry points**

Add to `include/hermes/node-compat/vm-options/vm_options.h`, above the closing namespaces, plus `#include "hermes/Public/RuntimeConfig.h"` at the top:

```cpp
/// Parses \p options -- each element one Hermes VM flag, exactly as spelled
/// on --vm= -- and writes the resulting configuration to \p out.
///
/// Starts from hermes-node's own defaults and lets a flag the caller
/// actually passed override them. "Actually passed" is
/// llvh::cl::opt::getNumOccurrences(), not a comparison against the
/// option's default value: -Xes6-block-scoping=false must be
/// distinguishable from -Xes6-block-scoping never being mentioned, and
/// hermes-node's default for that field is the opposite of Hermes's.
///
/// Every flag is classified before llvh::cl sees it, so a refused or
/// unknown flag produces our message rather than LLVM's, and -help never
/// reaches the help printer.
///
/// Safe to call more than once. llvh::cl's option registry is a process
/// global and ParseCommandLineOptions does not clear it, so this function
/// calls llvh::cl::ResetAllOptionOccurrences() first -- without which a
/// second call would see the first call's flags still set, and
/// getNumOccurrences() would report a flag the caller never passed. The
/// unit tests parse eight different option lists in one process and are
/// what this guarantee is for.
///
/// Returns false and sets \p error on any failure. Never exits the
/// process: the llvh::cl parse is given an error stream, which is what
/// makes it return rather than call exit().
bool buildVmRuntimeConfig(
    const std::vector<std::string> &options,
    hermes::vm::RuntimeConfig *out,
    std::string *error);

/// The --vm-help body: every honoured flag with the description Hermes
/// itself gives it, read out of the llvh::cl registry so the help cannot
/// drift from what is accepted.
std::string vmOptionsHelpText();
```

- [x] **Step 4: Implement the parse and the mapping**

Add to `lib/vm-options/vm_options.cpp`. Include `hermes/VM/RuntimeFlags.h`, `llvh/Support/CommandLine.h`, `llvh/Support/raw_ostream.h`, `<mutex>`, `<sstream>`.

```cpp
namespace {

/// hermes-node's own defaults, applied to any field the user did not name.
/// These are the whole reason this function does not simply call
/// hermes::cli::buildRuntimeConfig(): Hermes defaults the first two to
/// false, and turning them off would silently remove async generators and
/// block scoping from every program.
constexpr bool kDefaultES6BlockScoping = true;
constexpr bool kDefaultAsyncGenerators = true;
constexpr bool kDefaultMicrotaskQueue = true;

/// One instance for the process. Constructing it registers every VM option
/// with llvh::cl, which is a process-global registry, so this must happen
/// exactly once -- hence the function-local static.
hermes::cli::RuntimeFlags &runtimeFlags() {
  static hermes::cli::RuntimeFlags flags;
  return flags;
}

/// True when the user named \p opt on the command line, rather than it
/// merely holding its default.
template <typename T>
bool wasGiven(const T &opt) {
  return opt.getNumOccurrences() > 0;
}

} // namespace

bool buildVmRuntimeConfig(
    const std::vector<std::string> &options,
    hermes::vm::RuntimeConfig *out,
    std::string *error) {
  hermes::cli::RuntimeFlags &flags = runtimeFlags();

  // ParseCommandLineOptions does not clear prior state, so occurrences
  // from an earlier call would still be set -- and getNumOccurrences(),
  // which is how the hermes-node defaults below decide whether the user
  // named a flag, would then answer for a flag this call never saw.
  // llvh's own documentation for this function names parsing a command
  // line more than once as its purpose.
  llvh::cl::ResetAllOptionOccurrences();

  // Classify before parsing. Three things depend on doing it in this
  // order: a refused flag gets our message instead of silently doing
  // nothing, an unknown flag gets our message instead of LLVM's, and
  // -help never reaches llvh::cl's help printer, which would dump the
  // whole registry and exit the process.
  for (const std::string &opt : options) {
    std::string_view name = vmOptionFlagName(opt);
    switch (classifyVmOption(name)) {
      case VmOptionStatus::kHonoured:
        break;
      case VmOptionStatus::kConsoleHost:
        *error = "'-" + std::string(name) +
            "' is not supported by hermes-node.\n"
            "       It is implemented by Hermes's ConsoleHost, which this "
            "runtime does not use.\n"
            "       Run --vm-help for the flags that are supported.";
        return false;
      case VmOptionStatus::kUnknown:
        *error = "unknown VM option '" + opt +
            "'.\n       Run --vm-help for the flags that are supported.";
        return false;
    }
  }

  // A synthesized argv: llvh::cl sees only what --vm= collected, never
  // hermes-node's own arguments.
  std::vector<const char *> argv;
  argv.push_back("hermes-node --vm");
  for (const std::string &opt : options)
    argv.push_back(opt.c_str());

  std::string errText;
  llvh::raw_string_ostream errStream(errText);
  // Passing errStream is load-bearing: without it llvh::cl prints to
  // stderr and calls exit(), terminating the process out from under our
  // own error reporting. See CommandLine.h:55-61.
  if (!llvh::cl::ParseCommandLineOptions(
          static_cast<int>(argv.size()), argv.data(), "", &errStream)) {
    errStream.flush();
    *error = errText;
    return false;
  }

  // The mapping mirrors hermes/tools/hermes/hermes.cpp:110-141, which is
  // what the hermes binary itself uses. Diff against that function when
  // Hermes moves.
  auto gcConfig = hermes::vm::GCConfig::Builder()
                      .withInitHeapSize(flags.InitHeapSize.bytes)
                      .withMaxHeapSize(flags.MaxHeapSize.bytes)
                      .withOccupancyTarget(flags.OccupancyTarget)
                      .withSanitizeConfig(
                          hermes::vm::GCSanitizeConfig::Builder()
                              .withSanitizeRate(flags.GCSanitizeRate)
                              .withRandomSeed(flags.GCSanitizeRandomSeed)
                              .build())
                      .withShouldRecordStats(flags.GCPrintStats)
                      .withShouldReleaseUnused(hermes::vm::kReleaseUnusedOld)
                      .withAllocInYoung(flags.GCAllocYoung)
                      .withRevertToYGAtTTI(flags.GCRevertToYGAtTTI)
                      .build();

  using JITMode = hermes::cli::VMOnlyRuntimeFlags::JITMode;
  *out =
      hermes::vm::RuntimeConfig::Builder()
          .withGCConfig(gcConfig)
          .withMaxNumRegisters(flags.MaxNumRegisters)
          .withEnableJIT(flags.JIT != JITMode::Off)
          .withForceJIT(flags.JIT == JITMode::Force)
          .withJITThreshold(flags.JITThreshold)
          .withJITMemoryLimit(flags.JITMemoryLimit)
          .withEnableEval(flags.EnableEval)
          .withVerifyEvalIR(flags.VerifyIR)
          .withOptimizedEval(flags.OptimizedEval)
          .withAsyncBreakCheckInEval(flags.EmitAsyncBreakCheck)
          .withVMExperimentFlags(flags.VMExperimentFlags)
          .withES6Proxy(flags.ES6Proxy)
          .withIntl(flags.Intl)
          .withRandomizeMemoryLayout(flags.RandomizeMemoryLayout)
          .withTrackIO(flags.TrackBytecodeIO)
          .withEnableHermesInternal(flags.EnableHermesInternal)
          .withEnableHermesInternalTestMethods(
              flags.EnableHermesInternalTestMethods)
          .withTest262(flags.Test262)
          // The three fields where hermes-node's default differs from, or
          // deliberately restates, Hermes's.
          .withES6BlockScoping(
              wasGiven(flags.ES6BlockScoping) ? (bool)flags.ES6BlockScoping
                                              : kDefaultES6BlockScoping)
          .withEnableAsyncGenerators(
              wasGiven(flags.EnableAsyncGenerators)
                  ? (bool)flags.EnableAsyncGenerators
                  : kDefaultAsyncGenerators)
          .withMicrotaskQueue(
              wasGiven(flags.MicrotaskQueue) ? (bool)flags.MicrotaskQueue
                                             : kDefaultMicrotaskQueue)
          .build();
  return true;
}

std::string vmOptionsHelpText() {
  std::ostringstream os;
  auto &registry = llvh::cl::getRegisteredOptions();
  for (std::string_view name : honouredVmOptionNames()) {
    auto it = registry.find(llvh::StringRef(name.data(), name.size()));
    os << "  -" << name;
    if (it != registry.end() && !it->second->HelpStr.empty())
      os << "\n      " << it->second->HelpStr.str();
    os << "\n";
  }
  return os.str();
}
```

**Note on `getRegisteredOptions()`:** it needs the options to be registered, which happens when `runtimeFlags()` is first called. Call `runtimeFlags();` as the first line of `vmOptionsHelpText()` so `--vm-help` works before any parse. If the llvh API in this Hermes checkout differs (check `hermes/external/llvh/include/llvh/Support/CommandLine.h` for `getRegisteredOptions` and the `Option` member holding help text — it may be `HelpStr` as a `StringRef`), adapt the two lines that read it; the test only requires each honoured name to appear.

- [x] **Step 5: Run the test to verify it passes**

Run: `cmake --build cmake-build-asan --target VmOptionsTest && cmake-build-asan/unittests/VmOptionsTest`
Expected: PASS, 14 tests.

The `ResetAllOptionOccurrences()` call is what makes the eight tests independent of each other. Verify that by running them in both orders if the framework allows it, or at minimum confirm that `EmptyOptionsPreserveHermesNodeDefaults` still passes when it runs *after* `HermesNodeDefaultsCanBeOverridden` — that pair is precisely the leak the reset prevents, and it is the case that fails if the call is dropped.

- [x] **Step 6: Format and commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/vm-options lib/vm-options unittests/VmOptionsTest.cpp
git commit -m "vm-options: parse VM flags into a RuntimeConfig"
```

---

### Task 3: `--vm=` and `--vm-help` on the command line

Wire the library to a plain script run. No bundle involvement.

**Files:**
- Modify: `include/hermes/node-compat/runtime/hermes_node_runtime.h`
- Modify: `lib/runtime/hermes_node_runtime.cpp:691`
- Modify: `lib/runtime/CMakeLists.txt`
- Modify: `tools/hermes-node/hermes-node.cpp` (parse loop, `printUsage`, `checkToolOptions`)
- Create: `test/vm-options.js`
- Create: `test/vm-options-errors.js`

**Interfaces:**
- Consumes: `buildVmRuntimeConfig`, `vmOptionsHelpText` (Task 2).
- Produces: `HermesNodeProcessConfig::vmOptions` (`std::vector<std::string>`), populated by `main()`.

- [x] **Step 1: Write the failing tests**

Create `test/vm-options.js` (copyright header copied from `test/bundle-preload.js`):

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --vm= reaches the Hermes VM. Every flag asserted here is observable from
// JavaScript with no timing dependency, so these cases test effect rather
// than merely that a flag parsed.

// A flag takes effect.
// RUN: echo "console.log('PROXY', typeof Proxy);" > %t.proxy.js
// RUN: %hermes-node --vm=-Xes6-proxy=false %t.proxy.js | %FileCheck --check-prefix=NOPROXY %s
// NOPROXY: PROXY undefined

// Without the flag it is there.
// RUN: %hermes-node %t.proxy.js | %FileCheck --check-prefix=PROXY %s
// PROXY: PROXY function

// hermes-node's own defaults survive the introduction of the flag
// machinery. Hermes defaults -Xasync-generators to false; hermes-node
// forces it on, and this is the case that fails if that is ever lost.
// RUN: echo "async function* g() { yield 1; } g().next().then(v => console.log('ASYNCGEN', v.value));" > %t.agen.js
// RUN: %hermes-node %t.agen.js | %FileCheck --check-prefix=AGEN %s
// AGEN: ASYNCGEN 1

// And it can still be turned off explicitly.
// RUN: %not %hermes-node --vm=-Xasync-generators=false %t.agen.js 2>&1 | %FileCheck --check-prefix=NOAGEN %s
// NOAGEN: SyntaxError
//
// The NOAGEN text above is a guess. Run the command once, read what it
// actually prints, and pin a substring of THAT. A CHECK that matches
// anything (`{{.*}}`) asserts nothing and must not be committed. If
// -Xasync-generators=false turns out to produce no distinguishable
// failure for this source, delete the case and say so in your report --
// the positive AGEN case above already pins the default, which is the
// regression that matters.

// Last occurrence wins. buildVmRuntimeConfig deduplicates to make this
// true: a plain llvh::cl::opt is cl::Optional and rejects a second
// occurrence outright, so a repeat must not reach the parser twice.
// RUN: %hermes-node --vm=-Xes6-proxy=false --vm=-Xes6-proxy=true %t.proxy.js | %FileCheck --check-prefix=PROXY %s

// A memory size with a suffix parses, because Hermes's own parser handles
// it rather than a hand-rolled one.
// RUN: echo "console.log('RAN');" > %t.ok.js
// RUN: %hermes-node --vm=-gc-max-heap=512m %t.ok.js | %FileCheck --check-prefix=RAN %s
// RAN: RAN

// --vm-help lists the honoured flags and exits 0.
// RUN: %hermes-node --vm-help | %FileCheck --check-prefix=HELP %s
// HELP-DAG: -gc-max-heap
// HELP-DAG: -Xjit
// HELP-DAG: -Xes6-block-scoping
```

Create `test/vm-options-errors.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Every way --vm= can be refused. Nothing here needs a linker or a kit, so
// this file is deliberately not gated on any lit feature.

// RUN: echo "console.log('RAN');" > %t.js

// A ConsoleHost-only flag is refused by name, and says why -- rather than
// parsing cleanly and doing nothing.
// RUN: %not %hermes-node --vm=-Xdump-jitcode=1 %t.js 2>&1 | %FileCheck --check-prefix=CONSOLEHOST %s
// CONSOLEHOST: Xdump-jitcode
// CONSOLEHOST: ConsoleHost

// An unknown flag is our error, not LLVM's.
// RUN: %not %hermes-node --vm=-gc-max-hep=1g %t.js 2>&1 | %FileCheck --check-prefix=UNKNOWN %s
// UNKNOWN: unknown VM option
// UNKNOWN: gc-max-hep

// -help must never reach llvh::cl's help printer, which would dump the
// entire option registry and exit.
// RUN: %not %hermes-node --vm=-help %t.js 2>&1 | %FileCheck --check-prefix=UNKNOWN %s

// A bad value for an honoured flag is reported, and the process does not
// die inside llvh::cl.
// RUN: %not %hermes-node --vm=-gc-max-heap=banana %t.js 2>&1 | %FileCheck --check-prefix=BADVAL %s
// BADVAL: gc-max-heap

// An empty value names the flag rather than reporting a parse failure with
// nothing in it.
// RUN: %not %hermes-node --vm= %t.js 2>&1 | %FileCheck --check-prefix=EMPTY %s
// EMPTY: Error: --vm requires a value

// The conflict matrix. Each message names both flags.
// RUN: %not %hermes-node --vm=-Xjit=on --build-exe=%t.out %t.hbb 2>&1 | %FileCheck --check-prefix=EXE %s
// EXE: --vm cannot be combined with --build-exe

// RUN: %not %hermes-node --vm=-Xjit=on --bundle=%t.hbb --dump 2>&1 | %FileCheck --check-prefix=DUMP %s
// DUMP: --vm cannot be combined with --dump

// RUN: %not %hermes-node --vm=-Xjit=on --dump-bytecode=%t.hbc 2>&1 | %FileCheck --check-prefix=DUMPBC %s
// DUMPBC: --vm cannot be combined with --dump-bytecode

// RUN: %not %hermes-node --vm=-Xjit=on --bundle=%t.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=VERIFY %s
// VERIFY: --vm cannot be combined with --verify-natives

// RUN: %not %hermes-node --vm=-Xjit=on --bundle=%t.hbb --extract-module=x --out=%t.o 2>&1 | %FileCheck --check-prefix=EXTRACT %s
// EXTRACT: --vm cannot be combined with --extract-module

// --allow-vm-options-override only means something while building.
// RUN: %not %hermes-node --allow-vm-options-override %t.js 2>&1 | %FileCheck --check-prefix=ALLOW %s
// ALLOW: --allow-vm-options-override requires --build-bundle
```

- [x] **Step 2: Run the tests to verify they fail**

Run:
```bash
python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/vm-options.js $(pwd)/test/vm-options-errors.js \
  --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node \
  --param hermes=$(pwd)/cmake-build-asan/bin/hermes \
  --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck \
  --param not=$(pwd)/cmake-build-asan/bin/not \
  --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test
```
Expected: both FAIL — `--vm` is not a recognised option.

- [x] **Step 3: Add the config field**

In `include/hermes/node-compat/runtime/hermes_node_runtime.h`, add to `HermesNodeProcessConfig` (the struct whose doc comment explains that a field here is inherited by every runtime — that is exactly why this belongs there and not in `HermesNodeConfig`):

```cpp
  /// Hermes VM options, in the order they take effect: a container's baked
  /// options first, then HERMES_NODE_VM_OPTIONS, then --vm= from the
  /// command line. Later occurrences of the same flag win, which is
  /// last-wins; buildVmRuntimeConfig deduplicates to make that hold.
  ///
  /// Strings rather than a built vm::RuntimeConfig so this header stays
  /// free of Hermes VM headers. lib/runtime parses them once and caches
  /// the result; the inspector runtime receives a copy of this config and
  /// so is configured identically, which is the point of the field being
  /// here rather than on HermesNodeConfig.
  std::vector<std::string> vmOptions;
```

- [x] **Step 4: Use it when building the runtime**

In `lib/runtime/hermes_node_runtime.cpp`, replace the hardcoded builder at line 691:

```cpp
  // 1. Create the Hermes runtime. The configuration comes from
  //    --vm= / a container's baked options / HERMES_NODE_VM_OPTIONS,
  //    already resolved into one ordered list by the caller. With no
  //    options at all this produces exactly what the hardcoded
  //    Builder() call it replaced produced: microtask queue, async
  //    generators and ES6 block scoping on.
  //
  //    Parsed under call_once so that the inspector runtime -- a second
  //    runHermesNode() call in the same process -- is configured from the
  //    identical result rather than from a second parse of the same list.
  //    A debugger attached to a differently-configured VM would mislead in
  //    a way that is hard to notice, and re-deriving a config that cannot
  //    differ is work with only a downside. (buildVmRuntimeConfig is
  //    itself safe to call twice; this is about agreement, not safety.)
  static std::once_flag vmConfigOnce;
  static hermes::vm::RuntimeConfig vmRuntimeConfig;
  static std::string vmConfigError;
  std::call_once(vmConfigOnce, [&config]() {
    if (!buildVmRuntimeConfig(
            config.process.vmOptions, &vmRuntimeConfig, &vmConfigError))
      vmConfigError = "Error: --vm: " + vmConfigError;
  });
  if (!vmConfigError.empty()) {
    std::fprintf(stderr, "%s\n", vmConfigError.c_str());
    return 1;
  }
  auto hermesRT = facebook::hermes::makeHermesRuntime(vmRuntimeConfig);
```

Add `#include "hermes/node-compat/vm-options/vm_options.h"` and `#include <mutex>`. Add `hermesNodeVmOptions` to `target_link_libraries(hermesNodeRuntime ...)` in `lib/runtime/CMakeLists.txt`.

- [x] **Step 5: Add the command-line flags**

In `tools/hermes-node/hermes-node.cpp`:

Add to `ToolOptions`:
```cpp
  /// --allow-vm-options-override: record in the container being built that
  /// its VM options may be overridden at run time. A bool, not an
  /// optional<string>, because it takes no value.
  bool allowVmOptionsOverride = false;
```

Add branches to the parse loop, beside the existing `--include=` handling:
```cpp
    } else if (std::strncmp(argv[i], "--vm=", 5) == 0) {
      const char *value = argv[i] + 5;
      if (value[0] == '\0') {
        std::fprintf(stderr, "Error: --vm requires a value\n");
        return 1;
      }
      // Never split on whitespace: -Xperf-prof-dir=<dir> proves a value
      // can legitimately contain a space. One flag per occurrence.
      config.process.vmOptions.push_back(value);
    } else if (std::strcmp(argv[i], "--vm") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --vm requires a value\n");
        return 1;
      }
      config.process.vmOptions.push_back(argv[++i]);
    } else if (std::strcmp(argv[i], "--vm-help") == 0) {
      std::printf(
          "Hermes VM options, passed one per --vm=<flag>:\n\n%s",
          hermes::node_compat::vmOptionsHelpText().c_str());
      return 0;
    } else if (std::strcmp(argv[i], "--allow-vm-options-override") == 0) {
      tools.allowVmOptionsOverride = true;
```

Add to `checkToolOptions()`, following the same message shape ("Error: X cannot be combined with Y.\n"). **Placement matters:** the first check reads the local `const char *verb`, so it must go *after* the block that assigns it (the `if (tools.dump) verb = "--dump"; else if (...)` chain), not merely after the two-verbs-at-once rows above that chain.

Note that `verb` covers only the five read-only and link verbs. `--build-bundle` is deliberately not among them, so `--vm --build-bundle` is accepted by this check — which is what Task 5 needs. Do not add it.

```cpp
  // --vm configures a runtime. None of the read-only verbs creates one,
  // and --build-exe's options belong to the container it reads, not to
  // the command line that links it.
  if (!config.process.vmOptions.empty() && verb != nullptr) {
    std::fprintf(
        stderr, "Error: --vm cannot be combined with %s.\n", verb);
    return false;
  }
  if (tools.allowVmOptionsOverride && config.buildBundlePath.empty()) {
    std::fprintf(
        stderr,
        "Error: --allow-vm-options-override requires --build-bundle.\n"
        "It records a bit in a container; this run is not building one.\n");
    return false;
  }
```

Add to `printUsage()`, after the `--preload` block:
```
      "  --vm=<flag>                    Hermes VM option (repeatable); with\n"
      "                                 --build-bundle, record it in the "
      "container\n"
      "                                 instead of applying it to this run\n"
      "  --allow-vm-options-override    With --build-bundle, let the "
      "container's\n"
      "                                 VM options be overridden at run time\n"
      "  --vm-help                      List the supported VM options\n"
```

- [x] **Step 6: Run the tests to verify they pass**

Run the lit command from Step 2.
Expected: both PASS.

- [x] **Step 7: Run the whole suite, format, commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/runtime/hermes_node_runtime.h lib/runtime tools/hermes-node/hermes-node.cpp test/vm-options.js test/vm-options-errors.js
git commit -m "hermes-node: --vm=<flag> configures the Hermes VM"
```

The full suite matters here specifically: this task replaces the process's only `RuntimeConfig`, so a mistake shows up as unrelated tests failing.

---

### Task 4: Format v5 — the container carries VM options

Format, writer and reader only. Nothing produces or consumes these yet.

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_format.h`
- Modify: `include/hermes/node-compat/bundle/bundle_writer.h`, `lib/bundle/bundle_writer.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_reader.h`, `lib/bundle/bundle_reader.cpp`
- Modify: `unittests/BundleFormatTest.cpp`

**Interfaces:**
- Produces: `BundleWriter::addVmOption(std::string_view)`, `BundleWriter::setAllowVmOptionsOverride(bool)`, `BundleReader::vmOptionCount()`, `BundleReader::vmOption(uint32_t)`, `BundleReader::allowsVmOptionsOverride()`, `BundleReader::vmOptionsTableSize()`, `kBundleFlagAllowVmOptionsOverride`.

- [x] **Step 1: Write the failing test**

Append to `unittests/BundleFormatTest.cpp`, mirroring how the existing preload round-trip test is written there (read it first and follow its shape):

```cpp
TEST(BundleFormatTest, VmOptionsRoundTrip) {
  BundleWriter writer;
  uint32_t m = writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(m);
  writer.addVmOption("-gc-max-heap=2g");
  writer.addVmOption("-Xjit=on");
  std::vector<uint8_t> bytes = writer.serialize(0);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 0, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_EQ(reader->formatVersion(), 5u);
  ASSERT_EQ(reader->vmOptionCount(), 2u);
  EXPECT_EQ(reader->vmOption(0), "-gc-max-heap=2g");
  EXPECT_EQ(reader->vmOption(1), "-Xjit=on");
  // Order is the point: these are applied left to right and later wins.
  EXPECT_GT(reader->vmOptionsTableSize(), 0u);
}

TEST(BundleFormatTest, VmOptionsDefaultToLocked) {
  BundleWriter writer;
  uint32_t m = writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(m);
  writer.addVmOption("-Xjit=on");
  std::vector<uint8_t> bytes = writer.serialize(0);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 0, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_FALSE(reader->allowsVmOptionsOverride());
}

TEST(BundleFormatTest, VmOptionsOverrideBitRoundTrips) {
  BundleWriter writer;
  uint32_t m = writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(m);
  writer.addVmOption("-Xjit=on");
  writer.setAllowVmOptionsOverride(true);
  std::vector<uint8_t> bytes = writer.serialize(0);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 0, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_TRUE(reader->allowsVmOptionsOverride());
}

TEST(BundleFormatTest, NoVmOptionsIsAnEmptySection) {
  BundleWriter writer;
  uint32_t m = writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(m);
  std::vector<uint8_t> bytes = writer.serialize(0);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 0, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_EQ(reader->vmOptionCount(), 0u);
  EXPECT_EQ(reader->vmOptionsTableSize(), 0u);
  EXPECT_FALSE(reader->allowsVmOptionsOverride());
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-asan --target BundleFormatTest`
Expected: FAIL — `addVmOption` not a member.

- [x] **Step 3: Change the format**

In `include/hermes/node-compat/bundle/bundle_format.h`:

```cpp
constexpr uint32_t kBundleFormatVersion = 5;
```

Add to `BundleHeader`, immediately after the native table fields and before `payloadOffset` (the layout order in `serialize()` must match):

```cpp
  // The VM-options table: an array of uint32_t string indices, one per
  // recorded Hermes VM option, in the order they are applied -- later
  // wins (buildVmRuntimeConfig deduplicates). A section of its own for the same
  // reason the preload table is one: order is the whole content, and a
  // flag bit on a module record could not express it. String indices
  // rather than inline text so the existing string table does the storage.
  uint32_t vmOptionsTableOffset;
  uint32_t vmOptionsCount;
  // Container-wide flags: currently only kBundleFlagAllowVmOptionsOverride.
  // Distinct from BundleModuleRecord::flags, which is per module.
  uint32_t containerFlags;
```

And the flag bit, beside `kRequirable`:

```cpp
/// Set on a container whose recorded VM options may be overridden at run
/// time, by --vm= on a --bundle= command line or by HERMES_NODE_VM_OPTIONS
/// for a linked executable. Clear -- the default -- means the options are
/// a property of the artifact and an attempt to override them is an error,
/// because the honoured flag set includes -enable-eval and
/// -Xhermes-internal-test-methods, which are not tuning knobs.
constexpr uint32_t kBundleFlagAllowVmOptionsOverride = 1u << 0;
```

- [x] **Step 4: Change the writer**

In `bundle_writer.h`, declare beside `addPreload`:

```cpp
  /// Records a Hermes VM option. Call order is application order: later
  /// occurrences of the same flag win, so this is a list rather than a set
  /// and duplicates are kept.
  void addVmOption(std::string_view option);

  /// Records whether the container's VM options may be overridden at run
  /// time. False -- the default -- means locked.
  void setAllowVmOptionsOverride(bool allow);
```

with `std::vector<std::string> vmOptions_;` and `bool allowVmOptionsOverride_ = false;` as members.

In `bundle_writer.cpp::serialize()`, intern the options beside where native sidecars are interned:

```cpp
  std::vector<uint32_t> vmOptionStrings(vmOptions_.size());
  for (size_t i = 0; i < vmOptions_.size(); ++i)
    vmOptionStrings[i] = internString(vmOptions_[i]);
```

then extend the layout chain — the new section goes between the native table and the payload, matching the header field order:

```cpp
  size_t vmOptionsTableOffset = nativeTableOffset + nativeTableSize;
  size_t vmOptionsTableSize = vmOptions_.size() * sizeof(uint32_t);
  size_t payloadOffset =
      alignUp(vmOptionsTableOffset + vmOptionsTableSize, kBundlePayloadAlign);
```

set the header fields:

```cpp
  header.vmOptionsTableOffset = static_cast<uint32_t>(vmOptionsTableOffset);
  header.vmOptionsCount = static_cast<uint32_t>(vmOptions_.size());
  header.containerFlags =
      allowVmOptionsOverride_ ? kBundleFlagAllowVmOptionsOverride : 0;
```

and emit the table after the native table, before the payload padding, following exactly how the preload table is emitted a few lines above it.

- [x] **Step 5: Change the reader**

In `bundle_reader.h`, declare beside the preload accessors:

```cpp
  /// How many VM options the container records, in application order.
  uint32_t vmOptionCount() const;

  /// VM option \p i. Only valid for \p i below vmOptionCount(), exactly
  /// like edge() above. The view points into the mapped string table.
  std::string_view vmOption(uint32_t i) const;

  /// True when kBundleFlagAllowVmOptionsOverride is set -- the container
  /// permits its VM options to be overridden at run time.
  bool allowsVmOptionsOverride() const;

  /// Byte count, like the other ...Size() accessors: option count times
  /// sizeof(uint32_t), NOT an element count.
  uint32_t vmOptionsTableSize() const;
```

with `const uint32_t *vmOptions_ = nullptr;` as a member.

In `bundle_reader.cpp::openImpl()`, validate the new section exactly as the preload table is validated (find that block and mirror it): the table must lie inside `size`, `vmOptionsCount * sizeof(uint32_t)` must not overflow, and **every string index in it must be a valid string-table offset** — the preload table validates its module indices for the same reason, and an unvalidated index here would be a read outside the mapping.

- [x] **Step 6: Run the test to verify it passes**

Run: `cmake --build cmake-build-asan --target BundleFormatTest && cmake-build-asan/unittests/BundleFormatTest`
Expected: PASS.

- [x] **Step 7: Rebuild everything and commit**

The version bump invalidates every container the test suite builds, so the whole suite must pass before this is committed.

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle unittests/BundleFormatTest.cpp
git commit -m "bundle: format v5 records VM options and an override bit"
```

---

### Task 5: `--build-bundle --vm=` records the options, `--dump` shows them

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_build.h`, `lib/bundle/bundle_build.cpp`
- Modify: `lib/bundle/bundle_tools.cpp`
- Modify: `tools/hermes-node/hermes-node.cpp` (the `--build-bundle` call site)
- Create: `test/bundle-vm-options.js` (the build-and-dump half)

**Interfaces:**
- Consumes: `BundleWriter::addVmOption`, `setAllowVmOptionsOverride`, `BundleReader::vmOptionCount/vmOption/allowsVmOptionsOverride/vmOptionsTableSize` (Task 4).
- Produces: `buildBundle(..., const std::vector<std::string> &vmOptions, bool allowVmOptionsOverride)`.

- [x] **Step 1: Write the failing test**

Create `test/bundle-vm-options.js` with the copyright header, and this first half:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A container carries the VM options its program needs, and says so under
// --dump, so it can be audited before it ships.

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('PROXY', typeof Proxy);" > %t.tree/cli.js

// --vm= with --build-bundle records rather than applies.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --vm=-Xes6-proxy=false %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=DUMP %s
// DUMP: VM_OPTIONS (1)
// DUMP: overrides: locked
// DUMP: -Xes6-proxy=false
// The table is a section with real bytes, not a free extra.
// DUMP: vmopts {{[1-9][0-9]*}} B

// A container with no options dumps exactly as it did before this section
// existed.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/plain.hbb --dump | %FileCheck --check-prefix=PLAIN --implicit-check-not=VM_OPTIONS %s
// PLAIN: MODULES

// --vm is accepted alongside --build-bundle: it records rather than
// configures, so it is not one of the verbs checkToolOptions() refuses it
// with. All four RUN lines above already depend on that; this is the case
// that says so out loud.

// --allow-vm-options-override is recorded and shown.
// RUN: %hermes-node --build-bundle=%t.tree/open.hbb --vm=-Xes6-proxy=false --allow-vm-options-override %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/open.hbb --dump | %FileCheck --check-prefix=OPEN %s
// OPEN: overrides: allowed
```

**Deliberately not here:** an assertion that running the container applies
its options. That behaviour lands in Task 6; asserting it here would leave
this task's own test file failing. Task 6 adds it.

- [x] **Step 2: Run the test to verify it fails**

Run the lit command from Task 3 Step 2 with this file's path.
Expected: FAIL — `--vm` is refused with `--build-bundle`, or the dump has no `VM_OPTIONS`.

- [x] **Step 3: Thread the options through the producer**

In `bundle_build.h`, extend the signature and document the two new parameters in the doc comment's style:

```cpp
/// \p vmOptions are Hermes VM options recorded in the container's
/// VM-options table, in the order given, for the consumer to apply before
/// it creates its runtime. They are recorded, not applied: this run
/// compiles rather than executes, and a build machine's VM tuning is not
/// the artifact's business.
///
/// \p allowVmOptionsOverride records whether those options may be
/// overridden at run time. False -- the default -- locks them, because the
/// honoured flag set includes -enable-eval and
/// -Xhermes-internal-test-methods, which are not tuning knobs.
int buildBundle(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    bool verbose,
    const std::vector<std::string> &includes,
    const std::vector<std::string> &preloads,
    const std::vector<std::string> &vmOptions,
    bool allowVmOptionsOverride);
```

In `bundle_build.cpp`, where `addPreload` is called on the writer, add:

```cpp
  for (const std::string &opt : vmOptions)
    writer.addVmOption(opt);
  writer.setAllowVmOptionsOverride(allowVmOptionsOverride);
```

Under `verbose`, print one line per recorded option and the lock state, matching the narration style already there.

- [x] **Step 4: Add the dump section**

In `lib/bundle/bundle_tools.cpp`, after the `NATIVES` block and before the `SECTIONS` block:

```cpp
  // Same rule as PRELOADS and NATIVES above, for the same reason: a
  // container with no VM options must dump exactly as it did before this
  // section existed.
  const uint32_t vmOptionCount = reader->vmOptionCount();
  if (vmOptionCount > 0) {
    out << "\nVM_OPTIONS (" << vmOptionCount << ")\n";
    out << "  overrides: "
        << (reader->allowsVmOptionsOverride() ? "allowed" : "locked") << "\n";
    for (uint32_t i = 0; i < vmOptionCount; ++i)
      out << "  " << reader->vmOption(i) << "\n";
  }
```

Add `vmopts` to the `SECTIONS` block: include `reader->vmOptionsTableSize()` in the `sectionWidth` `std::max` list and add a row for it. The row is unconditional, like `natives` and `preloads` — the comment there explains why, and the same reasoning applies.

- [x] **Step 5: Update the call site**

In `tools/hermes-node/hermes-node.cpp`, pass `config.process.vmOptions` and `tools.allowVmOptionsOverride` to `buildBundle`.

Nothing in `checkToolOptions()` needs changing: Task 3's `--vm` check fires only when a verb is in play, and `--build-bundle` is not one of the five verbs. Read the check to confirm that rather than assuming it, then leave it alone.

- [x] **Step 6: Run the test to verify it passes**

Run the lit command.
Expected: PASS.

- [x] **Step 7: Format, full suite, commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle tools/hermes-node/hermes-node.cpp test/bundle-vm-options.js
git commit -m "bundle: --build-bundle --vm records VM options in the container"
```

---

### Task 6: A `--bundle=` run applies the container's options

**Files:**
- Modify: `include/hermes/node-compat/bundle/bundle_run.h`, `lib/bundle/bundle_run.cpp`
- Modify: `tools/hermes-node/hermes-node.cpp`
- Modify: `test/bundle-vm-options.js`

**Interfaces:**
- Consumes: `BundleReader::vmOption*` (Task 4).
- Produces: `struct BundleVmOptions { std::vector<std::string> options; bool allowOverride; }`, `readBundleVmOptions(const std::string &path, BundleVmOptions *out, std::string *error)`.

- [x] **Step 1: Write the failing test**

Append to `test/bundle-vm-options.js`:

```js
// Running a container applies the options it recorded. (This assertion
// belongs to this task rather than Task 5: Task 5 records them, this task
// is what makes them take effect.)
// RUN: %hermes-node --bundle=%t.tree/app.hbb | %FileCheck --check-prefix=NOPROXY %s
// NOPROXY: PROXY undefined

// Locked is the default, and an override attempt is an error rather than a
// setting that quietly does not take effect.
// RUN: %not %hermes-node --vm=-Xes6-proxy=true --bundle=%t.tree/app.hbb 2>&1 | %FileCheck --check-prefix=LOCKED %s
// LOCKED: locked
// LOCKED: --allow-vm-options-override

// Including from the environment.
// RUN: env HERMES_NODE_VM_OPTIONS=-Xes6-proxy=true %not %hermes-node --bundle=%t.tree/app.hbb 2>&1 | %FileCheck --check-prefix=LOCKEDENV %s
// LOCKEDENV: HERMES_NODE_VM_OPTIONS

// With the bit set, both work, and the run-time value wins because it is
// appended after the container's.
// RUN: %hermes-node --vm=-Xes6-proxy=true --bundle=%t.tree/open.hbb | %FileCheck --check-prefix=PROXY %s
// PROXY: PROXY function
// RUN: env HERMES_NODE_VM_OPTIONS=-Xes6-proxy=true %hermes-node --bundle=%t.tree/open.hbb | %FileCheck --check-prefix=PROXY %s

// The command line beats the environment.
// RUN: env HERMES_NODE_VM_OPTIONS=-Xes6-proxy=true %hermes-node --vm=-Xes6-proxy=false --bundle=%t.tree/open.hbb | %FileCheck --check-prefix=NOPROXY %s

// An unlocked container with no run-time override still applies its own.
// RUN: %hermes-node --bundle=%t.tree/open.hbb | %FileCheck --check-prefix=NOPROXY %s
```

- [x] **Step 2: Run the test to verify it fails**

Expected: FAIL — the container's options are ignored, so `PROXY function` is printed where `PROXY undefined` is expected.

- [x] **Step 3: Add the reader helper**

In `bundle_run.h`:

```cpp
/// What a container records about its VM configuration.
struct BundleVmOptions {
  /// The recorded options, in application order.
  std::vector<std::string> options;
  /// Whether they may be overridden at run time.
  bool allowOverride = false;
};

/// Reads a container's VM options without opening it for execution.
///
/// The options decide how the runtime is built, and the run path opens the
/// container only after the runtime exists (runBundle takes a napi_env).
/// Rather than restructure that, this maps and validates the file a second
/// time, reads the two things it needs and closes -- one extra
/// map-and-validate of a file that is about to be mapped again.
///
/// Uses openForInspection: this is not an execution path, and a container
/// whose generation tag does not match must still be able to say what its
/// options are. The subsequent openBundle() enforces the tag, so nothing
/// mismatched ever runs.
///
/// Returns false and sets \p error on any failure to read the file.
bool readBundleVmOptions(
    const std::string &path,
    BundleVmOptions *out,
    std::string *error);

/// The same, for a container linked into this executable rather than
/// mapped from a file. \p data and \p size name the payload object's
/// contents, exactly as openEmbeddedBundle() takes them.
bool readEmbeddedBundleVmOptions(
    const uint8_t *data,
    size_t size,
    BundleVmOptions *out,
    std::string *error);
```

Implement both in `bundle_run.cpp` using the same map-and-validate helper `openBundle()` uses; both are pure reads and must not touch the module-static that tracks the one open bundle.

- [x] **Step 4: Resolve the option list in `main()`**

In `tools/hermes-node/hermes-node.cpp`, after `checkToolOptions()` succeeds and before `runHermesNode`, when `config.bundlePath` is non-empty:

```cpp
  // A container's options come first, then the environment, then --vm=;
  // later occurrences win, so this order is the precedence order. Read
  // before the runtime exists, because they decide how it is built.
  if (!config.bundlePath.empty()) {
    hermes::node_compat::BundleVmOptions bundleVm;
    std::string error;
    if (!hermes::node_compat::readBundleVmOptions(
            config.bundlePath, &bundleVm, &error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    std::vector<std::string> runtimeVm = envVmOptions();
    const bool hasCliVm = !config.process.vmOptions.empty();
    if (!bundleVm.allowOverride && (hasCliVm || !runtimeVm.empty())) {
      std::fprintf(
          stderr,
          "Error: this bundle's VM options are locked and cannot be "
          "overridden.\n"
          "       %s\n"
          "       Rebuild with: --build-bundle --allow-vm-options-override\n",
          hasCliVm ? "--vm was given on the command line."
                   : "HERMES_NODE_VM_OPTIONS is set in the environment.");
      return 1;
    }
    std::vector<std::string> merged = bundleVm.options;
    merged.insert(merged.end(), runtimeVm.begin(), runtimeVm.end());
    merged.insert(
        merged.end(),
        config.process.vmOptions.begin(),
        config.process.vmOptions.end());
    config.process.vmOptions = std::move(merged);
  }
```

with `envVmOptions()` being a thin wrapper over a **shared** splitter. Put the splitter in `lib/vm-options/vm_options.cpp` and declare it in `vm_options.h`, because `bundle_main.cpp` needs exactly the same splitting in Task 7 and the rule must not drift between a `--bundle` run and a linked executable:

```cpp
/// Splits a HERMES_NODE_VM_OPTIONS value on whitespace. An environment
/// variable has no other shape, so a value containing a space cannot be
/// expressed there; --vm= has no such limit and is the answer when one is
/// needed.
///
/// Empty vector for a null or all-whitespace value, which is what lets a
/// caller distinguish "not set" from "set to something".
///
/// Shared rather than duplicated: tools/hermes-node/hermes-node.cpp and
/// tools/hermes-node/bundle_main.cpp both split this variable, and two
/// copies of the rule in two translation units is how it would come to
/// differ between a --bundle run and an executable.
std::vector<std::string> splitVmOptionsEnv(const char *value);
```

with the obvious `std::istringstream` implementation (`#include <sstream>`), and at the call site:

```cpp
static std::vector<std::string> envVmOptions() {
  return hermes::node_compat::splitVmOptionsEnv(
      std::getenv("HERMES_NODE_VM_OPTIONS"));
}
```

Add a GTest for `splitVmOptionsEnv` to `unittests/VmOptionsTest.cpp` covering: null, empty, one flag, several flags, and leading/trailing/repeated whitespace.

**Note:** `HERMES_NODE_VM_OPTIONS` applies only to a container run — a plain script already has `--vm=` on its own command line, and a second way to say the same thing on the same command line is a precedence question with no benefit. Do not apply it outside the `config.bundlePath` branch.

- [x] **Step 5: Run the tests to verify they pass**

Run lit on `test/bundle-vm-options.js`.
Expected: PASS.

- [x] **Step 6: Format, full suite, commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add include/hermes/node-compat/bundle lib/bundle tools/hermes-node/hermes-node.cpp test/bundle-vm-options.js
git commit -m "bundle: a --bundle run applies the container's VM options"
```

---

### Task 7: A produced executable inherits and locks

**Files:**
- Modify: `tools/hermes-node/bundle_main.cpp`
- Create: `test/build-exe-vm-options.js`

**Interfaces:**
- Consumes: `readEmbeddedBundleVmOptions`, `BundleVmOptions` (Task 6).

- [x] **Step 1: Write the failing test**

Create `test/build-exe-vm-options.js`:

```js
// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A linked executable carries its container's VM options. It has no
// command line of its own -- every argument belongs to the program -- so
// HERMES_NODE_VM_OPTIONS is the only way to reach it, and only when the
// container was built to allow it.
//
// REQUIRES: linker-available

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('PROXY', typeof Proxy);" > %t.tree/cli.js

// Locked.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --vm=-Xes6-proxy=false %t.tree/cli.js
// RUN: %hermes-node --build-exe=%t.tree/app --kit=%kit_dir %t.tree/app.hbb
// RUN: %t.tree/app | %FileCheck --check-prefix=NOPROXY %s
// NOPROXY: PROXY undefined

// The environment cannot open a locked artifact.
// RUN: env HERMES_NODE_VM_OPTIONS=-Xes6-proxy=true %not %t.tree/app 2>&1 | %FileCheck --check-prefix=LOCKED %s
// LOCKED: locked

// Unlocked.
// RUN: %hermes-node --build-bundle=%t.tree/open.hbb --vm=-Xes6-proxy=false --allow-vm-options-override %t.tree/cli.js
// RUN: %hermes-node --build-exe=%t.tree/open --kit=%kit_dir %t.tree/open.hbb
// RUN: %t.tree/open | %FileCheck --check-prefix=NOPROXY %s
// RUN: env HERMES_NODE_VM_OPTIONS=-Xes6-proxy=true %t.tree/open | %FileCheck --check-prefix=PROXY %s
// PROXY: PROXY function
```

`%kit_dir` and `REQUIRES: linker-available` are confirmed correct — they match `test/build-exe.js:10,44`.

- [x] **Step 2: Run the test to verify it fails**

Run lit with `--param kit_dir=$(pwd)/cmake-build-asan/kit` added to the standard command, after building the kit:
```bash
cmake --build cmake-build-asan --target hermes-node-kit
```
Expected: FAIL — the executable ignores its container's options.

- [x] **Step 3: Read the options in `bundle_main.cpp`**

Before the `runHermesNode` call, mirroring the `main()` logic from Task 6 but with the embedded reader and with no `--vm=` source (an executable has no flags of its own):

```cpp
  // The container's options, then HERMES_NODE_VM_OPTIONS if the container
  // allows it. There is no --vm= here: every argument this binary receives
  // belongs to the program, which is what makes process.argv.slice(2) mean
  // the same thing it means under --bundle=<f> arg.
  hermes::node_compat::BundleVmOptions bundleVm;
  std::string vmError;
  if (!hermes::node_compat::readEmbeddedBundleVmOptions(
          config.embeddedBundleData,
          config.embeddedBundleSize,
          &bundleVm,
          &vmError)) {
    std::fprintf(stderr, "error: %s\n", vmError.c_str());
    return 1;
  }
  std::vector<std::string> envVm = envVmOptionsForExe();
  if (!bundleVm.allowOverride && !envVm.empty()) {
    std::fprintf(
        stderr,
        "Error: this executable's VM options are locked and cannot be "
        "overridden.\n"
        "       HERMES_NODE_VM_OPTIONS is set in the environment.\n");
    return 1;
  }
  config.process.vmOptions = bundleVm.options;
  config.process.vmOptions.insert(
      config.process.vmOptions.end(), envVm.begin(), envVm.end());
```

There is no `envVmOptionsForExe()`. Task 6 put the splitter in `lib/vm-options/` and exported it from `vm_options.h` precisely so this file can call it:

```cpp
  std::vector<std::string> envVm = hermes::node_compat::splitVmOptionsEnv(
      std::getenv("HERMES_NODE_VM_OPTIONS"));
```

Link `hermesNodeVmOptions` into whatever target compiles `bundle_main.cpp` if it is not already reachable — it is compiled as the `hermesNodeBundleMain` OBJECT library and copied into the kit, so check `tools/hermes-node/CMakeLists.txt` and make sure a kit-linked executable resolves the symbol.

`bundle_main.cpp` already assigns `config.embeddedBundleData` and `config.embeddedBundleSize` from the `hermesNodeBundleStart`/`hermesNodeBundleEnd` linker symbols (lines 29-37). Place this block after those assignments and read the config fields, rather than the raw symbols, so there is one expression of the payload's extent.

- [x] **Step 4: Run the test to verify it passes**

Run the lit command with `--param kit_dir=...`.
Expected: PASS.

- [x] **Step 5: Format, full suite, commit**

```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
git add tools/hermes-node/bundle_main.cpp test/build-exe-vm-options.js
git commit -m "build-exe: a linked executable applies its container's VM options"
```

---

### Task 8: Document the feature

**Files:**
- Modify: `CLAUDE.md`

- [x] **Step 1: Add the section**

Add a `## Hermes VM Options` section to `CLAUDE.md`, placed after `## Compile Cache` and before `## AOT Bundles`. Match the surrounding prose style — it explains why things are the way they are, not just what they do. It must cover:

- `--vm=<flag>`, repeatable, one flag per occurrence, never whitespace-split, and why (`-Xperf-prof-dir=<dir>`).
- The delegation to `hermes::cli::RuntimeFlags` and the `Errs`-stream requirement that keeps `llvh::cl` from calling `exit()`.
- **The standing constraint that hermes-node must not link `hermesCompilerDriver`**, with the duplicate-registration reason.
- The 27/11 split and why the 11 are refused rather than ignored, naming the `vm::Runtime` boundary at `lib/runtime/hermes_node_runtime.cpp:718`.
- That `hermes::cli::buildRuntimeConfig()` is deliberately not called, and that `hermes/tools/hermes/hermes.cpp:110-141` is the mapping to diff against.
- That `-Xes6-block-scoping` and `-Xasync-generators` default to false in Hermes and true here, and that `getNumOccurrences()` is what preserves that.
- Format v5, the container section, the lock bit, and the `baked -> env -> --vm=` precedence.
- That `HERMES_NODE_VM_OPTIONS` applies to a container run only, and is whitespace-split.
- The test files.

- [x] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: record the VM options feature in CLAUDE.md"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
| --- | --- |
| Envelope spelling, `--vm-help` | 2, 3 |
| Delegation, `Errs`, synthesized argv, CompilerDriver constraint | 1, 2 |
| 27 honoured / 11 refused | 1 |
| Not calling `buildRuntimeConfig()`; `hermes.cpp` mapping | 2 |
| Preserving hermes-node's defaults via `getNumOccurrences()` | 2 |
| Parse once, share with both runtimes | 3 |
| Format v5, dump section | 4, 5 |
| Reading options before the runtime exists | 6, 7 |
| Override policy, lock bit, precedence | 5, 6, 7 |
| Conflict matrix | 3 |
| Testing (all six listed artifacts) | 1-7 |

**Not covered, deliberately:** the spec's "What this does not do" section describes absences, and needs no task.

**Type consistency:** `vmOptions` is the field name in `HermesNodeProcessConfig`, the parameter name in `buildBundle` and the member in `BundleWriter`. `BundleVmOptions::options` / `::allowOverride` are used identically in Tasks 6 and 7. `classifyVmOption` takes a bare name and `vmOptionFlagName` produces one; every call site pairs them.

**Soft spots, all three checked against this Hermes checkout before execution:**
- `llvh::cl::getRegisteredOptions()` exists and returns `StringMap<Option *> &` (`hermes/external/llvh/include/llvh/Support/CommandLine.h:1804`); `Option::HelpStr` is a public `StringRef` (line 266). Task 2's help-text code is correct as written.
- `%kit_dir` and `REQUIRES: linker-available` match `test/build-exe.js`. Correct as written.
- The payload reaches `bundle_main.cpp` as `config.embeddedBundleData` / `config.embeddedBundleSize`, assigned from `hermesNodeBundleStart`/`hermesNodeBundleEnd`. Task 7 has been corrected to use the config fields.

**Still genuinely unverified:** the `RuntimeConfig` getter names Task 2's tests use (`getES6BlockScoping()`, `getEnableAsyncGenerators()`, `getMicrotaskQueue()`, `getES6Proxy()`, `getGCConfig().getMaxHeapSize()`). They follow the `_HERMES_CTORCONFIG_STRUCT` macro's `get<Name>()` convention over the field names in `hermes/public/hermes/Public/RuntimeConfig.h:48-159`, but were not compiled. Adjust if the macro spells them differently.
