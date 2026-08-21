/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_writer.h>

#include <algorithm>
#include <cstring>
#include <tuple>

namespace hermes {
namespace node_compat {

namespace {

/// Rounds \p n up to the next multiple of \p align. \p align must be a
/// power of two.
size_t alignUp(size_t n, size_t align) {
  return (n + align - 1) & ~(align - 1);
}

/// Appends \p n zero bytes to \p out.
void appendPadding(std::vector<uint8_t> &out, size_t n) {
  out.insert(out.end(), n, 0);
}

/// Appends the raw bytes of \p value to \p out.
template <typename T>
void appendPod(std::vector<uint8_t> &out, const T &value) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

} // namespace

uint32_t BundleWriter::internString(std::string_view s) {
  auto it = internTable_.find(s);
  if (it != internTable_.end())
    return it->second;
  uint32_t offset = static_cast<uint32_t>(stringBytes_.size());
  BundleStringHeader header{static_cast<uint32_t>(s.size())};
  stringBytes_.append(reinterpret_cast<const char *>(&header), sizeof(header));
  stringBytes_.append(s.data(), s.size());
  internTable_.emplace(std::string(s), offset);
  return offset;
}

uint32_t BundleWriter::addModule(
    std::string_view identity,
    ModuleKind kind,
    uint32_t flags,
    std::string_view payload) {
  uint32_t identityString = internString(identity);
  modules_.push_back(
      PendingModule{identityString, kind, flags, std::string(payload)});
  return static_cast<uint32_t>(modules_.size() - 1);
}

void BundleWriter::addEdge(
    uint32_t importer,
    std::string_view specifier,
    uint32_t target) {
  edges_.push_back(PendingEdge{importer, std::string(specifier), target});
}

void BundleWriter::addPreload(uint32_t moduleIndex) {
  preloads_.push_back(moduleIndex);
}

void BundleWriter::addNative(
    uint32_t moduleIndex,
    std::string_view sidecar,
    uint32_t byteLength,
    std::string_view rawDigest) {
  natives_.push_back(PendingNative{
      moduleIndex, std::string(sidecar), byteLength, std::string(rawDigest)});
}

size_t BundleWriter::stringCount() const {
  return internTable_.size();
}

void BundleWriter::setEntry(uint32_t moduleIndex) {
  entry_ = moduleIndex;
  hasEntry_ = true;
}

std::vector<uint8_t> BundleWriter::serialize(uint32_t generationTag) {
  // No modules, or a caller who forgot setEntry(): either way there is no
  // valid entryModule to emit, so refuse rather than silently defaulting
  // to module 0.
  if (modules_.empty() || !hasEntry_)
    return {};

  // Sort edges by (importer, specifier bytes) -- the byte order the
  // reader's binary search relies on.
  std::sort(
      edges_.begin(),
      edges_.end(),
      [](const PendingEdge &a, const PendingEdge &b) {
        return std::tie(a.importer, a.specifier) <
            std::tie(b.importer, b.specifier);
      });

  // Intern every edge specifier before the string table's size is read
  // below. internString() may still be growing stringBytes_ at this point
  // (module identities were already interned in addModule, but specifiers
  // are not interned until here) -- the layout computed further down
  // freezes stringsSize, so nothing may append to the string table after
  // this loop.
  std::vector<uint32_t> edgeSpecifierStrings(edges_.size());
  for (size_t i = 0; i < edges_.size(); ++i)
    edgeSpecifierStrings[i] = internString(edges_[i].specifier);

  // Sort by module index: that is the order BundleReader::nativeFor()
  // binary-searches. The producer adds natives in discovery order, which
  // is not module-index order once an --include seeds one late.
  std::sort(
      natives_.begin(),
      natives_.end(),
      [](const PendingNative &a, const PendingNative &b) {
        return a.moduleIndex < b.moduleIndex;
      });
  std::vector<uint32_t> nativeSidecarStrings(natives_.size());
  std::vector<uint32_t> nativeDigestStrings(natives_.size());
  for (size_t i = 0; i < natives_.size(); ++i) {
    nativeSidecarStrings[i] = internString(natives_[i].sidecar);
    nativeDigestStrings[i] = internString(natives_[i].digest);
  }

  // Layout: header, strings, module table, edge table, preload table,
  // native table, payload. Each section is computed before any bytes are
  // emitted so offsets in the header are known up front.
  //
  // String entries are packed with no padding (length header immediately
  // followed by bytes, back to back), so the string table's raw size is
  // not generally a multiple of 4. The reader casts pointers directly onto
  // the module and edge tables, whose records are all-uint32_t and need
  // 4-byte alignment, so the gap between the string table and the module
  // table is padded out to a 4-byte boundary. header.stringsSize stays the
  // unpadded content size; only moduleTableOffset moves.
  constexpr size_t kTableAlign = alignof(uint32_t);
  size_t headerSize = sizeof(BundleHeader);
  size_t stringsOffset = headerSize;
  size_t stringsSize = stringBytes_.size();
  size_t moduleTableOffset = alignUp(stringsOffset + stringsSize, kTableAlign);
  size_t moduleTableSize = modules_.size() * sizeof(BundleModuleRecord);
  size_t edgeTableOffset = moduleTableOffset + moduleTableSize;
  size_t edgeTableSize = edges_.size() * sizeof(BundleEdgeRecord);
  size_t preloadTableOffset = edgeTableOffset + edgeTableSize;
  size_t preloadTableSize = preloads_.size() * sizeof(uint32_t);
  size_t nativeTableOffset = preloadTableOffset + preloadTableSize;
  size_t nativeTableSize = natives_.size() * sizeof(BundleNativeRecord);
  size_t payloadOffset =
      alignUp(nativeTableOffset + nativeTableSize, kBundlePayloadAlign);

  // Each payload's offset (relative to payloadOffset) and the total,
  // aligned payload size.
  std::vector<uint32_t> payloadOffsets(modules_.size());
  size_t payloadCursor = 0;
  for (size_t i = 0; i < modules_.size(); ++i) {
    payloadOffsets[i] = static_cast<uint32_t>(payloadCursor);
    payloadCursor += modules_[i].payload.size();
    payloadCursor = alignUp(payloadCursor, kBundlePayloadAlign);
  }
  size_t payloadSize = payloadCursor;

  std::vector<uint8_t> out;
  out.reserve(payloadOffset + payloadSize);

  BundleHeader header{};
  std::memcpy(header.magic, kBundleMagic, sizeof(header.magic));
  header.formatVersion = kBundleFormatVersion;
  header.generationTag = generationTag;
  header.entryModule = entry_;
  header.stringsOffset = static_cast<uint32_t>(stringsOffset);
  header.stringsSize = static_cast<uint32_t>(stringsSize);
  header.moduleTableOffset = static_cast<uint32_t>(moduleTableOffset);
  header.moduleCount = static_cast<uint32_t>(modules_.size());
  header.edgeTableOffset = static_cast<uint32_t>(edgeTableOffset);
  header.edgeCount = static_cast<uint32_t>(edges_.size());
  header.preloadTableOffset = static_cast<uint32_t>(preloadTableOffset);
  header.preloadCount = static_cast<uint32_t>(preloads_.size());
  header.nativeTableOffset = static_cast<uint32_t>(nativeTableOffset);
  header.nativeCount = static_cast<uint32_t>(natives_.size());
  header.payloadOffset = static_cast<uint32_t>(payloadOffset);
  header.payloadSize = static_cast<uint32_t>(payloadSize);
  appendPod(out, header);

  out.insert(out.end(), stringBytes_.begin(), stringBytes_.end());
  appendPadding(out, moduleTableOffset - out.size());

  for (size_t i = 0; i < modules_.size(); ++i) {
    const PendingModule &m = modules_[i];
    BundleModuleRecord record{};
    record.identityString = m.identityString;
    record.kind = static_cast<uint32_t>(m.kind);
    record.flags = m.flags;
    record.payloadOffset = payloadOffsets[i];
    record.payloadSize = static_cast<uint32_t>(m.payload.size());
    appendPod(out, record);
  }

  for (size_t i = 0; i < edges_.size(); ++i) {
    const PendingEdge &e = edges_[i];
    BundleEdgeRecord record{};
    record.importer = e.importer;
    record.specifierString = edgeSpecifierStrings[i];
    record.target = e.target;
    appendPod(out, record);
  }

  for (uint32_t moduleIndex : preloads_)
    appendPod(out, moduleIndex);

  for (size_t i = 0; i < natives_.size(); ++i) {
    BundleNativeRecord record{};
    record.moduleIndex = natives_[i].moduleIndex;
    record.sidecarString = nativeSidecarStrings[i];
    record.byteLength = natives_[i].byteLength;
    record.hashString = nativeDigestStrings[i];
    appendPod(out, record);
  }

  appendPadding(out, payloadOffset - out.size());

  for (size_t i = 0; i < modules_.size(); ++i) {
    const PendingModule &m = modules_[i];
    out.insert(out.end(), m.payload.begin(), m.payload.end());
    size_t nextAligned = alignUp(m.payload.size(), kBundlePayloadAlign);
    appendPadding(out, nextAligned - m.payload.size());
  }

  return out;
}

} // namespace node_compat
} // namespace hermes
