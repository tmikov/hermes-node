/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_reader.h>

#include <cstring>

namespace hermes {
namespace node_compat {

namespace {

/// True if [offset, offset + len) fits within a buffer of \p bufSize bytes,
/// with no overflow in the addition. Both \p offset and \p len come
/// straight from an untrusted header, so the addition is done in a wider
/// type before the overflow can happen.
bool inRange(size_t bufSize, uint32_t offset, uint32_t len) {
  uint64_t end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(len);
  return end <= static_cast<uint64_t>(bufSize);
}

/// True if a table of \p count fixed-size records, each \p recordSize
/// bytes, starting at \p offset, fits within a buffer of \p bufSize bytes.
/// \p count is untrusted and multiplying it by \p recordSize can overflow a
/// 32-bit width on its own, before the offset is even added, so the whole
/// computation is done in 64 bits.
bool tableInRange(
    size_t bufSize,
    uint32_t offset,
    uint32_t count,
    size_t recordSize) {
  uint64_t tableBytes = static_cast<uint64_t>(count) * recordSize;
  uint64_t end = static_cast<uint64_t>(offset) + tableBytes;
  return end <= static_cast<uint64_t>(bufSize);
}

/// Validates that a string-table index \p offset refers to a complete
/// entry (length header plus that many bytes) inside a table of
/// \p tableSize bytes starting at \p tableBase. Reads the length with
/// memcpy: entries are packed with no padding, so a later string's length
/// field is not generally 4-byte aligned.
bool validateStringIndex(
    const uint8_t *tableBase,
    uint32_t tableSize,
    uint32_t offset) {
  if (!inRange(tableSize, offset, sizeof(BundleStringHeader)))
    return false;
  uint32_t length;
  std::memcpy(&length, tableBase + offset, sizeof(length));
  // offset is already known to be in range for the header alone; now check
  // the header plus the string bytes it claims. Computed in 64 bits: an
  // untrusted length near UINT32_MAX would otherwise overflow a 32-bit sum.
  uint64_t end = static_cast<uint64_t>(offset) + sizeof(BundleStringHeader) +
      static_cast<uint64_t>(length);
  return end <= static_cast<uint64_t>(tableSize);
}

/// Reads the string at \p offset within a string table of \p tableBase.
/// Only valid to call once validateStringIndex() has confirmed \p offset is
/// in range -- unlike BundleReader::stringAt(), this is a free function so
/// it can run during openImpl(), before reader.data_/header_ exist.
std::string_view stringViewAt(const uint8_t *tableBase, uint32_t offset) {
  uint32_t length;
  std::memcpy(&length, tableBase + offset, sizeof(length));
  return std::string_view(
      reinterpret_cast<const char *>(
          tableBase + offset + sizeof(BundleStringHeader)),
      length);
}

/// True if \p identity is safe to use as a `root + "/" + identity` path
/// (BundleFileSource's contract) and as a Module._cache key: non-empty, not
/// absolute, no embedded NUL, and no "." or ".." path segment. Structure
/// validation alone (the checks around this one) never looked at identity
/// bytes -- identities happened to be safe because the producer only ever
/// derives them via lexically_relative() against a common ancestor, which
/// cannot produce ".." or an absolute path. That was inert while an
/// identity only indexed a payload inside the container; it stops being
/// inert once an identity becomes a filesystem-shaped path a hand-edited or
/// adversarial container could point outside root (e.g. "../etc/passwd").
bool isValidIdentity(std::string_view identity) {
  if (identity.empty() || identity[0] == '/')
    return false;
  if (identity.find('\0') != std::string_view::npos)
    return false;
  size_t start = 0;
  while (true) {
    size_t slash = identity.find('/', start);
    std::string_view segment = slash == std::string_view::npos
        ? identity.substr(start)
        : identity.substr(start, slash - start);
    if (segment == "." || segment == "..")
      return false;
    if (slash == std::string_view::npos)
      return true;
    start = slash + 1;
  }
}

} // namespace

std::optional<BundleReader> BundleReader::openImpl(
    const uint8_t *data,
    size_t size,
    uint32_t expectedGeneration,
    bool enforceGeneration,
    std::string *error) {
  auto fail = [&](const char *message) -> std::optional<BundleReader> {
    if (error != nullptr)
      *error = message;
    return std::nullopt;
  };

  if (size < sizeof(BundleHeader))
    return fail("hermes-node bundle: truncated (shorter than the header)");

  const auto *header = reinterpret_cast<const BundleHeader *>(data);

  if (std::memcmp(header->magic, kBundleMagic, sizeof(kBundleMagic)) != 0)
    return fail("hermes-node bundle: not a hermes-node bundle (bad magic)");

  if (header->formatVersion != kBundleFormatVersion) {
    return fail(
        "hermes-node bundle: format version mismatch (bundle was built by "
        "an incompatible hermes-node)");
  }

  if (enforceGeneration && header->generationTag != expectedGeneration) {
    return fail(
        "hermes-node bundle: built by a different hermes-node build "
        "(generation mismatch)");
  }

  // Every (offset, size) pair the header claims must land inside the
  // buffer, with no overflow.
  if (!inRange(size, header->stringsOffset, header->stringsSize))
    return fail("hermes-node bundle: string table out of range");
  if (!tableInRange(
          size,
          header->moduleTableOffset,
          header->moduleCount,
          sizeof(BundleModuleRecord)))
    return fail("hermes-node bundle: module table out of range");
  if (!tableInRange(
          size,
          header->edgeTableOffset,
          header->edgeCount,
          sizeof(BundleEdgeRecord)))
    return fail("hermes-node bundle: edge table out of range");
  if (!tableInRange(
          size,
          header->preloadTableOffset,
          header->preloadCount,
          sizeof(uint32_t)))
    return fail("hermes-node bundle: preload table out of range");
  if (!inRange(size, header->payloadOffset, header->payloadSize))
    return fail("hermes-node bundle: payload out of range");

  // The module and edge tables are read through pointer casts straight
  // onto the buffer (records are all-uint32_t), which is undefined
  // behavior at a misaligned address even where the target CPU tolerates
  // it. The writer always emits both tables 4-byte aligned; a corrupt or
  // adversarial file might not.
  if (header->moduleTableOffset % alignof(BundleModuleRecord) != 0 ||
      header->edgeTableOffset % alignof(BundleEdgeRecord) != 0 ||
      header->preloadTableOffset % alignof(uint32_t) != 0)
    return fail("hermes-node bundle: table offset is misaligned");

  if (header->moduleCount == 0)
    return fail("hermes-node bundle: has no modules");
  if (header->entryModule >= header->moduleCount)
    return fail("hermes-node bundle: entry module index out of range");

  const uint8_t *stringsBase = data + header->stringsOffset;
  const auto *modules = reinterpret_cast<const BundleModuleRecord *>(
      data + header->moduleTableOffset);
  const auto *edges = reinterpret_cast<const BundleEdgeRecord *>(
      data + header->edgeTableOffset);

  for (uint32_t i = 0; i < header->moduleCount; ++i) {
    const BundleModuleRecord &m = modules[i];
    if (!validateStringIndex(
            stringsBase, header->stringsSize, m.identityString))
      return fail("hermes-node bundle: module identity string out of range");
    if (!isValidIdentity(stringViewAt(stringsBase, m.identityString)))
      return fail("hermes-node bundle: module has a malformed identity");
    if (m.kind != static_cast<uint32_t>(ModuleKind::kJavaScript) &&
        m.kind != static_cast<uint32_t>(ModuleKind::kJSON))
      return fail("hermes-node bundle: module has an unknown kind");
    if ((m.flags & ~kRequirable) != 0)
      return fail("hermes-node bundle: module has unknown flags");
    if (!inRange(header->payloadSize, m.payloadOffset, m.payloadSize))
      return fail("hermes-node bundle: module payload out of range");
  }

  for (uint32_t i = 0; i < header->edgeCount; ++i) {
    const BundleEdgeRecord &e = edges[i];
    if (e.importer >= header->moduleCount || e.target >= header->moduleCount)
      return fail("hermes-node bundle: edge references an unknown module");
    if (!validateStringIndex(
            stringsBase, header->stringsSize, e.specifierString))
      return fail("hermes-node bundle: edge specifier string out of range");
  }

  const auto *preloads =
      reinterpret_cast<const uint32_t *>(data + header->preloadTableOffset);
  for (uint32_t i = 0; i < header->preloadCount; ++i) {
    if (preloads[i] >= header->moduleCount)
      return fail("hermes-node bundle: preload references an unknown module");
    // A preload names something the run will require(). A resolution-input
    // package.json cannot be required, so naming one is a malformed
    // container rather than a run-time surprise.
    if ((modules[preloads[i]].flags & kRequirable) == 0)
      return fail("hermes-node bundle: preload names a non-requirable module");
  }

  BundleReader reader;
  reader.data_ = data;
  reader.header_ = header;
  reader.modules_ = modules;
  reader.edges_ = edges;
  reader.preloads_ = preloads;
  return reader;
}

std::optional<BundleReader> BundleReader::open(
    const uint8_t *data,
    size_t size,
    uint32_t expectedGeneration,
    std::string *error) {
  return openImpl(
      data, size, expectedGeneration, /*enforceGeneration=*/true, error);
}

std::optional<BundleReader> BundleReader::openForInspection(
    const uint8_t *data,
    size_t size,
    std::string *error) {
  return openImpl(
      data, size, /*expectedGeneration=*/0, /*enforceGeneration=*/false, error);
}

std::string_view BundleReader::stringAt(uint32_t offset) const {
  const uint8_t *base = data_ + header_->stringsOffset + offset;
  uint32_t length;
  std::memcpy(&length, base, sizeof(length));
  return std::string_view(
      reinterpret_cast<const char *>(base + sizeof(BundleStringHeader)),
      length);
}

std::optional<uint32_t> BundleReader::lookup(
    uint32_t importer,
    std::string_view specifier) const {
  uint32_t lo = 0;
  uint32_t hi = header_->edgeCount;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const BundleEdgeRecord &e = edges_[mid];
    bool less;
    if (e.importer != importer) {
      less = e.importer < importer;
    } else {
      less = stringAt(e.specifierString).compare(specifier) < 0;
    }
    if (less)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < header_->edgeCount) {
    const BundleEdgeRecord &e = edges_[lo];
    if (e.importer == importer && stringAt(e.specifierString) == specifier)
      return e.target;
  }
  return std::nullopt;
}

std::string_view BundleReader::identity(uint32_t moduleIndex) const {
  return stringAt(modules_[moduleIndex].identityString);
}

std::string_view BundleReader::payload(uint32_t moduleIndex) const {
  const BundleModuleRecord &m = modules_[moduleIndex];
  return std::string_view(
      reinterpret_cast<const char *>(
          data_ + header_->payloadOffset + m.payloadOffset),
      m.payloadSize);
}

ModuleKind BundleReader::kind(uint32_t moduleIndex) const {
  return static_cast<ModuleKind>(modules_[moduleIndex].kind);
}

uint32_t BundleReader::flags(uint32_t moduleIndex) const {
  return modules_[moduleIndex].flags;
}

bool BundleReader::isRequirable(uint32_t moduleIndex) const {
  return (modules_[moduleIndex].flags & kRequirable) != 0;
}

uint32_t BundleReader::entry() const {
  return header_->entryModule;
}

uint32_t BundleReader::moduleCount() const {
  return header_->moduleCount;
}

uint32_t BundleReader::formatVersion() const {
  return header_->formatVersion;
}

uint32_t BundleReader::generationTag() const {
  return header_->generationTag;
}

uint32_t BundleReader::edgeCount() const {
  return header_->edgeCount;
}

BundleReader::EdgeView BundleReader::edge(uint32_t edgeIndex) const {
  const BundleEdgeRecord &e = edges_[edgeIndex];
  return EdgeView{e.importer, stringAt(e.specifierString), e.target};
}

uint32_t BundleReader::preloadCount() const {
  return header_->preloadCount;
}

uint32_t BundleReader::preload(uint32_t i) const {
  return preloads_[i];
}

uint32_t BundleReader::stringsSize() const {
  return header_->stringsSize;
}

uint32_t BundleReader::moduleTableSize() const {
  // Computed in 64 bits, like the tableInRange check that validated it at
  // open time: moduleCount * sizeof(record) can overflow a 32-bit product
  // before the truncating cast back to the declared uint32_t return type.
  return static_cast<uint32_t>(
      static_cast<uint64_t>(header_->moduleCount) * sizeof(BundleModuleRecord));
}

uint32_t BundleReader::edgeTableSize() const {
  return static_cast<uint32_t>(
      static_cast<uint64_t>(header_->edgeCount) * sizeof(BundleEdgeRecord));
}

uint32_t BundleReader::payloadSize() const {
  return header_->payloadSize;
}

} // namespace node_compat
} // namespace hermes
