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
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes {
namespace node_compat {

/// Escapes \p s for use inside a double-quoted JavaScript string literal.
/// Defined in bundle_build.cpp; shared so the native producer's own
/// tolerant-stub path (bundle_build_native.cpp) can build the identical
/// throw text makeThrowingStub below does.
std::string quoteForJSString(std::string_view s);

/// The source of the module that stands in for a path this build cannot
/// turn into code, because the compiler rejected it with \p error at \p
/// path. Shared between the bytecode producer's own compile-failure stub
/// site (bundle_build.cpp) and the native producer's (bundle_build_native.
/// cpp, Task 17) so the two can never package a different stub for the same
/// failure. See the doc comment on the definition in bundle_build.cpp for
/// why the stub throws rather than the build failing.
std::string makeThrowingStub(const std::string &path, const std::string &error);

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
  std::string identity; // relative to the bundle root; what -source-name gets
  std::string source; // unwrapped; the caller applies the CJS wrapper
  uint32_t moduleIndex = 0;
  bool typeScript = false;
  /// True for the entry and for a preload, both certain to run. The native
  /// producer (Task 17) uses this to decide whether a compile rejection may
  /// be packaged as a throwing stub or must fail the build -- the same
  /// entry-or-preload test bundle_build.cpp's own two stub sites already
  /// apply, just not derivable after the fact from moduleIndex alone.
  bool isEntry = false;
  bool isPreload = false;
};

/// One native addon's placement, still to be reported by a native build and
/// (unless already \c inPlace) still to be copied.
struct SidecarCopy {
  std::string src, dst;
  /// The addon already IS `dst`: nothing to copy, but it is still one of
  /// this executable's required addons and still gets its `native:` line
  /// and its place in the count -- both were missing for exactly this case
  /// until this field existed, because an in-place addon used to be
  /// dropped from BuildProducts entirely (see bundle_build.cpp's own
  /// NativePlacement, which this mirrors).
  bool inPlace = false;
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
  /// Every native addon this build requires, source and destination already
  /// resolved against sidecarDir -- including one whose destination is
  /// already its source (\c inPlace), which needs no copy but is still
  /// required and still reported. Empty in Bytecode mode, where the copies
  /// have already happened.
  std::vector<SidecarCopy> sidecarCopies;
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
///
/// \p products must not be null: it is dereferenced unconditionally, both
/// callers (buildBundle's own forwarder and buildNativeExecutable) always
/// pass the address of a live, local BuildProducts.
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
