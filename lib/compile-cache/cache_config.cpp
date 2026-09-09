/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/compile-cache/cache_config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

/// The text written when the file is created. Kept beside the parser so the
/// two cannot describe different keys.
const char *kDefaultConfigText =
    "# hermes-node compile cache configuration.\n"
    "#\n"
    "# recency: atime | mtime\n"
    "#   Which timestamp Wasm entry eviction orders by. atime gives real LRU\n"
    "#   under relatime; use mtime on a noatime mount.\n"
    "# max_wasm_bytes: <integer>\n"
    "#   Budget over Wasm bytecode entries. Oldest are evicted first once a\n"
    "#   new entry would exceed it.\n"
    "#\n"
    "# Edited values take effect on the next run.\n"
    "recency: atime\n"
    "max_wasm_bytes: 268435456\n";

} // namespace

/// Append "line <n>: <what>" to \p complaints, if anyone is collecting.
static void complain(
    std::vector<std::string> *complaints,
    size_t lineNo,
    const std::string &what) {
  if (complaints != nullptr)
    complaints->push_back("line " + std::to_string(lineNo) + ": " + what);
}

/// Render \p v for a message, so an invisible character in the file does not
/// produce an invisible complaint about it.
static std::string quoteValue(std::string_view v) {
  std::string out("'");
  for (char c : v) {
    if (c >= 0x20 && c < 0x7f) {
      out += c;
    } else {
      static const char kHex[] = "0123456789abcdef";
      out += "\\x";
      out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xf];
      out += kHex[static_cast<unsigned char>(c) & 0xf];
    }
  }
  out += '\'';
  return out;
}

CacheConfig cacheConfigParse(
    std::string_view text,
    std::vector<std::string> *complaints) {
  CacheConfig config;
  size_t pos = 0;
  size_t lineNo = 0;
  while (pos <= text.size()) {
    ++lineNo;
    size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(
        pos, nl == std::string_view::npos ? text.size() - pos : nl - pos);
    pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

    line = trim(line);
    if (line.empty() || line.front() == '#')
      continue;
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      complain(
          complaints,
          lineNo,
          "expected '<key>: <value>', found " + quoteValue(line));
      continue;
    }
    std::string_view key = trim(line.substr(0, colon));
    std::string_view value = trim(line.substr(colon + 1));

    if (key == "recency") {
      if (value == "atime")
        config.recency = CacheRecency::kAtime;
      else if (value == "mtime")
        config.recency = CacheRecency::kMtime;
      else
        complain(
            complaints,
            lineNo,
            "recency must be 'atime' or 'mtime', found " + quoteValue(value) +
                "; using 'atime'");
    } else if (key == "max_wasm_bytes") {
      std::string v(value);
      // Require digits and nothing else. Checking only for a leading sign is
      // not enough: strtoull skips leading whitespace of its own, and trim()
      // above removes only spaces and tabs -- so "\v-1" reaches strtoull with
      // its sign hidden behind a vertical tab, parses as a negation of the
      // magnitude, and yields 2^64-1 where the default was meant. A byte
      // count is digits; anything else is malformed and keeps the default.
      bool digitsOnly = !v.empty();
      for (char c : v) {
        if (c < '0' || c > '9') {
          digitsOnly = false;
          break;
        }
      }
      bool ok = false;
      if (digitsOnly) {
        char *end = nullptr;
        errno = 0;
        unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
        if (errno == 0 && *end == '\0') {
          config.maxWasmBytes = parsed;
          ok = true;
        }
      }
      if (!ok)
        complain(
            complaints,
            lineNo,
            "max_wasm_bytes must be a decimal byte count, found " +
                quoteValue(value) + "; using " +
                std::to_string(CacheConfig{}.maxWasmBytes));
    } else {
      complain(
          complaints, lineNo, "unknown key " + quoteValue(key) + "; ignored");
    }
  }
  return config;
}

CacheConfig cacheConfigLoadOrCreate(
    const std::string &root,
    std::vector<std::string> *complaints) {
  if (root.empty())
    return CacheConfig{};
  std::string path = root + "/" + kCacheConfigFileName;

  std::ifstream in(path, std::ios::binary);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return cacheConfigParse(ss.str(), complaints);
  }

  // Failing to READ is not the same as being absent, and the difference
  // matters because writing the defaults would otherwise truncate: a file
  // that exists but cannot be opened (permissions, or a directory sitting on
  // the name) would have the user's settings silently replaced.
  //
  // Written to a temporary file and published with link(), which fails
  // rather than replaces if the destination appeared meanwhile. Both halves
  // are load-bearing.
  //
  // Not a plain O_CREAT|O_EXCL write: that closes the overwrite race but
  // still publishes the name before the content, and a reader catching the
  // file mid-write does NOT simply get the defaults back. A prefix of this
  // text ending "max_wasm_bytes: 2" is well-formed and parses to a two-byte
  // budget, which evicts everything on the next store -- and a short write
  // that then fails would leave exactly that on disk permanently, since
  // nothing would ever replace it.
  //
  // Not rename() either: rename replaces, so two processes racing here
  // would let the loser clobber the winner. link() gives EEXIST instead,
  // which is the answer we want -- somebody else's file is as good as ours.
  // mkstemp rather than a name built from getpid(): a fixed name plus
  // O_TRUNC would follow a pre-existing symlink sitting there, and a crash
  // between the link() and the unlink() below leaves that name hard-linked
  // to the published config -- so a later process reusing the pid would
  // truncate the real file through it. mkstemp creates exclusively, under a
  // name nothing can have predicted, which closes both.
  std::string tmp = path + ".XXXXXX";
  std::vector<char> tmpBuf(tmp.begin(), tmp.end());
  tmpBuf.push_back('\0');
  int fd = ::mkstemp(tmpBuf.data());
  if (fd < 0)
    return CacheConfig{};
  bool existed = false;
  const std::string tmpPath(tmpBuf.data());
  ::fchmod(fd, 0644); // mkstemp creates 0600; this file is not a secret.

  const char *cursor = kDefaultConfigText;
  size_t remaining = std::strlen(cursor);
  bool complete = true;
  while (remaining > 0) {
    ssize_t n = ::write(fd, cursor, remaining);
    if (n <= 0) {
      complete = false;
      break;
    }
    cursor += n;
    remaining -= static_cast<size_t>(n);
  }
  // close() can report a deferred write error that every write() missed, so
  // it decides publication too.
  if (::close(fd) != 0)
    complete = false;

  // Only a whole file is worth publishing; a partial one is worse than none.
  // link() fails rather than replaces if the destination appeared meanwhile,
  // which is the answer we want -- somebody else's copy is as good as ours.
  // rename() would replace, letting the loser of a race clobber the winner.
  if (complete)
    existed = ::link(tmpPath.c_str(), path.c_str()) != 0 && errno == EEXIST;
  ::unlink(tmpPath.c_str());

  // link() refusing with EEXIST is the only evidence available that the file
  // is there: the read above failed, so "absent" and "present but
  // unreadable" are otherwise the same outcome, and the second is worth
  // saying out loud rather than silently running on defaults.
  if (existed && complaints != nullptr)
    complaints->push_back("exists but could not be read; using defaults");

  return CacheConfig{};
}

} // namespace node_compat
} // namespace hermes
