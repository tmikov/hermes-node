/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/file_source.h>

#include <hermes/node-compat/bundle/bundle_reader.h>

#include <algorithm>

namespace hermes {
namespace node_compat {

BundleFileSource::BundleFileSource(const BundleReader &reader, std::string root)
    : reader_(reader), root_(std::move(root)) {
  // root_ == "/" (the filesystem root -- reachable via bundle_run.cpp,
  // which derives root from a bundle file's parent directory, and a
  // bundle installed at the filesystem root, e.g. a Docker `COPY app.hbb
  // /`, makes that parent "/") already ends in the separator; appending
  // another would turn the prefix into "//" and make every path under the
  // real root miss. An empty root_ does not end in a separator and still
  // wants the "/" prefix appended, exactly as before.
  bool rootEndsInSlash = !root_.empty() && root_.back() == '/';
  rootPrefix_ = rootEndsInSlash ? root_ : root_ + "/";
}

const std::vector<BundleFileSource::Entry> &BundleFileSource::index() const {
  if (!entries_) {
    entries_.emplace();
    entries_->reserve(reader_.moduleCount());
    for (uint32_t i = 0, n = reader_.moduleCount(); i < n; ++i)
      entries_->push_back(Entry{reader_.identity(i), i});
    std::sort(
        entries_->begin(), entries_->end(), [](const Entry &a, const Entry &b) {
          return a.identity < b.identity;
        });
  }
  return *entries_;
}

std::optional<std::string_view> BundleFileSource::identityFor(
    std::string_view path) const {
  if (path == root_)
    return std::string_view(); // the root itself: always a directory.
  if (path.compare(0, rootPrefix_.size(), rootPrefix_) != 0)
    return std::nullopt; // not lexically under root_.
  return path.substr(rootPrefix_.size());
}

const BundleFileSource::Entry *BundleFileSource::find(
    std::string_view relIdentity) const {
  const std::vector<Entry> &entries = index();
  auto it = std::lower_bound(
      entries.begin(),
      entries.end(),
      relIdentity,
      [](const Entry &e, std::string_view key) { return e.identity < key; });
  if (it != entries.end() && it->identity == relIdentity)
    return &*it;
  return nullptr;
}

bool BundleFileSource::isRegularFile(const std::string &path) const {
  std::optional<std::string_view> rel = identityFor(path);
  if (!rel || rel->empty())
    return false; // the root itself is a directory, never a regular file.
  return find(*rel) != nullptr;
}

bool BundleFileSource::isDirectory(const std::string &path) const {
  std::string trimmed(trimOneTrailingSlash(path));
  std::optional<std::string_view> rel = identityFor(trimmed);
  if (!rel)
    return false;
  if (rel->empty())
    return true; // the root itself.

  // Appending the separator before comparing is what makes the match
  // segment-aware: the first identity lexically >= "dep/" is the first
  // candidate whose path starts with the "dep" segment followed by '/',
  // so a sibling like "depot/..." (which sorts after "dep/..." because
  // 'o' > '/') never matches, even though "dep" is a string prefix of it.
  std::string prefix = std::string(*rel) + "/";
  const std::vector<Entry> &entries = index();
  auto it = std::lower_bound(
      entries.begin(),
      entries.end(),
      std::string_view(prefix),
      [](const Entry &e, std::string_view key) { return e.identity < key; });
  return it != entries.end() &&
      it->identity.compare(0, prefix.size(), prefix) == 0;
}

std::optional<std::string> BundleFileSource::readPackageJson(
    const std::string &dir) {
  // path must outlive rel: identityFor() returns a string_view into its
  // argument, so binding the concatenation straight to a temporary would
  // leave rel dangling as soon as this statement ends.
  std::string path = std::string(trimOneTrailingSlash(dir)) + "/package.json";
  std::optional<std::string_view> rel = identityFor(path);
  if (!rel || rel->empty())
    return std::nullopt;
  const Entry *entry = find(*rel);
  if (!entry)
    return std::nullopt;
  return std::string(reader_.payload(entry->moduleIndex));
}

} // namespace node_compat
} // namespace hermes
