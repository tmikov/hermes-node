/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/native_digest.h>

#include <picohash_wrapper.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace hermes {
namespace node_compat {

std::optional<NativeDigest> nativeFileDigest(
    const std::string &path,
    std::string *error) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    *error = "cannot open " + path + ": " + std::strerror(errno);
    return std::nullopt;
  }

  picohash_ctx_t ctx;
  ph_init_sha256(&ctx);

  NativeDigest result;
  uint64_t total = 0;
  char buffer[64 * 1024];
  while (true) {
    size_t n = std::fread(buffer, 1, sizeof(buffer), f);
    if (n == 0)
      break;
    ph_update(&ctx, buffer, n);
    total += n;
    if (total > std::numeric_limits<uint32_t>::max()) {
      std::fclose(f);
      *error = path + " is larger than 4 GiB";
      return std::nullopt;
    }
  }
  bool failed = std::ferror(f) != 0;
  // Captured before fclose(), which is allowed to set errno itself and would
  // otherwise replace the read's reason with its own. This branch is how a
  // directory at the path reports: fopen() on a directory succeeds, the
  // first fread() fails with EISDIR.
  const int readErrno = errno;
  std::fclose(f);
  if (failed) {
    *error = "cannot read " + path + ": " + std::strerror(readErrno);
    return std::nullopt;
  }

  result.byteLength = static_cast<uint32_t>(total);
  result.raw.resize(PICOHASH_SHA256_DIGEST_LENGTH);
  ph_final(&ctx, result.raw.data());
  return result;
}

std::string nativeDigestToHex(std::string_view raw) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

} // namespace node_compat
} // namespace hermes
