/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_FILE_SOURCE_H
#define HERMES_NODE_COMPAT_BUNDLE_FILE_SOURCE_H

#include <hermes/node-compat/bundle/bundle_reader.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Trims exactly one trailing '/' from \p s, unless \p s is "/" itself.
///
/// std::filesystem::path::lexically_normal(), which resolveSpecifier() uses
/// to build the paths it probes, emits exactly one trailing slash whenever
/// a ".." component cancels the segment before it ("a/b/.." normalizes to
/// "a/"), so a directory argument reaching a FileSource may carry one --
/// require('..') is all it takes.
///
/// Both backends have to trim it, for different-looking reasons that are
/// the same reason. POSIX stat() ignores a trailing slash on a directory,
/// so BundleFileSource has to trim to match DiskFileSource's answers; and
/// DiskFileSource has to trim before BUILDING "<dir>/package.json", or it
/// records "<dir>//package.json" -- a path that misses the producer's
/// pathIndex dedupe and packages the same file a second time. Shared here,
/// rather than copied into each .cpp, so the two cannot drift.
///
/// Only directory arguments are trimmed. isRegularFile() deliberately does
/// not use this: a trailing slash still makes it miss, exactly as stat()
/// does for a regular file (ENOTDIR).
inline std::string_view trimOneTrailingSlash(std::string_view s) {
  if (s.size() > 1 && s.back() == '/')
    s.remove_suffix(1);
  return s;
}

/// Everything module resolution needs to know about a file tree.
///
/// resolveSpecifier() implements the algorithm; this supplies the facts it
/// asks for. Two implementations exist: DiskFileSource, over the real
/// filesystem, used by the producer, and BundleFileSource, over a
/// container's identity set, used at run time. Sharing the algorithm across
/// both is the point -- a specifier that resolved one way at build time and
/// another at run time would load the wrong module silently.
class FileSource {
 public:
  virtual ~FileSource() = default;

  virtual bool isRegularFile(const std::string &path) const = 0;
  virtual bool isDirectory(const std::string &path) const = 0;

  /// The text of <dir>/package.json, or nullopt when there is none.
  /// Returning the text rather than the parsed "main" keeps the JSON
  /// parsing in one place, next to the algorithm that needs it.
  virtual std::optional<std::string> readPackageJson(
      const std::string &dir) = 0;
};

/// FileSource over the real filesystem.
class DiskFileSource : public FileSource {
 public:
  bool isRegularFile(const std::string &path) const override;
  bool isDirectory(const std::string &path) const override;
  std::optional<std::string> readPackageJson(const std::string &dir) override;

  /// Absolute paths of every `package.json` this instance has successfully
  /// read via readPackageJson(), in first-read order, deduplicated. The AOT
  /// bundle producer packages these so a run-time resolver can reproduce
  /// the same "main" lookups without a source tree on disk -- see
  /// buildBundle() in lib/bundle/bundle_build.cpp. Recording accumulates
  /// on this object, so a caller that wants a complete record must hold one
  /// DiskFileSource for the whole build rather than constructing one per
  /// call.
  const std::vector<std::string> &readPackageJsonPaths() const {
    return readPaths_;
  }

 private:
  std::vector<std::string> readPaths_;
};

/// FileSource over a bundle container's identity set: no filesystem access
/// at all. Identities are tree-relative paths with forward slashes (e.g.
/// "cli.js", "node_modules/dep/main.js"); there are no directory records,
/// so isDirectory() is inferred from the identity set rather than looked
/// up directly.
///
/// \p root is prepended to every identity to form the paths this source
/// answers about, e.g. root "/app" makes "cli.js" answer to
/// isRegularFile("/app/cli.js"). Stripping \p root back off an incoming
/// path is lexical and requires an exact "root + '/'" prefix (or the path
/// being exactly \p root, which is always a directory) -- no ".." climbing
/// and no symlink resolution, matching the producer's no-realpath policy.
/// A path not under \p root answers false to both isRegularFile() and
/// isDirectory(): that containment is the point of this class, so it is
/// not loosened. \p root == "/" (the filesystem root, reachable when a
/// bundle sits at the root of its container image) is handled without
/// doubling the separator; see the comment in stripRoot()'s definition.
class BundleFileSource : public FileSource {
 public:
  /// \p reader must outlive this object: the identity index built here
  /// holds string_views into its mapped bytes.
  BundleFileSource(const BundleReader &reader, std::string root);

  bool isRegularFile(const std::string &path) const override;
  bool isDirectory(const std::string &path) const override;
  std::optional<std::string> readPackageJson(const std::string &dir) override;

 private:
  /// One packaged identity, indexed for a quick reader lookup once a path
  /// under root_ has been found.
  struct Entry {
    std::string_view identity;
    uint32_t moduleIndex;
  };

  /// Strips root_ off \p path. Returns an empty string_view for \p path ==
  /// root_ (the root itself), the suffix after "root_ + '/'" when \p path
  /// is lexically under root_, or nullopt when it is neither.
  std::optional<std::string_view> stripRoot(const std::string &path) const;

  /// Exact match for \p relIdentity in entries_, or nullptr.
  const Entry *find(std::string_view relIdentity) const;

  const BundleReader &reader_;
  std::string root_;
  std::vector<Entry> entries_; // sorted by identity.
};

} // namespace node_compat
} // namespace hermes

#endif
