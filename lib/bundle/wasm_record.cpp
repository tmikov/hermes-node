/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/wasm_record.h>

#include <hermes/node-compat/bundle/atomic_write.h>

#include <algorithm>
#include <cstring>
#include <sstream>

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

/// True if [offset, offset + len) fits within a buffer of \p bufSize bytes,
/// with no overflow in the addition. Both \p offset and \p len come
/// straight from an untrusted header, so the addition is done in a wider
/// type before the overflow can happen. Mirrors bundle_reader.cpp's copy of
/// the same check -- see lib/bundle/CMakeLists.txt for why this file does
/// not share that one instead of duplicating it.
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

} // namespace

WasmRecordWriter::WasmRecordWriter(std::string path, std::string buildVersion)
    : path_(std::move(path)), buildVersion_(std::move(buildVersion)) {}

bool WasmRecordWriter::record(const WasmRecordCommit &c) {
  std::string digest(
      reinterpret_cast<const char *>(c.configDigest), kNativeDigestBytes);
  std::string bytecode(
      reinterpret_cast<const char *>(c.bytecode), c.bytecodeSize);
  std::string srcDigest(
      reinterpret_cast<const char *>(c.sourceDigest), kNativeDigestBytes);
  bool changed = false;

  auto rec = std::find_if(records_.begin(), records_.end(), [&](auto &r) {
    return r.digest == digest;
  });
  if (rec == records_.end()) {
    records_.push_back(PendingRecord{std::move(digest), std::move(bytecode)});
    changed = true;
  } else if (rec->bytecode != bytecode) {
    rec->bytecode = std::move(bytecode);
    changed = true;
  }

  // Same digest, same bytes: SHA-256 over the module is the module, so a
  // second sighting adds nothing.
  bool haveSource = std::any_of(sources_.begin(), sources_.end(), [&](auto &s) {
    return s.digest == srcDigest;
  });
  if (!haveSource) {
    sources_.push_back(PendingSource{
        std::move(srcDigest),
        std::string(reinterpret_cast<const char *>(c.source), c.sourceSize)});
    changed = true;
  }
  return changed ? writeFile() : true;
}

bool WasmRecordWriter::flush() {
  return writeFile();
}

