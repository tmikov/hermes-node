/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_BUILD_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_BUILD_H

#include <node_api.h>

#include <string>

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
/// treat as the bundle's root when resolving disk fallbacks.
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
/// \return 0 on success, non-zero on any error.
int buildBundle(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    bool verbose);

} // namespace node_compat
} // namespace hermes

#endif
