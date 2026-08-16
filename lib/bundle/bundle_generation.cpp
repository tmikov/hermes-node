/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_generation.h>

#include <hermes/BCGen/HBC/BytecodeVersion.h>
#include <hermes/node-compat/version.h>

#include <zlib.h>

#include <cstdio>
#include <string>

namespace hermes {
namespace node_compat {

namespace {

/// Folds [data, data + size) into \p crc. zlib's crc32() returns 0 outright
/// when given a NULL buffer, discarding whatever was accumulated so far,
/// rather than leaving the running value alone -- see compileCacheCrc32 in
/// compile_cache.cpp for the same guard against the same pitfall. Every
/// call site below passes a non-empty, non-null range, but the guard is
/// kept so that stays true by construction rather than by care.
uLong crc32Update(uLong crc, const void *data, size_t size) {
  if (size == 0)
    return crc;
  return crc32(crc, static_cast<const Bytef *>(data), static_cast<uInt>(size));
}

uLong crc32UpdateStr(uLong crc, std::string_view s) {
  return crc32Update(crc, s.data(), s.size());
}

/// Separates fields in the fold below so that, e.g., version="1" + arch="20"
/// cannot hash the same as version="12" + arch="0" -- unlike
/// compileCacheGenerationName's human-readable name, the CRC has no other
/// framing to keep fields from silently running together. 0x1f (ASCII Unit
/// Separator) is not a character any of version/arch/decimal-digits would
/// plausibly contain.
constexpr char kFieldSep = '\x1f';

} // namespace

uint32_t bundleGenerationTagFor(
    std::string_view version,
    std::string_view arch,
    uint32_t bytecodeVersion,
    char optimizeByte) {
  char bytecodeVersionText[16];
  int n = std::snprintf(
      bytecodeVersionText, sizeof(bytecodeVersionText), "%u", bytecodeVersion);

  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32UpdateStr(crc, version);
  crc = crc32Update(crc, &kFieldSep, 1);
  crc = crc32UpdateStr(crc, arch);
  crc = crc32Update(crc, &kFieldSep, 1);
  crc = crc32Update(crc, bytecodeVersionText, static_cast<size_t>(n));
  crc = crc32Update(crc, &kFieldSep, 1);
  crc = crc32Update(crc, &optimizeByte, 1);

  uint32_t tag = static_cast<uint32_t>(crc);
  return tag != 0 ? tag : 1;
}

uint32_t bundleGenerationTag() {
  // The producer always optimizes, per the bundle spec. The 'O' byte is
  // present so a future non-optimizing producer gets a distinct tag rather
  // than silently colliding with today's optimized bundles.
  return bundleGenerationTagFor(
      HERMES_NODE_VERSION_STRING,
      HERMES_NODE_CACHE_ARCH,
      hermes::hbc::BYTECODE_VERSION,
      'O');
}

std::string bundleGenerationDescription() {
  // The same four fields the tag above folds, in the same order. "optimized"
  // is the 'O' byte spelled out; a future non-optimizing producer changes
  // both lines or neither.
  std::string text = "hermes-node ";
  text += HERMES_NODE_VERSION_STRING;
  text += ", ";
  text += HERMES_NODE_CACHE_ARCH;
  text += ", bytecode ";
  text += std::to_string(hermes::hbc::BYTECODE_VERSION);
  text += ", optimized";
  return text;
}

} // namespace node_compat
} // namespace hermes
