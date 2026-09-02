/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Settings that describe the process rather than any one runtime, shared by
/// every runtime hermes-node starts -- including the second runtime that
/// hosts the inspector.
///
/// The split from HermesNodeConfig is deliberate and load-bearing: a field
/// here is inherited by every runtime automatically, a field in
/// HermesNodeConfig is inherited by none. Neither default is safe for both
/// groups, and both directions have already gone wrong. Inheriting
/// scriptPath would run the user's program a second time on the inspector
/// thread, because scriptPath takes precedence over evalCode where the
/// runtime decides what to execute; inheriting requireModules would re-run
/// every -r preload; inheriting inspect would start a recursive inspector.
/// Conversely, NOT inheriting the compile-cache settings meant the inspector
/// runtime silently enabled a cache of its own at the default root, ignoring
/// --no-compile-cache and --compile-cache.
///
/// So: put a setting here only if every runtime in the process should share
/// it. Anything describing what a particular runtime runs, or how its
/// debugger is wired up, belongs in HermesNodeConfig.
/// Whether to run the optimization pipeline when compiling JavaScript.
enum class OptimizeMode : uint8_t {
  /// On when the compile cache is active, off when it is not.
  ///
  /// Optimizing costs compile time and buys execution speed, so it only pays
  /// when the result is kept. With a cache the compile happens once and every
  /// later run gets the faster bytecode; without one it would be paid on
  /// every run and thrown away, and the uncached path exists precisely to
  /// minimise start-up latency.
  kDefault,
  /// Always optimize. Rejected together with --inspect: optimizing requires
  /// the compile API, which emits DebugInfoSetting::THROWING rather than the
  /// ALL the debugger needs to set breakpoints.
  kOn,
  /// Never optimize.
  kOff,
};

struct HermesNodeProcessConfig {
  /// Override process.version. Empty = use default.
  std::string nodeVersion;

  /// Compile cache root directory. Empty = use the default XDG location.
  std::string compileCacheDir;

  /// Disable the on-disk compile cache entirely.
  bool disableCompileCache = false;

  /// Whether compiled JavaScript is run through the optimization pipeline.
  OptimizeMode optimize = OptimizeMode::kDefault;

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
};

/// Configuration for a hermes-node runtime instance.
struct HermesNodeConfig {
  /// Settings shared with every other runtime this process starts. Copied
  /// wholesale into the inspector runtime's config; see
  /// HermesNodeProcessConfig for why this is separated out.
  HermesNodeProcessConfig process;

  /// Script file to execute. Empty = no script file.
  std::string scriptPath;

  /// When non-empty, run in AOT bundle producer mode instead of executing
  /// scriptPath: walk the require() graph reachable from scriptPath, compile
  /// it, and write a single-file bundle to this path (see
  /// hermes/node-compat/bundle/bundle_build.h). scriptPath is still the
  /// entry point in this mode; it is just not itself run.
  std::string buildBundlePath;

  /// Narrate the --build-bundle walk to stderr (discovery, resolution,
  /// skips, per-module compile stats, totals). No effect outside bundle
  /// producer mode; never changes the bytes buildBundle writes.
  bool verbose = false;

  /// --include=<specifier>, repeatable. Extra roots to seed into the
  /// --build-bundle worklist alongside scriptPath, for a module the static
  /// require() scanner cannot discover -- e.g. a preset name Babel resolves
  /// from configuration at run time. Each is resolved from scriptPath's
  /// directory exactly like a require() the entry made. No effect outside
  /// bundle producer mode.
  std::vector<std::string> includeModules;

  /// --preload=<specifier>, repeatable. Like includeModules, an extra root
  /// seeded into the --build-bundle worklist alongside scriptPath and
  /// resolved from scriptPath's directory the same way -- but also recorded
  /// in the container's preload table (BundleWriter::addPreload), so the
  /// bundle consumer runs it before the entry point. No effect outside
  /// bundle producer mode.
  std::vector<std::string> preloadModules;

  /// When non-empty, run in AOT bundle consumer mode: map the container at
  /// this path (see hermes/node-compat/bundle/bundle_run.h), install the
  /// bundle-aware Module._load wrapper, and execute the bundle's entry
  /// module instead of a script from disk. scriptPath is not used in this
  /// mode; process.argv[1] is this path, exactly as given on the command
  /// line.
  std::string bundlePath;

  /// Non-null when this binary was linked with a bundle rather than given
  /// one. Set only by tools/hermes-node/bundle_main.cpp, which is the one
  /// translation unit that knows the payload symbols exist; the CLI never
  /// sets it, and the runtime never looks for the symbols itself. That is
  /// what makes "am I an app?" a link-time fact rather than a run-time
  /// question, with no fuse and no weak symbols.
  const uint8_t *embeddedBundleData = nullptr;
  size_t embeddedBundleSize = 0;

  /// Inline JS code to eval after bootstrap, before event loop.
  /// Useful for programmatic use (e.g. inspector runtime).
  std::string evalCode;

  /// process.argv values. First element should be the binary name.
  std::vector<std::string> argv;

  /// Modules to require before the script, eval code, or REPL runs, in the
  /// order given. Resolved like require() from the current directory.
  std::vector<std::string> requireModules;

  /// Start the REPL when no scriptPath and no evalCode are provided.
  bool enableRepl = false;

  /// Enable the inspector (CDP debugger).
  bool inspect = false;

  /// Pause before executing the first user statement (implies inspect).
  bool inspectBrk = false;

  /// Inspector host address.
  std::string inspectHost = "127.0.0.1";

  /// Inspector port (0 = OS-assigned).
  int inspectPort = 9229;

  /// Open the DevTools URL in the system browser once the inspector is
  /// listening (implies inspect).
  bool inspectOpen = false;

  /// Opaque pointer to the inspector bridge context for cross-thread CDP
  /// messaging. Null for the user runtime (normal operation). Set when this
  /// runtime is the inspector runtime running on the IO thread.
  void *inspectorBridgeContext = nullptr;
};

/// Run a complete hermes-node instance. Blocks until the event loop exits.
/// Thread-safe: can be called from any thread; each invocation is fully
/// independent (own runtime, event loop, bindings state).
/// Returns the process exit code.
int runHermesNode(const HermesNodeConfig &config);

} // namespace node_compat
} // namespace hermes
