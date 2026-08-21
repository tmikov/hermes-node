/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_FILE_SOURCE_H
#define HERMES_NODE_COMPAT_BUNDLE_FILE_SOURCE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes {
namespace node_compat {

// Forward-declared rather than including bundle_reader.h: this header only
// ever names BundleReader in a reference (BundleFileSource::reader_) and a
// constructor parameter, neither of which needs a complete type here. The
// .cpp files that call its methods include bundle_reader.h themselves.
class BundleReader;

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

  /// \p path may carry at most one trailing '/', never more:
  /// resolveSpecifier() builds every path it probes through
  /// std::filesystem::path::lexically_normal() (see joinNormalized() in
  /// bundle_resolve.cpp), which cannot emit two in a row. The two
  /// implementations honour that invariant by different mechanisms:
  /// DiskFileSource relies on POSIX stat(), which itself ignores a single
  /// trailing slash on a directory; BundleFileSource strips it explicitly
  /// (trimOneTrailingSlash(), below) before comparing against its identity
  /// set. Neither is required to handle more than one.
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
  // Membership mirror of readPaths_, so a repeat read is a hash lookup
  // instead of a linear scan of everything read so far.
  std::unordered_set<std::string> readPathsSet_;
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
/// doubling the separator; see the comment on the constructor's definition.
class BundleFileSource : public FileSource {
 public:
  /// \p reader must outlive this object: identityFor() and the identity
  /// index it is built from (lazily, on first query -- see index() below)
  /// hold string_views into \p reader's mapped bytes.
  BundleFileSource(const BundleReader &reader, std::string root);

  bool isRegularFile(const std::string &path) const override;
  bool isDirectory(const std::string &path) const override;
  std::optional<std::string> readPackageJson(const std::string &dir) override;

  /// The identity for \p path, i.e. \p path with root_ stripped off:
  /// an empty view for \p path == root_ (the root itself, always a
  /// directory), the suffix after "root_ + '/'" when \p path is lexically
  /// under root_, or nullopt when it is neither.
  ///
  /// The returned view points into \p path -- the caller's argument -- and
  /// NOT into this object or root_, so it is only valid as long as \p path
  /// is. This is the exact shape that already caused one dangling-view bug
  /// in this file's history (a view bound to a temporary that was gone by
  /// the next statement); see readPackageJson()'s definition for the
  /// pattern that avoids it.
  std::optional<std::string_view> identityFor(std::string_view path) const;

 private:
  /// One packaged identity, indexed for a quick reader lookup once a path
  /// under root_ has been found.
  struct Entry {
    std::string_view identity;
    uint32_t moduleIndex;
  };

  /// Builds (on first call) and returns the sorted identity index. Lazy so
  /// a bundle whose edge table already answers every require() -- the
  /// common case -- never pays to build it. entries_ is mutable so this
  /// method can stay const; this runtime is single-threaded (the callback
  /// that reaches this class runs on the one JS thread, never concurrently
  /// with anything else), so a plain std::optional is enough -- no locking.
  const std::vector<Entry> &index() const;

  /// Exact match for \p relIdentity in the identity index, or nullptr.
  const Entry *find(std::string_view relIdentity) const;

  const BundleReader &reader_;
  std::string root_;
  // root_ + '/', precomputed once so identityFor() can compare against it
  // without allocating on every query; root_ itself when it already ends
  // in '/' (the root == "/" case -- see the constructor's definition).
  std::string rootPrefix_;
  mutable std::optional<std::vector<Entry>> entries_; // sorted by identity.
};

} // namespace node_compat
} // namespace hermes

#endif
