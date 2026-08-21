/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_NATIVE_DIGEST_H
#define HERMES_NODE_COMPAT_BUNDLE_NATIVE_DIGEST_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// A native addon's length and content hash, as recorded in the container's
/// native table.
struct NativeDigest {
  uint32_t byteLength = 0;
  /// The raw SHA-256: exactly 32 bytes, which may include NULs.
  std::string raw;
};

/// SHA-256 and byte length of the file at \p path, streamed rather than
/// read whole: an addon can be tens of megabytes and nothing here needs the
/// bytes themselves.
///
/// SHA-256 rather than the CRC32 used for the generation tag and the
/// compile cache, because --verify-natives is offered as a manual security
/// check and a CRC can be forged to any value. It is still an audit and not
/// an enforcement -- see the design doc -- but the hash itself should not
/// be the weak part.
///
/// Returns nullopt with \p error set when the file cannot be opened or
/// read, or when it is larger than a uint32_t can express (the container
/// field is 32-bit, and a 4 GiB shared object is a mistake worth naming
/// rather than truncating).
std::optional<NativeDigest> nativeFileDigest(
    const std::string &path,
    std::string *error);

/// \p raw (32 bytes) as 64 lowercase hex characters.
std::string nativeDigestToHex(std::string_view raw);

} // namespace node_compat
} // namespace hermes

#endif
