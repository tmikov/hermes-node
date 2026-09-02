/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_RUN_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_RUN_H

#include <node_api.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

/// What a container records about its VM configuration.
struct BundleVmOptions {
  /// The recorded options, in application order.
  std::vector<std::string> options;
  /// Whether they may be overridden at run time.
  bool allowOverride = false;
};

/// Reads a container's VM options without opening it for execution.
///
/// The options decide how the runtime is built, but the run path opens a
/// container only after the runtime already exists -- runBundle() takes a
/// napi_env, so by the time it runs there is nothing left to configure.
/// Rather than restructure that ordering, this maps and validates the file
/// a second time, reads the two things it needs, and closes: one extra
/// map-and-validate of a file that is about to be mapped again by
/// openBundle(), in exchange for not threading VM configuration through
/// the run path's plumbing.
///
/// Uses BundleReader::openForInspection(), not open(): this is not an
/// execution path, and a container whose generation tag does not match
/// this binary must still be able to say what its options are (so that,
/// for instance, a caller can report the mismatch before deciding what
/// else to do). The subsequent openBundle() call still enforces the tag,
/// so nothing mismatched ever actually runs.
///
/// Unlike openBundle(), this touches none of the process-wide state that
/// tracks the one open bundle (OpenBundle in bundle_run.cpp): the mapping
/// and reader it builds are local to the call and go away when it
/// returns, so calling it does not count against -- and does not
/// interfere with -- openBundle()'s "only one bundle can be open at a
/// time" rule, whether called before or after that call.
///
/// Returns false and sets \p error on any failure to map or validate the
/// file -- the same failures openBundle() reports, in the same words.
bool readBundleVmOptions(
    const std::string &path,
    BundleVmOptions *out,
    std::string *error);

/// The same, for a container linked into this executable rather than
/// mapped from a file. \p data and \p size name the payload object's
/// contents, exactly as openEmbeddedBundle() takes them. Touches none of
/// the process-wide open-bundle state either, for the same reason.
bool readEmbeddedBundleVmOptions(
    const uint8_t *data,
    size_t size,
    BundleVmOptions *out,
    std::string *error);

/// Defines six functions on globalThis -- called "bundle natives" here in
/// the Node-API sense (native code exposed to JavaScript), not to be
/// confused with the native *addons* (.node files) this module also
/// supports: `__bundleLookup(importerIdentity, specifier)`,
/// `__bundleResolve(fromIdentity, request, paths?)`, `__bundleLoad(identity)`,
/// `__bundleEntry()`, `__bundleRoot()`, `__bundleNatives()` -- and returns,
/// through \p bundleObject, a plain `{lookup, resolve, load, entry, root,
/// natives}` object bound to the same six functions. That object is what
/// libjs/bundle-loader.js takes as its `bundle` parameter.
///
/// openBundle() must have succeeded first.
napi_status installBundleGlobals(napi_env env, napi_value *bundleObject);

} // namespace node_compat
} // namespace hermes

#endif
