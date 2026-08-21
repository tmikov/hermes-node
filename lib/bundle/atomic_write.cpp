/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/atomic_write.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>

namespace hermes {
namespace node_compat {

namespace {

/// Distinguishes concurrent writers targeting the same outPath. getpid()
/// alone is not enough -- runHermesNode is documented thread-safe, so two
/// runtimes in one process could in principle both write to the same
/// outPath concurrently (a build and an extraction, or two extractions)
/// and would otherwise share a temp path. Mirrors compileCacheWriteEntry's
/// identical reasoning (lib/compile-cache).
std::atomic<uint64_t> g_tempCounter{0};

} // namespace

bool writeFileAtomically(
    const std::string &outPath,
    const void *data,
    size_t size,
    std::ostream &err) {
  std::string tmp = outPath + "." + std::to_string(::getpid()) + "." +
      std::to_string(g_tempCounter.fetch_add(1, std::memory_order_relaxed)) +
      ".tmp";

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    err << "error: cannot open " << tmp
        << " for writing: " << std::strerror(errno) << "\n";
    return false;
  }

  const char *p = static_cast<const char *>(data);
  size_t remaining = size;
  bool ok = true;
  int savedErrno = 0;
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      savedErrno = errno;
      ok = false;
      break;
    }
    if (n == 0) {
      // No errno to report: write() simply made no progress on a call
      // that should always be able to make some on a regular file.
      ok = false;
      break;
    }
    p += n;
    remaining -= static_cast<size_t>(n);
  }

  // Checked even when the write loop above already succeeded: a deferred
  // write error surfacing here must not be lost by ignoring close()'s
  // return value, or a truncated temp file would go on to be renamed into
  // place as if it were complete.
  if (::close(fd) != 0 && ok) {
    savedErrno = errno;
    ok = false;
  }

  if (!ok) {
    if (savedErrno != 0) {
      err << "error: failed writing " << tmp << ": "
          << std::strerror(savedErrno) << "\n";
    } else {
      err << "error: failed writing " << tmp << "\n";
    }
    ::unlink(tmp.c_str());
    return false;
  }

  if (::rename(tmp.c_str(), outPath.c_str()) != 0) {
    err << "error: failed to rename " << tmp << " to " << outPath << ": "
        << std::strerror(errno) << "\n";
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool isSameFile(const std::string &a, const std::string &b) {
  struct stat sa {};
  struct stat sb {};
  if (::stat(a.c_str(), &sa) != 0 || ::stat(b.c_str(), &sb) != 0)
    return false;
  return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

} // namespace node_compat
} // namespace hermes
