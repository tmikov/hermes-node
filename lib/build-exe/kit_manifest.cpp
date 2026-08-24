/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Reads `<kitDir>/kit.manifest`, written by utils/make-kit.py. UTF-8 text,
// one `key: value` per line; `#` comments and blank lines are ignored.
// Repeated keys (`driverflag`, `linkarg`) accumulate in order -- that order
// is load-bearing, it is a linker command line. Keys:
//   version    -- exactly once, the hermes-node version the kit was cut
//                 from.
//   cc         -- exactly once, the compiler driver path.
//   driverflag -- repeated, ordered; flags that go before the input
//                 objects.
//   linkarg    -- repeated, ordered; arguments that go after them. `{kit}`
//                 inside a linkarg value is replaced with the kit
//                 directory -- it appears on kit-relative paths only, never
//                 on absolute system paths.
// Any other key is an error naming the key: an unknown key means the kit
// was cut by a newer make-kit.py recording something this reader would
// otherwise silently drop.

#include <hermes/node-compat/build-exe/kit_manifest.h>

#include <fstream>
#include <sstream>

namespace hermes {
namespace node_compat {

namespace {

/// Replaces every occurrence of "{kit}" in \p value with \p kitDir.
std::string substituteKitDir(
    const std::string &value,
    const std::string &kitDir) {
  static const std::string kPlaceholder = "{kit}";
  std::string result;
  result.reserve(value.size());
  size_t pos = 0;
  while (true) {
    size_t found = value.find(kPlaceholder, pos);
    if (found == std::string::npos) {
      result.append(value, pos, std::string::npos);
      break;
    }
    result.append(value, pos, found - pos);
    result.append(kitDir);
    pos = found + kPlaceholder.size();
  }
  return result;
}

/// Trims a trailing '\r', so a manifest with CRLF line endings (however it
/// got that way) still parses.
std::string trimTrailingCR(const std::string &line) {
  if (!line.empty() && line.back() == '\r')
    return line.substr(0, line.size() - 1);
  return line;
}

/// True if \p line is empty, all whitespace, or a '#' comment (the first
/// non-whitespace character is '#').
bool isBlankOrComment(const std::string &line) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    ++i;
  return i == line.size() || line[i] == '#';
}

} // namespace

std::optional<KitManifest> readKitManifest(
    const std::string &kitDir,
    std::string *error) {
  std::string manifestPath = kitDir + "/kit.manifest";
  std::ifstream in(manifestPath, std::ios::binary);
  if (!in.is_open()) {
    if (error)
      *error = "cannot open kit manifest: " + manifestPath;
    return std::nullopt;
  }

  KitManifest manifest;
  manifest.kitDir = kitDir;
  bool sawVersion = false;
  bool sawCC = false;

  std::string rawLine;
  size_t lineNo = 0;
  while (std::getline(in, rawLine)) {
    ++lineNo;
    std::string line = trimTrailingCR(rawLine);
    if (isBlankOrComment(line))
      continue;

    size_t sep = line.find(": ");
    if (sep == std::string::npos) {
      if (error) {
        std::ostringstream os;
        os << manifestPath << ":" << lineNo
           << ": malformed line (expected 'key: value'): " << line;
        *error = os.str();
      }
      return std::nullopt;
    }
    std::string key = line.substr(0, sep);
    std::string value = line.substr(sep + 2);

    if (key == "version") {
      if (sawVersion) {
        if (error)
          *error = manifestPath + ": duplicate key 'version'";
        return std::nullopt;
      }
      manifest.version = value;
      sawVersion = true;
    } else if (key == "cc") {
      if (sawCC) {
        if (error)
          *error = manifestPath + ": duplicate key 'cc'";
        return std::nullopt;
      }
      manifest.cc = value;
      sawCC = true;
    } else if (key == "driverflag") {
      manifest.driverFlags.push_back(value);
    } else if (key == "linkarg") {
      manifest.linkArgs.push_back(substituteKitDir(value, kitDir));
    } else {
      if (error)
        *error = manifestPath + ": unknown key '" + key + "'";
      return std::nullopt;
    }
  }

  if (!sawVersion) {
    if (error)
      *error = manifestPath + ": missing required key 'version'";
    return std::nullopt;
  }
  if (!sawCC) {
    if (error)
      *error = manifestPath + ": missing required key 'cc'";
    return std::nullopt;
  }

  return manifest;
}

} // namespace node_compat
} // namespace hermes
