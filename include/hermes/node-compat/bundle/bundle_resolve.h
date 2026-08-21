/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_RESOLVE_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_RESOLVE_H

#include <hermes/node-compat/bundle/file_source.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Resolves \p specifier as written in a `require()` call inside \p
/// fromFile to an absolute file on disk, approximating the subset of Node's
/// CommonJS resolution algorithm (see
/// libjs-node/internal/modules/cjs/loader.js) that the AOT bundle producer
/// needs, answering every filesystem question through \p src instead of
/// going straight to the real filesystem -- so a caller with its own
/// FileSource (e.g. a container's identity set) resolves exactly the way
/// the producer did:
///
///   1. A relative specifier is joined against dirname(fromFile). "Relative"
///      is Node's own predicate from Module._resolveLookupPaths: starts with
///      '.' and is either exactly ".", or has '.' or '/' as its second
///      character -- so "." and ".." are relative, not bare. Any other
///      specifier is resolved by walking "<dir>/node_modules/<specifier>"
///      from dirname(fromFile) up to the filesystem root, skipping any
///      directory already named "node_modules" exactly as
///      Module._nodeModulePaths does.
///   2. The resulting base path is probed, in order: the exact path;
///      base + ".js"; base + ".ts"; base + ".json"; then, if base is a
///      directory: its package.json "main" field (resolved recursively
///      through this same probe), then index.js, index.ts, index.json.
///
/// This is not a full implementation of Node's resolver: package.json
/// "exports" is not consulted (see the comment at the parsing site in
/// bundle_resolve.cpp), and there is no self-reference or scoped-package
/// special casing.
///
/// \return the first probed path that exists as a regular file, made
///     absolute and lexically normalized; nullopt if none does.
std::optional<std::string> resolveSpecifier(
    FileSource &src,
    std::string_view fromFile,
    std::string_view specifier);

/// Resolves \p specifier the same way as the three-argument overload above,
/// against the real filesystem: constructs a DiskFileSource and forwards to
/// it. See that overload for the algorithm.
std::optional<std::string> resolveSpecifier(
    std::string_view fromFile,
    std::string_view specifier);

/// True if \p specifier names one of hermes-node's embedded builtin
/// modules, with or without a leading "node:" prefix -- e.g. both "fs" and
/// "node:fs" return true. The list is kept in sync by hand with the
/// `builtinIds` array in libjs/shims/internal/bootstrap/realm.js, which
/// remains the source of truth; it deliberately excludes that file's
/// separate `vendoredIds` (e.g. "ws"), which are ordinary node_modules
/// resolutions, not embedded builtins.
bool isBuiltinSpecifier(std::string_view specifier);

/// True if \p specifier names one of hermes-node's vendored packages, with
/// or without a leading "node:" prefix -- e.g. both "ws" and "node:ws"
/// return true. Kept in sync by hand with the `vendoredIds` array in
/// libjs/shims/internal/bootstrap/realm.js, the source of truth.
///
/// A vendored package is NOT a builtin (see isBuiltinSpecifier): a bare
/// require() of one resolves out of node_modules first and only falls back
/// to the embedded copy when no such package is installed, which is why the
/// producer still bundles an installed copy when it finds one. This
/// predicate exists for the other direction: an unresolvable vendored
/// specifier is served by the embedded copy at run time, so the producer
/// must warn and skip rather than fail the build.
bool isVendoredSpecifier(std::string_view specifier);

/// The longest path prefix shared by the directories containing every path
/// in \p absPaths (each path's dirname, not the path itself). With one
/// input, returns that file's directory. With inputs that share no path
/// component, returns "/".
std::string commonAncestor(const std::vector<std::string> &absPaths);

} // namespace node_compat
} // namespace hermes

#endif
