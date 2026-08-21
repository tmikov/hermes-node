/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UNITTESTS_TEMP_TREE_H
#define HERMES_NODE_COMPAT_UNITTESTS_TEMP_TREE_H

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {
namespace test {

/// Creates directory \p path (POSIX mkdir, not the GNU `mkdir -p` shell
/// command) along with any missing parents.
inline void makeDirs(const std::string &path) {
  std::string partial;
  size_t start = 0;
  while (start < path.size()) {
    size_t slash = path.find('/', start + 1);
    partial = slash == std::string::npos ? path : path.substr(0, slash);
    if (!partial.empty())
      ::mkdir(partial.c_str(), 0755); // ignored if it already exists
    if (slash == std::string::npos)
      break;
    start = slash;
  }
}

/// Writes \p content to a new file at \p path, creating parent directories
/// first.
inline void writeFile(const std::string &path, const std::string &content) {
  size_t slash = path.find_last_of('/');
  if (slash != std::string::npos)
    makeDirs(path.substr(0, slash));
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f.is_open()) << path;
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  f.close();
  EXPECT_FALSE(f.fail()) << path;
}

/// Writes \p bytes to a new file at \p path, creating parent directories
/// first. A separate overload (rather than a std::string one accepting
/// arbitrary bytes) because callers that already hold binary data as
/// std::vector<uint8_t> -- a serialized container, in particular -- would
/// otherwise need to copy it into a std::string first just to call this.
inline void writeFile(
    const std::string &path,
    const std::vector<uint8_t> &bytes) {
  size_t slash = path.find_last_of('/');
  if (slash != std::string::npos)
    makeDirs(path.substr(0, slash));
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f.is_open()) << path;
  f.write(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  f.close();
  EXPECT_FALSE(f.fail()) << path;
}

/// A temporary directory, created via mkdtemp and removed (recursively) on
/// destruction. mkdtemp picks a name no other process or thread holds,
/// which is what makes this safe under a parallel test runner.
class TempTree {
 public:
  TempTree() {
    char tmpl[] = "/tmp/hntt-test-XXXXXX";
    const char *made = ::mkdtemp(tmpl);
    EXPECT_NE(nullptr, made);
    path_ = made ? made : "";
  }
  ~TempTree() {
    if (!path_.empty())
      ::system(("rm -rf " + path_).c_str());
  }
  const std::string &path() const {
    return path_;
  }

  /// The absolute path \p name would have directly inside this tree, with
  /// no file created -- for a caller that wants the path a later step
  /// (BundleWriter::serialize(), for instance) will write to, rather than
  /// content this class writes itself.
  std::string path(const std::string &name) const {
    return path_ + "/" + name;
  }

  /// Writes \p content to a new file named \p name directly inside this
  /// tree and returns its absolute path. A convenience wrapper around the
  /// free writeFile() above, for the common case of one file with no
  /// subdirectory nesting.
  std::string write(const std::string &name, const std::string &content) const {
    std::string full = path_ + "/" + name;
    writeFile(full, content);
    return full;
  }

  /// Writes \p bytes to a new file named \p name directly inside this tree
  /// and returns its absolute path. The std::vector<uint8_t> counterpart to
  /// write() above, for binary content -- a serialized container, in
  /// particular -- that would otherwise need copying into a std::string
  /// just to call it.
  std::string writeBytes(
      const std::string &name,
      const std::vector<uint8_t> &bytes) const {
    std::string full = path_ + "/" + name;
    writeFile(full, bytes);
    return full;
  }

 private:
  std::string path_;
};

} // namespace test
} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_UNITTESTS_TEMP_TREE_H