bool WasmRecordWriter::writeFile() {
  // Sorted by digest so a later reader (the bake step) can binary-search,
  // and so two writers given the same entries in a different order produce
  // byte-identical files.
  std::vector<const PendingRecord *> sorted;
  sorted.reserve(records_.size());
  for (const PendingRecord &r : records_)
    sorted.push_back(&r);
  std::sort(sorted.begin(), sorted.end(), [](auto *a, auto *b) {
    return a->digest < b->digest;
  });
  std::vector<const PendingSource *> sortedSources;
  sortedSources.reserve(sources_.size());
  for (const PendingSource &s : sources_)
    sortedSources.push_back(&s);
  std::sort(sortedSources.begin(), sortedSources.end(), [](auto *a, auto *b) {
    return a->digest < b->digest;
  });

  // Layout: header, version bytes, pad to 4, record table, pad to 4, source
  // table, pad to 8, payloads each padded to 8 -- every bytecode payload,
  // then every source payload. Source offsets are relative to payloadOffset,
  // exactly as bytecode offsets are. Every section's size is known up front,
  // so offsets in the header never need patching after the fact.
  constexpr size_t kTableAlign = alignof(BundleWasmRecord);
  constexpr size_t kSourceTableAlign = alignof(WasmSourceRecord);
  size_t headerSize = sizeof(WasmRecordHeader);
  size_t versionOffset = headerSize;
  size_t versionLength = buildVersion_.size();
  size_t recordTableOffset =
      alignUp(versionOffset + versionLength, kTableAlign);
  size_t recordTableSize = sorted.size() * sizeof(BundleWasmRecord);
  size_t sourceTableOffset =
      alignUp(recordTableOffset + recordTableSize, kSourceTableAlign);
  size_t sourceTableSize = sortedSources.size() * sizeof(WasmSourceRecord);
  size_t payloadOffset =
      alignUp(sourceTableOffset + sourceTableSize, kBundlePayloadAlign);

  std::vector<uint32_t> payloadOffsets(sorted.size());
  std::vector<uint32_t> sourceOffsets(sortedSources.size());
  size_t payloadCursor = 0;
  for (size_t i = 0; i < sorted.size(); ++i) {
    payloadOffsets[i] = static_cast<uint32_t>(payloadCursor);
    payloadCursor += sorted[i]->bytecode.size();
    payloadCursor = alignUp(payloadCursor, kBundlePayloadAlign);
  }
  for (size_t i = 0; i < sortedSources.size(); ++i) {
    sourceOffsets[i] = static_cast<uint32_t>(payloadCursor);
    payloadCursor += sortedSources[i]->bytes.size();
    payloadCursor = alignUp(payloadCursor, kBundlePayloadAlign);
  }
  size_t payloadSize = payloadCursor;

  std::vector<uint8_t> out;
  out.reserve(payloadOffset + payloadSize);

  WasmRecordHeader header{};
  std::memcpy(header.magic, kWasmRecordMagic, sizeof(header.magic));
  header.formatVersion = kWasmRecordFormatVersion;
  header.count = static_cast<uint32_t>(sorted.size());
  header.versionOffset = static_cast<uint32_t>(versionOffset);
  header.versionLength = static_cast<uint32_t>(versionLength);
  header.recordTableOffset = static_cast<uint32_t>(recordTableOffset);
  header.payloadOffset = static_cast<uint32_t>(payloadOffset);
  header.payloadSize = static_cast<uint32_t>(payloadSize);
  header.sourceTableOffset = static_cast<uint32_t>(sourceTableOffset);
  header.sourceCount = static_cast<uint32_t>(sortedSources.size());
  appendPod(out, header);

  out.insert(out.end(), buildVersion_.begin(), buildVersion_.end());
  appendPadding(out, recordTableOffset - out.size());

  for (size_t i = 0; i < sorted.size(); ++i) {
    BundleWasmRecord record{};
    std::memcpy(record.digest, sorted[i]->digest.data(), kNativeDigestBytes);
    record.payloadOffset = payloadOffsets[i];
    record.payloadSize = static_cast<uint32_t>(sorted[i]->bytecode.size());
    appendPod(out, record);
  }

  appendPadding(out, sourceTableOffset - out.size());

  for (size_t i = 0; i < sortedSources.size(); ++i) {
    WasmSourceRecord record{};
    std::memcpy(
        record.digest, sortedSources[i]->digest.data(), kNativeDigestBytes);
    record.payloadOffset = sourceOffsets[i];
    record.payloadSize = static_cast<uint32_t>(sortedSources[i]->bytes.size());
    appendPod(out, record);
  }

  appendPadding(out, payloadOffset - out.size());

  for (const PendingRecord *r : sorted) {
    out.insert(out.end(), r->bytecode.begin(), r->bytecode.end());
    size_t nextAligned = alignUp(r->bytecode.size(), kBundlePayloadAlign);
    appendPadding(out, nextAligned - r->bytecode.size());
  }
  for (const PendingSource *s : sortedSources) {
    out.insert(out.end(), s->bytes.begin(), s->bytes.end());
    size_t nextAligned = alignUp(s->bytes.size(), kBundlePayloadAlign);
    appendPadding(out, nextAligned - s->bytes.size());
  }

  // record() and flush() are bool-returning (see wasm_record.h): the caller
  // decides what a failure means and where the reason goes. It is kept
  // rather than discarded because the runtime prints it -- a --record-wasm
  // path that cannot be written must say why, and "cannot be written" alone
  // sends the reader looking for a reason this function already had.
  std::ostringstream err;
  if (writeFileAtomically(path_, out.data(), out.size(), err)) {
    lastError_.clear();
    return true;
  }
  lastError_ = err.str();
  return false;
}

