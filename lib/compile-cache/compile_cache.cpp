/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <zlib.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

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

} // namespace node_compat
} // namespace hermes
