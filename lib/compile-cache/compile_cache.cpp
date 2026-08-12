/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <zlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

uint32_t compileCacheCrc32(const void *data, size_t size) {
  uLong crc = crc32(0L, Z_NULL, 0);
  if (size == 0)
    return static_cast<uint32_t>(crc);
  return static_cast<uint32_t>(
      crc32(crc, static_cast<const Bytef *>(data), static_cast<uInt>(size)));
}

uint32_t compileCacheKey(const std::string &filename, CompileCacheKind kind) {
  auto kindByte = static_cast<uint8_t>(kind);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef *>(&kindByte), 1);
  crc = crc32(
      crc,
      reinterpret_cast<const Bytef *>(filename.data()),
      static_cast<uInt>(filename.size()));
  return static_cast<uint32_t>(crc);
}

std::string compileCacheGenerationName(
    const std::string &version,
    const std::string &arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "-bc%u-%08x", bytecodeVersion, configCrc);
  return version + "-" + arch + buf;
}

namespace {

/// 'HNCC' -- Hermes Node Compile Cache.
constexpr uint32_t kMagic = 0x4343484e;
/// Bumped when the header layout below changes.
constexpr uint32_t kHeaderVersion = 1;

/// Entry header, 24 bytes. Field order matches the design document.
struct EntryHeader {
  uint32_t magic;
  uint32_t headerVersion;
  uint32_t sourceCrc;
  uint32_t sourceSize;
  uint32_t bytecodeSize;
  uint32_t reserved;
};

static_assert(
    sizeof(EntryHeader) == kCompileCacheHeaderSize,
    "EntryHeader must match kCompileCacheHeaderSize");

/// Write exactly \p size bytes, retrying short writes. Returns false on error.
bool writeAll(int fd, const void *data, size_t size) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (size > 0) {
    ssize_t n = ::write(fd, p, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

/// Read exactly \p size bytes, retrying short reads. Returns false on error
/// or premature end of file.
bool readAll(int fd, void *data, size_t size) {
  auto *p = static_cast<uint8_t *>(data);
  while (size > 0) {
    ssize_t n = ::read(fd, p, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

} // namespace

void CacheMapping::destroy() {
  if (base != nullptr)
    ::munmap(base, length);
  delete this;
}

void CacheMapping::finalizer(const uint8_t *, size_t, void *hint) {
  static_cast<CacheMapping *>(hint)->destroy();
}

bool compileCacheWriteEntry(
    const std::string &path,
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize) {
  if (bytecode == nullptr || bytecodeSize == 0)
    return false;
  if (bytecodeSize > UINT32_MAX)
    return false;

  // Unique temp name so concurrent writers never collide.
  std::string tmp = path + "." + std::to_string(::getpid()) + ".tmp";

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;

  EntryHeader header{};
  header.magic = kMagic;
  header.headerVersion = kHeaderVersion;
  header.sourceCrc = entry.sourceCrc;
  header.sourceSize = entry.sourceSize;
  header.bytecodeSize = static_cast<uint32_t>(bytecodeSize);
  header.reserved = 0;

  bool ok = writeAll(fd, &header, sizeof(header)) &&
      writeAll(fd, bytecode, bytecodeSize);
  ::close(fd);

  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool compileCacheReadEntry(CompileCacheEntry &entry) {
  int fd = ::open(entry.cacheFilePath.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  EntryHeader header{};
  if (!readAll(fd, &header, sizeof(header))) {
    ::close(fd);
    return false;
  }

  // Cheapest checks first: a changed file usually changes length, so the
  // size check rejects most edits before the CRC matters.
  if (header.magic != kMagic || header.headerVersion != kHeaderVersion ||
      header.sourceSize != entry.sourceSize ||
      header.sourceCrc != entry.sourceCrc || header.bytecodeSize == 0) {
    ::close(fd);
    return false;
  }

  size_t mapSize = sizeof(EntryHeader) + header.bytecodeSize;

  // Refuse a file shorter than its header claims. Atomic writes make this
  // unreachable for entries we wrote, but the directory is user-writable.
  struct stat st {};
  if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < mapSize) {
    ::close(fd);
    return false;
  }

  void *base = ::mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd); // The mapping keeps the inode alive on its own.
  if (base == MAP_FAILED)
    return false;

  auto *mapping = new CacheMapping();
  mapping->base = base;
  mapping->length = mapSize;

  entry.mapping = mapping;
  entry.bytecode = static_cast<const uint8_t *>(base) + sizeof(EntryHeader);
  entry.bytecodeSize = header.bytecodeSize;
  return true;
}

namespace {

/// Recursively delete \p path. Best effort.
void removeTree(const std::string &path) {
  DIR *d = ::opendir(path.c_str());
  if (d != nullptr) {
    while (struct dirent *e = ::readdir(d)) {
      if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
        continue;
      std::string child = path + "/" + e->d_name;
      struct stat st {};
      if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        removeTree(child);
      else
        ::unlink(child.c_str());
    }
    ::closedir(d);
  }
  ::rmdir(path.c_str());
}

} // namespace

std::string compileCacheDefaultRoot() {
  if (const char *xdg = ::getenv("XDG_CACHE_HOME")) {
    if (xdg[0] != '\0')
      return std::string(xdg) + "/hermes-node/compile-cache";
  }
  if (const char *home = ::getenv("HOME")) {
    if (home[0] != '\0')
      return std::string(home) + "/.cache/hermes-node/compile-cache";
  }
  return std::string();
}

bool compileCacheMakeDirs(const std::string &path) {
  if (path.empty())
    return false;
  // Create each component in turn, tolerating EEXIST so concurrent
  // processes racing to create the same directory both succeed.
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/')
      continue;
    std::string component = path.substr(0, i);
    if (::mkdir(component.c_str(), 0755) != 0 && errno != EEXIST)
      return false;
  }
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void compileCachePruneGenerations(
    const std::string &versionedRoot,
    const std::string &keepName,
    size_t keepCount) {
  DIR *d = ::opendir(versionedRoot.c_str());
  if (d == nullptr)
    return;

  std::vector<std::pair<time_t, std::string>> others;
  while (struct dirent *e = ::readdir(d)) {
    if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
      continue;
    if (keepName == e->d_name)
      continue;
    std::string child = versionedRoot + "/" + e->d_name;
    struct stat st {};
    if (::lstat(child.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      continue;
    others.emplace_back(st.st_mtime, child);
  }
  ::closedir(d);

  if (others.size() <= keepCount)
    return;

  // Most recently modified first; everything past keepCount goes.
  std::sort(others.begin(), others.end(), [](const auto &a, const auto &b) {
    return a.first > b.first;
  });
  for (size_t i = keepCount; i < others.size(); ++i)
    removeTree(others[i].second);
}

bool CompileCache::enable(
    const std::string &root,
    const std::string &generationName) {
  if (root.empty() || generationName.empty())
    return false;

  // v1/ is a structure escape hatch: a fundamentally different layout later
  // becomes v2/ and can coexist with this one.
  std::string versionedRoot = root + "/v1";
  std::string generationDir = versionedRoot + "/" + generationName;

  bool existed = false;
  {
    struct stat st {};
    existed = ::stat(generationDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }

  if (!compileCacheMakeDirs(generationDir))
    return false;

  // Prune only when a generation was actually created, so a normal startup
  // never scans the cache root.
  if (!existed) {
    compileCachePruneGenerations(
        versionedRoot, generationName, kCompileCacheGenerationsKept);
  }

  generationDir_ = generationDir;
  enabled_ = true;
  return true;
}

void CompileCache::trace(const char *what, const std::string &filename) const {
  if (tracing_)
    std::fprintf(stderr, "[compile cache] %s %s\n", what, filename.c_str());
}

bool CompileCache::lookup(
    CompileCacheEntry &entry,
    const std::string &source,
    const std::string &filename,
    CompileCacheKind kind) {
  if (!enabled_)
    return false;

  entry.key = compileCacheKey(filename, kind);
  entry.sourceCrc = compileCacheCrc32(source.data(), source.size());
  entry.sourceSize = static_cast<uint32_t>(source.size());

  char rel[16];
  std::snprintf(
      rel, sizeof(rel), "/%02x/%08x", (entry.key >> 24) & 0xff, entry.key);
  entry.cacheFilePath = generationDir_ + rel;

  if (compileCacheReadEntry(entry)) {
    trace("hit ", filename);
    return true;
  }
  trace("miss", filename);
  return false;
}

void CompileCache::save(
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize) {
  if (!enabled_ || entry.cacheFilePath.empty())
    return;

  // Create the fanout directory. Cheap enough to attempt every time; mkdir
  // on an existing directory is a single failed syscall.
  size_t slash = entry.cacheFilePath.rfind('/');
  if (slash != std::string::npos)
    compileCacheMakeDirs(entry.cacheFilePath.substr(0, slash));

  if (!compileCacheWriteEntry(
          entry.cacheFilePath, entry, bytecode, bytecodeSize))
    trace("save-failed", entry.cacheFilePath);
}

void CompileCache::invalidate(const CompileCacheEntry &entry) {
  if (!enabled_ || entry.cacheFilePath.empty())
    return;
  ::unlink(entry.cacheFilePath.c_str());
  trace("invalidated", entry.cacheFilePath);
}

} // namespace node_compat
} // namespace hermes
