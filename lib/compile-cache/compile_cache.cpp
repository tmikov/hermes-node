/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <zlib.h>

#include <cstdio>

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

} // namespace node_compat
} // namespace hermes
