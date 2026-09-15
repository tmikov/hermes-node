/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_BUILD_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_BUILD_H

#include <hermes/node-compat/build-native/build_native.h>

#include <node_api.h>

#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Walks the CommonJS require() graph reachable from \p entryPath, compiles
/// every JavaScript file it finds to Hermes bytecode, and writes the result
/// as a single AOT bundle container (see bundle_writer.h / bundle_format.h)
/// to \p outPath.
///
/// The walk is two-pass: first the whole graph is discovered (reading every
/// file, scanning it for require() calls with scanRequires(), and resolving
/// each literal specifier with resolveSpecifier()), and only once that is
/// complete are modules and edges added to a BundleWriter -- module indices
/// must exist before an edge can reference them.
///
/// A literal specifier that fails to resolve is a hard error: it names a
/// module the runtime could never load either, whether from the bundle or
/// from disk, so failing the build is preferable to shipping a bundle whose
/// require() throws at run time.
///
/// Everything the CommonJS loader would execute as JavaScript is packaged as
/// JavaScript: .js, .cjs, .ts, and files with no extension at all (a bare
/// `node_modules/<pkg>/<name>` entry point is a real and common shape). .json
/// is packaged as its raw text. A specifier that resolves to anything else (a
/// .node addon, an asset, a config format some loader hook understands) is
/// not an error: it is left out of the bundle with a warning, and the
/// runtime's on-disk fallback handles it at load time, the same way it
/// already does for anything not found in an embedded bytecode table.
///
/// .mjs is skipped by the same mechanism but for a different reason, and the
/// difference matters: there is no working fallback to preserve, because
/// require() of an ESM file throws with or without a bundle. It is skipped
/// because its import/export syntax would be a syntax error inside the
/// CommonJS wrapper, failing the entire build over a module that could never
/// have run.
///
/// \p env supplies the Hermes runtime that compiles each module
/// (hermes_compile_to_bytecode); it must already have a runtime attached
/// (see runHermesNode). Every JavaScript file is compiled with the same
/// module wrapper the loader applies at run time (libjs/loader.js),
/// `(function(exports, require, module, __filename, __dirname) { ... })`,
/// and with optimization unconditionally on -- this is an ahead-of-time
/// artifact, so there is no fast/uncached path to protect the way there is
/// for an interactively-run script.
///
/// The container is written to a temporary file next to \p outPath and
/// rename()d into place on success, so a build that fails partway through
/// never leaves a truncated or partial bundle at \p outPath.
///
/// Diagnostics (errors and warnings) are printed to stderr. On success,
/// prints `bundle root: <root>` to stdout, where <root> is the longest
/// path prefix common to every file the walk visited (see
/// bundle_resolve.h's commonAncestor) -- the directory the consumer must
/// treat as the bundle's root when resolving disk fallbacks -- followed by
/// a final `bundle: <N> modules[, <M> packaged as throwing stubs]` line,
/// printed whether or not \p verbose was given, with the stub count
/// present only when it is non-zero.
///
/// When \p verbose is true, the walk additionally narrates itself to
/// stderr: the entry, the absolute output path, and the generation tag with
/// the version, architecture, bytecode format and optimization level folded
/// into it; every file as it is discovered; every require() as it is
/// resolved (including a `known` line when a specifier lands on a module
/// already discovered elsewhere -- how a shared dependency or a cycle shows
/// up); every skip and why; a `compile` line per module with source and
/// bytecode sizes, their ratio, and timing; and a summary of the finished
/// container -- module counts by kind, edge count and distinct specifiers,
/// string table entries and bytes, payload and bytecode bytes, the largest
/// single module, the total file size, and total compile time.
///
/// This is purely observational: with or without \p verbose, buildBundle
/// walks the same graph in the same order and writes the same bytes to
/// \p outPath. The summary is emitted after the container is serialized,
/// because three of its lines describe the laid-out container, but still
/// before the file is written.
///
/// \p includes names extra modules to seed into the worklist alongside \p
/// entryPath, each resolved from \p entryPath's directory exactly like a
/// require() the entry made (see resolveSpecifier). This is how a module
/// the static require() scanner cannot discover -- a specifier assembled at
/// run time, the shape Babel's preset loading has -- gets into the bundle
/// anyway: the caller names it explicitly with --include, and the walk
/// below packages it and everything it requires, the same as if the entry
/// had required it directly. An include that fails to resolve, or resolves
/// to something classifyFile() marks kSkip (a .node addon, an .mjs file),
/// is a hard build error rather than a silent skip: the caller named it, so
/// silence would hide a mistake rather than tolerate one.
///
/// \p preloads names extra modules to seed into the worklist the same way \p
/// includes does -- each resolved from \p entryPath's directory, and subject
/// to the same hard-error treatment for a specifier that fails to resolve or
/// resolves to something classifyFile() marks kSkip. What sets a preload
/// apart is that it is also recorded, in the order given (duplicates
/// collapsed to their first occurrence), in the container's preload table
/// (BundleWriter::addPreload), so the consumer runs it before the entry
/// point. A preload the entry (or an include) already reaches is packaged
/// once, same as any module reached two ways, and still recorded once as a
/// preload.
///
/// \p vmOptions are Hermes VM options recorded in the container's
/// VM-options table, in the order given, for the consumer to apply before
/// it creates its runtime. They are recorded, not applied: this run
/// compiles rather than executes, and a build machine's VM tuning is not
/// the artifact's business.
///
/// \p bakeWasmPaths names `--record-wasm` files (see wasm_record.h) whose
/// entries are copied into the container's Wasm table
/// (BundleWriter::addWasm), in the order given. Each is mapped and opened
/// with WasmRecordReader::open(); a file that fails to open, or whose
/// recorded build version does not exactly match HERMES_NODE_VERSION_STRING,
/// is a hard build error -- a recording made by a different build of
/// hermes-node is not trustworthy bytecode for this one, and finding that
/// out at run time (as a recompile nothing warned about) is worse than
/// refusing the build. A file that opens cleanly but records zero modules is
/// not an error, only a warning: it costs nothing to bake and is more likely
/// a forgotten `--record-wasm` run than a mistake worth failing over. Two
/// files that both record the same digest are not a conflict either: the
/// first one to name it wins and the rest are skipped, exactly like a module
/// reached by two require() edges.
///
/// \p allowVmOptionsOverride records whether those options may be
/// overridden at run time. False -- the default -- locks them, because the
/// honoured flag set includes -enable-eval and
/// -Xhermes-internal-test-methods, which are not tuning knobs.
///
/// \return 0 on success, non-zero on any error.
int buildBundle(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    bool verbose,
    const std::vector<std::string> &includes,
    const std::vector<std::string> &preloads,
    const std::vector<std::string> &bakeWasmPaths,
    const std::vector<std::string> &vmOptions,
    bool allowVmOptionsOverride);

