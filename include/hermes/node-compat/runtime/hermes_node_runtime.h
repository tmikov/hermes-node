/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/runtime/embedder_module_prefix.h>

#include <node_api_types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// A native module the embedder (rather than a compiled-in Node builtin or
/// a file on disk) makes available under a "hermes-node:" specifier (see
/// kEmbedderModulePrefix, embedder_module_prefix.h).
///
/// Resolved by libjs/loader.js's requireModule() before any vendored-package
/// check or disk fallback, and by its __initCJS wrapper before Module._load
/// ever calls the real loader -- so a request for one of these never
/// performs a filesystem lookup, matched or not. The same holds for
/// require.resolve(): __initCJS also wraps Module._resolveFilename, and
/// makeRequireFunction's own resolve() is covered by the wrapped _load it
/// calls through, so require() and require.resolve() agree the way they do
/// for a real builtin.
struct EmbedderModule {
  /// Full specifier a user script requires, e.g. "hermes-node:ui". Must
  /// start with kEmbedderModulePrefix; there is no bare (unprefixed) form,
  /// unlike a real Node builtin's optional "node:" scheme, and no
  /// "node:"-doubled form either (RULING 36: "node:hermes-node:x" is
  /// rejected, not accepted as a spelling of "hermes-node:x"). Validated at
  /// registration -- see registerEmbedderModule() below.
  std::string name;

  /// Populates and returns the module's exports object. Follows the same
  /// napi_addon_register_func convention as BindingRegistry's bindings and
  /// process.dlopen's native addons: given an empty exports object, either
  /// mutate and return it, or return a different value to replace it
  /// entirely. Returning null leaves a pending JS exception, exactly like a
  /// binding's init function.
  napi_addon_register_func init;
};

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

  /// --allow-vm-options-override: record in the container being built that
  /// process.vmOptions may be overridden at run time by a later --vm= or
  /// HERMES_NODE_VM_OPTIONS. False -- the default -- locks them. No effect
  /// outside bundle producer mode.
  bool allowVmOptionsOverride = false;

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

  /// Native modules the embedder exposes under a "hermes-node:" specifier,
  /// resolved by the module loader ahead of any filesystem lookup. Empty by
  /// default; guarded end to end by HERMES_NODE_ENABLE_UI in the CLI that
  /// populates this (--test-embedder-module today, hermes-node:ui later).
  /// Not inherited by the inspector runtime -- see HermesNodeProcessConfig
  /// for why a per-runtime field like this one stays here rather than
  /// there.
  std::vector<EmbedderModule> embedderModules;

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

/// Validates \p module and appends it to \p config.embedderModules.
///
/// A module registered under a name that does not start with
/// kEmbedderModulePrefix would be a module nothing can ever require -- a
/// silent no-op registration, the swallow-and-continue pattern this
/// codebase removes on sight elsewhere -- so that is refused outright, with
/// a message on stderr naming the bad value. A second registration under a
/// name already taken is refused the same way rather than silently
/// replacing or shadowing the first: registration order becoming
/// load-bearing, unannounced, is worse than an embedder finding out at
/// startup that two of its own modules collided.
///
/// \return true and appends on success; false (config unchanged) on either
///     validation failure, having already printed why.
bool registerEmbedderModule(HermesNodeConfig &config, EmbedderModule module);

/// Run a complete hermes-node instance. Blocks until the event loop exits.
/// Thread-safe: can be called from any thread; each invocation is fully
/// independent (own runtime, event loop, bindings state).
/// Returns the process exit code.
int runHermesNode(const HermesNodeConfig &config);

} // namespace node_compat
} // namespace hermes
