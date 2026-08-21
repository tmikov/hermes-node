/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/file_source.h>

#include <sys/stat.h>

#include <fstream>
#include <sstream>

namespace hermes {
namespace node_compat {

bool DiskFileSource::isRegularFile(const std::string &path) const {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool DiskFileSource::isDirectory(const std::string &path) const {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::optional<std::string> DiskFileSource::readPackageJson(
    const std::string &dir) {
  // Trimmed, not concatenated raw: lexically_normal() hands this a
  // trailing slash whenever a ".." cancelled a segment (require('..') is
  // enough), and "<dir>//package.json" opens the same file while reading
  // as a different string afterwards -- which is what made the producer
  // package one package.json twice, under two paths and one identity. See
  // trimOneTrailingSlash().
  std::string path = std::string(trimOneTrailingSlash(dir)) + "/package.json";
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  // Dedup through the set (a hash lookup) rather than a linear scan of
  // readPaths_, which a large tree (many packages sharing few common
  // dependencies, each read repeatedly) can make expensive.
  if (readPathsSet_.insert(path).second)
    readPaths_.push_back(std::move(path));
  return ss.str();
}

} // namespace node_compat
} // namespace hermes