/// Everything buildNativeExecutable() needs, gathered in one struct because
/// it is long even by this codebase's standard: a native build is
/// buildBundle's whole discovery/resolution/classification walk PLUS a
/// toolchain drive (shermes then cc, per module, then one link), so it has
/// every knob buildBundle has and every knob --build-exe has too.
struct NativeBuildOptions {
  std::string entryPath, outPath, kitDir, ccOverride, shermesOverride;
  std::vector<std::string> includes, preloads, bakeWasmPaths, vmOptions;
  bool allowVmOptionsOverride = false;
  unsigned jobs = 0; // 0 -> hardware_concurrency()
  OptLevel opt = OptLevel::O3;
  bool keepTemp = false, verbose = false;
};

/// Builds a standalone executable directly from an entry script, compiling
/// every JavaScript module it discovers to native code with Static Hermes
/// rather than to Hermes bytecode.
///
/// Shares its whole discovery/resolution/classification/container-assembly
/// walk with buildBundle() (see buildBundleImpl in bundle_build_internal.h)
/// -- two implementations that packaged different graphs for the same entry
/// would be the same class of defect as a specifier resolving differently
/// at build and run time. What differs is the payload step: instead of
/// bytecode, every module's unwrapped source comes back in
/// BuildProducts::pendingNative, compiled here with shermes and cc (see
/// lib/build-native), and linked into the container's own module-index
/// table of Static Hermes units.
///
/// A module's own JavaScript gets the same tolerance buildBundle() gives
/// it, just split across two stages instead of one. The scanner (shared by
/// both producers) catches a parse or sema failure before either ever
/// compiles, packaging it as a module that throws if required. Past that,
/// shermes gets the second chance buildBundle()'s own compile step gets:
/// an IRGen-or-later rejection -- `import()` inside a `.cjs` is the case
/// that forced this, since it parses fine and fails only here -- is
/// classified from the CommandResult shermes returned and, on a module
/// that is neither the entry nor a preload (both of which still hard-fail
/// unconditionally, being certain to run), packaged as a throwing stub the
/// same way.
///
/// A failure at the cc stage is always a hard build error, with no such
/// second chance: cc compiles the C shermes generated, never the module's
/// own JavaScript, so a rejection there says nothing about the module --
/// it is the toolchain or the machine (a clang allocation failure at -O3
/// on one large generated file, measured at 3.66 GB peak RSS on a
/// 1,500-module build, is the realistic case) and must never be reported
/// to the program as if it were a `SyntaxError` in its own source. Every
/// failed module is reported, not just the first.
///
/// \return a process exit code -- 0, or 1 with the reason reported on
/// stderr.
int buildNativeExecutable(const NativeBuildOptions &options);

} // namespace node_compat
} // namespace hermes

#endif