std::optional<WasmRecordReader>
WasmRecordReader::open(const uint8_t *data, size_t size, std::string *error) {
  auto fail = [&](const char *message) -> std::optional<WasmRecordReader> {
    if (error != nullptr)
      *error = message;
    return std::nullopt;
  };

  if (size < kWasmRecordHeaderV1Size)
    return fail("wasm record file: truncated (shorter than the header)");
  if (std::memcmp(data, kWasmRecordMagic, sizeof(kWasmRecordMagic)) != 0)
    return fail(
        "wasm record file: not a hermes-node wasm record file (bad magic)");
  uint32_t version;
  std::memcpy(
      &version,
      data + offsetof(WasmRecordHeader, formatVersion),
      sizeof(version));
  // An unknown version is refused before anything past the common prefix is
  // interpreted: its layout is not known here.
  if (version != 1 && version != kWasmRecordFormatVersion)
    return fail(
        "wasm record file: format version mismatch (file was written by "
        "an incompatible hermes-node)");
  WasmRecordHeader h{};
  if (version == 1) {
    std::memcpy(&h, data, kWasmRecordHeaderV1Size); // v2 fields stay zero
  } else {
    if (size < sizeof(WasmRecordHeader))
      return fail("wasm record file: truncated (shorter than the header)");
    std::memcpy(&h, data, sizeof(WasmRecordHeader));
  }

  if (!inRange(size, h.versionOffset, h.versionLength))
    return fail("wasm record file: build version string out of range");

  if (!tableInRange(
          size, h.recordTableOffset, h.count, sizeof(BundleWasmRecord)))
    return fail("wasm record file: record table out of range");

  if (!inRange(size, h.payloadOffset, h.payloadSize))
    return fail("wasm record file: payload out of range");

  // The record table is read through a pointer cast straight onto the
  // buffer (BundleWasmRecord holds uint32_t fields after its digest), which
  // is undefined behavior at a misaligned address even where the target CPU
  // tolerates it. The writer always emits the table aligned; a corrupt or
  // adversarial file might not.
  if (h.recordTableOffset % alignof(BundleWasmRecord) != 0)
    return fail("wasm record file: record table offset is misaligned");

  // Payload bytes are Hermes bytecode, executed in place once baked into a
  // container -- the same alignment the container's own payload section
  // requires (see kBundlePayloadAlign in bundle_format.h).
  if (h.payloadOffset % kBundlePayloadAlign != 0)
    return fail("wasm record file: payload offset is misaligned");

  const auto *records =
      reinterpret_cast<const BundleWasmRecord *>(data + h.recordTableOffset);
  for (uint32_t i = 0; i < h.count; ++i) {
    if (!inRange(
            h.payloadSize, records[i].payloadOffset, records[i].payloadSize))
      return fail("wasm record file: record payload out of range");
  }

  const WasmSourceRecord *sources = nullptr;
  if (version != 1) {
    if (!tableInRange(
            size, h.sourceTableOffset, h.sourceCount, sizeof(WasmSourceRecord)))
      return fail("wasm record file: source table out of range");
    if (h.sourceTableOffset % alignof(WasmSourceRecord) != 0)
      return fail("wasm record file: source table offset is misaligned");
    sources =
        reinterpret_cast<const WasmSourceRecord *>(data + h.sourceTableOffset);
    for (uint32_t i = 0; i < h.sourceCount; ++i)
      if (!inRange(
              h.payloadSize, sources[i].payloadOffset, sources[i].payloadSize))
        return fail("wasm record file: source payload out of range");
  }

  // Order and per-payload alignment are writer properties, not validated
  // invariants, for both tables, as they already were for the v1 table. No
  // reader depends on either: the bake step iterates and de-duplicates
  // through a std::set, BundleWriter copies each payload and serializes its
  // own sorted, aligned table (bundle_writer.cpp), and source bytes are never
  // executed in place. So nobody should add a check believing one is
  // missing, or remove the sort in writeFile() believing one exists.

  WasmRecordReader reader;
  reader.data_ = data;
  reader.header_ = h;
  reader.records_ = records;
  reader.sources_ = sources;
  return reader;
}

uint32_t WasmRecordReader::formatVersion() const {
  return header_.formatVersion;
}

std::string_view WasmRecordReader::buildVersion() const {
  return std::string_view(
      reinterpret_cast<const char *>(data_ + header_.versionOffset),
      header_.versionLength);
}

uint32_t WasmRecordReader::count() const {
  return header_.count;
}

std::string_view WasmRecordReader::digest(uint32_t i) const {
  return std::string_view(
      reinterpret_cast<const char *>(records_[i].digest), kNativeDigestBytes);
}

std::string_view WasmRecordReader::payload(uint32_t i) const {
  const BundleWasmRecord &r = records_[i];
  return std::string_view(
      reinterpret_cast<const char *>(
          data_ + header_.payloadOffset + r.payloadOffset),
      r.payloadSize);
}

uint32_t WasmRecordReader::sourceCount() const {
  return header_.sourceCount;
}

std::string_view WasmRecordReader::sourceDigest(uint32_t i) const {
  return std::string_view(
      reinterpret_cast<const char *>(sources_[i].digest), kNativeDigestBytes);
}

std::string_view WasmRecordReader::source(uint32_t i) const {
  const WasmSourceRecord &r = sources_[i];
  return std::string_view(
      reinterpret_cast<const char *>(
          data_ + header_.payloadOffset + r.payloadOffset),
      r.payloadSize);
}

} // namespace node_compat
} // namespace hermes
