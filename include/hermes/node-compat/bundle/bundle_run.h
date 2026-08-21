/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_RUN_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_RUN_H

#include <node_api.h>

#include <string>

namespace hermes {
namespace node_compat {

/// Maps the container at \p path read-only and validates it against this
/// build's generation tag. Returns false and sets \p error on any failure:
/// bad magic, format version mismatch, generation mismatch, truncation, or
/// an I/O error. A failure is fatal to the process by policy -- a bundle is
/// a deliverable, and silently recompiling from a source tree that may not
/// exist is worse than refusing to start -- so the caller prints the message
/// and exits non-zero rather than falling back to disk.
///
/// On success the mapping is never unmapped: bundled bytecode is executed in
/// place out of it and stays reachable from the runtime for as long as the
/// process lives. On failure nothing is left mapped. Only one bundle can be
/// open at a time; a second call fails.
bool openBundle(const std::string &path, std::string *error);

/// Defines the five bundle natives on globalThis --
/// `__bundleLookup(importerIdentity, specifier)`,
/// `__bundleResolve(fromIdentity, request, paths?)`, `__bundleLoad(identity)`,
/// `__bundleEntry()`, `__bundleRoot()` -- and returns, through
/// \p bundleObject, a plain `{lookup, resolve, load, entry, root}` object
/// bound to the same five functions. That object is what
/// libjs/bundle-loader.js takes as its `bundle` parameter.
///
/// openBundle() must have succeeded first.
napi_status installBundleGlobals(napi_env env, napi_value *bundleObject);

} // namespace node_compat
} // namespace hermes

#endif
