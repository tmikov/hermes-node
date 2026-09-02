/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_READER_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_READER_H

#include <hermes/node-compat/bundle/bundle_format.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// A read-only view over a mapped container. Does not own the bytes; the
/// caller keeps the mapping alive for the reader's lifetime.
///
/// open() validates every offset and length in the header against \p size
/// before any accessor can be called, so accessors perform no bounds checks
/// of their own beyond index range.
///
/// That validation covers the container's structure only -- offsets,
/// lengths, alignment, index ranges -- and never the payload bytes. There
/// is no checksum over a module's bytecode or JSON text, so a bit flipped
/// inside a payload is not detected: it reaches Hermes, which either
/// rejects the bytecode (a load error for that one module) or runs
/// something subtly wrong. This is a deliberate asymmetry with the compile
/// cache, which does store a CRC32 per entry (see lib/compile-cache): a
/// cache entry is validated against a source file that may have changed
/// underneath it, whereas a bundle is a signed-off artifact with no second
/// copy to compare against. Adding a payload checksum is an additive change
/// to the format if the tradeoff ever changes.
class BundleReader {
 public:
  /// Returns std::nullopt and sets \p error on bad magic, format version
  /// mismatch, generation mismatch, truncation, or any out-of-range offset.
  static std::optional<BundleReader> open(
      const uint8_t *data,
      size_t size,
      uint32_t expectedGeneration,
      std::string *error);

  /// Opens without enforcing the generation tag, for inspection tools.
  /// Every structural check open() performs still applies: magic, format
  /// version, and every offset, length and index.
  ///
  /// Bytecode from a mismatched generation must never be executed, which is
  /// why this is a separate entry point rather than a parameter on open() --
  /// a caller cannot reach it without meaning to.
  static std::optional<BundleReader>
  openForInspection(const uint8_t *data, size_t size, std::string *error);

  /// Binary search for the edge (importer, specifier).
  ///
  /// The edge table is sorted by (importer index, specifier bytes) rather
  /// than by (importer, specifier index). At run time require() supplies
  /// bytes, not an index; sorting by index would force a string-to-index
  /// hash built at load time, which is exactly the startup pass this format
  /// exists to avoid. Comparing bytes costs about a dozen short memcmps per
  /// require and keeps load-time work at zero.
  std::optional<uint32_t> lookup(uint32_t importer, std::string_view specifier)
      const;

  std::string_view identity(uint32_t moduleIndex) const;
  std::string_view payload(uint32_t moduleIndex) const;
  ModuleKind kind(uint32_t moduleIndex) const;

  /// The raw flags bitfield stored on the module record (see
  /// bundle_format.h).
  uint32_t flags(uint32_t moduleIndex) const;

  /// True when kRequirable is set -- the module require() may load. False
  /// for a record that exists only so the resolver can read it (a
  /// package.json consulted for `main` but never itself required).
  bool isRequirable(uint32_t moduleIndex) const;

  uint32_t entry() const;
  uint32_t moduleCount() const;

  uint32_t formatVersion() const;
  uint32_t generationTag() const;

  /// One packaged edge (importer, specifier, target), as stored in the
  /// edge table -- see edge() below.
  struct EdgeView {
    uint32_t importer;
    std::string_view specifier;
    uint32_t target;
  };

  uint32_t edgeCount() const;

  /// Only valid for \p edgeIndex below edgeCount(), exactly like identity()
  /// is only valid for a module index below moduleCount(): no runtime check.
  EdgeView edge(uint32_t edgeIndex) const;

  /// How many modules the container names as preloads -- modules that must
  /// run before the entry point, in the order returned by preload().
  uint32_t preloadCount() const;

  /// The module index of preload \p i. Only valid for \p i below
  /// preloadCount(), exactly like edge() above.
  uint32_t preload(uint32_t i) const;

  /// One entry of the native table -- see bundle_format.h. `sidecar` and
  /// `digest` are views into the mapped string table; `digest` is the raw
  /// 32-byte SHA-256 and may contain NUL bytes.
  struct NativeView {
    uint32_t moduleIndex;
    std::string_view sidecar;
    uint32_t byteLength;
    std::string_view digest;
  };

  uint32_t nativeCount() const;

  /// Only valid for \p i below nativeCount(), exactly like edge() above.
  NativeView native(uint32_t i) const;

  /// The native table entry for \p moduleIndex, or nullopt when that module
  /// is not a kNative. Binary search: the table is sorted by module index.
  std::optional<NativeView> nativeFor(uint32_t moduleIndex) const;

  /// How many VM options the container records, in application order.
  uint32_t vmOptionCount() const;

  /// VM option \p i. Only valid for \p i below vmOptionCount(), exactly
  /// like edge() above. The view points into the mapped string table.
  std::string_view vmOption(uint32_t i) const;

  /// True when kBundleFlagAllowVmOptionsOverride is set -- the container
  /// permits its VM options to be overridden at run time.
  bool allowsVmOptionsOverride() const;

  /// Section sizes, straight from the header, for a dump that reports them.
  /// stringsSize() and payloadSize() are byte counts, same as the header
  /// fields they return. moduleTableSize(), edgeTableSize(),
  /// preloadTableSize() and nativeTableSize() are also byte counts --
  /// record count times record size -- NOT element counts; use
  /// moduleCount()/edgeCount()/preloadCount()/nativeCount() for the latter.
  uint32_t stringsSize() const;
  uint32_t moduleTableSize() const;
  uint32_t edgeTableSize() const;
  uint32_t preloadTableSize() const;
  uint32_t nativeTableSize() const;
  /// Byte count, like the other ...Size() accessors: option count times
  /// sizeof(uint32_t), NOT an element count.
  uint32_t vmOptionsTableSize() const;
  uint32_t payloadSize() const;

 private:
  BundleReader() = default;

  /// Shared validation chain for open() and openForInspection(). \p
  /// enforceGeneration selects whether a generation mismatch is a hard
  /// error; every other check (magic, format version, and every offset,
  /// length and index) runs unconditionally.
  static std::optional<BundleReader> openImpl(
      const uint8_t *data,
      size_t size,
      uint32_t expectedGeneration,
      bool enforceGeneration,
      std::string *error);

  std::string_view stringAt(uint32_t offset) const;

  const uint8_t *data_ = nullptr;
  const BundleHeader *header_ = nullptr;
  const BundleModuleRecord *modules_ = nullptr;
  const BundleEdgeRecord *edges_ = nullptr;
  const uint32_t *preloads_ = nullptr;
  const BundleNativeRecord *natives_ = nullptr;
  const uint32_t *vmOptions_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif
